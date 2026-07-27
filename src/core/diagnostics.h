#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
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
    FaceLock,     ///< Face-lock probe verdicts, for grading the gates.
    Perf,         ///< Frame, analysis-pass, and capture-cadence timings.
    Modules,      ///< Scope modules: what registered, what was refused, what they log.
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

/// A subsystem's account of what is currently true - the lines it would
/// already have written had a recording been open when its state settled.
/// Every registered report runs when a recording opens, so a log started at
/// any moment begins with the state of the application instead of only the
/// changes that follow it.
///
/// A report may equally re-arm whatever its subsystem dedupes against, which
/// is the other half of the same problem: dedupe state that outlives a
/// recording makes the next one's first line wrong rather than missing.
///
/// A report describes; it must not open, close, or register anything.
using DiagStateReport = std::function<void()>;

/// Keeps a state report registered for as long as it lives and drops it on
/// destruction, so a report never outlives the subsystem it describes.
/// Declare it as the LAST member of that subsystem: members die in reverse
/// order, so the report goes before the state it reads.
class DiagRegistration
{
public:
    DiagRegistration() = default;
    DiagRegistration(DiagRegistration&&) noexcept = default;
    DiagRegistration& operator=(DiagRegistration&&) noexcept = default;
    DiagRegistration(const DiagRegistration&) = delete;
    DiagRegistration& operator=(const DiagRegistration&) = delete;

private:
    friend DiagRegistration diagAddStateReport(DiagStateReport report);

    explicit DiagRegistration(std::shared_ptr<DiagStateReport> report)
        : m_report(std::move(report))
    {
    }

    std::shared_ptr<DiagStateReport> m_report;
};

/// Registers @p report to run whenever a recording opens - which is what lets
/// a subsystem whose state settled before anyone could reach the menu still be
/// heard from. A subsystem registering while a recording is already open has
/// nothing settled to state yet, so nothing runs until the next one opens.
///
/// @return The registration; letting it go removes the report.
[[nodiscard]] DiagRegistration diagAddStateReport(DiagStateReport report);

/// Writes one finished line to the sink: "t=<seconds> <channel>
/// <message>", where t counts from the sink's initialization on the
/// steady clock - the single timeline every channel shares. A no-op when
/// the sink is closed or the channel is off. Prefer the SS_DIAG macro,
/// which formats printf-style and skips all evaluation when the channel
/// is off.
void diagEmit(DiagChannel channel, const char* message) noexcept;

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

/// A value worth logging when it changes and not thirty times a second - the
/// capture format, the crop in force.
///
/// It holds the last value ANNOUNCED rather than the last one seen, so a value
/// that changed while nothing was recording is still stated to the recording
/// that follows; and it forgets on every new recording, so a second recording
/// is told what the first one was told. Both halves are needed. Dedupe state
/// that advances while the channel is off loses the line entirely, and the
/// reader concludes nothing happened; dedupe state that outlives its recording
/// produces a line that is wrong rather than missing, which is worse, because
/// a wrong line is read and believed.
///
/// shouldLog is called from the one thread that owns the value; the forgetting
/// a new recording asks for may come from another.
template <typename Value>
class DiagOnChange
{
public:
    explicit DiagOnChange(DiagChannel channel)
        : m_channel(channel),
          m_registration(diagAddStateReport([this] { m_restate.store(true, std::memory_order_relaxed); }))
    {
    }

    DiagOnChange(const DiagOnChange&) = delete;
    DiagOnChange& operator=(const DiagOnChange&) = delete;

    /// @return Whether @p value must be logged now: its channel is recording,
    ///         and this is not a value the recording has already been told.
    ///         Answering yes is what marks it told, so a caller that asks must
    ///         log.
    [[nodiscard]] bool shouldLog(const Value& value)
    {
        if (!diagEnabled(m_channel)) {
            return false;
        }
        // Always taken, never short-circuited: a restatement left pending
        // would spend itself on some later frame instead of this one.
        const bool restate = m_restate.exchange(false, std::memory_order_relaxed);
        if (!restate && m_told && m_value == value) {
            return false;
        }
        m_value = value;
        m_told = true;

        return true;
    }

private:
    DiagChannel m_channel;
    Value m_value{};
    bool m_told = false;
    std::atomic<bool> m_restate{false};
    DiagRegistration m_registration;
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
