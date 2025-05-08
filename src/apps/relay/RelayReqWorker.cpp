#include "RelayServer.h"
#include "QueryScheduler.h"


void RelayServer::runReqWorker(ThreadPool<MsgReqWorker>::Thread &thr) {
    Decompressor decomp;
    QueryScheduler queries;
    
    // For COUNT requests, keep track of the count
    using CountKey = std::pair<uint64_t, SubId>;
    flat_hash_map<CountKey, uint64_t> countResults;
    
    auto makeCountKey = [](const Subscription &sub) {
        return std::make_pair(sub.connId, sub.subId);
    };

    queries.onEvent = [&](lmdb::txn &txn, const auto &sub, uint64_t levId, std::string_view eventPayload){
        if (sub.isCount) {
            // For COUNT requests, increment the counter instead of sending events
            auto key = makeCountKey(sub);
            
            if (countResults.find(key) == countResults.end()) {
                countResults[key] = 1;
            } else {
                countResults[key]++;
            }
        } else {
            // For normal REQ requests, send events as usual
            sendEvent(sub.connId, sub.subId, decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr));
        }
    };

    queries.onComplete = [&](lmdb::txn &, Subscription &sub){
        if (sub.isCount) {
            // For COUNT requests, send the count result
            auto key = makeCountKey(sub);
            
            // Get the count (if the key doesn't exist, this will default to 0)
            uint64_t count = 0;
            if (countResults.find(key) != countResults.end()) {  // Compatible with all C++ standards
                count = countResults[key];
                countResults.erase(key);  // Clean up the entry
            }
            
            // Send COUNT response
            auto reply = tao::json::value::array({ "COUNT", sub.subId.str(), { { "count", count } } });
            sendToConn(sub.connId, tao::json::to_string(reply));
            //TODO: remove subscription?
        } else {
            // For normal REQ requests, send EOSE and add to monitor as usual
            sendToConn(sub.connId, tao::json::to_string(tao::json::value::array({ "EOSE", sub.subId.str() })));
            tpReqMonitor.dispatch(sub.connId, MsgReqMonitor{MsgReqMonitor::NewSub{std::move(sub)}});
        }
    };

    while(1) {
        auto newMsgs = queries.running.empty() ? thr.inbox.pop_all() : thr.inbox.pop_all_no_wait();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReqWorker::NewSub>(&newMsg.msg)) {
                auto connId = msg->sub.connId;

                if (!queries.addSub(txn, std::move(msg->sub))) {
                    sendNoticeError(connId, std::string("too many concurrent REQs"));
                }

                queries.process(txn);
            } else if (auto msg = std::get_if<MsgReqWorker::RemoveSub>(&newMsg.msg)) {
                queries.removeSub(msg->connId, msg->subId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::RemoveSub{msg->connId, msg->subId}});
            } else if (auto msg = std::get_if<MsgReqWorker::CloseConn>(&newMsg.msg)) {
                queries.closeConn(msg->connId);
                tpReqMonitor.dispatch(msg->connId, MsgReqMonitor{MsgReqMonitor::CloseConn{msg->connId}});
            }
        }

        queries.process(txn);

        txn.abort();
    }
}
