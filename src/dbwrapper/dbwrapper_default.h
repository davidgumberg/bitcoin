// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_DEFAULT_H
#define BITCOIN_DBWRAPPER_DEFAULT_H

#include <dbwrapper/leveldb.h>

using DBWrapper = LevelDBWrapper;
using DBIterator = LevelDBIterator;
using DBBatch = LevelDBBatch;

#endif // BITCOIN_DBWRAPPER_DEFAULT_H
