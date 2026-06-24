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
    KeyFingerprint fingerprint;
    std::string xpub;
};

enum class AddHDKeyErrorCode {
    PRIVATE_KEYS_DISABLED,
    WALLET_LOCKED,
    DUPLICATE_KEY,
    DESCRIPTOR_ADD_FAILED
};

struct AddHDKeyError {
    AddHDKeyErrorCode code;
    bilingual_str message;
};

util::Expected<AddHDKeyResult, AddHDKeyError> AddHDKey(CWallet& wallet, const std::optional<CExtKey>& existing_key);

} // namespace wallet

#endif // BITCOIN_WALLET_HD_KEYS_H
