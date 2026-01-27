// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_LEVELDB_H
#define BITCOIN_DBWRAPPER_LEVELDB_H

#include <dbwrapper/dbwrapper.h>

static const size_t LEVELDBWRAPPER_MAX_FILE_SIZE = 32 << 20; // 32 MiB

struct LevelDBContext;

class LevelDBBatch : public DBBatchBase
{
    friend class DBWrapperBase;
    friend class LevelDBWrapper;

private:
    struct WriteBatchImpl;
    const std::unique_ptr<WriteBatchImpl> m_impl_batch;

    void WriteImpl(std::span<const std::byte> key, DataStream& value) override;
    void EraseImpl(std::span<const std::byte> key) override;
public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit LevelDBBatch(const DBWrapperBase& _parent);
    ~LevelDBBatch() override;
    void Clear() override;
    size_t ApproximateSize() const override;
};

class LevelDBWrapper : public DBWrapperBase
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
    LevelDBWrapper(const DBParams& params);
    ~LevelDBWrapper() override;

    std::unique_ptr<DBBatchBase> CreateBatch() override {
        return std::make_unique<LevelDBBatch>(*this);
    }
    void WriteBatch(DBBatchBase& batch, bool fSync = false) override;
    DBIteratorBase* NewIterator() override;

    static bool DestroyDB(const std::string& path_str);
};

// LevelDB implementation of iterator
class LevelDBIterator : public DBIteratorBase
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
    LevelDBIterator(const DBWrapperBase& _parent, std::unique_ptr<IteratorImpl> _piter);
    ~LevelDBIterator() override;

    bool Valid() const override;
    void SeekToFirst() override;
    void Next() override;
};

#endif // BITCOIN_DBWRAPPER_LEVELDB_H
