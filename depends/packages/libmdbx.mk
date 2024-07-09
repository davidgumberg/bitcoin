package=libmdbx
$(package)_version=0.12.9
$(package)_download_path=https://libmdbx.dqdkfa.ru/release
$(package)_file_name=$(package)-amalgamated-$($(package)_version).tar.xz
$(package)_sha256_hash=6ccc5277bfb13ce744fb6d2128de0b11c8f58c81c1fe06179ceaac5c28125a6e

define $(package)_extract_cmds
    mkdir -p $($(package)_extract_dir) && \
    echo "$($(package)_sha256_hash)  $($(package)_source)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
     $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
    $(build_TAR) --no-same-owner -xf $($(package)_source)
endef

define $(package)_config_cmds
  $($(package)_cmake) -S . -B .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef
