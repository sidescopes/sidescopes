#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/parallel_for.h"

namespace sidescopes {

TEST_CASE("parallelChunkCount stays single-threaded until the work justifies threads")
{
    CHECK(parallelChunkCount(0, 64) == 1);
    CHECK(parallelChunkCount(-5, 64) == 1);
    CHECK(parallelChunkCount(1000, 0) == 1);
    CHECK(parallelChunkCount(63, 64) == 1);

    // Never more chunks than the work or the cap allow, always at least one.
    for (const int rowCount : {64, 100, 500, 1000, 100000}) {
        const int chunks = parallelChunkCount(rowCount, 64);
        CHECK(chunks >= 1);
        CHECK(chunks <= MaxParallelChunks);
        CHECK(chunks <= rowCount / 64);
    }
}

TEST_CASE("runParallelChunks visits every row exactly once")
{
    // Each chunk writes only its own half-open range, so disjoint indices are
    // touched by disjoint threads - a clean count verifies both completeness
    // (nothing skipped) and disjointness (nothing double-counted).
    for (const int chunkCount : {1, 2, 3, 4, 8}) {
        for (const int rowCount : {0, 1, 7, 64, 100, 257}) {
            std::vector<int> visits(static_cast<std::size_t>(rowCount), 0);
            runParallelChunks(chunkCount, rowCount, [&](int, int begin, int end) {
                for (int i = begin; i < end; ++i) {
                    ++visits[static_cast<std::size_t>(i)];
                }
            });
            for (const int count : visits) {
                CHECK(count == 1);
            }
        }
    }
}

TEST_CASE("mergeBins sums privatized buffers exactly")
{
    constexpr std::size_t BinCount = 5;
    constexpr int ChunkCount = 3;
    std::vector<uint32_t> threadBins(BinCount * ChunkCount);
    for (int chunk = 0; chunk < ChunkCount; ++chunk) {
        for (std::size_t bin = 0; bin < BinCount; ++bin) {
            threadBins[static_cast<std::size_t>(chunk) * BinCount + bin] =
                static_cast<uint32_t>(static_cast<std::size_t>(chunk) * 10 + bin);
        }
    }

    std::vector<uint32_t> out(BinCount, 999u);
    mergeBins(threadBins.data(), BinCount, ChunkCount, out.data());

    // Sum over chunks of (chunk * 10 + bin) = (0 + 10 + 20) + 3 * bin.
    for (std::size_t bin = 0; bin < BinCount; ++bin) {
        CHECK(out[bin] == static_cast<uint32_t>(30 + 3 * bin));
    }
}

TEST_CASE("mergeBins over a large grid matches a serial reduction")
{
    // The waveform's grids are big enough that mergeBins splits the reduction
    // across threads; the result must equal a plain serial sum bit for bit.
    constexpr std::size_t BinCount = 500'000;
    constexpr int ChunkCount = 4;
    std::vector<uint32_t> threadBins(BinCount * ChunkCount);
    for (int chunk = 0; chunk < ChunkCount; ++chunk) {
        for (std::size_t bin = 0; bin < BinCount; ++bin) {
            threadBins[static_cast<std::size_t>(chunk) * BinCount + bin] =
                static_cast<uint32_t>((bin * 7 + static_cast<std::size_t>(chunk)) & 0xFFFFu);
        }
    }

    std::vector<uint32_t> out(BinCount, 0u);
    mergeBins(threadBins.data(), BinCount, ChunkCount, out.data());

    bool matches = true;
    for (std::size_t bin = 0; bin < BinCount; ++bin) {
        uint32_t expected = 0;
        for (int chunk = 0; chunk < ChunkCount; ++chunk) {
            expected += threadBins[static_cast<std::size_t>(chunk) * BinCount + bin];
        }
        matches = matches && out[bin] == expected;
    }
    CHECK(matches);
}

}  // namespace sidescopes
