// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <bench/bench.h>
#include <interfaces/chain.h>
#include <kernel/cs_main.h>
#include <script/interpreter.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cassert>
#include <vector>

/*
 * Creates a test block containing transactions with the following properties:
 * - Each transaction has the same number of inputs and outputs
 * - All Taproot inputs use simple key path spends (no script path spends)
 * - All signatures use SIGHASH_ALL (default sighash)
 * - Each transaction spends all outputs from the previous transaction
 */
CBlock CreateTestBlock(
    TestChain100Setup& test_setup,
    const std::vector<CKey>& keys,
    const std::vector<CTxOut>& outputs,
    int num_txs = 1000)
{
    Chainstate& chainstate{test_setup.m_node.chainman->ActiveChainstate()};

    const WitnessV1Taproot coinbase_taproot{XOnlyPubKey(test_setup.coinbaseKey.GetPubKey())};

    // Create the outputs that will be spent in the first transaction of the test block
    // Doing this in a separate blocks excludes the validation of it's inputs from the benchmark
    auto coinbase_to_spend{test_setup.m_coinbase_txns[0]};
    const auto [first_tx, _]{test_setup.CreateValidTransaction(
        std::vector{coinbase_to_spend},
        std::vector{COutPoint(coinbase_to_spend->GetHash(), 0)},
        chainstate.m_chain.Height() + 1, keys, outputs, std::nullopt, std::nullopt)};
    const auto test_block_parent_coinbase{GetScriptForDestination(coinbase_taproot)};
    test_setup.CreateAndProcessBlock(std::vector{first_tx}, test_block_parent_coinbase, &chainstate);

    std::vector<CMutableTransaction> txs;
    txs.reserve(num_txs);
    CTransactionRef input_tx{MakeTransactionRef(first_tx)};
    for (int i{0}; i < num_txs; i++) {
        std::vector<COutPoint> inputs;
        inputs.reserve(outputs.size());

        for (size_t j{0}; j < outputs.size(); j++) {
            inputs.emplace_back(input_tx->GetHash(), j);
        }

        const auto [taproot_tx, _]{test_setup.CreateValidTransaction(
            std::vector{input_tx}, inputs, chainstate.m_chain.Height() + 1, keys, outputs, std::nullopt, std::nullopt)};
        txs.emplace_back(taproot_tx);
        input_tx = MakeTransactionRef(taproot_tx);
    }

    // Coinbase output can use any output type as it is not spent and will not change the benchmark
    const CScript coinbase_spk{GetScriptForDestination(coinbase_taproot)};
    return test_setup.CreateBlock(txs, coinbase_spk, chainstate);
}

/*
 * Creates key pairs and corresponding outputs for the benchmark transactions.
 * - For Taproot outputs: Creates simple key path spendable outputs
 * - For non-Taproot outputs: Creates P2WPKH (native SegWit v0) outputs
 * - All outputs have value of 1 BTC
 */
std::pair<std::vector<CKey>, std::vector<CTxOut>> CreateKeysAndOutputs(const CKey& coinbaseKey, size_t num_taproot, size_t num_nontaproot, size_t num_pkh)
{
    std::vector<CKey> keys{coinbaseKey};
    keys.reserve(num_taproot + num_nontaproot + 1);

    std::vector<CTxOut> outputs;
    outputs.reserve(num_taproot + num_nontaproot);

    for (size_t i{0}; i < num_nontaproot; i++) {
        const CKey key{GenerateRandomKey()};
        keys.emplace_back(key);
        outputs.emplace_back(COIN, GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()}));
    }

    for (size_t i{0}; i < num_taproot; i++) {
        CKey key{GenerateRandomKey()};
        keys.emplace_back(key);
        outputs.emplace_back(COIN, GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey(key.GetPubKey())}));
    }

    for (size_t i{0}; i < num_pkh; i++) {
        CKey key{GenerateRandomKey()};
        keys.emplace_back(key);
        outputs.emplace_back(COIN, GetScriptForDestination(PKHash{key.GetPubKey()}));
    }

    return {keys, outputs};
}

void BenchmarkConnectBlock(benchmark::Bench& bench, std::vector<CKey>& keys, std::vector<CTxOut>& outputs, TestChain100Setup& test_setup)
{
    const auto test_block{CreateTestBlock(test_setup, keys, outputs)};
    auto pindex{std::make_unique<CBlockIndex>(test_block)};
    auto test_blockhash{std::make_unique<uint256>(test_block.GetHash())};

    Chainstate& chainstate{test_setup.m_node.chainman->ActiveChainstate()};

    pindex->nHeight = chainstate.m_chain.Height() + 1;
    pindex->phashBlock = test_blockhash.get();
    pindex->pprev = chainstate.m_chain.Tip();

    BlockValidationState test_block_state;
    bench.unit("block").run([&] {
        LOCK(cs_main);
        CCoinsViewCache viewNew{&chainstate.CoinsTip()};
        assert(chainstate.ConnectBlock(test_block, test_block_state, pindex.get(), viewNew));
    });
}

static void ConnectBlockAllSchnorr(benchmark::Bench& bench)
{
    const std::unique_ptr test_setup{MakeNoLogFileContext<TestChain100Setup>()};
    auto [keys, outputs]{CreateKeysAndOutputs(test_setup->coinbaseKey, /*num_taproot=*/4, /*num_nontaproot=*/0, /*num_pkh=*/0)};
    BenchmarkConnectBlock(bench, keys, outputs, *test_setup);
}

/**
 * This benchmark is expected to be slower than the AllSchnorr or NoSchnorr benchmark
 * because it uses transactions with both Schnorr and ECDSA signatures
 * which requires the transaction to be hashed multiple times for
 * the different signature allgorithms
 */
static void ConnectBlockMixed(benchmark::Bench& bench)
{
    const std::unique_ptr test_setup{MakeNoLogFileContext<TestChain100Setup>()};
    // Blocks in range 848000 to 868000 have a roughly 20 to 80 ratio of schnorr to ecdsa inputs
    auto [keys, outputs]{CreateKeysAndOutputs(test_setup->coinbaseKey, /*num_taproot=*/1, /*num_nontaproot=*/4, /*num_pkh=*/0)};
    BenchmarkConnectBlock(bench, keys, outputs, *test_setup);
}

static void ConnectBlockNoSchnorr(benchmark::Bench& bench)
{
    const std::unique_ptr test_setup{MakeNoLogFileContext<TestChain100Setup>()};
    auto [keys, outputs]{CreateKeysAndOutputs(test_setup->coinbaseKey, /*num_taproot=*/0, /*num_nontaproot=*/4, /*num_pkh=*/0)};
    BenchmarkConnectBlock(bench, keys, outputs, *test_setup);
}

static void ConnectBlockPKH(benchmark::Bench& bench)
{
    const std::unique_ptr test_setup{MakeNoLogFileContext<TestChain100Setup>()};
    auto [keys, outputs]{CreateKeysAndOutputs(test_setup->coinbaseKey, /*num_taproot=*/0, /*num_nontaproot=*/0, /*num_pkh=*/4)};
    BenchmarkConnectBlock(bench, keys, outputs, *test_setup);
}

BENCHMARK(ConnectBlockAllSchnorr, benchmark::PriorityLevel::HIGH);
BENCHMARK(ConnectBlockMixed, benchmark::PriorityLevel::HIGH);
BENCHMARK(ConnectBlockNoSchnorr, benchmark::PriorityLevel::HIGH);
BENCHMARK(ConnectBlockPKH, benchmark::PriorityLevel::HIGH);
