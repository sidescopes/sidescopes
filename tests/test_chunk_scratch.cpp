// The room a split accumulate gives each chunk, and what it must not change.
//
// The point of lending it is memory: the per-chunk bin sets are several times
// the size of the bins they merge into, and a stack of scopes used to hold one
// set each. The point of these tests is that lending changes the memory and
// nothing else - the merge is integer addition over disjoint chunks, so a
// borrowed buffer is arithmetically the same buffer.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <new>
#include <thread>
#include <vector>

#include "core/page_allocator.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/histogram.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"
#include "modules/module_registry.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

/// A lender that records what it was asked for, so a test can tell a pass
/// that borrowed from one that quietly did not.
struct RecordingArena
{
    std::vector<uint32_t> room;
    int borrows = 0;
    std::size_t largest = 0;
    bool decline = false;

    static uint32_t* lend(const void* context, std::size_t count)
    {
        // The lender is handed its own arena as the opaque context; the
        // constness is the ABI's, and the arena is this test's own.
        auto* self = const_cast<RecordingArena*>(static_cast<const RecordingArena*>(context));
        ++self->borrows;
        self->largest = std::max(self->largest, count);
        if (self->decline) {
            return nullptr;
        }
        if (self->room.size() < count) {
            self->room.resize(count);
        }

        return self->room.data();
    }
};

/// Detailed enough that every engine splits its pass across chunks - a region
/// under a chunk's worth of rows runs inline and borrows nothing, which would
/// make the comparison below vacuous.
TestFrame busyFrame()
{
    TestFrame frame(512, 512, 0);
    for (int row = 0; row < 512; ++row) {
        for (int column = 0; column < 512; ++column) {
            frame.setColor(column, row,
                           Color{static_cast<uint8_t>(column % 256), static_cast<uint8_t>(row % 256),
                                 static_cast<uint8_t>((column + row) % 256)});
        }
    }

    return frame;
}

/// Runs @p engine over the whole frame twice - once on room of its own, once
/// on a lent arena - and returns whether the two images are byte-identical.
/// @p arena reports whether the second pass really borrowed.
template <typename Engine>
bool sameEitherWay(const TestFrame& frame, RecordingArena& arena)
{
    const IntRect whole{0, 0, frame.width, frame.height};

    Engine own;
    own.accumulate(frame.view(), whole);
    const ScopeImage expected = own.image();

    Engine lent;
    lent.lendScratch(&RecordingArena::lend, &arena);
    lent.accumulate(frame.view(), whole);

    const ScopeImage& actual = lent.image();

    return actual.width == expected.width && actual.height == expected.height && actual.rgba == expected.rgba;
}

}  // namespace

TEST_CASE("A lent arena leaves every scope's image byte for byte")
{
    const TestFrame frame = busyFrame();

    RecordingArena waveform;
    CHECK(sameEitherWay<Waveform>(frame, waveform));
    CHECK(waveform.borrows > 0);

    RecordingArena histogram;
    CHECK(sameEitherWay<Histogram>(frame, histogram));
    CHECK(histogram.borrows > 0);

    RecordingArena vectorscope;
    CHECK(sameEitherWay<Vectorscope>(frame, vectorscope));
    CHECK(vectorscope.borrows > 0);
}

TEST_CASE("A lender that declines is answered from the engine's own room")
{
    // The host declines when it cannot grow the arena. That must cost the
    // picture nothing - it is the memory the lending was for, not the result.
    const TestFrame frame = busyFrame();
    RecordingArena refusing;
    refusing.decline = true;

    CHECK(sameEitherWay<Waveform>(frame, refusing));
    CHECK(refusing.borrows > 0);
    CHECK(refusing.room.empty());
}

TEST_CASE("A lender that declined once is asked again")
{
    // Declining is how the host reports that it could not grow the arena this
    // time, not that it has withdrawn it: a pass that falls back to its own
    // room must not leave the engine on its own for good.
    ChunkScratch scratch;
    RecordingArena arena;
    arena.decline = true;
    scratch.lendFrom(&RecordingArena::lend, &arena);

    uint32_t* mine = scratch.borrow(1024);
    REQUIRE(mine != nullptr);
    CHECK(mine != arena.room.data());

    // Each borrow is taken before the arena is looked at, never inside the
    // same comparison: borrowing grows the arena, and the two sides of an
    // equality are not sequenced against each other.
    arena.decline = false;
    uint32_t* lent = scratch.borrow(1024);
    CHECK(lent == arena.room.data());
    uint32_t* larger = scratch.borrow(2048);
    CHECK(larger == arena.room.data());
    CHECK(arena.borrows == 3);
}

TEST_CASE("An engine told nothing keeps room of its own")
{
    // Which is what a test, a benchmark, and any host without the extension
    // all do; nothing about a pass may depend on the lending having happened.
    ChunkScratch scratch;
    uint32_t* first = scratch.borrow(16);
    REQUIRE(first != nullptr);
    first[15] = 3;

    // A null lender puts it back on its own after having been lent to.
    RecordingArena arena;
    scratch.lendFrom(&RecordingArena::lend, &arena);
    uint32_t* lent = scratch.borrow(16);
    CHECK(lent == arena.room.data());
    scratch.lendFrom(nullptr, nullptr);
    uint32_t* again = scratch.borrow(16);
    CHECK(again != arena.room.data());
    CHECK(arena.borrows == 1);
}

TEST_CASE("The host lends one arena, not one per scope")
{
    // The saving is exactly this: two scopes asking in turn are handed the
    // same room, because they never accumulate at the same time.
    const SsHost& host = builtinModules().host();
    REQUIRE(host.get_extension != nullptr);

    const auto* scratch = static_cast<const SsHostScratch*>(host.get_extension(&host, SS_EXT_HOST_SCRATCH));
    REQUIRE(scratch != nullptr);
    REQUIRE(scratch->borrow != nullptr);

    uint32_t* first = scratch->borrow(&host, 4096);
    REQUIRE(first != nullptr);
    CHECK(scratch->borrow(&host, 4096) == first);

    // Growing it keeps it one arena; shrinking never gives room back, so the
    // next scope in the stack is served from what the largest one needed.
    uint32_t* grown = scratch->borrow(&host, 1u << 20);
    REQUIRE(grown != nullptr);
    CHECK(scratch->borrow(&host, 16) == grown);
}

TEST_CASE("The host declines scratch requests too large to represent")
{
    const SsHost& host = builtinModules().host();
    const auto* scratch = static_cast<const SsHostScratch*>(host.get_extension(&host, SS_EXT_HOST_SCRATCH));
    REQUIRE(scratch != nullptr);
    CHECK(scratch->borrow(&host, std::numeric_limits<uint64_t>::max()) == nullptr);
    CHECK(scratch->borrow(&host, 16) != nullptr);
}

TEST_CASE("Every scope the host creates borrows the host's arena")
{
    // The lending is per-module wiring, so it can be dropped from one scope
    // and leave the other three still saving. Each one is asked on a thread of
    // its own, because the arena belongs to the thread that borrows: a thread
    // that has never accumulated has no arena at all, and that is what makes
    // "this scope borrowed" something a test can see.
    const SsHost& host = builtinModules().host();
    const auto* scratch = static_cast<const SsHostScratch*>(host.get_extension(&host, SS_EXT_HOST_SCRATCH));
    REQUIRE(scratch != nullptr);

    const TestFrame frame = busyFrame();
    for (const char* id : {"org.sidescopes.waveform", "org.sidescopes.parade", "org.sidescopes.histogram",
                           "org.sidescopes.vectorscope"}) {
        INFO(id);
        bool pristine = false;
        bool borrowed = false;
        std::thread probe([&] {
            pristine = scratch->borrow(&host, 0) == nullptr;
            const ScopeInstance instance = builtinModules().createInstance(id);
            if (instance.valid()) {
                const SsFrameView view{frame.pixels.data(),  frame.width * 4,     frame.width,
                                       frame.height,         SS_COLOR_SPACE_SRGB, 1,
                                       SS_PIXEL_FORMAT_BGRA8};
                (void)instance.accumulate(view, SsRect{0, 0, frame.width, frame.height});
            }
            borrowed = scratch->borrow(&host, 0) != nullptr;
        });
        probe.join();
        CHECK(pristine);
        CHECK(borrowed);
    }
}

TEST_CASE("An unknown host extension is refused")
{
    const SsHost& host = builtinModules().host();
    CHECK(host.get_extension(&host, "sidescopes.nothing/1") == nullptr);
}

TEST_CASE("Page allocation rejects a byte-count overflow before allocating")
{
    PageAllocator<uint64_t> allocator;
    CHECK_THROWS_AS(allocator.allocate(std::numeric_limits<std::size_t>::max()), std::bad_array_new_length);
}

}  // namespace sidescopes
