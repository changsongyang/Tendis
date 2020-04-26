#include <list>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <string>
#include <set>
#include <map>
#include <limits>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/en.h"
#include "glog/logging.h"
#include "tendisplus/replication/repl_manager.h"
#include "tendisplus/storage/record.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/rate_limiter.h"
#include "tendisplus/lock/lock.h"
#include "tendisplus/network/network.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/server/session.h"

namespace tendisplus {

const char* ReplStateStr[] = {
        "none",
        "connecting",
        "send_bulk",
        "online",
        "error"
};

const char* FullPushStateStr[] = {
    "send_bulk",
    "send_bulk_success",
    "send_bulk_err",
};

template <typename T>
std::string getEnumStr(T element) {
    size_t size = 0;
    const char** arr = nullptr;
    if (typeid(T) == typeid(FullPushState)) {
        arr = FullPushStateStr;
        size = sizeof(FullPushStateStr) / sizeof(FullPushStateStr[0]);
    } else if (typeid(T) == typeid(ReplState)) {
        arr = ReplStateStr;
        size = sizeof(ReplStateStr) / sizeof(ReplStateStr[0]);
    } else {
        INVARIANT_D(0);
        return "not support";
    }

    uint32_t index = static_cast<uint32_t>(element);
    if (index >= size) {
        INVARIANT_D(0);
        return "not support";
    }

    return arr[index];
}

std::string MPovFullPushStatus::toString() {
    stringstream ss_state;
    ss_state << "storeId:" << storeid
        << " node:" << slave_listen_ip << ":" << slave_listen_port
        << " state:" << getEnumStr(state)
        << " binlogPos:" << binlogPos
        << " startTime:" << startTime.time_since_epoch().count() / 1000000
        << " endTime:" << endTime.time_since_epoch().count() / 1000000;
    return ss_state.str();
}

ReplManager::ReplManager(std::shared_ptr<ServerEntry> svr,
                          const std::shared_ptr<ServerParams> cfg)
    :_cfg(cfg),
     _isRunning(false),
     _svr(svr),
     _rateLimiter(std::make_unique<RateLimiter>(cfg->binlogRateLimitMB*1024*1024)),
     _incrPaused(false),
     _clientIdGen(0),
     _dumpPath(cfg->dumpPath),
     _fullPushMatrix(std::make_shared<PoolMatrix>()),
     _incrPushMatrix(std::make_shared<PoolMatrix>()),
     _fullReceiveMatrix(std::make_shared<PoolMatrix>()),
     _incrCheckMatrix(std::make_shared<PoolMatrix>()),
     _logRecycleMatrix(std::make_shared<PoolMatrix>()),
     _connectMasterTimeoutMs(1000) {
}

Status ReplManager::stopStore(uint32_t storeId) {
    std::lock_guard<std::mutex> lk(_mutex);

    INVARIANT_D(storeId < _svr->getKVStoreCount());

    _syncStatus[storeId]->nextSchedTime = SCLOCK::time_point::max();

    _logRecycStatus[storeId]->nextSchedTime = SCLOCK::time_point::max();

    for (auto& mpov : _pushStatus[storeId]) {
        mpov.second->nextSchedTime = SCLOCK::time_point::max();
    }
    _fullPushStatus[storeId].clear();

    return { ErrorCodes::ERR_OK, "" };
}

Status ReplManager::startup() {
    std::lock_guard<std::mutex> lk(_mutex);
    Catalog *catalog = _svr->getCatalog();
    INVARIANT(catalog != nullptr);

    for (uint32_t i = 0; i < _svr->getKVStoreCount(); i++) {
        Expected<std::unique_ptr<StoreMeta>> meta = catalog->getStoreMeta(i);
        if (meta.ok()) {
            _syncMeta.emplace_back(std::move(meta.value()));
        } else if (meta.status().code() == ErrorCodes::ERR_NOTFOUND) {
            auto pMeta = std::unique_ptr<StoreMeta>(
                new StoreMeta(i, "", 0, -1,
                    Transaction::TXNID_UNINITED, ReplState::REPL_NONE));
            Status s = catalog->setStoreMeta(*pMeta);
            if (!s.ok()) {
                return s;
            }
            _syncMeta.emplace_back(std::move(pMeta));
        } else {
            return meta.status();
        }
    }

    INVARIANT(_syncMeta.size() == _svr->getKVStoreCount());

    for (size_t i = 0; i < _syncMeta.size(); ++i) {
        if (i != _syncMeta[i]->id) {
            std::stringstream ss;
            ss << "meta:" << i << " has id:" << _syncMeta[i]->id;
            return {ErrorCodes::ERR_INTERNAL, ss.str()};
        }
    }

    _incrPusher = std::make_unique<WorkerPool>(
            "repl-minc", _incrPushMatrix);
    Status s = _incrPusher->startup(_cfg->incrPushThreadnum);
    if (!s.ok()) {
        return s;
    }
    _fullPusher = std::make_unique<WorkerPool>(
            "repl-mfull", _fullPushMatrix);
    s = _fullPusher->startup(_cfg->fullPushThreadnum);
    if (!s.ok()) {
        return s;
    }

    _fullReceiver = std::make_unique<WorkerPool>(
            "repl-sfull", _fullReceiveMatrix);
    s = _fullReceiver->startup(_cfg->fullReceiveThreadnum);
    if (!s.ok()) {
        return s;
    }

    _incrChecker = std::make_unique<WorkerPool>(
            "repl-scheck", _incrCheckMatrix);
    s = _incrChecker->startup(2);
    if (!s.ok()) {
        return s;
    }

    _logRecycler = std::make_unique<WorkerPool>(
            "log-recyc", _logRecycleMatrix);
    s = _logRecycler->startup(_cfg->logRecycleThreadnum);
    if (!s.ok()) {
        return s;
    }

    for (uint32_t i = 0; i < _svr->getKVStoreCount(); i++) {
        // here we are starting up, dont acquire a storelock.
        auto expdb = _svr->getSegmentMgr()->getDb(nullptr, i,
                            mgl::LockMode::LOCK_NONE, true);
        if (!expdb.ok()) {
            return expdb.status();
        }
        auto store = std::move(expdb.value().store);
        INVARIANT(store != nullptr);

        bool isOpen = store->isOpen();
        SCLOCK::time_point tp = SCLOCK::now();
        uint32_t fileSeq = std::numeric_limits<uint32_t>::max();

        if (!isOpen) {
            LOG(INFO) << "store:" << i
                << " is not opened";

            // NOTE(vinchen): Here the max timepiont value is tp used
            // to control the _syncStatus/_logRecycStatus
            // do nothing when the storeMode == STORE_NONE.
            // And it would be easier to reopen the closed store in the
            // future.
            tp = SCLOCK::time_point::max();
        }

        _syncStatus.emplace_back(
            std::unique_ptr<SPovStatus>(
                new SPovStatus{
                     false,
                     std::numeric_limits<uint64_t>::max(),
                     tp,
                     tp,
        }));

        // NOTE(vinchen): if the mode == STORE_NONE, _pushStatus would do
        // nothing, more detailed in ReplManager::registerIncrSync()
        // init master's pov, incrpush status
#if defined(_WIN32) && _MSC_VER > 1900
        _pushStatus.emplace_back(
            std::map<uint64_t, MPovStatus*>());
        _fullPushStatus.emplace_back(
            std::map<string, MPovFullPushStatus*>());
#else
        _pushStatus.emplace_back(
            std::map<uint64_t, std::unique_ptr<MPovStatus>>());

        _fullPushStatus.emplace_back(
                std::map<string, std::unique_ptr<MPovFullPushStatus>>());
#endif

        Status status;

        if (isOpen) {
            if (_syncMeta[i]->syncFromHost == "") {
                status = _svr->setStoreMode(store,
                    KVStore::StoreMode::READ_WRITE);
            } else {
                status = _svr->setStoreMode(store,
                    KVStore::StoreMode::REPLICATE_ONLY);

                // NOTE(vinchen): the binlog of slave is sync from master,
                // when the slave startup, _syncMeta[i]->binlogId should depend
                // on store->getHighestBinlogId();
                _syncMeta[i]->binlogId = store->getHighestBinlogId();
            }
            if (!status.ok()) {
                return status;
            }

            Expected<uint32_t> efileSeq = maxDumpFileSeq(i);
            if (!efileSeq.ok()) {
                return efileSeq.status();
            }
            fileSeq = efileSeq.value();
        }

        auto recBinlogStat = std::unique_ptr<RecycleBinlogStatus>(
            new RecycleBinlogStatus {
                false,
                tp,
                Transaction::TXNID_UNINITED,
                Transaction::TXNID_UNINITED,
                fileSeq,
                0,
                tp,
                0,
                nullptr,
            });

        if (isOpen) {
            auto ptxn = store->createTransaction(nullptr);
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            auto txn = std::move(ptxn.value());
#ifdef BINLOG_V1
            std::unique_ptr<BinlogCursor> cursor =
                txn->createBinlogCursor(Transaction::MIN_VALID_TXNID);
            Expected<ReplLog> explog = cursor->next();
            if (explog.ok()) {
                const auto& rlk = explog.value().getReplLogKey();
                recBinlogStat->firstBinlogId = rlk.getTxnId();
#else
            auto explog = RepllogCursorV2::getMinBinlog(txn.get());
            if (explog.ok()) {
                recBinlogStat->firstBinlogId = explog.value().getBinlogId();
                recBinlogStat->timestamp = explog.value().getTimestamp();
                recBinlogStat->lastFlushBinlogId = Transaction::TXNID_UNINITED;
#endif
            } else {
                if (explog.status().code() == ErrorCodes::ERR_EXHAUST) {
                    // void compiler ud-link about static constexpr
                    // TODO(takenliu): fix the relative logic
                    recBinlogStat->firstBinlogId = Transaction::MIN_VALID_TXNID;
                    recBinlogStat->timestamp = 0;
                    recBinlogStat->lastFlushBinlogId = Transaction::TXNID_UNINITED;
                } else {
                    return explog.status();
                }
            }
        }
        _logRecycStatus.emplace_back(std::move(recBinlogStat));
        LOG(INFO) << "store:" << i
             << ",_firstBinlogId:"
             << _logRecycStatus.back()->firstBinlogId
             << ",_timestamp:"
             << _logRecycStatus.back()->timestamp;
    }

    INVARIANT(_logRecycStatus.size() == _svr->getKVStoreCount());

    _isRunning.store(true, std::memory_order_relaxed);
    _controller = std::make_unique<std::thread>(std::move([this]() {
        pthread_setname_np(pthread_self(), "repl_loop");
        controlRoutine();
    }));

    return {ErrorCodes::ERR_OK, ""};
}

void ReplManager::changeReplStateInLock(const StoreMeta& storeMeta,
                                        bool persist) {
    if (persist) {
        Catalog *catalog = _svr->getCatalog();
        Status s = catalog->setStoreMeta(storeMeta);
        if (!s.ok()) {
            LOG(FATAL) << "setStoreMeta failed:" << s.toString();
        }
    }
    _syncMeta[storeMeta.id] = std::move(storeMeta.copy());
}

Expected<uint32_t> ReplManager::maxDumpFileSeq(uint32_t storeId) {
    std::string subpath = _dumpPath + "/" + std::to_string(storeId) + "/";
#ifdef _WIN32
    subpath = replaceAll(subpath, "/", "\\");
#endif
    try {
        if (!filesystem::exists(_dumpPath)) {
            filesystem::create_directory(_dumpPath);
        }
        if (!filesystem::exists(subpath)) {
            filesystem::create_directory(subpath);
        }
    } catch (const std::exception& ex) {
        LOG(ERROR) << "create dir:" << _dumpPath
                    << " or " << subpath << " failed reason:" << ex.what();
        return {ErrorCodes::ERR_INTERNAL, ex.what()};
    }
    uint32_t maxFno = 0;
    try {
        for (auto& p : filesystem::recursive_directory_iterator(subpath)) {
            const filesystem::path& path = p.path();
            if (!filesystem::is_regular_file(p)) {
                LOG(INFO) << "maxDumpFileSeq ignore:" << p.path();
                continue;
            }
            // assert path with dir prefix
            INVARIANT(path.string().find(subpath) == 0);
            std::string relative = path.string().erase(0, subpath.size());
            if (relative.substr(0, 6) != "binlog") {
                LOG(INFO) << "maxDumpFileSeq ignore:" << relative;
            }
            size_t i = 0, start = 0, end = 0, first = 0;
            for (; i < relative.size(); ++i) {
                if (relative[i] == '-') {
                    first += 1;
                    if (first == 2) {
                        start = i+1;
                    }
                    if (first == 3) {
                        end = i;
                        break;
                    }
                }
            }
            Expected<uint64_t> fno = ::tendisplus::stoul(
                                relative.substr(start, end-start));
            if (!fno.ok()) {
                LOG(ERROR) << "parse fileno:" << relative << " failed:"
                           << fno.status().toString();
                return fno.value();
            }
            if (fno.value() >= std::numeric_limits<uint32_t>::max()) {
                LOG(ERROR) << "invalid fileno:" << fno.value();
                return {ErrorCodes::ERR_INTERNAL, "invalid fileno"};
            }
            maxFno = std::max(maxFno, (uint32_t)fno.value());
        }
    } catch (const std::exception& ex) {
        LOG(ERROR) << "store:" << storeId << " get fileno failed:" << ex.what();
        return {ErrorCodes::ERR_INTERNAL, "parse fileno failed"};
    }
    return maxFno;
}

void ReplManager::changeReplState(const StoreMeta& storeMeta,
                                        bool persist) {
    std::lock_guard<std::mutex> lk(_mutex);
    changeReplStateInLock(storeMeta, persist);
}

Status ReplManager::resetRecycleState(uint32_t storeId) {
    // set _logRecycStatus::firstBinlogId with MinBinlog get from rocksdb
    auto expdb = _svr->getSegmentMgr()->getDb(nullptr, storeId,
                                              mgl::LockMode::LOCK_NONE);
    if (!expdb.ok()) {
        return expdb.status();
    }
    auto store = std::move(expdb.value().store);
    INVARIANT(store != nullptr);
    auto exptxn = store->createTransaction(nullptr);
    if (!exptxn.ok()) {
        return exptxn.status();
    }

    Transaction *txn = exptxn.value().get();
    auto explog = RepllogCursorV2::getMinBinlog(txn);
    if (explog.ok()) {
        std::lock_guard<std::mutex> lk(_mutex);
        _logRecycStatus[storeId]->firstBinlogId = explog.value().getBinlogId();
        _logRecycStatus[storeId]->timestamp = explog.value().getTimestamp();
        _logRecycStatus[storeId]->lastFlushBinlogId = Transaction::TXNID_UNINITED;
    } else {
        if (explog.status().code() == ErrorCodes::ERR_EXHAUST) {
            // void compiler ud-link about static constexpr
            // TODO(takenliu): fix the relative logic
            std::lock_guard<std::mutex> lk(_mutex);
            _logRecycStatus[storeId]->firstBinlogId = Transaction::MIN_VALID_TXNID;
            _logRecycStatus[storeId]->timestamp = 0;
            _logRecycStatus[storeId]->lastFlushBinlogId = Transaction::TXNID_UNINITED;
        } else {
            LOG(ERROR) << "ReplManager::restart failed, storeid:" << storeId;
            return {ErrorCodes::ERR_INTERGER, "getMinBinlog failed."};

        }
    }
    return {ErrorCodes::ERR_OK, ""};
}

void ReplManager::controlRoutine() {
    using namespace std::chrono_literals;  // (NOLINT)
    auto schedSlaveInLock = [this](const SCLOCK::time_point& now) {
        // slave's POV
        bool doSth = false;
        for (size_t i = 0; i < _syncStatus.size(); i++) {
            if (_syncStatus[i]->isRunning
                    || now < _syncStatus[i]->nextSchedTime
                    || _syncMeta[i]->replState == ReplState::REPL_NONE) {
                continue;
            }
            doSth = true;
            // NOTE(deyukong): we dispatch fullsync/incrsync jobs into
            // different pools.
            if (_syncMeta[i]->replState == ReplState::REPL_CONNECT) {
                _syncStatus[i]->isRunning = true;
                _fullReceiver->schedule([this, i]() {
                    slaveSyncRoutine(i);
                });
            } else if (_syncMeta[i]->replState == ReplState::REPL_CONNECTED ||
                    _syncMeta[i]->replState == ReplState::REPL_ERR) {
                _syncStatus[i]->isRunning = true;
                _incrChecker->schedule([this, i]() {
                    slaveSyncRoutine(i);
                });
            } else if (_syncMeta[i]->replState == ReplState::REPL_TRANSFER) {
                LOG(FATAL) << "sync store:" << i
                    << " REPL_TRANSFER should not be visitable";
            } else {  // REPL_NONE
                // nothing to do with REPL_NONE
            }
        }
        return doSth;
    };
    auto schedMasterInLock = [this](const SCLOCK::time_point& now) {
        // master's POV
        recycleFullPushStatus();

        bool doSth = false;
        for (size_t i = 0; i < _pushStatus.size(); i++) {
            for (auto& mpov : _pushStatus[i]) {
                if (mpov.second->isRunning
                        || now < mpov.second->nextSchedTime) {
                    continue;
                }

                doSth = true;
                mpov.second->isRunning = true;
                uint64_t clientId = mpov.first;
                _incrPusher->schedule([this, i, clientId]() {
                    masterPushRoutine(i, clientId);
                });
            }
        }
        return doSth;
    };
    auto schedRecycLogInLock = [this](const SCLOCK::time_point& now) {
        bool doSth = false;
        for (size_t i = 0; i < _logRecycStatus.size(); ++i) {
            if (_logRecycStatus[i]->isRunning
                    || now < _logRecycStatus[i]->nextSchedTime) {
                continue;
            }
            doSth = true;
            _logRecycStatus[i]->isRunning = true;

            _logRecycler->schedule(
                [this, i]() {
                    recycleBinlog(i);
            });
        }
        return doSth;
    };
    while (_isRunning.load(std::memory_order_relaxed)) {
        bool doSth = false;
        auto now = SCLOCK::now();
        {
            std::lock_guard<std::mutex> lk(_mutex);
            doSth = schedSlaveInLock(now);
            doSth = schedMasterInLock(now) || doSth;
            // TODO(takenliu): make recycLog work
            doSth = schedRecycLogInLock(now) || doSth;
        }
        if (doSth) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    LOG(INFO) << "repl controller exits";
}

void ReplManager::recycleFullPushStatus() {
    auto now = SCLOCK::now();
    for (size_t i = 0; i < _fullPushStatus.size(); i++) {
        for (auto &mpov : _fullPushStatus[i]) {
            // if timeout, delte it.
            if (mpov.second->state == FullPushState::SUCESS
                && now > mpov.second->endTime + std::chrono::seconds(600)) {
                LOG(ERROR) << "timeout, _fullPushStatus erase," << mpov.second->toString();
                _fullPushStatus[i].erase(mpov.first);
            }
        }
    }
}
void ReplManager::onFlush(uint32_t storeId, uint64_t binlogid) {
    std::lock_guard<std::mutex> lk(_mutex);
    auto& v = _logRecycStatus[storeId];
    v->lastFlushBinlogId = binlogid;
    LOG(INFO) << "ReplManager::onFlush, storeId:" << storeId
        << " binlogid:" << binlogid;
}

bool ReplManager::hasSomeSlave(uint32_t storeId) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_pushStatus[storeId].size() != 0) {
        return true;
    }
    if (_fullPushStatus[storeId].size() != 0) {
        return true;
    }
    return false;
}

bool ReplManager::isSlaveOfSomeone(uint32_t storeId) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_syncMeta[storeId]->syncFromHost != "") {
        return true;
    }
    return false;
}

void ReplManager::recycleBinlog(uint32_t storeId) {
    SCLOCK::time_point nextSched = SCLOCK::now();
    float randRatio = redis_port::random() % 40 / 100.0 + 0.80;  // 0.80 to 1.20
    uint32_t nextSchedInterval = _cfg->truncateBinlogIntervalMs * randRatio;
    nextSched = nextSched + std::chrono::milliseconds(nextSchedInterval);

    uint64_t start = Transaction::MIN_VALID_TXNID;
    uint64_t end = Transaction::MIN_VALID_TXNID;
    bool saveLogs;

    bool hasError = false;
    auto guard = MakeGuard([this, &nextSched, &start, storeId, &hasError] {
        std::lock_guard<std::mutex> lk(_mutex);
        auto& v = _logRecycStatus[storeId];
        INVARIANT_D(v->isRunning);
        v->isRunning = false;
        // v->nextSchedTime maybe time_point::max()
        if (v->nextSchedTime < nextSched) {
            v->nextSchedTime = nextSched;
        }
        // NOTE(vinchen): like flushdb, the binlog is deleted, is should
        // reset the firstBinlogId
        if (hasError) {
            v->firstBinlogId = Transaction::TXNID_UNINITED;
        } else if (start != Transaction::MIN_VALID_TXNID) {
            v->firstBinlogId = start;
        }
        //DLOG(INFO) << "_logRecycStatus[" << storeId << "].firstBinlogId reset:" << start;

        // currently nothing waits for recycleBinlog's complete
        // _cv.notify_all();
    });
    LocalSessionGuard sg(_svr.get());

    auto segMgr = _svr->getSegmentMgr();
    INVARIANT(segMgr != nullptr);
    auto expdb = segMgr->getDb(sg.getSession(), storeId,
                        mgl::LockMode::LOCK_IX);
    if (!expdb.ok()) {
        LOG(ERROR) << "recycleBinlog getDb failed:"
                   << expdb.status().toString();
        hasError = true;
        return;
    }
    auto kvstore = std::move(expdb.value().store);
    if (!kvstore->isRunning()) {
        LOG(WARNING) << "dont need do recycleBinlog, kvstore is not running:" << storeId;
        nextSched = SCLOCK::now() + std::chrono::seconds(1);
        return;
    }
    if (kvstore->getBgError() != "") {
        LOG(WARNING) << "dont need do recycleBinlog, " << kvstore->getBgError();
        nextSched = SCLOCK::now() + std::chrono::seconds(1);
        return;
    }

    bool tailSlave = false;
    uint64_t highest = kvstore->getHighestBinlogId();
    end = highest;
    {
        std::unique_lock<std::mutex> lk(_mutex);

        start = _logRecycStatus[storeId]->firstBinlogId;

        saveLogs = _syncMeta[storeId]->syncFromHost != "";  // REPLICATE_ONLY
        if (_syncMeta[storeId]->syncFromHost == "" && _pushStatus[storeId].size() == 0) {  // single node
            saveLogs = true;
        }
        for (auto& mpov : _fullPushStatus[storeId]) {
            end = std::min(end, mpov.second->binlogPos);
        }
        for (auto& mpov : _pushStatus[storeId]) {
            end = std::min(end, mpov.second->binlogPos);
        }
        // NOTE(deyukong): we should keep at least 1 binlog
        uint64_t maxKeepLogs = _cfg->maxBinlogKeepNum;
        if (_syncMeta[storeId]->syncFromHost != "" && _pushStatus[storeId].size() == 0) {
            tailSlave = true;
        }
        if (tailSlave) {
            maxKeepLogs = _cfg->slaveBinlogKeepNum;
        }

        maxKeepLogs = std::max((uint64_t)1, maxKeepLogs);
        if (highest >= maxKeepLogs && end > highest - maxKeepLogs) {
            end = highest - maxKeepLogs;
        }
        if (highest < maxKeepLogs) {
            end = 0;
        }
    }

    if (_svr->isClusterEnabled() && _svr->getMigrateManager() != nullptr) {
        end = std::min(end, _svr->getMigrateManager()->getProtectBinlogid(storeId));
    }
    DLOG(INFO) << "recycleBinlog port:" << _svr->getParams()->port << " store: " << storeId << " " << start << " " << end;
    if (start > end) {
        return;
    }

    auto ptxn = kvstore->createTransaction(sg.getSession());
    if (!ptxn.ok()) {
        LOG(ERROR) << "recycleBinlog create txn failed:"
                   << ptxn.status().toString();
        hasError = true;
        return;
    }
    auto txn = std::move(ptxn.value());

#ifdef BINLOG_V1
    auto toDel = kvstore->getTruncateLog(start, end, txn.get());
    if (!toDel.ok()) {
        LOG(ERROR) << "get to be truncated binlog store:" << storeId
                    << "start:" << start
                    << ",end:" << end
                    << ",failed:" << toDel.status().toString();
        hasError = true;
        return;
    }
    if (start == toDel.value().first) {
        INVARIANT(toDel.value().second.size() == 0);
        nextSched = nextSched + std::chrono::seconds(1);
        return;
    }

    if (saveLogs) {
        auto s = saveBinlogs(storeId, toDel.value().second);
        if (!s.ok()) {
            LOG(ERROR) << "save binlog store:" << storeId
                        << "failed:" << s.toString();
            hasError = true;
            return;
        }
    }
    auto s = kvstore->truncateBinlog(toDel.value().second, txn.get());
    if (!s.ok()) {
        LOG(ERROR) << "truncate binlog store:" << storeId
                    << "failed:" << s.toString();
        hasError = true;
        return;
    }
    uint64_t newStart = toDel.value().first;
#else
    uint64_t newStart = 0;
    {
        std::ofstream* fs = nullptr;
        int64_t maxWriteLen = 0;
        if (saveLogs) {
            fs = getCurBinlogFs(storeId);
            if (!fs) {
                LOG(ERROR) << "getCurBinlogFs() store;" << storeId
                    << "failed:";
                hasError = true;
                return;
            }
            std::lock_guard<std::mutex> lk(_mutex);
            maxWriteLen = _cfg->binlogFileSizeMB*1024*1024 - _logRecycStatus[storeId]->fileSize;
        }

        auto s = kvstore->truncateBinlogV2(start, end, txn.get(), fs, maxWriteLen, tailSlave);
        if (!s.ok()) {
            LOG(ERROR) << "kvstore->truncateBinlogV2 store:" << storeId
                << "failed:" << s.status().toString();
            hasError = true;
            return;
        }
        bool changeNewFile = s.value().ret < 0;
        updateCurBinlogFs(storeId, s.value().written, s.value().timestamp, changeNewFile);
        // TODO(vinchen): stat for binlog deleted
        newStart = s.value().newStart;
    }
#endif
    auto commitStat = txn->commit();
    if (!commitStat.ok()) {
        LOG(ERROR) << "truncate binlog store:" << storeId
                    << "commit failed:" << commitStat.status().toString();
        hasError = true;
        return;
    }
    //DLOG(INFO) << "storeid:" << storeId << " truncate binlog from:" << start
    //    << " to end:" << newStart << " success."
    //    << "addr:" << _svr->getNetwork()->getIp()
    //    << ":" << _svr->getNetwork()->getPort();
    start = newStart;
}

void ReplManager::flushCurBinlogFs(uint32_t storeId) {
    // TODO(takenliu): let truncateBinlogV2 return quickly.
    updateCurBinlogFs(storeId, 0, 0, true);
}

Status ReplManager::changeReplSource(Session* sess, uint32_t storeId, std::string ip,
        uint32_t port, uint32_t sourceStoreId) {
    auto expdb = _svr->getSegmentMgr()->getDb(sess, storeId,
                                             mgl::LockMode::LOCK_X, true);
    if (!expdb.ok()) {
        return expdb.status();
    }
    if (!expdb.value().store->isOpen()) {
        return {ErrorCodes::ERR_OK, ""};
    }
    if (ip != "" && !expdb.value().store->isEmpty(true)) {
        return {ErrorCodes::ERR_MANUAL, "store not empty"};
    }
    Status s = changeReplSourceInLock(storeId, ip, port, sourceStoreId);
    if (!s.ok()) {
        return s;
    }
    return {ErrorCodes::ERR_OK, ""};
}

// changeReplSource should be called with LOCK_X held
Status ReplManager::changeReplSourceInLock(uint32_t storeId, std::string ip,
            uint32_t port, uint32_t sourceStoreId) {
    uint64_t oldTimeout = _connectMasterTimeoutMs;
    if (ip != "") {
        _connectMasterTimeoutMs = 1000;
    } else {
        _connectMasterTimeoutMs = 1;
    }

    LOG(INFO) << "wait for store:" << storeId << " to yield work";
    std::unique_lock<std::mutex> lk(_mutex);
    // NOTE(deyukong): we must wait for the target to stop before change meta,
    // or the meta may be rewrited
    if (!_cv.wait_for(lk, std::chrono::milliseconds(oldTimeout + 2000),
            [this, storeId] { return !_syncStatus[storeId]->isRunning; })) {
        return {ErrorCodes::ERR_TIMEOUT, "wait for yeild failed"};
    }
    LOG(INFO) << "wait for store:" << storeId << " to yield work succ";
    INVARIANT_D(!_syncStatus[storeId]->isRunning);

    if (storeId >= _syncMeta.size()) {
        return {ErrorCodes::ERR_INTERNAL, "invalid storeId"};
    }
    auto segMgr = _svr->getSegmentMgr();
    INVARIANT(segMgr != nullptr);
    auto expdb = segMgr->getDb(nullptr, storeId, mgl::LockMode::LOCK_NONE);
    if (!expdb.ok()) {
        return expdb.status();
    }
    auto kvstore = std::move(expdb.value().store);

    auto newMeta = _syncMeta[storeId]->copy();
    if (ip != "") {
        if (_syncMeta[storeId]->syncFromHost != "") {
            return {ErrorCodes::ERR_BUSY,
                    "explicit set sync source empty before change it"};
        }
        _connectMasterTimeoutMs = 1000;

        Status s = _svr->setStoreMode(kvstore,
                        KVStore::StoreMode::REPLICATE_ONLY);
        if (!s.ok()) {
            return s;
        }
        newMeta->syncFromHost = ip;
        newMeta->syncFromPort = port;
        newMeta->syncFromId = sourceStoreId;
        newMeta->replState = ReplState::REPL_CONNECT;
        newMeta->binlogId = Transaction::TXNID_UNINITED;
        LOG(INFO) << "change store:" << storeId
                  << " syncSrc from no one to " << newMeta->syncFromHost
                  << ":" << newMeta->syncFromPort
                  << ":" << newMeta->syncFromId;
        changeReplStateInLock(*newMeta, true);
        return {ErrorCodes::ERR_OK, ""};
    } else {  // ip == ""
        if (newMeta->syncFromHost == "") {
            return {ErrorCodes::ERR_OK, ""};
        }
        LOG(INFO) << "change store:" << storeId
                  << " syncSrc:" << newMeta->syncFromHost
                  << " to no one";
        _connectMasterTimeoutMs = 1;

        Status closeStatus =
            _svr->cancelSession(_syncStatus[storeId]->sessionId);
        if (!closeStatus.ok()) {
            // this error does not affect much, just log and continue
            LOG(WARNING) << "cancel store:" << storeId << " session failed:"
                        << closeStatus.toString();
        }
        _syncStatus[storeId]->sessionId = std::numeric_limits<uint64_t>::max();

        Status s = _svr->setStoreMode(kvstore, KVStore::StoreMode::READ_WRITE);
        if (!s.ok()) {
            return s;
        }

        newMeta->syncFromHost = ip;
        INVARIANT_D(port == 0 && sourceStoreId == 0);
        newMeta->syncFromPort = port;
        newMeta->syncFromId = sourceStoreId;
        newMeta->replState = ReplState::REPL_NONE;
        newMeta->binlogId = Transaction::TXNID_UNINITED;
        changeReplStateInLock(*newMeta, true);
        return {ErrorCodes::ERR_OK, ""};
    }
}

void ReplManager::getReplInfo(std::stringstream& ss) const {
    getReplInfoSimple(ss);
    getReplInfoDetail(ss);
}

struct ReplMPovStatus {
    uint32_t dstStoreId = 0;
    uint64_t binlogpos = 0;
    uint64_t clientId = 0;
    string state;
    SCLOCK::time_point lastSendBinlogTime;
    string slave_listen_ip;
    uint16_t slave_listen_port = 0;
};

uint64_t ReplManager::getLastSyncTime() const {
	std::lock_guard<std::mutex> lk(_mutex);
    uint64_t min = 0;
	for (size_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        if (_syncMeta[i]->syncFromHost != "") {
            // it is a slave
			uint64_t last_sync_time = nsSinceEpoch(_syncStatus[i]->lastSyncTime) / 1000000;  // ms
			if (last_sync_time < min || min == 0) {
				min = last_sync_time;
			}
        }
	}

	return min;
}

uint64_t ReplManager::replicationGetSlaveOffset() const {
   // INVARIANT_D(0);
    // TODO(vinchen)
    const uint32_t  storeId = 0;
    LocalSessionGuard sg(_svr.get());

    auto expdb = _svr->getSegmentMgr()->getDb(sg.getSession(), storeId,
                                    mgl::LockMode::LOCK_IS);
    if (!expdb.ok()) {
        LOG(ERROR) << "slave offset get db error";
        return 0;
    }
    auto kvstore = std::move(expdb.value().store);
    uint64_t max = kvstore->getHighestBinlogId();

    return max;
}

uint64_t ReplManager::replicationGetMasterOffset() const {
 //   INVARIANT_D(0);
 // TODO(vinchen)
    const uint32_t  storeId = 0;
    LocalSessionGuard sg(_svr.get());
    auto expdb = _svr->getSegmentMgr()->getDb(sg.getSession(), storeId,
                                              mgl::LockMode::LOCK_IS);
    if (!expdb.ok()) {
        LOG(ERROR) << "slave offset get db error";
        return 0;
    }
    auto kvstore = std::move(expdb.value().store);
    uint64_t max = kvstore->getHighestBinlogId();

    return max;
}

void ReplManager::getReplInfoSimple(std::stringstream& ss) const {
    // NOTE(takenliu), only consider slaveof all rockskvstores.
    string role = "master";
    int32_t master_repl_offset = 0;
    string master_host = "";
    uint16_t master_port = 0;
    string master_link_status = "up";
    int64_t master_last_io_seconds_ago = 0;
    int32_t master_sync_in_progress = 0;
    int32_t slave_repl_offset = 0;
    int32_t slave_priority = 100;
    int32_t slave_read_only = 1;
    SCLOCK::time_point minlastSyncTime = SCLOCK::now();
    SCLOCK::time_point mindownSyncTime = SCLOCK::now();
    for (size_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_syncMeta[i]->syncFromHost != "") {
            role = "slave";
            if (_syncStatus[i]->lastSyncTime < minlastSyncTime) {
                minlastSyncTime = _syncStatus[i]->lastSyncTime;
            }
            master_host = _syncMeta[i]->syncFromHost;
            master_port = _syncMeta[i]->syncFromPort;
            slave_repl_offset += _syncMeta[i]->binlogId;
            if (_syncMeta[i]->replState == ReplState::REPL_TRANSFER) {
                master_sync_in_progress = 1;
            }
            else if (_syncMeta[i]->replState == ReplState::REPL_ERR) {
                master_link_status = "down";
                if (_syncStatus[i]->lastSyncTime < mindownSyncTime) {
                    mindownSyncTime = _syncStatus[i]->lastSyncTime;
                }
            }
        }
    }
    master_last_io_seconds_ago = sinceEpoch() - sinceEpoch(minlastSyncTime);
    ss << "role:" << role << "\r\n";
    if (role == "slave") {
        ss << "master_host:" << master_host << "\r\n";
        ss << "master_port:" << master_port << "\r\n";
        ss << "master_link_status:" << master_link_status << "\r\n";
        ss << "master_last_io_seconds_ago:" << master_last_io_seconds_ago << "\r\n";
        ss << "master_sync_in_progress:" << master_sync_in_progress << "\r\n";
        ss << "slave_repl_offset:" << slave_repl_offset << "\r\n";
        if (master_sync_in_progress == 1) {
            // TODO(takenliu):
            ss << "master_sync_left_bytes:" << -1 << "\r\n";
            ss << "master_sync_last_io_seconds_ago:" << master_last_io_seconds_ago << "\r\n";
        }
        if (master_link_status == "down") {
            auto master_link_down_since_seconds = sinceEpoch() - sinceEpoch(mindownSyncTime);
            ss << "master_link_down_since_seconds:" << master_link_down_since_seconds << "\r\n";
        }
        ss << "slave_priority:" << slave_priority << "\r\n";
        ss << "slave_read_only:" << slave_read_only << "\r\n";
    }
    // master point of view
    std::map<std::string, ReplMPovStatus> pstatus;
    for (size_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        auto expdb = _svr->getSegmentMgr()->getDb(nullptr, i,
            mgl::LockMode::LOCK_IS, true, 0);
        if (!expdb.ok()) {
            continue;
        }
        uint64_t highestBinlogid = expdb.value().store->getHighestBinlogId();
        master_repl_offset += highestBinlogid;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            for (auto& iter : _pushStatus[i]) {
                string key = iter.second->slave_listen_ip +
                    "#" + std::to_string(iter.second->slave_listen_port);
                auto s = pstatus.find(key);
                if (s == pstatus.end()) {
                    pstatus[key] = ReplMPovStatus();
                    pstatus[key].slave_listen_ip = iter.second->slave_listen_ip;
                    pstatus[key].slave_listen_port = iter.second->slave_listen_port;
                    pstatus[key].state = "online";
                    pstatus[key].lastSendBinlogTime = iter.second->lastSendBinlogTime;
                }
                if (iter.second->lastSendBinlogTime < pstatus[key].lastSendBinlogTime) {
                    pstatus[key].lastSendBinlogTime = iter.second->lastSendBinlogTime;
                }
                pstatus[key].binlogpos += iter.second->binlogPos;
            }
        }
        {
            std::lock_guard<std::mutex> lk(_mutex);
            for (auto& iter : _fullPushStatus[i]) {
                string key = iter.second->slave_listen_ip +
                    "#" + std::to_string(iter.second->slave_listen_port);
                auto s = pstatus.find(key);
                if (s == pstatus.end()) {
                    pstatus[key] = ReplMPovStatus();
                    pstatus[key].slave_listen_ip = iter.second->slave_listen_ip;
                    pstatus[key].slave_listen_port = iter.second->slave_listen_port;
                    pstatus[key].state = "send_bulk";
                }
                pstatus[key].binlogpos += iter.second->binlogPos;
            }
        }
    }
    ss << "connected_slaves:" << pstatus.size() << "\r\n";
    if (pstatus.size() > 0) {
        int i = 0;
        for (auto& iter : pstatus) {
            ss << "slave" << i << ":ip=" << iter.second.slave_listen_ip
                << ",port=" << iter.second.slave_listen_port
                << ",state=" << iter.second.state
                << ",offset=" << iter.second.binlogpos
                << ",lag=" << (sinceEpoch() - sinceEpoch(iter.second.lastSendBinlogTime))
                << "\r\n";
        }
    }
    ss << "master_repl_offset:" << master_repl_offset << "\r\n";
}

void ReplManager::getReplInfoDetail(std::stringstream& ss) const {
    for (size_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_syncMeta[i]->syncFromHost != "") {
            std::string state = getEnumStr(_syncMeta[i]->replState);
            ss << "rocksdb" << i << "_master:";
            ss << "ip=" << _syncMeta[i]->syncFromHost;
            ss << ",port=" << _syncMeta[i]->syncFromPort;
            ss << ",src_store_id=" << _syncMeta[i]->syncFromId;
            ss << ",state=" << state;
            ss << ",binlog_pos=" << _syncMeta[i]->binlogId;
            ss << ",lag=" << (sinceEpoch() - sinceEpoch(_syncStatus[i]->lastSyncTime));
            if (_syncMeta[i]->replState == ReplState::REPL_ERR) {
                ss << ",error=" << _syncMeta[i]->replErr;
            }
            ss << "\r\n";
        }
    }
    for (size_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        auto expdb = _svr->getSegmentMgr()->getDb(nullptr, i,
            mgl::LockMode::LOCK_IS, true, 0);
        if (!expdb.ok()) {
            continue;
        }
        uint64_t highestBinlogid = expdb.value().store->getHighestBinlogId();
        std::lock_guard<std::mutex> lk(_mutex);
        size_t j = 0;
        for (auto iter = _pushStatus[i].begin(); iter != _pushStatus[i].end(); ++iter) {
            ss << "rocksdb" << i << "_slave" << j++ << ":";
            ss << "ip=" << iter->second->slave_listen_ip;
            ss << ",port=" << iter->second->slave_listen_port;
            ss << ",dest_store_id=" << iter->second->dstStoreId;
            ss << ",state=" << "online";        // TODO(vinchen)
            ss << ",binlog_pos=" << iter->second->binlogPos;
            ss << ",lag=" << (nsSinceEpoch() - nsSinceEpoch(iter->second->lastSendBinlogTime)) / 1000000000;
            ss << ",binlog_lag=" << highestBinlogid - iter->second->binlogPos;
            ss << "\r\n";
        }
        j = 0;
        for (auto iter = _fullPushStatus[i].begin(); iter != _fullPushStatus[i].end(); ++iter) {
            std::string state = getEnumStr(iter->second->state);
            ss << "rocksdb" << i << "_slave" << j++ << ":";
            ss << ",ip=" << iter->second->slave_listen_ip;
            ss << ",port=" << iter->second->slave_listen_port;
            ss << ",dest_store_id=" << iter->second->storeid;
            ss << ",state=" << state;
            ss << ",binlog_pos=" << iter->second->binlogPos;
            ss << ",duration=" << (nsSinceEpoch() - nsSinceEpoch(iter->second->startTime)) / 1000000000;
            ss << ",binlog_lag=" << highestBinlogid - iter->second->binlogPos;
            ss << "\r\n";
        }
    }
}

void ReplManager::appendJSONStat(
        rapidjson::PrettyWriter<rapidjson::StringBuffer>& w) const {
    std::lock_guard<std::mutex> lk(_mutex);
    INVARIANT(_pushStatus.size() == _svr->getKVStoreCount());
    INVARIANT(_syncStatus.size() == _svr->getKVStoreCount());
    for (size_t i = 0; i < _svr->getKVStoreCount(); ++i) {
        std::stringstream ss;
        ss << i;
        w.Key(ss.str().c_str());
        w.StartObject();

        w.Key("first_binlog");
        w.Uint64(_logRecycStatus[i]->firstBinlogId);

        w.Key("timestamp");
        w.Uint64(_logRecycStatus[i]->timestamp);

        w.Key("incr_paused");
        w.Uint64(_incrPaused);

        w.Key("sync_dest");
        w.StartObject();
        // sync to
        for (auto& mpov : _pushStatus[i]) {
            std::stringstream ss;
            ss << "client_" << mpov.second->clientId;
            w.Key(ss.str().c_str());
            w.StartObject();
            w.Key("is_running");
            w.Uint64(mpov.second->isRunning);
            w.Key("dest_store_id");
            w.Uint64(mpov.second->dstStoreId);
            w.Key("binlog_pos");
            w.Uint64(mpov.second->binlogPos);
            w.Key("remote_host");
            if (mpov.second->client != nullptr) {
                w.String(mpov.second->client->getRemoteRepr());
            } else {
                w.String("???");
            }
            w.EndObject();
        }
        w.EndObject();

        // sync from
        w.Key("sync_source");
        ss.str("");
        ss << _syncMeta[i]->syncFromHost << ":"
           << _syncMeta[i]->syncFromPort << ":"
           << _syncMeta[i]->syncFromId;
        w.String(ss.str().c_str());
        w.Key("binlog_id");
        w.Uint64(_syncMeta[i]->binlogId);
        w.Key("repl_state");
        w.Uint64(static_cast<uint64_t>(_syncMeta[i]->replState));
        w.Key("last_sync_time");
        w.String(timePointRepr(_syncStatus[i]->lastSyncTime));

        w.EndObject();
    }
}

void ReplManager::stop() {
    LOG(WARNING) << "repl manager begins stops...";
    _isRunning.store(false, std::memory_order_relaxed);
    _controller->join();

    // make sure all workpool has been stopped; otherwise calling
    // the destructor of a std::thread that is running will crash
    _fullPusher->stop();
    _incrPusher->stop();
    _fullReceiver->stop();
    _incrChecker->stop();
    _logRecycler->stop();

#if defined(_WIN32) && _MSC_VER > 1900
    for (size_t i = 0; i < _pushStatus.size(); i++) {
        for (auto& mpov : _pushStatus[i]) {
            delete mpov.second;
        }
        _pushStatus[i].clear();
    }

    for (size_t i = 0; i < _fullPushStatus.size(); i++) {
        for (auto& mpov : _fullPushStatus[i]) {
            delete mpov.second;
        }
        _fullPushStatus[i].clear();
    }
#endif

    LOG(WARNING) << "repl manager stops succ";
}

}  // namespace tendisplus
