// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include <map>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/version.hpp>

#include "checkpoints.h"
#include "kernel.h"
#include "main.h"
#include "txdb.h"
#include "util.h"

#include "SerializationTester.h"

std::unique_ptr<MDB_env, void (*)(MDB_env*)> dbEnv(nullptr, [](MDB_env*) {});

DbSmartPtrType glob_db_main(nullptr, [](MDB_dbi*) {});
DbSmartPtrType glob_db_blockIndex(nullptr, [](MDB_dbi*) {});
DbSmartPtrType glob_db_blocks(nullptr, [](MDB_dbi*) {});
DbSmartPtrType glob_db_tx(nullptr, [](MDB_dbi*) {});
DbSmartPtrType glob_db_ntp1Tx(nullptr, [](MDB_dbi*) {});
DbSmartPtrType glob_db_ntp1tokenNames(nullptr, [](MDB_dbi*) {});

using namespace std;
using namespace boost;

boost::filesystem::path CTxDB::DB_DIR                         = "txlmdb";
bool                    CTxDB::QuickSyncHigherControl_Enabled = true;

std::atomic<uint64_t> mdb_txn_safe::num_active_txns{0};
std::atomic_flag      mdb_txn_safe::creation_gate = ATOMIC_FLAG_INIT;

// threshold_size is used for batch transactions
bool CTxDB::need_resize(uint64_t threshold_size)
{
#ifdef DEEP_LMDB_LOGGING
    printf("CTxDB::%s\n", __func__);
#endif
#if defined(ENABLE_AUTO_RESIZE)
    MDB_envinfo mei;

    mdb_env_info(dbEnv.get(), &mei);

    MDB_stat mst;

    mdb_env_stat(dbEnv.get(), &mst);

    // size_used doesn't include data yet to be committed, which can be
    // significant size during batch transactions. For that, we estimate the size
    // needed at the beginning of the batch transaction and pass in the
    // additional size needed.
    uint64_t size_used = mst.ms_psize * mei.me_last_pgno;

#ifdef DEEP_LMDB_LOGGING
    printf("DB map size:     %zu\n", mei.me_mapsize);
    printf("Space used:      %zu\n", size_used);
    printf("Space remaining: %zu\n", mei.me_mapsize - size_used);
    printf("Size threshold:  %zu\n", threshold_size);
#endif
    float resize_percent = DB_RESIZE_PERCENT;
#ifdef DEEP_LMDB_LOGGING
    printf("Percent used: %.04f  Percent threshold: %.04f\n", ((double)size_used / mei.me_mapsize),
           resize_percent);
#endif

    if (threshold_size > 0) {
        if (mei.me_mapsize - size_used < threshold_size) {
            printf("Threshold met (size-based)\n");
            return true;
        } else
            return false;
    }

    if ((double)size_used / mei.me_mapsize > resize_percent) {
        printf("Mapsize threshold met (percent-based)\n");
        return true;
    }
    return false;
#else
    return false;
#endif
}

void lmdb_resized(MDB_env* env)
{
    // TODO: use RAII to restore allowing txns
    mdb_txn_safe::prevent_new_txns();

    printf("LMDB map resize detected.\n");

    MDB_envinfo mei;

    mdb_env_info(env, &mei);
    uint64_t old = mei.me_mapsize;

    mdb_txn_safe::wait_no_active_txns();

    int result = mdb_env_set_mapsize(env, 0);
    if (result)
        printf("Failed to set new mapsize: %d\n", result);

    mdb_env_info(env, &mei);
    uint64_t new_mapsize = mei.me_mapsize;

    std::stringstream ss;
    ss << "LMDB Mapsize increased."
       << "  Old: " << old / (1024 * 1024) << "MiB"
       << ", New: " << new_mapsize / (1024 * 1024) << "MiB";
    printf("%s\n", ss.str().c_str());

    mdb_txn_safe::allow_new_txns();
}

void CTxDB::do_resize(uint64_t increase_size)
{
    printf("CTxDB::%s\n", __func__);
    const uint64_t add_size = 1LL << 30;

    // check disk capacity
    try {
        boost::filesystem::path       path(GetDataDir() / DB_DIR);
        boost::filesystem::space_info si = boost::filesystem::space(path);
        if (si.available < add_size) {
            stringstream ss;
            ss << "!! WARNING: Insufficient free space to extend database !!: " << (si.available >> 20L)
               << " MB available, " << (add_size >> 20L) << " MB needed";
            throw std::runtime_error(ss.str());
        }
    } catch (...) {
        // print something but proceed.
        throw std::runtime_error("Unable to query free disk space.");
    }

    MDB_envinfo mei;

    mdb_env_info(dbEnv.get(), &mei);

    MDB_stat mst;

    mdb_env_stat(dbEnv.get(), &mst);

    // add 1Gb per resize, instead of doing a percentage increase
    uint64_t new_mapsize = (double)mei.me_mapsize + add_size;

    // If given, use increase_size instead of above way of resizing.
    // This is currently used for increasing by an estimated size at start of new
    // batch txn.
    if (increase_size > 0)
        new_mapsize = mei.me_mapsize + increase_size;

    new_mapsize += (new_mapsize % mst.ms_psize);

    mdb_txn_safe::prevent_new_txns();

    if (activeBatch) {
        throw std::runtime_error(
            "attempting resize with write transaction in progress, this should not happen!");
    }

    mdb_txn_safe::wait_no_active_txns();

    int result = mdb_env_set_mapsize(dbEnv.get(), new_mapsize);
    if (result)
        throw std::runtime_error("Failed to set new mapsize: " + std::to_string(result));

    std::stringstream ss;
    ss << "LMDB Mapsize increased."
       << "  Old: " << mei.me_mapsize / (1024 * 1024) << "MiB"
       << ", New: " << new_mapsize / (1024 * 1024) << "MiB";
    printf("%s", ss.str().c_str());

    mdb_txn_safe::allow_new_txns();
}

bool IsQuickSyncOSCompatible(const std::string& osValue)
{
    if (osValue == "any") {
        return true;
    } else {
        return false;
    }
}

void DownloadQuickSyncFile(const json_spirit::Value& fileVal, const filesystem::path& dbdir)
{
    // get json fields of this file
    std::string url    = NTP1Tools::GetStrField(fileVal.get_obj(), "url");
    std::string sum    = NTP1Tools::GetStrField(fileVal.get_obj(), "sha256sum");
    std::string sumBin = boost::algorithm::unhex(sum);
    //    uint64_t    fileSize = static_cast<uint64_t>(NTP1Tools::GetInt64Field(fileVal.get_obj(),
    //    "size"));
    // calculate binary values of the checksum

    // check available diskspace (disabled because it doesn't work properly on Windows
    //    std::size_t availableSpace = GetFreeDiskSpace(dbdir);
    //    std::size_t requiredSpace  = static_cast<std::size_t>(static_cast<double>(fileSize) * 1.2);
    //    if (requiredSpace > availableSpace) {
    //        throw std::runtime_error("Diskspace insufficient to download the blockchain; Available: " +
    //                                 std::to_string(availableSpace / ONE_MB) +
    //                                 " MB; required: " + std::to_string(requiredSpace / ONE_MB) + "
    //                                 MB");
    //    }

    std::string        leaf           = filesystem::path(url).filename().string();
    filesystem::path   downloadTarget = dbdir / leaf;
    std::atomic<float> progress;
    progress.store(0);
    // download the file asynchronously
    std::atomic_bool finishedDownload;
    finishedDownload.store(false);
    boost::thread downloadThread([&]() {
        cURLTools::GetLargeFileFromHTTPS(url, 30, downloadTarget, progress);
        finishedDownload.store(true);
    });
    while (!finishedDownload.load(std::memory_order_relaxed)) {
        std::stringstream ss;
        ss.setf(std::ios::fixed);
        ss << "Downloading QuickSync file " << leaf << ": " << std::setprecision(2)
           << progress.load(std::memory_order_relaxed) << "%...";
        uiInterface.InitMessage(ss.str());
        boost::this_thread::sleep_for(boost::chrono::milliseconds(250));
    }
    uiInterface.InitMessage("Done downloading");
    printf("Done downloading %s\n", leaf.c_str());
    std::string calculatedHash = CalculateHashOfFile<Sha256Calculator>(downloadTarget);
    if (calculatedHash != sumBin) {
        throw std::runtime_error("The calculated checksum for the downloaded file: " +
                                 downloadTarget.string() + "; does not match the expected one.");
    }
}

void DoQuickSync(const filesystem::path& dbdir)
{
    unsigned         failedAttempts      = 0;
    static const int MAX_FAILED_ATTEMPTS = 3;

    bool success = false;

    while (failedAttempts < MAX_FAILED_ATTEMPTS) {
        {
            std::string msg = "Attempting quicksync... (attempt " + std::to_string(failedAttempts + 1) +
                              " out of " + std::to_string(MAX_FAILED_ATTEMPTS) + ")";
            uiInterface.InitMessage(msg);
            printf("%s\n", msg.c_str());
        }
        try {
            filesystem::remove_all(dbdir);
            filesystem::create_directories(dbdir);

            std::string        jsonStrData = cURLTools::GetFileFromHTTPS(QuickSyncDataLink, 30, false);
            json_spirit::Value parsedJsonData;
            json_spirit::read_or_throw(jsonStrData, parsedJsonData);
            json_spirit::Array rootArray = parsedJsonData.get_array();
            for (const json_spirit::Value& val : rootArray) {
                std::string        os        = NTP1Tools::GetStrField(val.get_obj(), "os");
                uint64_t           dbversion = NTP1Tools::GetUint64Field(val.get_obj(), "dbversion");
                json_spirit::Array files     = NTP1Tools::GetArrayField(val.get_obj(), "files");

                if (dbversion < DATABASE_VERSION) {
                    printf("Skipping database with version %" PRIu64 "", dbversion);
                    continue;
                }

                if (!IsQuickSyncOSCompatible(os)) {
                    printf("Skipping database with OS %" PRIu64 "", dbversion);
                    continue;
                }
                for (const json_spirit::Value& fileVal : files) {
                    DownloadQuickSyncFile(fileVal, dbdir);
                }
                success = true;
                break; // after downloading one set of files, stop
            }
            break; // download is done, exit the "failedAttempts" counter
        } catch (std::exception& ex) {
            static const int WAIT_TIME_SECONDS = 5;
            std::string      msg               = "Quick sync failed... ";
            failedAttempts++;
            if (failedAttempts < MAX_FAILED_ATTEMPTS) {
                msg += "retrying in " + std::to_string(WAIT_TIME_SECONDS) + " seconds...";
            }
            uiInterface.InitMessage(msg);
            printf("Quick sync failed (attempt %i of %i). Error: %s\n", failedAttempts,
                   MAX_FAILED_ATTEMPTS, ex.what());
            boost::this_thread::sleep_for(boost::chrono::seconds(WAIT_TIME_SECONDS));
        }
    }
    uiInterface.InitMessage("QuickSync done");
    if (!success) {
        throw std::runtime_error("QuickSync error: None of the files matched the correct settings.");
    }
    printf("QuickSync done\n");
}

bool ShouldQuickSyncBeDone(const filesystem::path& dbdir)
{
    if (CTxDB::QuickSyncHigherControl_Enabled == false) {
        return false;
    }

    if (GetBoolArg("-noquicksync") == true) {
        return false;
    }

    return (!filesystem::exists(dbdir) || !filesystem::exists(dbdir / "data.mdb") ||
            !filesystem::exists(dbdir / "lock.mdb")) &&
           !fTestNet;
}

void CTxDB::init_blockindex(bool fRemoveOld)
{
    // First time init.
    filesystem::path directory = GetDataDir() / DB_DIR;

    if (fRemoveOld ||
        SC_CheckOperationOnRestartScheduleThenDeleteIt(SC_SCHEDULE_ON_RESTART_OPNAME__RESYNC)) {
        filesystem::remove_all(directory); // remove directory

        // close the database before deleting
        this->Close();

        // delete block data files
        {
            unsigned int nFile = 1;

            while (true) {
                filesystem::path strBlockFile = GetDataDir() / strprintf("blk%04u.dat", nFile);

                // Break if no such file
                if (!filesystem::exists(strBlockFile))
                    break;

                filesystem::remove(strBlockFile);

                nFile++;
            }
        }

        // delete NTP1 transaction data files
        {
            unsigned int nFile = 1;

            while (true) {
                filesystem::path strBlockFile = GetDataDir() / strprintf("ntp1txs%04u.dat", nFile);

                // Break if no such file
                if (!filesystem::exists(strBlockFile))
                    break;

                filesystem::remove(strBlockFile);

                nFile++;
            }
        }
    }

    // if the directory doesn't exist, use quicksync
    if (ShouldQuickSyncBeDone(directory)) {
        // close the database before running quicksync
        this->Close();

        try {
            DoQuickSync(directory);
        } catch (std::exception& ex) {
            printf("Quicksync exited with an exception (this is not expected to happen: %s\n",
                   ex.what());
            filesystem::remove_all(directory);
        }
    }

    printf("Opening the blockchain database...\n");
    uiInterface.InitMessage("Opening the blockchain database...");

    // open the database in the traditional way (whether quicksync succeeded or not)
    filesystem::create_directories(directory);
    printf("Opening lmdb in %s\n", directory.string().c_str());
    MDB_env* envPtr = nullptr;
    if (const int rc = mdb_env_create(&envPtr)) {
        throw std::runtime_error("Error creating lmdb environment: " + std::to_string(rc) +
                                 "; message: " + std::string(mdb_strerror(rc)));
    }
    dbEnv = std::unique_ptr<MDB_env, void (*)(MDB_env*)>(envPtr, [](MDB_env* p) {
        if (p)
            mdb_env_close(p);
    });

    mdb_env_set_maxdbs(dbEnv.get(), 20);

    if (auto result = mdb_env_open(dbEnv.get(), directory.string().c_str(), /*MDB_NOTLS*/ 0, 0644)) {
        throw std::runtime_error("Failed to open lmdb environment: " + std::to_string(result) +
                                 "; message: " + std::string(mdb_strerror(result)));
    }

    MDB_envinfo mei;
    mdb_env_info(dbEnv.get(), &mei);
    std::size_t currMapSize = mei.me_mapsize;

    std::size_t mapSize = DB_DEFAULT_MAPSIZE;

    if (currMapSize < mapSize) {
        if (auto mapSizeErr = mdb_env_set_mapsize(dbEnv.get(), mapSize))
            throw std::runtime_error(
                "Error: set max memory map size failed: " + std::to_string(mapSizeErr) +
                "; message: " + std::string(mdb_strerror(mapSizeErr)));

        mdb_env_info(dbEnv.get(), &mei);
        currMapSize = (double)mei.me_mapsize;
        printf("LMDB memory map size: %zu\n", currMapSize);
    }

    if (CTxDB::need_resize()) {
        printf("LMDB memory map needs to be resized, doing that now.\n");
        CTxDB::do_resize();
    }

    mdb_txn_safe txn;
    if (auto mdb_res = mdb_txn_begin(dbEnv.get(), NULL, 0, txn)) {
        throw std::runtime_error(
            "Failed to create a transaction for the db: " + std::to_string(mdb_res) +
            "; message: " + std::string(mdb_strerror(mdb_res)));
    }

    glob_db_main           = DbSmartPtrType(new MDB_dbi, dbDeleter);
    glob_db_blockIndex     = DbSmartPtrType(new MDB_dbi, dbDeleter);
    glob_db_blocks         = DbSmartPtrType(new MDB_dbi, dbDeleter);
    glob_db_tx             = DbSmartPtrType(new MDB_dbi, dbDeleter);
    glob_db_ntp1Tx         = DbSmartPtrType(new MDB_dbi, dbDeleter);
    glob_db_ntp1tokenNames = DbSmartPtrType(new MDB_dbi, dbDeleter);

    // MDB_CREATE: Create the named database if it doesn't exist.
    CTxDB::lmdb_db_open(txn, LMDB_MAINDB.c_str(), MDB_CREATE, *glob_db_main,
                        "Failed to open db handle for db_main");
    CTxDB::lmdb_db_open(txn, LMDB_BLOCKINDEXDB.c_str(), MDB_CREATE, *glob_db_blockIndex,
                        "Failed to open db handle for db_blockIndex");
    CTxDB::lmdb_db_open(txn, LMDB_BLOCKSDB.c_str(), MDB_CREATE, *glob_db_blocks,
                        "Failed to open db handle for db_blocks");
    CTxDB::lmdb_db_open(txn, LMDB_TXDB.c_str(), MDB_CREATE, *glob_db_tx,
                        "Failed to open db handle for glob_db_tx");
    CTxDB::lmdb_db_open(txn, LMDB_NTP1TXDB.c_str(), MDB_CREATE, *glob_db_ntp1Tx,
                        "Failed to open db handle for glob_db_ntp1Tx");
    CTxDB::lmdb_db_open(txn, LMDB_NTP1TOKENNAMESDB.c_str(), MDB_CREATE | MDB_DUPSORT,
                        *glob_db_ntp1tokenNames, "Failed to open db handle for glob_db_ntp1Tx");

    // commit the transaction
    txn.commit();

    if (!glob_db_main) {
        throw std::runtime_error("LMDB nullptr after opening the db_main database.");
    }
    if (!glob_db_blockIndex) {
        throw std::runtime_error("LMDB nullptr after opening the db_blockIndex database.");
    }
    if (!glob_db_blocks) {
        throw std::runtime_error("LMDB nullptr after opening the db_blocks database.");
    }
    if (!glob_db_tx) {
        throw std::runtime_error("LMDB nullptr after opening the db_tx database.");
    }
    if (!glob_db_ntp1Tx) {
        throw std::runtime_error("LMDB nullptr after opening the db_ntp1Tx database.");
    }
    if (!glob_db_ntp1tokenNames) {
        throw std::runtime_error("LMDB nullptr after opening the db_ntp1tokenNames database.");
    }

    printf("Done opening the database\n");
    uiInterface.InitMessage("Done opening the database");
}

// CDB subclasses are created and destroyed VERY OFTEN. That's why
// we shouldn't treat this as a free operations.
CTxDB::CTxDB(const char* pszMode)
{
    assert(pszMode);
    fReadOnly = (!strchr(pszMode, '+') && !strchr(pszMode, 'w'));

    if (glob_db_main) {
        loadDbPointers();
        return;
    }

    RunCrossPlatformSerializationTests();
    printf("Binary format tests have passed.\n");

    printf("Initializing lmdb with db size: %" PRIu64 "\n", DB_DEFAULT_MAPSIZE);
    bool fCreate = strchr(pszMode, 'c');

    init_blockindex(); // Init directory
    loadDbPointers();

    if (Exists(string("version"), db_main)) {
        ReadVersion(nVersion);
        printf("Transaction index version is %d\n", nVersion);

        if (nVersion < DATABASE_VERSION) {
            printf("Required index version is %d, removing old database\n", DATABASE_VERSION);

            // lmdb instance destruction
            resetDbPointers();
            resetGlobalDbPointers();
            if (activeBatch) {
                activeBatch->abort();
                activeBatch.reset();
            }

            init_blockindex(true); // Remove directory and create new database
            loadDbPointers();

            bool fTmp = fReadOnly;
            fReadOnly = false;
            WriteVersion(DATABASE_VERSION); // Save transaction index version
            fReadOnly = fTmp;
        }
    } else if (fCreate) {
        bool fTmp = fReadOnly;
        fReadOnly = false;
        WriteVersion(DATABASE_VERSION);
        fReadOnly = fTmp;
    }

    printf("Opened LMDB successfully\n");
}

void CTxDB::Close()
{
    if (activeBatch) {
        activeBatch->abort();
        activeBatch.reset();
    }
    resetDbPointers();
    resetGlobalDbPointers();
}

void CTxDB::__deleteDb()
{
    try {
        boost::filesystem::remove_all(GetDataDir() / DB_DIR);
    } catch (...) {
    }
}

bool CTxDB::TxnBegin(size_t required_size)
{
    assert(activeBatch == nullptr);
    if (CTxDB::need_resize(required_size)) {
        printf("LMDB memory map needs to be resized, doing that now.\n");
        CTxDB::do_resize(required_size);
    }
    activeBatch = std::unique_ptr<mdb_txn_safe>(new mdb_txn_safe);
    if (auto res = lmdb_txn_begin(dbEnv.get(), nullptr, 0, *activeBatch)) {
        printf("Failed to begin transaction at read with error code %i; with error: %s\n", res,
               mdb_strerror(res));
        activeBatch.reset();
    }
    return true;
}

bool CTxDB::TxnCommit()
{
    assert(activeBatch);
    if (activeBatch) {
        activeBatch->commit();
        activeBatch.reset();
    }
    return true;
}

bool CTxDB::TxnAbort()
{
    assert(activeBatch);
    if (activeBatch) {
        activeBatch->abort();
        activeBatch.reset();
    }
    return true;
}

bool CTxDB::test1_WriteStrKeyVal(const string& key, const string& val)
{
    return Write(key, val, db_main);
}

bool CTxDB::test1_ReadStrKeyVal(const string& key, string& val) { return Read(key, val, db_main); }
bool CTxDB::test1_ExistsStrKeyVal(const string& key) { return Exists(key, db_main); }
bool CTxDB::test1_EraseStrKeyVal(const string& key) { return Erase(key, db_main); }

bool CTxDB::test2_ReadMultipleStr1KeyVal(const string& key, std::vector<string>& val)
{
    return ReadMultiple(key, val, db_ntp1tokenNames);
}

bool CTxDB::test2_WriteStrKeyVal(const string& key, const string& val)
{
    return Write(key, val, db_ntp1tokenNames);
}

bool CTxDB::test2_ExistsStrKeyVal(const string& key) { return Exists(key, db_ntp1tokenNames); }
bool CTxDB::test2_EraseStrKeyVal(const string& key) { return EraseAll(key, db_ntp1tokenNames); }

bool CTxDB::ReadVersion(int& nVersion)
{
    nVersion = 0;
    return Read(std::string("version"), nVersion, db_main);
}

bool CTxDB::WriteVersion(int nVersion) { return Write(std::string("version"), nVersion, db_main); }

bool CTxDB::ReadTxIndex(uint256 hash, CTxIndex& txindex)
{
    txindex.SetNull();
    return Read(hash, txindex, db_tx);
}

bool CTxDB::UpdateTxIndex(uint256 hash, const CTxIndex& txindex) { return Write(hash, txindex, db_tx); }

bool CTxDB::ReadTx(const CDiskTxPos& txPos, CTransaction& tx)
{
    tx.SetNull();
    return Read(txPos.nBlockPos, tx, db_blocks, 0, txPos.nTxPos);
}

bool CTxDB::ReadNTP1Tx(uint256 hash, NTP1Transaction& ntp1tx)
{
    ntp1tx.setNull();
    return Read(hash, ntp1tx, db_ntp1Tx);
}

bool CTxDB::ReadNTP1TxsWithTokenSymbol(const std::string& tokenName, std::vector<uint256>& txs)
{
    return ReadMultiple(tokenName, txs, db_ntp1tokenNames);
}

bool CTxDB::WriteNTP1TxWithTokenSymbol(const std::string& tokenSymbol, const NTP1Transaction& ntp1tx)
{
    if (ntp1tx.isNull()) {
        printf("Attempted to store token symbol information of token with given symbol %s",
               tokenSymbol.c_str());
        return false;
    }
    std::string symbol;
    try {
        symbol = ntp1tx.getTokenSymbolIfIssuance();
    } catch (std::exception& ex) {
        printf("Failed to get token symbol for transaction: %s; with claimed token symbol %s. Error: %s",
               ntp1tx.getTxHash().ToString().c_str(), tokenSymbol.c_str(), ex.what());
        return false;
    } catch (...) {
        printf("Failed to get token symbol for transaction: %s; with claimed token symbol %s. Unknown "
               "error.",
               ntp1tx.getTxHash().ToString().c_str(), tokenSymbol.c_str());
        return false;
    }
    if (symbol != tokenSymbol) {
        printf("While writing NTP1 tx for token names, the token name provided is not equal to the "
               "token name calculated: %s != %s",
               symbol.c_str(), tokenSymbol.c_str());
        return false;
    }
    return Write(tokenSymbol, ntp1tx.getTxHash(), db_ntp1tokenNames);
}

bool CTxDB::WriteNTP1Tx(uint256 hash, const NTP1Transaction& ntp1tx)
{
    return Write(hash, ntp1tx, db_ntp1Tx);
}

bool CTxDB::ReadBlock(uint256 hash, CBlock& blk, bool fReadTransactions)
{
    blk.SetNull();
    int modifiers = (fReadTransactions ? 0 : SER_BLOCKHEADERONLY);
    return Read(hash, blk, db_blocks, modifiers);
}

bool CTxDB::WriteBlock(uint256 hash, const CBlock& blk)
{
    assert(blk.GetHash() != 0);
    return Write(hash, blk, db_blocks);
}

bool CTxDB::EraseTxIndex(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    return Erase(hash, db_tx);
}

bool CTxDB::ContainsTx(uint256 hash) { return Exists(hash, db_tx); }

bool CTxDB::ContainsNTP1Tx(uint256 hash) { return Exists(hash, db_ntp1Tx); }

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex)
{
    tx.SetNull();
    if (!ReadTxIndex(hash, txindex))
        return false;
    return (tx.ReadFromDisk(txindex.pos, *this));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex)
{
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(blockindex.GetBlockHash(), blockindex, db_blockIndex);
}

bool CTxDB::ReadHashBestChain(uint256& hashBestChain)
{
    return Read(string("hashBestChain"), hashBestChain, db_main);
}

bool CTxDB::WriteHashBestChain(uint256 hashBestChain)
{
    return Write(string("hashBestChain"), hashBestChain, db_main);
}

bool CTxDB::ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust)
{
    return Read(string("bnBestInvalidTrust"), bnBestInvalidTrust, db_main);
}

bool CTxDB::WriteBestInvalidTrust(CBigNum bnBestInvalidTrust)
{
    return Write(string("bnBestInvalidTrust"), bnBestInvalidTrust, db_main);
}

bool CTxDB::ReadSyncCheckpoint(uint256& hashCheckpoint)
{
    return Read(string("hashSyncCheckpoint"), hashCheckpoint, db_main);
}

bool CTxDB::WriteSyncCheckpoint(uint256 hashCheckpoint)
{
    return Write(string("hashSyncCheckpoint"), hashCheckpoint, db_main);
}

bool CTxDB::ReadCheckpointPubKey(string& strPubKey)
{
    return Read(string("strCheckpointPubKey"), strPubKey, db_main);
}

bool CTxDB::WriteCheckpointPubKey(const string& strPubKey)
{
    return Write(string("strCheckpointPubKey"), strPubKey, db_main);
}

static CBlockIndex* InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return nullptr;

    // Return existing
    unordered_map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi                    = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

std::string LmdbValToString(const MDB_val& val)
{
    return std::string((const char*)val.mv_data, val.mv_size);
}

bool CTxDB::LoadBlockIndex()
{
    if (mapBlockIndex.size() > 0) {
        // Already loaded once in this session. It can happen during migration
        // from BDB.
        return true;
    }

    // The block index is an in-memory structure that maps hashes to on-disk
    // locations where the contents of the block can be found. Here, we scan it
    // out of the DB and into mapBlockIndex.

    MDB_cursor*  cursorRawPtr = nullptr;
    mdb_txn_safe localTxn;
    if (auto res = lmdb_txn_begin(dbEnv.get(), nullptr, MDB_RDONLY, localTxn)) {
        return error("Failed to begin transaction at read with error code %i; and error: %s\n", res,
                     mdb_strerror(res));
    }
    if (auto rc = mdb_cursor_open(localTxn, *db_blockIndex, &cursorRawPtr)) {
        return error(
            "CTxDB::LoadBlockIndex() : Failed to open lmdb cursor with error code %d; and error: %s\n",
            rc, mdb_strerror(rc));
    }
    std::unique_ptr<MDB_cursor, void (*)(MDB_cursor*)> cursorPtr(cursorRawPtr, [](MDB_cursor* p) {
        if (p)
            mdb_cursor_close(p);
    });

    // Seek to start key.
    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << uint256(0);
    std::string&& keyBin = ssStartKey.str();
    MDB_val       key    = {(size_t)ssStartKey.size(), (void*)keyBin.data()};
    MDB_val       data;

    int itemRes = mdb_cursor_get(cursorPtr.get(), &key, &data, MDB_FIRST);
    if (itemRes != 0 && itemRes != MDB_NOTFOUND) {
        return error("Error while opening cursor to load index. Error code %i, and error: %s\n", itemRes,
                     mdb_strerror(itemRes));
    }

    uint64_t loadedCount = 0;

    // Now read each entry.
    do {
        // if the first item is empty, break immediately
        if (itemRes) {
            break;
        }

        std::string keyStr = LmdbValToString(key);
        std::string valStr = LmdbValToString(data);

        // Unpack keys and values.
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(keyStr.data(), keyStr.size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(valStr.data(), valStr.size());

        if (fRequestShutdown)
            break;
        CDiskBlockIndex diskindex;
        ssValue >> diskindex;

        uint256 blockHash = diskindex.GetBlockHash();

        // Construct block index object
        CBlockIndex* pindexNew    = InsertBlockIndex(blockHash);
        pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
        pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
        pindexNew->blockKeyInDB   = diskindex.blockKeyInDB;
        pindexNew->nHeight        = diskindex.nHeight;
        pindexNew->nMint          = diskindex.nMint;
        pindexNew->nMoneySupply   = diskindex.nMoneySupply;
        pindexNew->nFlags         = diskindex.nFlags;
        pindexNew->nStakeModifier = diskindex.nStakeModifier;
        pindexNew->prevoutStake   = diskindex.prevoutStake;
        pindexNew->nStakeTime     = diskindex.nStakeTime;
        pindexNew->hashProof      = diskindex.hashProof;
        pindexNew->nVersion       = diskindex.nVersion;
        pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
        pindexNew->nTime          = diskindex.nTime;
        pindexNew->nBits          = diskindex.nBits;
        pindexNew->nNonce         = diskindex.nNonce;

        // Watch for genesis block
        if (pindexGenesisBlock == nullptr &&
            blockHash == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))
            pindexGenesisBlock = pindexNew;

        if (!pindexNew->CheckIndex()) {
            cursorPtr.reset();
            return error("LoadBlockIndex() : CheckIndex failed at %d", pindexNew->nHeight);
        }

        // NovaCoin: build setStakeSeen
        if (pindexNew->IsProofOfStake())
            setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

        itemRes = mdb_cursor_get(cursorRawPtr, &key, &data, MDB_NEXT);

        loadedCount++;
        if (loadedCount % 1000 == 0) {
            uiInterface.InitMessage(_("Loading block index...") +
                                    " (block: " + std::to_string(loadedCount) + ")");
        }
        //        std::cout << "Read status: " << itemRes << "\t" << mdb_strerror(itemRes) << std::endl;
    } while (itemRes == 0);
    printf("Done reading block index\n");
    uiInterface.InitMessage(_("Loading block index...") + " (done reading block index)");

    cursorPtr.reset();
    localTxn.commit();

    if (fRequestShutdown)
        return true;

    // Calculate nChainTrust
    vector<pair<int, CBlockIndex*>> vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    BOOST_FOREACH (const PAIRTYPE(uint256, CBlockIndex*) & item, mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH (const PAIRTYPE(int, CBlockIndex*) & item, vSortedByHeight) {
        CBlockIndex* pindex = item.second;
        pindex->nChainTrust = (pindex->pprev ? pindex->pprev->nChainTrust : 0) + pindex->GetBlockTrust();
        // NovaCoin: calculate stake modifier checksum
        pindex->nStakeModifierChecksum = GetStakeModifierChecksum(pindex);
        if (!CheckStakeModifierCheckpoints(pindex->nHeight, pindex->nStakeModifierChecksum))
            return error("CTxDB::LoadBlockIndex() : Failed stake modifier checkpoint height=%d, "
                         "modifier=0x%016" PRIx64,
                         pindex->nHeight, pindex->nStakeModifier);
    }

    // Load hashBestChain pointer to end of best chain
    if (!ReadHashBestChain(hashBestChain)) {
        if (pindexGenesisBlock == nullptr)
            return true;
        return error("CTxDB::LoadBlockIndex() : hashBestChain not loaded");
    }
    if (!mapBlockIndex.count(hashBestChain))
        return error("CTxDB::LoadBlockIndex() : hashBestChain not found in the block index");
    pindexBest      = mapBlockIndex[hashBestChain];
    nBestHeight     = pindexBest->nHeight;
    nBestChainTrust = pindexBest->nChainTrust;

    printf("LoadBlockIndex(): hashBestChain=%s  height=%d  trust=%s  date=%s\n",
           hashBestChain.ToString().substr(0, 20).c_str(), nBestHeight.load(),
           CBigNum(nBestChainTrust).ToString().c_str(),
           DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    // NovaCoin: load hashSyncCheckpoint
    if (!ReadSyncCheckpoint(Checkpoints::hashSyncCheckpoint))
        return error("CTxDB::LoadBlockIndex() : hashSyncCheckpoint not loaded");
    printf("LoadBlockIndex(): synchronized checkpoint %s\n",
           Checkpoints::hashSyncCheckpoint.ToString().c_str());

    // Load bnBestInvalidTrust, OK if it doesn't exist
    CBigNum bnBestInvalidTrust;
    ReadBestInvalidTrust(bnBestInvalidTrust);
    nBestInvalidTrust = bnBestInvalidTrust.getuint256();

    CTxDB txdb;
    // Verify blocks in the best chain
    int nCheckLevel = GetArg("-checklevel", 1);
    int nCheckDepth = GetArg("-checkblocks", 2500);
    if (nCheckDepth == 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > nBestHeight)
        nCheckDepth = nBestHeight;
    printf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CBlockIndex*               pindexFork = nullptr;
    map<uint256, CBlockIndex*> mapBlockPos;
    loadedCount = 0;
    for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev) {

        if (loadedCount % 10 == 0) {
            uiInterface.InitMessage("Verifying latest blocks (" + std::to_string(loadedCount) + "/" +
                                    std::to_string(nCheckDepth) + ")");
        }
        loadedCount++;

        if (fRequestShutdown || pindex->nHeight < nBestHeight - nCheckDepth)
            break;
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        // check level 1: verify block validity
        // check level 7: verify block signature too
        if (nCheckLevel > 0 && !block.CheckBlock(true, true, (nCheckLevel > 6))) {
            printf("LoadBlockIndex() : *** found bad block at %d, hash=%s\n", pindex->nHeight,
                   pindex->GetBlockHash().ToString().c_str());
            pindexFork = pindex->pprev;
        }
        // check level 2: verify transaction index validity
        if (nCheckLevel > 1) {
            uint256 pos      = pindex->blockKeyInDB;
            mapBlockPos[pos] = pindex;
            BOOST_FOREACH (const CTransaction& tx, block.vtx) {
                uint256  hashTx = tx.GetHash();
                CTxIndex txindex;
                if (ReadTxIndex(hashTx, txindex)) {
                    // check level 3: checker transaction hashes
                    if (nCheckLevel > 2 || pindex->blockKeyInDB != txindex.pos.nBlockPos) {
                        // either an error or a duplicate transaction
                        CTransaction txFound;
                        if (!txFound.ReadFromDisk(txindex.pos, txdb)) {
                            printf("LoadBlockIndex() : *** cannot read mislocated transaction %s\n",
                                   hashTx.ToString().c_str());
                            pindexFork = pindex->pprev;
                        } else if (txFound.GetHash() != hashTx) // not a duplicate tx
                        {
                            printf("LoadBlockIndex(): *** invalid tx position for %s\n",
                                   hashTx.ToString().c_str());
                            pindexFork = pindex->pprev;
                        }
                    }
                    // check level 4: check whether spent txouts were spent within the main chain
                    unsigned int nOutput = 0;
                    if (nCheckLevel > 3) {
                        BOOST_FOREACH (const CDiskTxPos& txpos, txindex.vSpent) {
                            if (!txpos.IsNull()) {
                                uint256 posFind = txpos.nBlockPos;
                                if (!mapBlockPos.count(posFind)) {
                                    printf("LoadBlockIndex(): *** found bad spend at %d, hashBlock=%s, "
                                           "hashTx=%s\n",
                                           pindex->nHeight, pindex->GetBlockHash().ToString().c_str(),
                                           hashTx.ToString().c_str());
                                    pindexFork = pindex->pprev;
                                }
                                // check level 6: check whether spent txouts were spent by a valid
                                // transaction that consume them
                                if (nCheckLevel > 5) {
                                    CTransaction txSpend;
                                    if (!txSpend.ReadFromDisk(txpos, txdb)) {
                                        printf("LoadBlockIndex(): *** cannot read spending transaction "
                                               "of %s:%i from disk\n",
                                               hashTx.ToString().c_str(), nOutput);
                                        pindexFork = pindex->pprev;
                                    } else if (!txSpend.CheckTransaction()) {
                                        printf("LoadBlockIndex(): *** spending transaction of %s:%i is "
                                               "invalid\n",
                                               hashTx.ToString().c_str(), nOutput);
                                        pindexFork = pindex->pprev;
                                    } else {
                                        bool fFound = false;
                                        BOOST_FOREACH (const CTxIn& txin, txSpend.vin)
                                            if (txin.prevout.hash == hashTx && txin.prevout.n == nOutput)
                                                fFound = true;
                                        if (!fFound) {
                                            printf("LoadBlockIndex(): *** spending transaction of %s:%i "
                                                   "does not spend it\n",
                                                   hashTx.ToString().c_str(), nOutput);
                                            pindexFork = pindex->pprev;
                                        }
                                    }
                                }
                            }
                            nOutput++;
                        }
                    }
                }
                // check level 5: check whether all prevouts are marked spent
                if (nCheckLevel > 4) {
                    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
                        CTxIndex txindex;
                        if (ReadTxIndex(txin.prevout.hash, txindex))
                            if (txindex.vSpent.size() - 1 < txin.prevout.n ||
                                txindex.vSpent[txin.prevout.n].IsNull()) {
                                printf("LoadBlockIndex(): *** found unspent prevout %s:%i in %s\n",
                                       txin.prevout.hash.ToString().c_str(), txin.prevout.n,
                                       hashTx.ToString().c_str());
                                pindexFork = pindex->pprev;
                            }
                    }
                }
            }
        }
    }

    printf("Verifying latest blocks done.\n");
    uiInterface.InitMessage("Verifying latest blocks done");

    if (pindexFork && !fRequestShutdown) {
        // Reorg back to the fork
        printf("LoadBlockIndex() : *** moving best chain pointer back to block %d\n",
               pindexFork->nHeight);
        CBlock block;
        if (!block.ReadFromDisk(pindexFork))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        CTxDB txdb;
        block.SetBestChain(txdb, pindexFork);
    }

    return true;
}

mdb_txn_safe::mdb_txn_safe(const bool check) : m_txn(nullptr), m_check(check)
{
    if (check) {
        while (creation_gate.test_and_set()) {
            boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
        }
        num_active_txns++;
        creation_gate.clear();
    }
}

mdb_txn_safe::~mdb_txn_safe()
{
    if (!m_check)
        return;
    //    printf("mdb_txn_safe: destructor\n");
    if (m_txn != nullptr) {
        if (m_batch_txn) // this is a batch txn and should have been handled before this point for safety
        {
            printf("WARNING: mdb_txn_safe: m_txn is a batch txn and it's not NULL in destructor - "
                   "calling mdb_txn_abort()\n");
        } else {
            // Example of when this occurs: a lookup fails, so a read-only txn is
            // aborted through this destructor. However, successful read-only txns
            // ideally should have been committed when done and not end up here.
            //
            // NOTE: not sure if this is ever reached for a non-batch write
            // transaction, but it's probably not ideal if it did.
            printf("mdb_txn_safe: m_txn not NULL in destructor - calling mdb_txn_abort()\n");
        }
    }
    mdb_txn_abort(m_txn);

    num_active_txns--;
}

mdb_txn_safe& mdb_txn_safe::operator=(mdb_txn_safe&& other)
{
    m_txn             = other.m_txn;
    m_batch_txn       = other.m_batch_txn;
    m_check           = other.m_check;
    other.m_check     = false;
    other.m_txn       = nullptr;
    other.m_batch_txn = false;
    return *this;
}

mdb_txn_safe::mdb_txn_safe(mdb_txn_safe&& other)
    : m_txn(other.m_txn), m_batch_txn(other.m_batch_txn), m_check(other.m_check)
{
    other.m_check     = false;
    other.m_txn       = nullptr;
    other.m_batch_txn = false;
}

void mdb_txn_safe::uncheck()
{
    num_active_txns--;
    m_check = false;
}

void mdb_txn_safe::commit(std::string message)
{
    if (message.size() == 0) {
        message = "Failed to commit a transaction to the db";
    }

    if (auto result = mdb_txn_commit(m_txn)) {
        m_txn = nullptr;
        throw std::runtime_error(message + ": " + std::to_string(result));
    }
    m_txn = nullptr;
}

void mdb_txn_safe::commitIfValid(string message)
{
    if (m_txn) {
        commit(message);
    }
}

void mdb_txn_safe::abort()
{
    if (m_txn != nullptr) {
        mdb_txn_abort(m_txn);
        m_txn = nullptr;
    } else {
        printf("WARNING: mdb_txn_safe: abort() called, but m_txn is NULL\n");
    }
}

void mdb_txn_safe::abortIfValid()
{
    if (m_txn) {
        abort();
    }
}

uint64_t mdb_txn_safe::num_active_tx() const { return num_active_txns; }

void mdb_txn_safe::prevent_new_txns()
{
    while (creation_gate.test_and_set()) {
        boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
    }
}

void mdb_txn_safe::wait_no_active_txns()
{
    while (num_active_txns > 0) {
        boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
    }
}

void mdb_txn_safe::allow_new_txns() { creation_gate.clear(); }

CTxDB::~CTxDB()
{
    // Note that this is not the same as Close() because it deletes only
    // data scoped to this TxDB object.
    resetDbPointers();
}
