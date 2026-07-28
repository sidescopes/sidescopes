#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "sidescopes/module.h"

namespace sidescopes {

/// @brief State one module's scopes share with each other, one set per host.
///
/// Some scopes of one module compute the same thing: the waveform and the RGB
/// parade scatter a region into bins that agree bin for bin, because they are
/// one engine over one region at one geometry. Sharing that work needs somewhere
/// for the result to live that is neither an instance - each would keep its own -
/// nor the module itself.
///
/// PER HOST, not per module. A module is a translation unit and outlives every
/// host that loads it, so state hung off the module would be shared by two hosts
/// in one process that have nothing to do with each other. This is keyed by the
/// host that created the instances, so each gets its own and no host can see
/// another's.
///
/// RELEASED WITH THE LAST INSTANCE. The state is held by shared_ptr and the
/// table holds only weak references, so the bins go when the last scope holding
/// them does. That is load-bearing rather than tidy: a shared accumulation is
/// the largest block a scope owns, and the application drops every instance when
/// there is no region, so an empty selection must go on holding no engine memory.
///
/// THREADING. Instances accumulate on the analysis thread, one at a time, which
/// is what makes the shared state itself safe to use without a lock. Only the
/// table is guarded, and only while a state is found or created - once per
/// instance, never per pass.
///
/// ONE TABLE PER STATE TYPE, which is what keeps modules out of each other's
/// way: the statics belong to this template's instantiation, so a module asking
/// for its own state type can never reach another's. Two modules cannot share
/// anyway - bins of different shapes measuring different things - so that is the
/// boundary rather than a limitation.
template <typename State>
[[nodiscard]] std::shared_ptr<State> sharedStateFor(const SsHost* host)
{
    if (host == nullptr) {
        // A module created without a host - a test, a benchmark - shares with
        // nobody and keeps whatever it would have kept on its own.
        return nullptr;
    }

    static std::mutex mutex;
    static std::vector<std::pair<const SsHost*, std::weak_ptr<State>>> byHost;
    const std::lock_guard<std::mutex> lock(mutex);

    std::shared_ptr<State> found;
    std::size_t live = 0;
    for (auto& entry : byHost) {
        std::shared_ptr<State> state = entry.second.lock();
        if (!state) {
            // A host whose scopes have all gone leaves an expired reference;
            // dropping them here is what keeps the table the size of the hosts
            // that really exist.
            continue;
        }
        if (entry.first == host) {
            found = state;
        }
        byHost[live++] = std::pair{entry.first, entry.second};
    }
    byHost.resize(live);
    if (found) {
        return found;
    }

    found = std::make_shared<State>();
    byHost.emplace_back(host, found);

    return found;
}

}  // namespace sidescopes
