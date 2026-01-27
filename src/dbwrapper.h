// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_H
#define BITCOIN_DBWRAPPER_H

#include <attributes.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/check.h>
#include <util/fs.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

static const size_t DBWRAPPER_PREALLOC_KEY_SIZE = 64;
static const size_t DBWRAPPER_PREALLOC_VALUE_SIZE = 1024;
static const size_t DBWRAPPER_MAX_FILE_SIZE = 32 << 20; // 32 MiB

//! User-controlled performance and debug options.
struct DBOptions {
    //! Compact database on startup.
    bool force_compact = false;
};

//! Application-specific storage settings.
struct DBParams {
    //! Location in the filesystem where leveldb data will be stored.
    fs::path path;
    //! Configures various leveldb cache settings.
    size_t cache_bytes;
    //! If true, use leveldb's memory environment.
    bool memory_only = false;
    //! If true, remove all existing data.
    bool wipe_data = false;
    //! If true, store data obfuscated via simple XOR. If false, XOR with a
    //! zero'd byte array.
    bool obfuscate = false;
    //! Passed-through options.
    DBOptions options{};
};

class dbwrapper_error : public std::runtime_error
{
public:
    explicit dbwrapper_error(const std::string& msg) : std::runtime_error(msg) {}
};

class DBWrapperBase;

bool DestroyDB(const std::string& path_str);

/** Batch of changes queued to be written to a CDBWrapper */
class DBBatchBase
{
protected:
    const DBWrapperBase &m_parent;

    DataStream ssKey{};
    DataStream ssValue{};

    virtual void WriteImpl(std::span<const std::byte> key, DataStream& value) = 0;
    virtual void EraseImpl(std::span<const std::byte> key) = 0;

public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit DBBatchBase(const DBWrapperBase& _parent) : m_parent{_parent} {}
    virtual ~DBBatchBase() = default;
    virtual void Clear() = 0;
    virtual size_t ApproximateSize() const = 0;

    template <typename K, typename V>
    void Write(const K& key, const V& value)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssValue.reserve(DBWRAPPER_PREALLOC_VALUE_SIZE);
        ssKey << key;
        ssValue << value;
        WriteImpl(ssKey, ssValue);
        ssKey.clear();
        ssValue.clear();
    }

    template <typename K>
    void Erase(const K& key)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        EraseImpl(ssKey);
        ssKey.clear();
    }
};

class DBBatch : public DBBatchBase
{
    friend class DBWrapperBase;
    friend class DBWrapper;

private:
    struct WriteBatchImpl;
    const std::unique_ptr<WriteBatchImpl> m_impl_batch;

    void WriteImpl(std::span<const std::byte> key, DataStream& value) override;
    void EraseImpl(std::span<const std::byte> key) override;
public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit DBBatch(const DBWrapperBase& _parent);
    ~DBBatch() override;
    void Clear() override;
    size_t ApproximateSize() const override;
};

class DBIteratorBase;

class DBWrapperBase
{
protected:
    DBWrapperBase(const DBParams& params) : m_name(fs::PathToString(params.path.stem())) {}
    //! the name of this database
    std::string m_name;

    //! optional XOR-obfuscation of the database
    Obfuscation m_obfuscation;

    //! obfuscation key storage key, null-prefixed to avoid collisions
    inline static const std::string OBFUSCATION_KEY{"\000obfuscate_key", 14}; // explicit size to avoid truncation at leading \0

    virtual std::optional<std::string> ReadImpl(std::span<const std::byte> key) const = 0;
    virtual bool ExistsImpl(std::span<const std::byte> key) const = 0;
    virtual size_t EstimateSizeImpl(std::span<const std::byte> key1, std::span<const std::byte> key2) const = 0;

    //! Initializes m_obfuscation from DB if one exists, otherwise generates and
    //! writes an obfuscation key.
    void InitializeObfuscation(const DBParams& params);

public:
    virtual ~DBWrapperBase() = default;

    DBWrapperBase(const DBWrapperBase&) = delete;
    DBWrapperBase& operator=(const DBWrapperBase&) = delete;
    DBWrapperBase(DBWrapperBase&& other) = default;
    DBWrapperBase& operator=(DBWrapperBase&& other) = default;

    const Obfuscation& GetObfuscation() const
    {
        return m_obfuscation;
    }

    virtual std::unique_ptr<DBBatchBase> CreateBatch() = 0;

    template <typename K, typename V>
    bool Read(const K& key, V& value) const
    {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        std::optional<std::string> strValue{ReadImpl(ssKey)};
        if (!strValue) {
            return false;
        }
        try {
            DataStream ssValue{MakeByteSpan(*strValue)};
            m_obfuscation(ssValue);
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template <typename K, typename V>
    void Write(const K& key, const V& value, bool fSync = false)
    {
        auto batch = CreateBatch();
        batch->Write(key, value);
        WriteBatch(*batch, fSync);
    }

    template <typename K>
    bool Exists(const K& key) const
    {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return ExistsImpl(ssKey);
    }

    template <typename K>
    void Erase(const K& key, bool fSync = false)
    {
        auto batch = CreateBatch();
        batch->Erase(key);
        WriteBatch(*batch, fSync);
    }

    virtual void WriteBatch(DBBatchBase& batch, bool fSync = false) = 0;

    // Get an estimate of LevelDB memory usage (in bytes).
    virtual size_t DynamicMemoryUsage() const = 0;

    virtual DBIteratorBase* NewIterator() = 0;

    /**
     * Return true if the database managed by this class contains no entries.
     */
    bool IsEmpty();

    template<typename K>
    size_t EstimateSize(const K& key_begin, const K& key_end) const
    {
        DataStream ssKey1{}, ssKey2{};
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        return EstimateSizeImpl(ssKey1, ssKey2);
    }
};

struct LevelDBContext;

class DBWrapper : public DBWrapperBase
{
protected:
private:
    //! holds all leveldb-specific fields of this class
    std::unique_ptr<LevelDBContext> m_db_context;
    auto& DBContext() const LIFETIMEBOUND { return *Assert(m_db_context); }

    std::optional<std::string> ReadImpl(std::span<const std::byte> key) const override;
    bool ExistsImpl(std::span<const std::byte> key) const override;
    size_t EstimateSizeImpl(std::span<const std::byte> key1, std::span<const std::byte> key2) const override;

    size_t DynamicMemoryUsage() const override;

public:
    DBWrapper(const DBParams& params);
    ~DBWrapper() override;

    std::unique_ptr<DBBatchBase> CreateBatch() override {
        return std::make_unique<DBBatch>(*this);
    }
    void WriteBatch(DBBatchBase& batch, bool fSync = false) override;
    DBIteratorBase* NewIterator() override;

    static bool DestroyDB(const std::string& path_str);
};

class DBIteratorBase
{
protected:
    const DBWrapperBase &parent;

    virtual void SeekImpl(std::span<const std::byte> key) = 0;
    virtual std::span<const std::byte> GetKeyImpl() const = 0;
    virtual std::span<const std::byte> GetValueImpl() const = 0;
public:
    explicit DBIteratorBase(const DBWrapperBase& _parent)
        : parent(_parent) {}
    virtual ~DBIteratorBase() = default;

    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void Next() = 0;

    template<typename K> void Seek(const K& key) {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        SeekImpl(ssKey);
    }

    template<typename K> bool GetKey(K& key) {
        try {
            DataStream ssKey{GetKeyImpl()};
            ssKey >> key;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template<typename V> bool GetValue(V& value) {
        try {
            DataStream ssValue{GetValueImpl()};
            parent.GetObfuscation()(ssValue);
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
};

// LevelDB implementation of iterator
class DBIterator : public DBIteratorBase
{
public:
    struct IteratorImpl;
private:
    const std::unique_ptr<IteratorImpl> m_impl_iter;

    void SeekImpl(std::span<const std::byte> key) override;
    std::span<const std::byte> GetKeyImpl() const override;
    std::span<const std::byte> GetValueImpl() const override;
public:
    /**
     * @param[in] _parent          Parent CDBWrapper instance.
     * @param[in] _piter           The original leveldb iterator.
     */
    DBIterator(const DBWrapperBase& _parent, std::unique_ptr<IteratorImpl> _piter);
    ~DBIterator() override;

    bool Valid() const override;
    void SeekToFirst() override;
    void Next() override;
};

#endif // BITCOIN_DBWRAPPER_H
