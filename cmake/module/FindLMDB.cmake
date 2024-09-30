# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

find_path(LMDB_INCLUDE_DIR
  NAMES lmdb.h
)

find_library(LMDB_LIBRARY
  NAMES lmdb
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LMDB
  REQUIRED_VARS LMDB_LIBRARY LMDB_INCLUDE_DIR
)

if(LMDB_FOUND AND NOT TARGET LMDB::LMDB)
  add_library(LMDB::LMDB UNKNOWN IMPORTED)
  set_target_properties(LMDB::LMDB PROPERTIES
    IMPORTED_LOCATION "${LMDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LMDB_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  LMDB_INCLUDE_DIR
  LMDB_LIBRARY
)
