// Copyright (c) 2016-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "cuckoocache.h"
#include <algorithm>
#include <bench/bench.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <dbwrapper.h>
#include <kernel/mempool_options.h>
#include <key.h>
#include <memusage.h>
#include <node/caches.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <test/util/transaction_utils.h>
#include <test/util/coins.h>
#include <txdb.h>
#include <util/fs.h>

#include <cassert>

class CCoinsViewDBBench
{
public:
    // This is hard-coded in `CompleteChainstateInitialization()`
    static constexpr double init_cache_fraction = 0.2;
    fs::path m_db_path;
    size_t m_cache_size;
    CCoinsViewDB m_db;

    CCoinsViewDBBench(fs::path path, size_t cache_size_bytes, bool obfuscate = true) :
        m_db_path(path),
        m_cache_size(cache_size_bytes),
        m_db(
            DBParams{
                .path = path,
                .cache_bytes = m_cache_size,
                .memory_only = false,
                .wipe_data = true, // DB gets wiped at the end of every bench run
                .obfuscate = obfuscate,
                .options = DBOptions{}},
            CoinsViewOptions{}
        ){}
};

// The below comes from `Chainstate::GetCoinsCacheSize()
static constexpr int64_t MAX_BLOCK_COINSDB_USAGE_BYTES = 10 * 1024 * 1024;  // 10MB

// Calculate large cache threshold, accounting for the mempool allocation during IBD
static constexpr size_t large_threshold(int64_t cache_size_bytes)
{
    int64_t nTotalSpace{DEFAULT_MAX_MEMPOOL_SIZE_MB * 1'000'000 + cache_size_bytes};
    return  std::max((9 * nTotalSpace) / 10, nTotalSpace - MAX_BLOCK_COINSDB_USAGE_BYTES);
}

// Bench a CCoinsViewCache filled with random coins flushing to a CCoinsViewDB.
static void CCoinsViewDBFlush(benchmark::Bench& bench)
{
    auto test_setup = MakeNoLogFileContext<TestingSetup>();
    FastRandomContext random{/*fDeterministic=*/true};

    auto db_path = test_setup->m_path_root / "test_coinsdb";

    // This comes from `CompleteChainstateInitialization()`.
    size_t cache_size_bytes = node::CalculateCacheSizes(ArgsManager{},/*n_indexes=*/0).coins_db * CCoinsViewDBBench::init_cache_fraction;

    std::vector<std::pair<COutPoint, Coin>> utxo_batch{};
    size_t coins_usage{0};
    while (memusage::DynamicUsage(utxo_batch) + coins_usage < large_threshold(cache_size_bytes)){
        auto inserted = utxo_batch.emplace_back(RandUTXO(random, /*spk_len=*/56));
        // DynamicUsage doesn't recurse, so we need to calculate the space taken by the Coins'
        // CTxOuts.
        coins_usage += inserted.second.DynamicMemoryUsage();
    }

    auto randBlockHash = random.rand256();

    // Benchmark flushing a CCoinsViewCache to a CCoinsViewDB,
    // This times, extraneously, adding coins to the cache view, and opening and wiping the db.
    bench.batch(utxo_batch.size()).unit("coin").run([&] {
        auto bench_db = CCoinsViewDBBench(db_path, cache_size_bytes);
        CCoinsViewCache coinsviewcache(&bench_db.m_db);
        coinsviewcache.SetBestBlock(randBlockHash);

        // Add coins until the large threshold
        for (auto [outpoint, coin] : utxo_batch){
            coinsviewcache.AddCoin(std::move(outpoint), std::move(coin), /*possible_overwrite=*/false);
        }

        coinsviewcache.Flush();
    });
}

// Bench a CCoinsViewCache being filled with random coins.
static void CCoinsViewCacheAddingCoins(benchmark::Bench& bench)
{
    FastRandomContext random{/*fDeterministic=*/true};

    size_t cache_size_bytes = node::CalculateCacheSizes(ArgsManager{},/*n_indexes=*/0).coins_db * CCoinsViewDBBench::init_cache_fraction;

    std::vector<std::pair<COutPoint, Coin>> utxo_batch{};
    size_t coins_usage{0};
    while (memusage::DynamicUsage(utxo_batch) + coins_usage < large_threshold(cache_size_bytes)){
        auto inserted = utxo_batch.emplace_back(RandUTXO(random, /*spk_len=*/56));
        // DynamicUsage doesn't recurse, so we need to calculate the space taken by the Coins'
        // CTxOuts.
        coins_usage += inserted.second.DynamicMemoryUsage();
    }

    auto randBlockHash = random.rand256();

    CCoinsView dummyView{};
    // Bench.
    bench.batch(utxo_batch.size()).unit("coin").run([&] {
        CCoinsViewCache coinsviewcache(&dummyView);
        coinsviewcache.SetBestBlock(randBlockHash);

        // Add coins until the large threshold
        for (auto [outpoint, coin] : utxo_batch){
            coinsviewcache.AddCoin(std::move(outpoint), std::move(coin), false);
        }
    });
}

BENCHMARK(CCoinsViewDBFlush, benchmark::PriorityLevel::HIGH);
BENCHMARK(CCoinsViewCacheAddingCoins, benchmark::PriorityLevel::HIGH);

