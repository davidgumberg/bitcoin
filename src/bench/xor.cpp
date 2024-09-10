// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <algorithm>
#include <bench/bench.h>
#include <filesystem>
#include <random.h>
#include <span.h>
#include <streams.h>

#include <cstddef>
#include <vector>

static void Xor(benchmark::Bench& bench)
{
    FastRandomContext frc{/*fDeterministic=*/true};
    auto data{frc.randbytes<std::byte>(1024)};
    auto key{frc.randbytes<std::byte>(31)};

    bench.batch(data.size()).unit("byte").run([&] {
        util::Xor(data, key);
    });
}

static void AutoFileXor(benchmark::Bench& bench)
{
    FastRandomContext frc{/*fDeterministic=*/true};

    auto tmp_dir = std::filesystem::temp_directory_path();
    std::string unique_filename = "xortest_" + std::to_string(frc.rand32());
    auto path = tmp_dir / unique_filename;

    auto data{frc.randbytes<std::byte>(4'096)};
    Span<const std::byte> src{data};

    auto key{frc.randbytes<std::byte>(8)};

    FILE *m_file = fsbridge::fopen(path, "wb+");

    bench.batch(src.size()).unit("byte").run([&] {
        auto current_pos{std::ftell(m_file)};
        std::array<std::byte, 4096> buf;

        auto buf_now{Span{buf}.first(std::min<size_t>(src.size(), buf.size()))};
        std::copy(src.begin(), src.begin() + buf_now.size(), buf_now.begin());
        util::Xor(buf_now, key, current_pos);

        std::fwrite(buf_now.data(), 1, buf_now.size(), m_file);
    });

    std::fclose(m_file);
    std::filesystem::remove(path);
}

BENCHMARK(Xor, benchmark::PriorityLevel::HIGH);
BENCHMARK(AutoFileXor, benchmark::PriorityLevel::HIGH);
