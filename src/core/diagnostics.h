#pragma once

#include <chrono>
#include <cstdio>
#include <string>

namespace sidescopes {

/// A diagnostic channel: one subsystem's stream of trace lines. Channels are
/// enabled per run with the SIDESCOPES_DIAG environment variable - a comma
/// list of lowercase channel names ("attach,border") or "all"; unset means
/// every channel is off and logging costs one branch per site.
enum class DiagChannel
{
    Attach,  ///< Window-attach focus routing: one line per follow tick.
    Border,  ///< Region-border presentation: hide/show/present events.
    Count    ///< Sentinel; keep last.
};

/// How the sink is set up. Production reads it from the environment on first
/// use; tests inject one directly through diagConfigure.
struct DiagConfig
{
    /// Comma list of channel names, or "all"; empty disables everything.
    std::string channels;
    /// The log file path; empty picks the default in the OS temp directory.
    std::string filePath;
    /// Flush after every line - crash-safe and live-tailable, the default.
    /// SIDESCOPES_DIAG_FLUSH=0 turns it off for measurement runs, where a
    /// flush inside a hot loop would distort what is being measured.
    bool flushEachLine = true;
};

/// @return Whether @p channel is enabled this run. The first call reads
///         SIDESCOPES_DIAG and SIDESCOPES_DIAG_FILE and opens the sink;
///         every later call is a bool lookup.
[[nodiscard]] bool diagEnabled(DiagChannel channel);

/// Forces the sink open now instead of at the first logged line, so an
/// enabled run shows its log (and its header) from launch and t=0 means
/// application start. Harmless when diagnostics are off.
void diagInit();

/// Reconfigures the sink from @p config, closing any open log. Test seam;
/// production code never calls it.
void diagConfigure(const DiagConfig& config);

/// Writes one finished line to the sink: "t=<seconds> <channel>
/// <message>", where t counts from the sink's initialization on the
/// steady clock - the single timeline every channel shares. A no-op when
/// the sink is closed or the channel is off.
void diagEmit(DiagChannel channel, const char* message);

/// Formats printf-style and emits the line on @p channel. Prefer the
/// SS_DIAG macro, which skips argument evaluation when the channel is
/// off.
template <typename... Args>
void diagLogf(DiagChannel channel, const char* format, Args... args)
{
    if constexpr (sizeof...(Args) == 0) {
        diagEmit(channel, format);
    } else {
        char message[1024];
        (void)std::snprintf(message, sizeof(message), format, args...);
        diagEmit(channel, message);
    }
}

/// Measures a scope's wall time and logs it as one "<name>_ms=<elapsed>"
/// line when the scope closes - the aggregatable shape performance questions
/// want. Costs two clock reads when its channel is enabled and only a branch
/// otherwise. Prefer the SS_DIAG_SPAN macro.
class DiagSpan
{
public:
    /// Starts timing @p name against @p channel. @p name must outlive the
    /// span; a string literal is the intended use.
    DiagSpan(DiagChannel channel, const char* name);
    ~DiagSpan();

    DiagSpan(const DiagSpan&) = delete;
    DiagSpan& operator=(const DiagSpan&) = delete;

private:
    DiagChannel m_channel;
    const char* m_name;
    std::chrono::steady_clock::time_point m_begin;
    bool m_armed;
};

}  // namespace sidescopes

/// Logs printf-style to a diagnostic channel by bare name:
/// SS_DIAG(Border, "hide visible=%d", visible). Arguments are not evaluated
/// unless the channel is enabled, so call sites may include syscalls.
#define SS_DIAG(channel, ...)                                                    \
    do {                                                                         \
        if (sidescopes::diagEnabled(sidescopes::DiagChannel::channel)) {         \
            sidescopes::diagLogf(sidescopes::DiagChannel::channel, __VA_ARGS__); \
        }                                                                        \
    } while (false)

/// Times the rest of the enclosing scope on a channel by bare name:
/// SS_DIAG_SPAN(Attach, "observe") logs "observe_ms=..." at scope exit.
#define SS_DIAG_SPAN_CONCAT2(a, b) a##b
#define SS_DIAG_SPAN_CONCAT(a, b) SS_DIAG_SPAN_CONCAT2(a, b)
#define SS_DIAG_SPAN(channel, name) \
    const sidescopes::DiagSpan SS_DIAG_SPAN_CONCAT(ssDiagSpan, __LINE__)(sidescopes::DiagChannel::channel, name)
