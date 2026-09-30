#!/usr/bin/env python3
# Copyright (c) 2014-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the wallet keypool and interaction with wallet encryption/locking."""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.descriptors import descsum_create
from test_framework.extendedkey import ExtendedPrivateKey
from test_framework.util import (
    assert_equal,
    assert_not_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet_util import WalletUnlock

class KeyPoolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        nodes = self.nodes
        nodes[0].createwallet("encrypted", passphrase=self.default_wallet_pass)
        wallet = nodes[0].get_wallet_rpc("encrypted")
        nodes[0].get_wallet_rpc(self.default_wallet_name).sendtoaddress(wallet.getnewaddress(), 10)
        self.generate(nodes[0], 1)

        # Import hardened derivation only descriptors
        with WalletUnlock(wallet, self.default_wallet_pass):
            wallet.importdescriptors([
                {
                    "desc": descsum_create(f"wpkh({ExtendedPrivateKey.generate().to_string()}/0h/*h)"),
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True
                },
                {
                    "desc": descsum_create(f"pkh({ExtendedPrivateKey.generate().to_string()}/1h/*h)"),
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True
                },
                {
                    "desc": descsum_create(f"sh(wpkh({ExtendedPrivateKey.generate().to_string()}/2h/*h))"),
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True
                },
                {
                    "desc": descsum_create(f"wpkh({ExtendedPrivateKey.generate().to_string()}/3h/*h)"),
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True,
                    "internal": True
                },
                {
                    "desc": descsum_create(f"pkh({ExtendedPrivateKey.generate().to_string()}/4h/*h)"),
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True,
                    "internal": True
                },
                {
                    "desc": descsum_create(f"sh(wpkh({ExtendedPrivateKey.generate().to_string()}/5h/*h))"),
                    "timestamp": "now",
                    "range": [0,0],
                    "active": True,
                    "internal": True
                }
            ])
        # Keep creating keys
        addr = wallet.getnewaddress()
        addr_data = wallet.getaddressinfo(addr)
        assert_raises_rpc_error(-12, "Error: Keypool ran out, please call keypoolrefill first", wallet.getnewaddress)

        # put six (plus 2) new keys in the keypool (100% external-, +100% internal-keys, 1 in min)
        with WalletUnlock(wallet, self.default_wallet_pass):
            wallet.keypoolrefill(6)
        wi = wallet.getwalletinfo()
        assert_equal(wi['keypoolsize_hd_internal'], 24)
        assert_equal(wi['keypoolsize'], 24)

        # drain the internal keys
        wallet.getrawchangeaddress()
        wallet.getrawchangeaddress()
        wallet.getrawchangeaddress()
        wallet.getrawchangeaddress()
        wallet.getrawchangeaddress()
        wallet.getrawchangeaddress()
        # remember keypool sizes
        wi = wallet.getwalletinfo()
        kp_size_before = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        # the next one should fail
        assert_raises_rpc_error(-12, "Keypool ran out", wallet.getrawchangeaddress)
        # check that keypool sizes did not change
        wi = wallet.getwalletinfo()
        kp_size_after = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        assert_equal(kp_size_before, kp_size_after)

        # drain the external keys
        addr = set()
        addr.add(wallet.getnewaddress(address_type="bech32"))
        addr.add(wallet.getnewaddress(address_type="bech32"))
        addr.add(wallet.getnewaddress(address_type="bech32"))
        addr.add(wallet.getnewaddress(address_type="bech32"))
        addr.add(wallet.getnewaddress(address_type="bech32"))
        addr.add(wallet.getnewaddress(address_type="bech32"))
        assert_equal(len(addr), 6)
        # remember keypool sizes
        wi = wallet.getwalletinfo()
        kp_size_before = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        # the next one should fail
        assert_raises_rpc_error(-12, "Error: Keypool ran out, please call keypoolrefill first", wallet.getnewaddress)
        # check that keypool sizes did not change
        wi = wallet.getwalletinfo()
        kp_size_after = [wi['keypoolsize_hd_internal'], wi['keypoolsize']]
        assert_equal(kp_size_before, kp_size_after)

        # refill keypool with three new addresses
        wallet.walletpassphrase(self.default_wallet_pass, 1)
        wallet.keypoolrefill(3)

        # test walletpassphrase timeout
        # CScheduler relies on condition_variable::wait_until() which does not
        # guarantee accurate timing. We'll wait up to 5 seconds to execute a 1
        # second scheduled event.
        nodes[0].wait_until(lambda: wallet.getwalletinfo()["unlocked_until"] == 0, timeout=5)

        # drain the keypool
        for _ in range(3):
            wallet.getnewaddress()
        assert_raises_rpc_error(-12, "Keypool ran out", wallet.getnewaddress)

        with WalletUnlock(wallet, self.default_wallet_pass):
            wallet.keypoolrefill(100)
            wi = wallet.getwalletinfo()
            assert_equal(wi['keypoolsize_hd_internal'], 400)
            assert_equal(wi['keypoolsize'], 400)

        # create a blank wallet
        nodes[0].createwallet(wallet_name='w2', blank=True, disable_private_keys=True)
        w2 = nodes[0].get_wallet_rpc('w2')

        # refer to initial wallet as w1
        w1 = wallet

        # import private key and fund it
        address = addr.pop()
        desc = w1.getaddressinfo(address)['desc']
        res = w2.importdescriptors([{'desc': desc, 'timestamp': 'now'}])
        assert_equal(res[0]['success'], True)

        with WalletUnlock(w1, self.default_wallet_pass):
            res = w1.sendtoaddress(address=address, amount=0.00010000)
        self.generatetoaddress(nodes[0], 1, w1.getnewaddress())
        destination = addr.pop()

        # Using a fee rate (10 sat / byte) well above the minimum relay rate
        # creating a 5,000 sat transaction with change should not be possible
        assert_raises_rpc_error(-4, "Transaction needs a change address, but we can't generate it.", w2.walletcreatefundedpsbt, inputs=[], outputs=[{addr.pop(): 0.00005000}], subtractFeeFromOutputs=[0], feeRate=0.00010)

        # creating a 10,000 sat transaction without change, with a manual input, should still be possible
        res = w2.walletcreatefundedpsbt(inputs=w2.listunspent(), outputs=[{destination: 0.00010000}], subtractFeeFromOutputs=[0], feeRate=0.00010)
        assert_equal("psbt" in res, True)

        # creating a 10,000 sat transaction without change should still be possible
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00010000}], subtractFeeFromOutputs=[0], feeRate=0.00010)
        assert_equal("psbt" in res, True)
        # should work without subtractFeeFromOutputs if the exact fee is subtracted from the amount
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00008900}], feeRate=0.00010)
        assert_equal("psbt" in res, True)

        # dust change should be removed
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00008800}], feeRate=0.00010)
        assert_equal("psbt" in res, True)

        # create a transaction without change at the maximum fee rate, such that the output is still spendable:
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00010000}], subtractFeeFromOutputs=[0], feeRate=0.0008823)
        assert_equal("psbt" in res, True)
        assert_equal(res["fee"], Decimal("0.00009706"))

        # creating a 10,000 sat transaction with a manual change address should be possible
        res = w2.walletcreatefundedpsbt(inputs=[], outputs=[{destination: 0.00010000}], subtractFeeFromOutputs=[0], feeRate=0.00010, changeAddress=addr.pop())
        assert_equal("psbt" in res, True)

if __name__ == '__main__':
    KeyPoolTest(__file__).main()
