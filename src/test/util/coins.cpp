// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/coins.h>

#include <coins.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/random.h>
#include <test/util/script.h>
#include <uint256.h>

#include <stdint.h>
#include <utility>

COutPoint AddTestCoin(FastRandomContext& rng, CCoinsViewCache& coins_view)
{
    auto [outpoint, coin] = RandUTXO(rng, /*spk_len=*/56);
    coins_view.AddCoin(outpoint, std::move(coin), /*possible_overwrite=*/false);

    return outpoint;
}

std::pair<COutPoint, Coin> RandUTXO(FastRandomContext& rng, size_t spk_len)
{
    Coin coin;
    COutPoint outpoint{Txid::FromUint256(rng.rand256()), /*nIn=*/0};
    coin.nHeight = rng.rand32();
    coin.out.nValue = RandMoney(rng);
    coin.out.scriptPubKey = RandScript(rng, spk_len);

    return {outpoint, coin};
}
