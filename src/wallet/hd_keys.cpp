// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/hd_keys.h>

#include <key_io.h>
#include <script/descriptor.h>
#include <util/check.h>
#include <util/translation.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace wallet {

bilingual_str AddHDKeyErrorString(AddHDKeyError error)
{
    switch (error) {
    case AddHDKeyError::PRIVATE_KEYS_DISABLED:
        return _("addhdkey is not available for wallets without private keys");
    case AddHDKeyError::WALLET_LOCKED:
        return _("Please enter the wallet passphrase with walletpassphrase first");
    case AddHDKeyError::DUPLICATE_KEY:
        return _("HD key already exists");
    case AddHDKeyError::DESCRIPTOR_ADD_FAILED:
        return _("Could not add HD key descriptor");
    }
    assert(false);
}

util::Expected<AddHDKeyResult, AddHDKeyError> AddHDKey(CWallet &wallet, const std::optional<CExtKey> &existing_key)
{
    LOCK(wallet.cs_wallet);

    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return util::Unexpected{AddHDKeyError::PRIVATE_KEYS_DISABLED};
    }

    if (wallet.IsLocked()) {
        return util::Unexpected{AddHDKeyError::WALLET_LOCKED};
    }

    CExtKey hdkey;
    if (existing_key) {
        hdkey = *existing_key;
    } else {
        CKey seed_key = GenerateRandomKey();
        hdkey.SetSeed(seed_key);
    }

    std::string desc_str = "unused(" + EncodeExtKey(hdkey) + ")";
    FlatSigningProvider keys;
    std::string error;
    std::vector<std::unique_ptr<Descriptor>> descs = Parse(desc_str, keys, error, false);
    CHECK_NONFATAL(!descs.empty());
    WalletDescriptor w_desc(std::move(descs.at(0)), GetTime(), 0, 0, 0);

    if (wallet.GetDescriptorScriptPubKeyMan(w_desc) != nullptr) {
        return util::Unexpected{AddHDKeyError::DUPLICATE_KEY};
    }

    auto spkm = wallet.AddWalletDescriptor(w_desc, keys, /*label=*/"", /*internal=*/false);
    if(!spkm) {
        return util::Unexpected{AddHDKeyError::DESCRIPTOR_ADD_FAILED};
    }

    const DescriptorScriptPubKeyMan& desc_spkm = spkm->get();
    LOCK(desc_spkm.cs_desc_man);
    std::set<CPubKey> pubkeys;
    std::set<CExtPubKey> extpubs;
    desc_spkm.GetWalletDescriptor().descriptor->GetPubKeys(pubkeys, extpubs);
    CHECK_NONFATAL(pubkeys.size() == 0);
    CHECK_NONFATAL(extpubs.size() == 1);

    Fingerprint fingerprint;
    std::copy_n(hdkey.key.GetPubKey().GetID().begin(), 4, fingerprint.begin());
    return AddHDKeyResult{
        fingerprint,
        EncodeExtPubKey(*extpubs.begin()),
    };
}

} // namespace wallet
