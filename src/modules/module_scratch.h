#pragma once

#include <cstddef>
#include <cstdint>

#include "core/scopes/chunk_scratch.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace detail {

/// Adapts the host's borrow to the shape an engine lends from. The context is
/// the host itself, which outlives every instance created from it.
inline std::uint32_t* borrowFromHost(const void* context, std::size_t count)
{
    const auto* host = static_cast<const SsHost*>(context);
    const auto* scratch = static_cast<const SsHostScratch*>(host->get_extension(host, SS_EXT_HOST_SCRATCH));

    return scratch != nullptr && scratch->borrow != nullptr ? scratch->borrow(host, count) : nullptr;
}

}  // namespace detail

/// Points @p engine at the host's shared accumulation arena, so the per-chunk
/// bins of every scope in a stack come from one buffer rather than one each.
///
/// Asked on each pass rather than once at creation: the ABI keeps creation
/// inert, no host function may be called there, and a query costs a pointer
/// comparison against a pass measured in milliseconds. A module created
/// without a host - or one whose host offers no arena - simply keeps the room
/// it always kept.
template <typename Engine>
void lendHostScratch(Engine& engine, const SsHost* host)
{
    if (host == nullptr || host->get_extension == nullptr) {
        return;
    }
    engine.lendScratch(detail::borrowFromHost, host);
}

}  // namespace sidescopes
