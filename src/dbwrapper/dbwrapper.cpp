// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <random.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/obfuscation.h>
#include <util/strencodings.h>

#include <cassert>
#include <dbwrapper/dbwrapper.h>
#include <dbwrapper/leveldb.h>
#include <memory>

void DBWrapperBase::InitializeObfuscation(const DBParams& params)
{
    const auto& db_path = fs::PathToString(params.path);
    const bool obfuscate_exists = Read(OBFUSCATION_KEY, m_obfuscation);
    if (!obfuscate_exists && params.obfuscate && IsEmpty()) {
        // Generate and write the new obfuscation key.
        const Obfuscation obfuscation{FastRandomContext{}.randbytes<Obfuscation::KEY_SIZE>()};
        assert(!m_obfuscation); // Make sure the key is written without obfuscation.
        Write(OBFUSCATION_KEY, obfuscation);
        m_obfuscation = obfuscation;
        LogInfo("Wrote new obfuscation key for %s: %s", db_path, m_obfuscation.HexKey());
    }
    LogInfo("Using obfuscation key for %s: %s", db_path, m_obfuscation.HexKey());
}

bool DBWrapperBase::IsEmpty()
{
    std::unique_ptr<DBIteratorBase> it(NewIterator());
    it->SeekToFirst();
    return !(it->Valid());
}
