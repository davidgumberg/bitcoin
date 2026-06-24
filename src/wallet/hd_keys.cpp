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

util::Expected<AddHDKeyResult, AddHDKeyError> AddHDKey(CWallet &wallet, const std::optional<CExtKey> &existing_key)
{
    LOCK(wallet.cs_wallet);

    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return util::Unexpected{AddHDKeyError{
            AddHDKeyErrorCode::PRIVATE_KEYS_DISABLED,
            _("addhdkey is not available for wallets without private keys"),
        }};
    }

    if (wallet.IsLocked()) {
        return util::Unexpected{AddHDKeyError{
            AddHDKeyErrorCode::WALLET_LOCKED,
            _("Please enter the wallet passphrase with walletpassphrase first"),
        }};
    }

    CExtKey hdkey;
    if (existing_key) {
        hdkey = *existing_key;
    } else {
        CKey seed_key = GenerateRandomKey();
        hdkey.SetSeed(seed_key);
    }

    std::string descriptor_str = "unused(" + EncodeExtKey(hdkey) + ")";
    FlatSigningProvider keys;
    std::string error;
    std::vector<std::unique_ptr<Descriptor>> descriptors = Parse(descriptor_str, keys, error, /*require_checksum=*/false);
    CHECK_NONFATAL(!descriptors.empty());
    WalletDescriptor wallet_descriptor(std::move(descriptors.at(0)), GetTime(), /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);

    if (wallet.GetDescriptorScriptPubKeyMan(wallet_descriptor) != nullptr) {
        return util::Unexpected{AddHDKeyError{
            AddHDKeyErrorCode::DUPLICATE_KEY,
            _("HD key already exists")
        }};
    }

    auto spkm = wallet.AddWalletDescriptor(wallet_descriptor, keys, /*label=*/"", /*internal=*/false);
    if(!spkm) {
        return util::Unexpected{AddHDKeyError{
            AddHDKeyErrorCode::DESCRIPTOR_ADD_FAILED,
            util::ErrorString(spkm),
        }};
    }

    const DescriptorScriptPubKeyMan& desc_spkm = spkm->get();
    LOCK(desc_spkm.cs_desc_man);
    std::set<CPubKey> pubkeys;
    std::set<CExtPubKey> extpubs;
    desc_spkm.GetWalletDescriptor().descriptor->GetPubKeys(pubkeys, extpubs);
    CHECK_NONFATAL(pubkeys.empty());
    CHECK_NONFATAL(extpubs.size() == 1);

    KeyFingerprint fingerprint;
    std::copy_n(hdkey.key.GetPubKey().GetID().begin(), 4, fingerprint.begin());
    return AddHDKeyResult{
        fingerprint,
        EncodeExtPubKey(*extpubs.begin()),
    };
}

} // namespace wallet
