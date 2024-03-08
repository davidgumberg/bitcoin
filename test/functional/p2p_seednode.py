#!/usr/bin/env python3
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test seednode interaction with the AddrMan
"""
import time
import random

from test_framework.test_framework import BitcoinTestFramework
from test_framework.messages import (CAddress, msg_addr)
from test_framework.p2p import (
    P2PInterface,
    P2P_SERVICES,
)

def setup_addr_msg(num, time):
    addrs = []
    for i in range(num):
        addr = CAddress()
        addr.time = time + random.randrange(-100, 100)
        addr.nServices = P2P_SERVICES
        addr.ip = f"{random.randrange(128,169)}.{random.randrange(1,255)}.{random.randrange(1,255)}.{random.randrange(1,255)}"
        addr.port = 8333 + i
        addrs.append(addr)

    msg = msg_addr()
    msg.addrs = addrs
    return msg


class P2PSeedNodes(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.disable_autoconnect = False

    def test_no_seednode(self):
        # Check that the seednode is added to m_addr_fetches on bootstrap on an empty addrman
        with self.nodes[0].assert_debug_log(expected_msgs=[], unexpected_msgs=["Empty addrman, adding seednode", "Couldn't connect to peers from addrman after 10 seconds. Adding seednode"], timeout=10):
            self.restart_node(0)

    def test_seednode_empty_addrman(self):
        seed_node = "0.0.0.1"
        # Check that the seednode is added to m_addr_fetches on bootstrap on an empty addrman
        with self.nodes[0].assert_debug_log(expected_msgs=[f"Empty addrman, adding seednode ({seed_node}) to addrfetch"], timeout=10):
            self.restart_node(0, extra_args=[f'-seednode={seed_node}'])

    def test_seednode_addrman_unreachable_peers(self):
        seed_node = "0.0.0.2"
        # Fill the addrman with unreachable nodes
        addr_source = self.nodes[0].add_p2p_connection(P2PInterface())
        addr_source.send_and_ping(setup_addr_msg(10, int(time.time())))

        # Restart the node so seednode is processed again
        with self.nodes[0].assert_debug_log(expected_msgs=[f"Couldn't connect to peers from addrman after 10 seconds. Adding seednode ({seed_node}) to addrfetch"], timeout=20):
            self.restart_node(0, extra_args=[f'-seednode={seed_node}'])

    def run_test(self):
        self.test_no_seednode()
        self.test_seednode_empty_addrman()
        self.test_seednode_addrman_unreachable_peers()


if __name__ == '__main__':
    P2PSeedNodes().main()

