/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/kill_sessions_local.h"

#include "mongo/db/client.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

/**
 * Shortcut method shared by the various forms of session kill below. Every session kill operation
 * consists of the following stages:
 *  1) Select the sessions to kill, based on their lsid or owning user account (achieved through the
 *     'matcher') and further refining that list through the 'filterFn'.
 *  2) If any of the selected sessions are currently checked out, interrupt the owning operation
 *     context with 'reason' as the code.
 *  3) Finish killing the selected and interrupted sessions through the 'killSessionFn'.
 */ 
void killSessionsAction(
    OperationContext* opCtx,
    const SessionKiller::Matcher& matcher,
    const std::function<bool(const ObservableSession&)>& filterFn,
    const std::function<void(OperationContext*, const SessionToKill&)>& killSessionFn,
    ErrorCodes::Error reason = ErrorCodes::Interrupted) {
    const auto catalog = SessionCatalog::get(opCtx);

    std::vector<SessionCatalog::KillToken> sessionKillTokens;
    catalog->scanSessions(matcher, [&](const ObservableSession& session) {
        if (filterFn(session))
            sessionKillTokens.emplace_back(session.kill(reason));
    });

    for (auto& sessionKillToken : sessionKillTokens) {
        auto session = catalog->checkOutSessionForKill(opCtx, std::move(sessionKillToken));

        // TODO (SERVER-33850): Rename KillAllSessionsByPattern and
        // ScopedKillAllSessionsByPatternImpersonator to not refer to session kill
        const KillAllSessionsByPattern* pattern = matcher.match(session.getSessionId());
        invariant(pattern);

        ScopedKillAllSessionsByPatternImpersonator impersonator(opCtx, *pattern);
        killSessionFn(opCtx, session);
    }
}

}  // namespace

void killSessionsAbortUnpreparedTransactions(OperationContext* opCtx,
                                             const SessionKiller::Matcher& matcher,
                                             ErrorCodes::Error reason) {
    killSessionsAction(opCtx,
                       matcher,
                       [](const ObservableSession& session) {
                           auto participant = TransactionParticipant::get(session);
                           return participant.transactionIsInProgress();
                       },
                       [](OperationContext* opCtx, const SessionToKill& session) {
                           auto participant = TransactionParticipant::get(session);
                           // This is the same test as in the filter, but we must check again now
                           // that the session is checked out.
                           if (participant.transactionIsInProgress()) {
                               participant.abortTransaction(opCtx);
                           }
                       },
                       reason);
}

SessionKiller::Result killSessionsLocal(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher,
                                        SessionKiller::UniformRandomBitGenerator* urbg) {
    killSessionsAbortUnpreparedTransactions(opCtx, matcher);
    uassertStatusOK(killSessionsLocalKillOps(opCtx, matcher));

    auto res = CursorManager::get(opCtx)->killCursorsWithMatchingSessions(opCtx, matcher);
    uassertStatusOK(res.first);

    return {std::vector<HostAndPort>{}};
}


/*
不采用 「Query Yielding」也就意味着存在上节所说的“WiredTiger Cache 压力过大”的问题，在 “snapshot” readConcern 下，
当前版本没有太好的解法（在 4.4 中会通过 durable history，即支持把多版本数据写到磁盘，而不是只保存在内存中来解决这个问题）。
MongoDB 目前采用了另外一个比较简单粗暴的方式来缓解这个问题，即限制事务执行的时长，transactionLifetimeLimitSeconds 配置
的值决定了多文档事务的最大执行时长，默认为 60 秒。

超出最大执行时长的事务由后台线程负责清理，默认每 30 秒进行一次清理动作。每个多文档事务都会和一个 Logical Session 关联，
清理线程会遍历内存中的 SessionCatalog 缓存找到所有过期事务，清理和事务关联的 Session，然后 abortTransaction（具体可参
考killAllExpiredTransactions()）。
*/
void killAllExpiredTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(
        opCtx,
        matcherAllSessions,
        [when = opCtx->getServiceContext()->getPreciseClockSource()->now()](
            const ObservableSession& session) {
            return TransactionParticipant::get(session).expiredAsOf(when);
        },
        [](OperationContext* opCtx, const SessionToKill& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            // If the transaction is aborted here, it means it was aborted after
            // the filter.  The most likely reason for this is that the transaction
            // was active and the session kill aborted it.  We still want to log
            // that as aborted due to transactionLifetimeLimitSessions.
            if (txnParticipant.transactionIsInProgress() || txnParticipant.transactionIsAborted()) {
                LOGV2(
                    20707,
                    "Aborting transaction with session id {sessionId} and txnNumber {txnNumber}  "
                    "because it has been running for longer than 'transactionLifetimeLimitSeconds'",
                    "Aborting transaction because it has been running for longer than "
                    "'transactionLifetimeLimitSeconds'",
                    "sessionId"_attr = session.getSessionId().getId(),
                    "txnNumber"_attr = txnParticipant.getActiveTxnNumber());
                if (txnParticipant.transactionIsInProgress()) {
                    txnParticipant.abortTransaction(opCtx);
                }
            }
        },
        ErrorCodes::TransactionExceededLifetimeLimitSeconds);
}

void killSessionsLocalShutdownAllTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(opCtx,
                       matcherAllSessions,
                       [](const ObservableSession& session) {
                           return TransactionParticipant::get(session).transactionIsOpen();
                       },
                       [](OperationContext* opCtx, const SessionToKill& session) {
                           TransactionParticipant::get(session).shutdown(opCtx);
                       },
                       ErrorCodes::InterruptedAtShutdown);
}

void killSessionsAbortAllPreparedTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(opCtx,
                       matcherAllSessions,
                       [](const ObservableSession& session) {
                           // Filter for sessions that have a prepared transaction.
                           return TransactionParticipant::get(session).transactionIsPrepared();
                       },
                       [](OperationContext* opCtx, const SessionToKill& session) {
                           // We're holding the RSTL, so the transaction shouldn't be otherwise
                           // affected.
                           invariant(TransactionParticipant::get(session).transactionIsPrepared());
                           // Abort the prepared transaction and invalidate the session it is
                           // associated with.
                           TransactionParticipant::get(session).abortTransaction(opCtx);
                           TransactionParticipant::get(session).invalidate(opCtx);
                       });
}

void yieldLocksForPreparedTransactions(OperationContext* opCtx) {
    // Create a new opCtx because we need an empty locker to refresh the locks.
    auto newClient = opCtx->getServiceContext()->makeClient("prepared-txns-yield-locks");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();

    // Scan the sessions again to get the list of all sessions with prepared transaction
    // to yield their locks.
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(newOpCtx.get())});
    killSessionsAction(newOpCtx.get(),
                       matcherAllSessions,
                       [](const ObservableSession& session) {
                           return TransactionParticipant::get(session).transactionIsPrepared();
                       },
                       [](OperationContext* killerOpCtx, const SessionToKill& session) {
                           auto txnParticipant = TransactionParticipant::get(session);
                           // Yield locks for prepared transactions. When scanning and killing
                           // operations, all prepared transactions are included in the list. Even
                           // though new sessions may be created after the scan, none of them can
                           // become prepared during stepdown, since the RSTL has been enqueued,
                           // preventing any new writes.
                           if (txnParticipant.transactionIsPrepared()) {
                               LOGV2_DEBUG(20708,
                                           3,
                                           "Yielding locks of prepared transaction. SessionId: "
                                           "{sessionId} TxnNumber: {txnNumber}",
                                           "Yielding locks of prepared transaction",
                                           "sessionId"_attr = session.getSessionId().getId(),
                                           "txnNumber"_attr = txnParticipant.getActiveTxnNumber());
                               txnParticipant.refreshLocksForPreparedTransaction(killerOpCtx, true);
                           }
                       },
                       ErrorCodes::InterruptedDueToReplStateChange);
}

void invalidateSessionsForStepdown(OperationContext* opCtx) {
    // It is illegal to invalidate the sessions if the operation has a session checked out.
    invariant(!OperationContextSession::get(opCtx));

    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(opCtx,
                       matcherAllSessions,
                       [](const ObservableSession& session) {
                           return !TransactionParticipant::get(session).transactionIsPrepared();
                       },
                       [](OperationContext* killerOpCtx, const SessionToKill& session) {
                           auto txnParticipant = TransactionParticipant::get(session);
                           if (!txnParticipant.transactionIsPrepared()) {
                               txnParticipant.invalidate(killerOpCtx);
                           }
                       },
                       ErrorCodes::InterruptedDueToReplStateChange);
}

}  // namespace mongo
