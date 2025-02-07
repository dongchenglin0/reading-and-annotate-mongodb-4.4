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

#pragma once

#include <wiredtiger.h>

#include <boost/optional.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/util/timer.h"

namespace mongo {

using RoundUpPreparedTimestamps = WiredTigerBeginTxnBlock::RoundUpPreparedTimestamps;
using RoundUpReadTimestamp = WiredTigerBeginTxnBlock::RoundUpReadTimestamp;

class BSONObjBuilder;


//慢日志中的"storage":{"data":{"bytesRead":3679,"timeReadingMicros":58}}就是从存储引擎获取的
//表示指定操作在存储引擎层面的统计数据
class WiredTigerOperationStats final : public StorageStats {
public:
    /**
     *  There are two types of statistics provided by WiredTiger engine - data and wait.
     */
    enum class Section { DATA, WAIT };

    BSONObj toBSON() final;

    StorageStats& operator+=(const StorageStats&) final;

    WiredTigerOperationStats& operator+=(const WiredTigerOperationStats&);

    /**
     * Fetches an operation's storage statistics from WiredTiger engine.
     */
    void fetchStats(WT_SESSION*, const std::string&, const std::string&);

    std::shared_ptr<StorageStats> getCopy() final;

private:
    /**
     * Each statistic in WiredTiger has an integer key, which this map associates with a section
     * (either DATA or WAIT) and user-readable name.
     */
    static std::map<int, std::pair<StringData, Section>> _statNameMap;

    /**
     * Stores the value for each statistic returned by a WiredTiger cursor. Each statistic is
     * associated with an integer key, which can be mapped to a name and section using the
     * '_statNameMap'.
     */
    std::map<int, long long> _stats;
};

/*
insertDocuments {
    WriteUnitOfWork wuow1(opCtx);
    ...
    CollectionImpl::insertDocuments->OpObserverImpl::onInserts
    {
        WriteUnitOfWork wuow2(opCtx);
        ...
        wuow2.commit();
    }
    ...
    wuow1.commit(); 
}

wuow1和wuow2的_opCtx是一样的，所以对应的_opCtx->recoveryUnit()也是同一个

*/
class WiredTigerRecoveryUnit final : public RecoveryUnit {
public:
    WiredTigerRecoveryUnit(WiredTigerSessionCache* sc);

    /**
     * It's expected a consumer would want to call the constructor that simply takes a
     * `WiredTigerSessionCache`. That constructor accesses the `WiredTigerKVEngine` to find the
     * `WiredTigerOplogManager`. However, unit tests construct `WiredTigerRecoveryUnits` with a
     * `WiredTigerSessionCache` that do not have a valid `WiredTigerKVEngine`. This constructor is
     * expected to only be useful in those cases.
     */
    WiredTigerRecoveryUnit(WiredTigerSessionCache* sc, WiredTigerOplogManager* oplogManager);
    ~WiredTigerRecoveryUnit();

    void beginUnitOfWork(OperationContext* opCtx) override;
    void prepareUnitOfWork() override;

    bool waitUntilDurable(OperationContext* opCtx) override;

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx, bool stableCheckpoint) override;

    void preallocateSnapshot() override;

    Status obtainMajorityCommittedSnapshot() override;

    boost::optional<Timestamp> getPointInTimeReadTimestamp() override;

    Status setTimestamp(Timestamp timestamp) override;

    void setCommitTimestamp(Timestamp timestamp) override;

    void clearCommitTimestamp() override;

    Timestamp getCommitTimestamp() const override;

    void setDurableTimestamp(Timestamp timestamp) override;

    Timestamp getDurableTimestamp() const override;

    void setPrepareTimestamp(Timestamp timestamp) override;

    Timestamp getPrepareTimestamp() const override;

    void setPrepareConflictBehavior(PrepareConflictBehavior behavior) override;

    PrepareConflictBehavior getPrepareConflictBehavior() const override;

    void setRoundUpPreparedTimestamps(bool value) override;

    void setCatalogConflictingTimestamp(Timestamp timestamp) override;

    Timestamp getCatalogConflictingTimestamp() const override;

    void setTimestampReadSource(ReadSource source,
                                boost::optional<Timestamp> provided = boost::none) override;

    ReadSource getTimestampReadSource() const override;

    //wiredTiger_recovery_unit.h中的setOrderedCommit
    virtual void setOrderedCommit(bool orderedCommit) override {
        _orderedCommit = orderedCommit;
    }

    void setReadOnce(bool readOnce) override {
        // Do not allow a session to use readOnce and regular cursors at the same time.
        invariant(!_isActive() || readOnce == _readOnce || getSession()->cursorsOut() == 0);
        _readOnce = readOnce;
    };

    bool getReadOnce() const override {
        return _readOnce;
    };

    std::shared_ptr<StorageStats> getOperationStatistics() const override;

    void refreshSnapshot() override;

    // ---- WT STUFF

    WiredTigerSession* getSession();
    void setIsOplogReader() {
        _isOplogReader = true;
    }

    bool getIsOplogReader() const {
        return _isOplogReader;
    }

    /**
     * Enter a period of wait or computation during which there are no WT calls.
     * Any non-relevant cached handles can be closed.
     */
    void beginIdle();

    /**
     * Returns a session without starting a new WT txn on the session. Will not close any already
     * running session.
     */

    WiredTigerSession* getSessionNoTxn();

    WiredTigerSessionCache* getSessionCache() {
        return _sessionCache;
    }
    bool inActiveTxn() const {
        return _isActive();
    }
    void assertInActiveTxn() const;

    boost::optional<int64_t> getOplogVisibilityTs();

    static WiredTigerRecoveryUnit* get(OperationContext* opCtx) {
        return checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());
    }

    static void appendGlobalStats(BSONObjBuilder& b);

private:
    void doCommitUnitOfWork() override;
    void doAbortUnitOfWork() override;

    void doAbandonSnapshot() override;

    void _abort();
    void _commit();

    void _ensureSession();
    void _txnClose(bool commit);
    void _txnOpen();

    /**
     * Starts a transaction at the current all_durable timestamp.
     * Returns the timestamp the transaction was started at.
     */
    Timestamp _beginTransactionAtAllDurableTimestamp(WT_SESSION* session);

    /**
     * Starts a transaction at the no-overlap timestamp. Returns the timestamp the transaction
     * was started at.
     */
    Timestamp _beginTransactionAtNoOverlapTimestamp(WT_SESSION* session);

    /**
     * Starts a transaction at the lastApplied timestamp. Returns the timestamp at which the
     * transaction was started.
     */
    Timestamp _beginTransactionAtLastAppliedTimestamp(WT_SESSION* session);

    /**
     * Returns the timestamp at which the current transaction is reading.
     */
    Timestamp _getTransactionReadTimestamp(WT_SESSION* session);

    WiredTigerSessionCache* _sessionCache;  // not owned
    WiredTigerOplogManager* _oplogManager;  // not owned
    UniqueWiredTigerSession _session;
    bool _isTimestamped = false;

    // Specifies which external source to use when setting read timestamps on transactions.
    ReadSource _timestampReadSource = ReadSource::kNoTimestamp;

    // Commits are assumed ordered.  Unordered commits are assumed to always need to reserve a
    // new optime, and thus always call oplogDiskLocRegister() on the record store.
    bool _orderedCommit = true;

    // When 'true', data read from disk should not be kept in the storage engine cache.
    bool _readOnce = false;

    // The behavior of handling prepare conflicts.
    PrepareConflictBehavior _prepareConflictBehavior{PrepareConflictBehavior::kEnforce};
    // Dictates whether to round up prepare and commit timestamp of a prepared transaction.
    RoundUpPreparedTimestamps _roundUpPreparedTimestamps{RoundUpPreparedTimestamps::kNoRound};
    Timestamp _commitTimestamp;
    Timestamp _durableTimestamp;
    Timestamp _prepareTimestamp;
    boost::optional<Timestamp> _lastTimestampSet;
    Timestamp _majorityCommittedSnapshot;
    Timestamp _readAtTimestamp;
    Timestamp _catalogConflictTimestamp;
    std::unique_ptr<Timer> _timer;
    bool _isOplogReader = false;
    boost::optional<int64_t> _oplogVisibleTs = boost::none;
};

}  // namespace mongo
