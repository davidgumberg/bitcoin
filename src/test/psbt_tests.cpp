// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/hex_base.h>
#include <musig.h>
#include <serialize.h>
#include <psbt.h>
#include <span.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <cstdint>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

using keypairs = std::map<std::vector<unsigned char>, std::vector<unsigned char>>;

std::vector<unsigned char> BuildRawPSBTWithUnsignedTx(
    const keypairs& inputs = {},
    const keypairs& outputs = {})
{
    // Construct a serialized unsigned transaction that will go in the global map.
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid{}, 0});
    mtx.vout.emplace_back(CAmount{1}, CScript() << OP_RETURN);

    std::vector<unsigned char> unsigned_tx;
    VectorWriter vw_tx{unsigned_tx, 0};
    vw_tx << TX_NO_WITNESS(CTransaction(mtx));

    // https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki#specification
    std::vector<unsigned char> psbt;
    VectorWriter vw{psbt, 0};
    vw << PSBT_MAGIC_BYTES;

    // Global map
    std::vector<unsigned char> global_key{PSBT_GLOBAL_UNSIGNED_TX};
    vw << global_key;
    vw << unsigned_tx;
    vw << PSBT_SEPARATOR;

    // Input map
    for (auto &input : inputs) {
        vw << input;
    }
    vw << PSBT_SEPARATOR;

    // Output map
    for (auto &output : outputs) {
        vw << output;
    }
    vw << PSBT_SEPARATOR;

    return psbt;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(psbt_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(musig2_invalid_pubkey_rejected)
{
    CKey key;
    key.MakeNewKey(true);
    CPubKey valid_pub = key.GetPubKey();

    std::vector<unsigned char> valid_pk(valid_pub.begin(), valid_pub.end());
    std::vector<unsigned char> invalid_pk(CPubKey::COMPRESSED_SIZE, 0);

    auto psbt_musig_key = [](uint8_t type,
                       const std::vector<unsigned char>& part_pk,
                       const std::vector<unsigned char>& agg_pk) {
        std::vector<unsigned char> k;
        k.reserve(1 + 33 + 33);
        k.push_back(type);
        k.insert(k.end(), part_pk.begin(), part_pk.end());
        k.insert(k.end(), agg_pk.begin(), agg_pk.end());
        return k;
    };


    // Only the keys vary inside of the test.
     struct TestCase {
        std::string name;
        uint8_t keytype;
        std::vector<unsigned char> value;
    };

    const std::vector<TestCase> test_cases = {
        {"Partial signature", PSBT_IN_MUSIG2_PARTIAL_SIG, std::vector<unsigned char>(sizeof(uint256), 0x00)},
        {"Public nonce", PSBT_IN_MUSIG2_PUB_NONCE, std::vector<unsigned char>(MUSIG2_PUBNONCE_SIZE, 0x00)}
    };

    for (const auto &test: test_cases) {
        // Invalid participant key
        {
            keypairs inputs{{psbt_musig_key(test.keytype, /*part_pk=*/invalid_pk, /*agg_pk=*/valid_pk), test.value}};
            auto raw = BuildRawPSBTWithUnsignedTx(inputs);
            PartiallySignedTransaction psbt;
            std::string err;
            BOOST_CHECK_MESSAGE(!DecodeRawPSBT(psbt, MakeByteSpan(raw), err),
                test.name + " with invalid participant pubkey should be rejected");
        }
        // Invalid aggregate key
        {
            keypairs inputs{{psbt_musig_key(test.keytype, /*part_pk=*/valid_pk, /*agg_pk=*/invalid_pk), test.value}};
            auto raw = BuildRawPSBTWithUnsignedTx(inputs);
            PartiallySignedTransaction psbt;
            std::string err;
            BOOST_CHECK_MESSAGE(!DecodeRawPSBT(psbt, MakeByteSpan(raw), err),
                test.name + " with invalid aggregate pubkey should be rejected");
        }
        // Both keys valid
        {
            keypairs inputs{{psbt_musig_key(test.keytype, /*part_pk=*/valid_pk, /*agg_pk=*/valid_pk), test.value}};
            auto raw = BuildRawPSBTWithUnsignedTx(inputs);
            PartiallySignedTransaction psbt;
            std::string err;
            BOOST_CHECK_MESSAGE(DecodeRawPSBT(psbt, MakeByteSpan(raw), err),
                "Valid " + test.name + " partial sig was rejected: " + err);

            // Round-trip
            std::vector<uint8_t> rt;
            VectorWriter{rt, 0, psbt};
            PartiallySignedTransaction psbt_rt;
            std::string err_rt;
            BOOST_CHECK_MESSAGE(DecodeRawPSBT(psbt_rt, MakeByteSpan(rt), err_rt),
                test.name + " round-trip failed: " + err_rt);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
