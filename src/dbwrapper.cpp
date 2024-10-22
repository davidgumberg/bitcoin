// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>

#include <logging.h>
#include <random.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <leveldb/filter_policy.h>
#include <leveldb/helpers/memenv/memenv.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <leveldb/write_batch.h>
#include <libmdbx/mdbx.h++>
#include <memory>
#include <optional>
#include <utility>

static auto CharCast(const std::byte* data) { return reinterpret_cast<const char*>(data); }

bool CDBWrapper::DestroyDB(const std::string& path_str)
{
    return leveldb::DestroyDB(path_str, {}).ok();
}

bool MDBXWrapper::DestroyDB(const std::string& path_str)
{
    return mdbx::env::remove(path_str);
}

struct CDBWrapper::StatusImpl
{
    const leveldb::Status status;
};

/** Handle database error by throwing dbwrapper_error exception.
 */
void CDBWrapper::HandleError(const CDBWrapper::StatusImpl& _status)
{
    const leveldb::Status& status = _status.status;
    if (status.ok())
        return;
    const std::string errmsg = "Fatal LevelDB error: " + status.ToString();
    LogWarning("%s\n", errmsg);
    LogWarning("You can use -debug=leveldb to get more complete diagnostic messages\n");
    throw dbwrapper_error(errmsg);
}

class CBitcoinLevelDBLogger : public leveldb::Logger {
public:
    // This code is adapted from posix_logger.h, which is why it is using vsprintf.
    // Please do not do this in normal code
    void Logv(const char * format, va_list ap) override {
            if (!LogAcceptCategory(BCLog::LEVELDB, BCLog::Level::Debug)) {
                return;
            }
            char buffer[500];
            for (int iter = 0; iter < 2; iter++) {
                char* base;
                int bufsize;
                if (iter == 0) {
                    bufsize = sizeof(buffer);
                    base = buffer;
                }
                else {
                    bufsize = 30000;
                    base = new char[bufsize];
                }
                char* p = base;
                char* limit = base + bufsize;

                // Print the message
                if (p < limit) {
                    va_list backup_ap;
                    va_copy(backup_ap, ap);
                    // Do not use vsnprintf elsewhere in bitcoin source code, see above.
                    p += vsnprintf(p, limit - p, format, backup_ap);
                    va_end(backup_ap);
                }

                // Truncate to available space if necessary
                if (p >= limit) {
                    if (iter == 0) {
                        continue;       // Try again with larger buffer
                    }
                    else {
                        p = limit - 1;
                    }
                }

                // Add newline if necessary
                if (p == base || p[-1] != '\n') {
                    *p++ = '\n';
                }

                assert(p <= limit);
                base[std::min(bufsize - 1, (int)(p - base))] = '\0';
                LogDebug(BCLog::LEVELDB, "%s\n", util::RemoveSuffixView(base, "\n"));
                if (base != buffer) {
                    delete[] base;
                }
                break;
            }
    }
};

static void SetMaxOpenFiles(leveldb::Options *options) {
    // On most platforms the default setting of max_open_files (which is 1000)
    // is optimal. On Windows using a large file count is OK because the handles
    // do not interfere with select() loops. On 64-bit Unix hosts this value is
    // also OK, because up to that amount LevelDB will use an mmap
    // implementation that does not use extra file descriptors (the fds are
    // closed after being mmap'ed).
    //
    // Increasing the value beyond the default is dangerous because LevelDB will
    // fall back to a non-mmap implementation when the file count is too large.
    // On 32-bit Unix host we should decrease the value because the handles use
    // up real fds, and we want to avoid fd exhaustion issues.
    //
    // See PR #12495 for further discussion.

    int default_open_files = options->max_open_files;
#ifndef WIN32
    if (sizeof(void*) < 8) {
        options->max_open_files = 64;
    }
#endif
    LogDebug(BCLog::LEVELDB, "LevelDB using max_open_files=%d (default=%d)\n",
             options->max_open_files, default_open_files);
}

static leveldb::Options GetOptions(size_t nCacheSize)
{
    leveldb::Options options;
    options.block_cache = leveldb::NewLRUCache(nCacheSize / 2);
    options.write_buffer_size = nCacheSize / 4; // up to two write buffers may be held in memory simultaneously
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.compression = leveldb::kNoCompression;
    options.info_log = new CBitcoinLevelDBLogger();
    if (leveldb::kMajorVersion > 1 || (leveldb::kMajorVersion == 1 && leveldb::kMinorVersion >= 16)) {
        // LevelDB versions before 1.16 consider short writes to be corruption. Only trigger error
        // on corruption in later versions.
        options.paranoid_checks = true;
    }
    SetMaxOpenFiles(&options);
    return options;
}

struct CDBBatch::WriteBatchImpl {
    leveldb::WriteBatch batch;
};

CDBBatch::CDBBatch(CDBWrapperBase& _parent)
    : CDBBatchBase(_parent),
      m_impl_batch{std::make_unique<CDBBatch::WriteBatchImpl>()} {};

CDBBatch::~CDBBatch() = default;

void CDBBatch::WriteImpl(Span<const std::byte> key, DataStream& value, bool possible_dup)
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());
    value.Xor(m_parent.GetObfuscateKey());
    leveldb::Slice slValue(CharCast(value.data()), value.size());
    m_impl_batch->batch.Put(slKey, slValue);
    // LevelDB serializes writes as:
    // - byte: header
    // - varint: key length (1 byte up to 127B, 2 bytes up to 16383B, ...)
    // - byte[]: key
    // - varint: value length
    // - byte[]: value
    // The formula below assumes the key and value are both less than 16k.
    size_estimate += 3 + (slKey.size() > 127) + slKey.size() + (slValue.size() > 127) + slValue.size();
}

void CDBBatch::EraseImpl(Span<const std::byte> key, bool sorted)
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());
    m_impl_batch->batch.Delete(slKey);
    // LevelDB serializes erases as:
    // - byte: header
    // - varint: key length
    // - byte[]: key
    // The formula below assumes the key is less than 16kB.
    size_estimate += 2 + (slKey.size() > 127) + slKey.size();
}

struct LevelDBContext {
    //! custom environment this database is using (may be nullptr in case of default environment)
    leveldb::Env* penv;

    //! database options used
    leveldb::Options options;

    //! options used when reading from the database
    leveldb::ReadOptions readoptions;

    //! options used when iterating over values of the database
    leveldb::ReadOptions iteroptions;

    //! options used when writing to the database
    leveldb::WriteOptions writeoptions;

    //! options used when sync writing to the database
    leveldb::WriteOptions syncoptions;

    //! the database itself
    leveldb::DB* pdb;
};

CDBWrapper::CDBWrapper(const DBParams& params)
    : CDBWrapperBase(params),
    m_db_context{std::make_unique<LevelDBContext>()}
{
    DBContext().penv = nullptr;
    DBContext().readoptions.verify_checksums = true;
    DBContext().iteroptions.verify_checksums = true;
    DBContext().iteroptions.fill_cache = false;
    DBContext().syncoptions.sync = true;
    DBContext().options = GetOptions(params.cache_bytes);
    DBContext().options.create_if_missing = true;
    if (params.memory_only) {
        DBContext().penv = leveldb::NewMemEnv(leveldb::Env::Default());
        DBContext().options.env = DBContext().penv;
    } else {
        if (params.wipe_data) {
            LogPrintf("Wiping LevelDB in %s\n", fs::PathToString(params.path));
            StatusImpl result{leveldb::DestroyDB(fs::PathToString(params.path), DBContext().options)};
            HandleError(result);
        }
        TryCreateDirectories(params.path);
        LogPrintf("Opening LevelDB in %s\n", fs::PathToString(params.path));
    }
    // PathToString() return value is safe to pass to leveldb open function,
    // because on POSIX leveldb passes the byte string directly to ::open(), and
    // on Windows it converts from UTF-8 to UTF-16 before calling ::CreateFileW
    // (see env_posix.cc and env_windows.cc).
    StatusImpl status{leveldb::DB::Open(DBContext().options, fs::PathToString(params.path), &DBContext().pdb)};
    HandleError(status);
    LogPrintf("Opened LevelDB successfully\n");

    if (params.options.force_compact) {
        LogPrintf("Starting database compaction of %s\n", fs::PathToString(params.path));
        DBContext().pdb->CompactRange(nullptr, nullptr);
        LogPrintf("Finished database compaction of %s\n", fs::PathToString(params.path));
    }
    if(params.obfuscate && WriteObfuscateKeyIfNotExists()){
        LogInfo("Wrote new obfuscate key for %s: %s\n", fs::PathToString(params.path), HexStr(obfuscate_key));
    }
    LogInfo("Using obfuscation key for %s: %s\n", fs::PathToString(params.path), HexStr(GetObfuscateKey()));
}

CDBWrapper::~CDBWrapper()
{
    delete DBContext().pdb;
    DBContext().pdb = nullptr;
    delete DBContext().options.filter_policy;
    DBContext().options.filter_policy = nullptr;
    delete DBContext().options.info_log;
    DBContext().options.info_log = nullptr;
    delete DBContext().options.block_cache;
    DBContext().options.block_cache = nullptr;
    delete DBContext().penv;
    DBContext().options.env = nullptr;
}

bool CDBWrapper::WriteBatch(CDBBatchBase& _batch, bool fSync)
{
    CDBBatch& batch = static_cast<CDBBatch&>(_batch);
    const bool log_memory = LogAcceptCategory(BCLog::LEVELDB, BCLog::Level::Debug);
    double mem_before = 0;
    if (log_memory) {
        mem_before = DynamicMemoryUsage() / 1024.0 / 1024;
    }
    StatusImpl status{DBContext().pdb->Write(fSync ? DBContext().syncoptions : DBContext().writeoptions, &batch.m_impl_batch->batch)};
    HandleError(status);
    if (log_memory) {
        double mem_after = DynamicMemoryUsage() / 1024.0 / 1024;
        LogDebug(BCLog::LEVELDB, "WriteBatch memory usage: db=%s, before=%.1fMiB, after=%.1fMiB\n",
                 m_name, mem_before, mem_after);
    }
    return true;
}

size_t CDBWrapper::DynamicMemoryUsage() const
{
    std::string memory;
    std::optional<size_t> parsed;
    if (!DBContext().pdb->GetProperty("leveldb.approximate-memory-usage", &memory) || !(parsed = ToIntegral<size_t>(memory))) {
        LogDebug(BCLog::LEVELDB, "Failed to get approximate-memory-usage property\n");
        return 0;
    }
    return parsed.value();
}

// Prefixed with null character to avoid collisions with other keys
//
// We must use a string constructor which specifies length so that we copy
// past the null-terminator.
const std::string CDBWrapperBase::OBFUSCATE_KEY_KEY("\000obfuscate_key", 14);

const unsigned int CDBWrapperBase::OBFUSCATE_KEY_NUM_BYTES = 8;

/**
 * Returns a string (consisting of 8 random bytes) suitable for use as an
 * obfuscating XOR key.
 */
std::vector<unsigned char> CDBWrapperBase::CreateObfuscateKey() const
{
    std::vector<uint8_t> ret(OBFUSCATE_KEY_NUM_BYTES);
    GetRandBytes(ret);
    return ret;
}

bool CDBWrapperBase::WriteObfuscateKeyIfNotExists()
{
    // The base-case obfuscation key, which is a noop.
    obfuscate_key = std::vector<unsigned char>(OBFUSCATE_KEY_NUM_BYTES, '\000');

    bool key_exists = Read(OBFUSCATE_KEY_KEY, obfuscate_key);

    if (!key_exists && IsEmpty()) {
        // Initialize non-degenerate obfuscation if it won't upset
        // existing, non-obfuscated data.
        std::vector<unsigned char> new_key = CreateObfuscateKey();

        // Write `new_key` so we don't obfuscate the key with itself
        Write(OBFUSCATE_KEY_KEY, new_key,/*fSync=*/false,/*sorted=*/false);
        obfuscate_key = new_key;
        return true;
    }
    else {
        return false;
    }
}


std::optional<std::string> CDBWrapper::ReadImpl(Span<const std::byte> key, bool partitioned) const
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());
    std::string strValue;
    leveldb::Status status = DBContext().pdb->Get(DBContext().readoptions, slKey, &strValue);
    if (!status.ok()) {
        if (status.IsNotFound())
            return std::nullopt;
        LogPrintf("LevelDB read failure: %s\n", status.ToString());
        HandleError(StatusImpl{status});
    }
    return strValue;
}

bool CDBWrapper::ExistsImpl(Span<const std::byte> key, bool partitioned) const
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());

    std::string strValue;
    leveldb::Status status = DBContext().pdb->Get(DBContext().readoptions, slKey, &strValue);
    if (!status.ok()) {
        if (status.IsNotFound())
            return false;
        LogPrintf("LevelDB read failure: %s\n", status.ToString());
        HandleError(StatusImpl{status});
    }
    return true;
}

size_t CDBWrapper::EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const
{
    leveldb::Slice slKey1(CharCast(key1.data()), key1.size());
    leveldb::Slice slKey2(CharCast(key2.data()), key2.size());
    uint64_t size = 0;
    leveldb::Range range(slKey1, slKey2);
    DBContext().pdb->GetApproximateSizes(&range, 1, &size);
    return size;
}

// TODO: IsEmpty shouldn't be virtual in CDBWrapperBase
bool CDBWrapper::IsEmpty()
{
    std::unique_ptr<CDBIterator> it(static_cast<CDBIterator*>(CDBWrapper::NewIterator()));
    it->SeekToFirst();
    return !(it->Valid());
}

struct MDBXContext {
    mdbx::env::operate_parameters operate_params;
    mdbx::env_managed::create_parameters create_params;

    mdbx::env::geometry geometry;

    // MDBX environment handle
    mdbx::env_managed env;

    // MDBX map handle
    mdbx::map_handle map;

    ~MDBXContext() {
        env.close();
    }
};


MDBXWrapper::MDBXWrapper(const DBParams& params)
    : CDBWrapperBase(params),
    m_db_context{std::make_unique<MDBXContext>()}
{
    if (params.wipe_data) {
        LogInfo("Wiping MDBX in %s\n", fs::PathToString(params.path));
        DestroyDB(fs::PathToString(params.path));
    }

    TryCreateDirectories(params.path);

    LogPrintf("Opening MDBX in %s\n", fs::PathToString(params.path));

    DBContext().create_params.geometry.pagesize = 4096;
    // We need this because of some unpleasant (for us) passing around of the
    // Chainstate between threads during initialization.
    DBContext().operate_params.options.no_sticky_threads = true;

    DBContext().operate_params.durability = mdbx::env::whole_fragile;

    // initialize the mdbx environment.
    DBContext().env = mdbx::env_managed(params.path, DBContext().create_params, DBContext().operate_params);

    auto txn = DBContext().env.start_read();
    DBContext().map = txn.open_map(nullptr, mdbx::key_mode::usual, mdbx::value_mode::single);
    txn.commit();

    if (params.obfuscate && WriteObfuscateKeyIfNotExists()){
        LogInfo("Wrote new obfuscate key for %s: %s\n", fs::PathToString(params.path), HexStr(obfuscate_key));
    }
    LogInfo("Using obfuscation key for %s: %s\n", fs::PathToString(params.path), HexStr(GetObfuscateKey()));

    WritePartitionPrefixIfNotExists();
}


MDBXWrapper::~MDBXWrapper() = default;

void MDBXWrapper::Sync()
{
    DBContext().env.sync_to_disk();
}

void MDBXWrapper::WritePartitionPrefix(uint16_t idx)
{
    Write(DBWrapperMetaEntry{DB_PARTITION_KEY}, idx, /*fSync=*/false, /*sorted=*/false);
}

void MDBXWrapper::PartitionInc()
{
    partition_index++;
    LogInfo("Incrementing partition key to: %d", partition_index);
    WritePartitionPrefix(partition_index);
}

bool MDBXWrapper::WritePartitionPrefixIfNotExists()
{
    uint16_t partition_prefix_value(sizeof(partition_index));
    auto partition_key = DBWrapperMetaEntry{DB_PARTITION_KEY};

    bool key_exists = Read(partition_key, partition_prefix_value, /*partitioned=*/false);

    if (!key_exists) {
        partition_prefix_value = uint16_t{0x0001};

        // Write `new_key` so we don't obfuscate the key with itself
        LogInfo("Writing new partition key %02x", partition_prefix_value);
        WritePartitionPrefix(partition_prefix_value);
        partition_index = partition_prefix_value;
        return true;
    }
    else {
        LogInfo("Using existing partition key %02x", partition_prefix_value);
        partition_index = partition_prefix_value;
        return false;
    }
}

static std::string bytesToHexString(Span<const std::byte> bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (const auto& byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }

    return ss.str();
}

static constexpr mdbx::slice TX_ERASE_VAL("\x00");

std::optional<std::string> MDBXWrapper::ReadImpl(Span<const std::byte> key, bool partitioned) const
{
    auto read_txn = DBContext().env.start_read();
    mdbx::slice slValue = mdbx::slice::invalid();

    if(partitioned) {
        LogDebug(BCLog::COINDB, "Reading for key 0x%02x", bytesToHexString(key));
        uint16_t cur_part = partition_index - 1;
        // Ugly.. 0x0000 is reserved for metadata
        while (cur_part > DB_METADATA && slValue == mdbx::slice::invalid()) {
            DataStream partitioned_key;
            ::Serialize(partitioned_key, DBPartitionedEntry{cur_part, key}); // FIXME: Extra copying to avoid CDBatchBase refactor
            mdbx::slice slKey(CharCast(partitioned_key.data()), partitioned_key.size());
            LogDebug(BCLog::COINDB, "Looking with pp 0x%02x", slKey.as_hex_string());

            // Get the next tx
            slValue = read_txn.get(DBContext().map, slKey, mdbx::slice::invalid());

            if (slValue == TX_ERASE_VAL){
                break;
            }
            // Search an earlier partition
            cur_part--;
        }
    }
    else {
        mdbx::slice slKey(CharCast(key.data()), key.size());
        slValue = read_txn.get(DBContext().map, slKey, mdbx::slice::invalid());
    }

    std::optional<std::string> ret;

    if(slValue == mdbx::slice::invalid() || slValue == TX_ERASE_VAL) {
        ret = std::nullopt;
        LogDebug(BCLog::COINDB, "Found nothing!");
    }
    else {
        ret = std::string(slValue.as_string());
        LogDebug(BCLog::COINDB, "Found something!");
    }

    read_txn.commit();
    return ret;
}

bool MDBXWrapper::ExistsImpl(Span<const std::byte> key, bool partitioned) const {
    return (ReadImpl(key, partitioned).has_value() ? true : false);
}

size_t MDBXWrapper::EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const
{
    // Only relevant for `gettxoutsetinfo` rpc.
    // Hint: (leaves + inner pages + overflow pages) * page size.
    return size_t{0};
}

bool MDBXWrapper::WriteBatch(CDBBatchBase& _batch, bool fSync)
{
    auto& batch = static_cast<MDBXBatch&>(_batch);

    LogDebug(BCLog::COINDB, "There are %d many readers before this batchwrite.\n", DBContext().env.get_info().mi_numreaders);

    batch.CommitAndReset();

    if(fSync) {
        Sync();
    }

    return true;
}

size_t MDBXWrapper::DynamicMemoryUsage() const
{
    // Only relevant for some logging that happens in WriteBatch
    // TODO: how can I estimate this? I believe mmap makes this a challenge
    return size_t{0};
}

struct MDBXBatch::MDBXWriteBatchImpl {
    mdbx::map_handle &m_map;
    mdbx::txn_managed m_txn;

    explicit MDBXWriteBatchImpl(mdbx::map_handle &map, mdbx::txn_managed &&txn)
        : m_map{map}, m_txn{std::move(txn)} {}
};

MDBXBatch::MDBXBatch (CDBWrapperBase& _parent) : CDBBatchBase(_parent)
{
    MDBXWrapper& parent = static_cast<MDBXWrapper&>(m_parent);
    m_impl_batch = std::make_unique<MDBXWriteBatchImpl>(parent.DBContext().map,
        parent.DBContext().env.start_write());

};

MDBXBatch::~MDBXBatch()
{
    if(m_impl_batch->m_txn){
        m_impl_batch->m_txn.abort();
    }
}

void MDBXBatch::CommitAndReset()
{
    auto &parent = static_cast<MDBXWrapper&>(m_parent);

/*
    auto tx_stat = m_impl_batch->m_txn.get_info(true);
    LogDebug(BCLog::COINDB, "Pre-Commit tx stats:\n");
    LogDebug(BCLog::COINDB, "  dirtied pages: %d, retired pages: %d, read lag: %d\n",
        tx_stat.txn_space_dirty / 4096, tx_stat.txn_space_retired / 4096, tx_stat.txn_reader_lag);

    auto map_stat = m_impl_batch->m_txn.get_map_stat(parent.DBContext().map);
    LogDebug(BCLog::COINDB, "Map stats:\n");
    LogDebug(BCLog::COINDB, "  depth: %d, leaves: %d, overpages: %d\n",
        map_stat.ms_depth, map_stat.ms_leaf_pages, map_stat.ms_overflow_pages);

    auto env_stat = parent.DBContext().env.get_info(m_impl_batch->m_txn);
    LogDebug(BCLog::COINDB, "Env stats:\n");
    LogDebug(BCLog::COINDB, "  mapsize: %d, unsynced_bytes: %d\n",
        env_stat.mi_mapsize, env_stat.mi_unsync_volume);

    auto pageop_stat = env_stat.mi_pgop_stat;
    LogDebug(BCLog::COINDB, "Pageop stats:");
    LogDebug(BCLog::COINDB, "  CoW'ed pgs: %d, Merged pgs: %d, New pgs: %d, Spill pgs: %d, Split pgs: %d\n",
        pageop_stat.cow, pageop_stat.merge, pageop_stat.newly, pageop_stat.spill, pageop_stat.split);
*/

    m_impl_batch->m_txn.commit();
    if(fPartitionUsed) parent.PartitionInc();

    m_impl_batch->m_txn = parent.DBContext().env.start_write();
}

void MDBXBatch::WriteImpl(Span<const std::byte> key, DataStream& value, bool sorted)
{
    try {
        value.Xor(m_parent.GetObfuscateKey());
        mdbx::slice slValue(CharCast(value.data()), value.size());
        if(sorted) {
            LogDebug(BCLog::COINDB, "Sorted write of key: 0x%s", bytesToHexString(key));
            DataStream partitioned_key;
            ::Serialize(partitioned_key, DBPartitionedEntry{CurrentPartition(), key}); // FIXME: Extra copying to avoid CDBatchBase refactor
            mdbx::slice slKey(CharCast(partitioned_key.data()), partitioned_key.size());
            LogDebug(BCLog::COINDB, "Appending sorted key: %s\n", slKey.as_hex_string());
            m_impl_batch->m_txn.append(m_impl_batch->m_map, slKey, slValue);
            fPartitionUsed = true;
        }
        else {
            mdbx::slice slKey(CharCast(key.data()), key.size());
            LogDebug(BCLog::COINDB, "Appending unsorted key: %s\n", slKey.as_hex_string());
            m_impl_batch->m_txn.put(m_impl_batch->m_map, slKey, slValue, mdbx::upsert);
        }
    }
    catch (mdbx::error err) {
        const std::string errmsg = "Fatal MDBX error while writing: " + err.message();
        std::cout << errmsg << std::endl;
        std::cout << "K: " << key.data() << "V: " << value.data();
        throw dbwrapper_error(errmsg);
    }
}


void MDBXBatch::EraseImpl(Span<const std::byte> key, bool sorted)
{
    if (sorted) {
        DataStream partitioned_key;
        ::Serialize(partitioned_key, DBPartitionedEntry{CurrentPartition(), key}); // FIXME: Extra copying to avoid CDBatchBase refactor
        mdbx::slice slKey(CharCast(partitioned_key.data()), partitioned_key.size());
        LogDebug(BCLog::COINDB, "Appending sorted erase of key: %s\n", slKey.as_hex_string());
        m_impl_batch->m_txn.append(m_impl_batch->m_map, slKey, TX_ERASE_VAL);
        fPartitionUsed = true;
    }
    else {
        mdbx::slice slKey(CharCast(key.data()), key.size());
        LogDebug(BCLog::COINDB, "Performing unsorted erase of key: %s\n", slKey.as_hex_string());
        m_impl_batch->m_txn.erase(m_impl_batch->m_map, slKey);
    }
}

size_t MDBXBatch::SizeEstimate() const
{
    return m_impl_batch->m_txn.size_current();
}

struct MDBXIterator::IteratorImpl {
    MDBXContext &db_context;
    mdbx::txn_managed txn;
    const std::unique_ptr<mdbx::cursor_managed> cursor;

    IteratorImpl(MDBXContext& db_context, mdbx::txn_managed&& tx, mdbx::cursor_managed&& cur)
        : db_context(db_context),
        txn(std::move(tx)),
        cursor(std::make_unique<mdbx::cursor_managed>(std::move(cur)))
    {
        txn.park_reading();
    }
};

MDBXIterator::MDBXIterator(const CDBWrapperBase& _parent, MDBXContext &db_context) : CDBIteratorBase(_parent)
{
    auto txn{db_context.env.start_read()};
    mdbx::cursor_managed cursor = txn.open_cursor(db_context.map);
    txn.park_reading();

    m_impl_iter = std::unique_ptr<IteratorImpl>(new IteratorImpl(db_context, std::move(txn), std::move(cursor)));
}

MDBXIterator::~MDBXIterator()
{
    m_impl_iter->cursor->close();
    m_impl_iter->txn.unpark_reading();
    m_impl_iter->txn.commit();
}

void MDBXIterator::SeekImpl(Span<const std::byte> key)
{
    mdbx::slice slKey(CharCast(key.data()), key.size());
    valid = m_impl_iter->cursor->lower_bound(slKey);
}

CDBIteratorBase* MDBXWrapper::NewIterator()
{
    return new MDBXIterator{*this, DBContext()};
}

bool MDBXWrapper::IsEmpty()
{
    auto read_txn = DBContext().env.start_read();
    auto cursor{read_txn.open_cursor(DBContext().map)};

    // the done parameter indicates whether or not the cursor move succeeded.
    auto ret = !cursor.to_first(/*throw_notfound=*/false).done;
    return ret;
}

struct CDBIterator::IteratorImpl {
    const std::unique_ptr<leveldb::Iterator> iter;

    explicit IteratorImpl(leveldb::Iterator* _iter) : iter{_iter} {}
};

CDBIterator::CDBIterator(const CDBWrapperBase& _parent, std::unique_ptr<IteratorImpl> _piter): CDBIteratorBase(_parent),
                                                                                            m_impl_iter(std::move(_piter)) {}

CDBIteratorBase* CDBWrapper::NewIterator()
{
    return new CDBIterator{*this, std::make_unique<CDBIterator::IteratorImpl>(DBContext().pdb->NewIterator(DBContext().iteroptions))};
}

void CDBIterator::SeekImpl(Span<const std::byte> key)
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());
    m_impl_iter->iter->Seek(slKey);
}

Span<const std::byte> CDBIterator::GetKeyImpl() const
{
    return MakeByteSpan(m_impl_iter->iter->key());
}

Span<const std::byte> CDBIterator::GetValueImpl() const
{
    return MakeByteSpan(m_impl_iter->iter->value());
}

CDBIterator::~CDBIterator() = default;
bool CDBIterator::Valid() const { return m_impl_iter->iter->Valid(); }
void CDBIterator::SeekToFirst() { m_impl_iter->iter->SeekToFirst(); }
void CDBIterator::Next() { m_impl_iter->iter->Next(); }

Span<const std::byte> MDBXIterator::GetKeyImpl() const
{
    // 'AsBytes(Span(...' is necessary since mdbx::slice::bytes() returns std::span<char8_t>
    // Rather than Span<std::byte>
    auto ret = AsBytes(Span(m_impl_iter->cursor->current().key.bytes()));

    return ret;
}

Span<const std::byte> MDBXIterator::GetValueImpl() const
{
    // 'AsBytes(Span(...' is necessary since mdbx::slice::bytes() returns std::span<char8_t>
    // Rather than Span<std::byte>
    auto ret = AsBytes(Span(m_impl_iter->cursor->current().value.bytes()));

    return ret;
}

bool MDBXIterator::Valid() const {
    return valid;
}

void MDBXIterator::SeekToFirst()
{
    valid = m_impl_iter->cursor->to_first(/*throw_notfound=*/false).done;
}

void MDBXIterator::Next()
{
    assert(0); // should never be called, need a param for deciding if a dbwrapper needs to be partitioned.
    valid = m_impl_iter->cursor->to_next(/*throw_notfound=*/false).done;
}

uint16_t CDBBatchBase::CurrentPartition() const
{
    LogDebug(BCLog::COINDB, "current partition value is: %d", m_parent.partition_index);
    return m_parent.partition_index;
}

const std::vector<unsigned char>& CDBWrapperBase::GetObfuscateKey() const
{
    return obfuscate_key;
}
