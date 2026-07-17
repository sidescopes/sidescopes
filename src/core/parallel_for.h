#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace sidescopes {

/// Upper bound on the worker threads a single accumulate or compose pass may
/// use, the calling thread included. The analysis worker already runs off the
/// UI thread, so this caps the nested fan-out below a typical core count.
inline constexpr int MaxParallelChunks = 8;

/// The number of contiguous chunks to split @p rowCount rows across. Bounded
/// by the hardware thread count and MaxParallelChunks, and forced to one when
/// fewer than @p minRowsPerChunk rows would fall to each chunk, so small
/// regions never pay the threading cost.
[[nodiscard]] inline int parallelChunkCount(int rowCount, int minRowsPerChunk)
{
    if (rowCount <= 0 || minRowsPerChunk <= 0) {
        return 1;
    }
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        hardware = 1;
    }
    const int byHardware = std::min(static_cast<int>(hardware), MaxParallelChunks);
    const int byWork = rowCount / minRowsPerChunk;

    return std::max(1, std::min(byHardware, byWork));
}

/// Splits [0, @p rowCount) into @p chunkCount contiguous half-open ranges and
/// runs @p body(chunkIndex, rowBegin, rowEnd) for each, one per thread, joining
/// before returning. With @p chunkCount <= 1 the body runs inline on the
/// caller, spawning nothing. Each invocation must confine its writes to state
/// private to its chunk index or to output rows in its own range; nothing
/// between chunks is synchronized, so this is data-race free only under that
/// discipline.
template <typename Body>
void runParallelChunks(int chunkCount, int rowCount, const Body& body)
{
    if (chunkCount <= 1 || rowCount <= 0) {
        body(0, 0, rowCount);
        return;
    }

    const int base = rowCount / chunkCount;
    const int remainder = rowCount % chunkCount;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(chunkCount) - 1);
    int begin = 0;
    for (int chunk = 0; chunk < chunkCount; ++chunk) {
        const int end = begin + base + (chunk < remainder ? 1 : 0);
        if (chunk + 1 < chunkCount) {
            threads.emplace_back(body, chunk, begin, end);
        } else {
            // The caller runs the last chunk itself rather than idling.
            body(chunk, begin, end);
        }
        begin = end;
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

/// Sums @p chunkCount privatized bin buffers - each @p binCount uint32 values,
/// laid out back to back in @p threadBins - element-wise into @p out. Bit-exact
/// regardless of chunk count because integer addition is associative and
/// commutative; the reduction runs across threads over disjoint output ranges.
/// @p out must not alias @p threadBins.
inline void mergeBins(const uint32_t* threadBins, std::size_t binCount, int chunkCount, uint32_t* out)
{
    if (chunkCount <= 0 || binCount == 0) {
        return;
    }
    // Only the waveform's multi-megabyte grids are worth splitting the merge
    // over; the smaller grids clear in a fraction of a millisecond serially.
    constexpr int MergeRowsPerChunk = 1 << 18;
    const int rows = static_cast<int>(binCount);
    const int mergeChunks = parallelChunkCount(rows, MergeRowsPerChunk);
    runParallelChunks(mergeChunks, rows, [&](int, int begin, int end) {
        for (int i = begin; i < end; ++i) {
            uint32_t sum = threadBins[i];
            for (int chunk = 1; chunk < chunkCount; ++chunk) {
                sum += threadBins[static_cast<std::size_t>(chunk) * binCount + static_cast<std::size_t>(i)];
            }
            out[i] = sum;
        }
    });
}

}  // namespace sidescopes
