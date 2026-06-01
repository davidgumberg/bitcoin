// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_HD_KEYS_H
#define BITCOIN_WALLET_HD_KEYS_H

#include <key.h>
#include <util/expected.h>
#include <util/translation.h>
#include <wallet/types.h>

#include <optional>
#include <string>

namespace wallet {

class CWallet;

struct AddHDKeyResult {
    Fingerprint fingerprint;
    std::string xpub;
};

enum class AddHDKeyError {
    PRIVATE_KEYS_DISABLED,
    WALLET_LOCKED,
    DUPLICATE_KEY,
    DESCRIPTOR_ADD_FAILED
};

util::Expected<AddHDKeyResult, AddHDKeyError> AddHDKey(CWallet& wallet, const std::optional<CExtKey>& existing_key);

bilingual_str AddHDKeyErrorString(AddHDKeyError error);

} // namespace wallet

#endif // BITCOIN_WALLET_HD_KEYS_H
