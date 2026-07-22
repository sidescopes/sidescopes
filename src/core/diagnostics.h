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
    Attach,       ///< Window-attach focus routing: one line per follow tick.
    Border,       ///< Region-border presentation: hide/show/present events.
    Suggestions,  ///< Region-picker suggestions and the pick's rectangle chain.
    FacePin,      ///< Face-pin probe verdicts, for grading the gates.
    Perf,         ///< Frame, analysis-pass, and capture-cadence timings.
    Count         ///< Sentinel; keep last.
};

/// How often logged lines are pushed to disk. The default flushes on a
/// bounded interval: live enough to tail, at most one write syscall per
/// interval on the logging thread, and a crash loses no more than the
/// interval - though a quiet channel's last line waits for the next
/// event or the close. EveryLine (SIDESCOPES_DIAG_FLUSH=1) is
/// crash-hunting mode; OnClose (SIDESCOPES_DIAG_FLUSH=0) is measurement
/// mode, where any flush in a hot loop would distort what is measured.
enum class DiagFlush
{
    EveryLine,
    Interval,
    OnClose
};

/// How the sink is set up. Production reads it from the environment on first
/// use; tests inject one directly through diagConfigure.
struct DiagConfig
{
    /// Comma list of channel names, or "all"; empty disables everything.
    std::string channels;
    /// The log file path; empty picks the default in the OS temp directory.
    std::string filePath;
    /// When lines reach the disk; the bounded interval by default.
    DiagFlush flush = DiagFlush::Interval;
};

/// @return Whether @p channel is enabled this run. The first call reads
///         SIDESCOPES_DIAG and SIDESCOPES_DIAG_FILE and opens the sink;
///         every later call is a bool lookup.
[[nodiscard]] bool diagEnabled(DiagChannel channel);

/// Forces the sink open now instead of at the first logged line, so an
/// enabled run shows its log (and its header) from launch and t=0 means
/// application start. Harmless when diagnostics are off.
void diagInit();

/// Reconfigures the sink from @p config, closing any open log. The menu's
/// recording toggle and the tests drive it; the environment configures
/// only the initial state.
void diagConfigure(const DiagConfig& config);

/// @return Whether the sink is open and lines are being recorded - the
///         truth behind the menu checkbox, whichever way recording was
///         switched on.
[[nodiscard]] bool diagRecording();

/// @return The log file path recording writes to, or would write to: the
///         configured path while the sink is open, the environment's or
///         the default location otherwise.
[[nodiscard]] std::string diagLogPath();

/// @return The directory for this application's diagnostic files: a
///         sidescopes folder inside the OS temp directory, created on
///         first use so "show the log" always has a folder to open.
[[nodiscard]] std::string diagDirectory();

/// Writes one finished line to the sink: "t=<seconds> <channel>
/// <message>", where t counts from the sink's initialization on the
/// steady clock - the single timeline every channel shares. A no-op when
/// the sink is closed or the channel is off. Prefer the SS_DIAG macro,
/// which formats printf-style and skips all evaluation when the channel
/// is off.
void diagEmit(DiagChannel channel, const char* message);

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
/// SS_DIAG(Border, "hide visible=%d", visible). The format string meets
/// snprintf right here in the expansion, so every compiler checks it
/// against its arguments; neither the arguments nor the formatting cost
/// anything when the channel is off, so call sites may include syscalls.
/// A literal percent in the message needs %% like any format string.
#define SS_DIAG(channel, ...)                                                      \
    do {                                                                           \
        if (sidescopes::diagEnabled(sidescopes::DiagChannel::channel)) {           \
            char ssDiagMessage[1024];                                              \
            (void)std::snprintf(ssDiagMessage, sizeof ssDiagMessage, __VA_ARGS__); \
            sidescopes::diagEmit(sidescopes::DiagChannel::channel, ssDiagMessage); \
        }                                                                          \
    } while (false)

/// Times the rest of the enclosing scope on a channel by bare name:
/// SS_DIAG_SPAN(Attach, "observe") logs "observe_ms=..." at scope exit.
#define SS_DIAG_SPAN_CONCAT2(a, b) a##b
#define SS_DIAG_SPAN_CONCAT(a, b) SS_DIAG_SPAN_CONCAT2(a, b)
#define SS_DIAG_SPAN(channel, name) \
    const sidescopes::DiagSpan SS_DIAG_SPAN_CONCAT(ssDiagSpan, __LINE__)(sidescopes::DiagChannel::channel, name)
