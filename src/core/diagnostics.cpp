#include "core/diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

#include "core/environment.h"

#ifdef _WIN32
#include <process.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace sidescopes {

// A snapshot keeps this gate alive, not the subsystem the function captures.
// Disarming takes the same gate as invocation, so it also joins a running
// report before the captured state can be destroyed.
struct DiagReportState
{
    explicit DiagReportState(DiagStateReport callback)
        : report(std::move(callback))
    {
    }

    void invoke()
    {
        const std::lock_guard lock(mutex);
        if (report) {
            report();
        }
    }

    void disarm()
    {
        const std::lock_guard lock(mutex);
        report = {};
    }

    std::mutex mutex;
    DiagStateReport report;
};

namespace {

// The bounded-interval flush cadence: short enough that a live tail and a
// crash both see everything but the last beat, long enough that recording
// costs the frame loop at most ten write syscalls a second.
constexpr double FlushIntervalSeconds = 0.1;

// Indexed by DiagChannel; the env-list parser and the line prefix share it.
constexpr const char* ChannelNames[] = {"attach", "border", "suggestions", "facelock", "perf", "modules", "interface"};
static_assert(std::size(ChannelNames) == static_cast<std::size_t>(DiagChannel::Count));

// The secure-CRT deprecations make std::fopen a hard error under MSVC's
// warnings-as-errors, so file access goes through the annex Microsoft
// accepts. Environment reads go through core/environment.h for the same
// reason.
std::filesystem::path utf8Path(const std::string& path)
{
    return std::u8string(path.begin(), path.end());
}

std::FILE* openFile(const std::filesystem::path& path)
{
#ifdef _WIN32
    // Not fopen_s: that denies all sharing, and a live tail of the log
    // while the application runs is the point of the sink.
    return _wfsopen(path.c_str(), L"w", _SH_DENYWR);
#else
    return std::fopen(path.c_str(), "w");
#endif
}

struct DiagState
{
    std::FILE* sink = nullptr;
    bool channels[static_cast<std::size_t>(DiagChannel::Count)] = {};
    DiagFlush flush = DiagFlush::Interval;
    double lastFlushSeconds = 0.0;
    std::chrono::steady_clock::time_point start;
    std::string path;
    /// Whether this opening of the sink has had its state reported. Reset with
    /// the rest of the state, so every new recording is told afresh.
    bool reported = false;
};

// The registry does not retain dead registrations; snapshots retain only
// their invocation gates and must still pass those gates before reporting.
struct StateReports
{
    std::mutex mutex;
    std::vector<std::weak_ptr<DiagReportState>> reports;
};

StateReports& stateReports()
{
    static StateReports reports;

    return reports;
}

DiagFlush flushFromEnv(const std::string& value)
{
    if (value == "0") {
        return DiagFlush::OnClose;
    }

    return value.empty() ? DiagFlush::Interval : DiagFlush::EveryLine;
}

/// Enables every channel named in the comma list ("all" names them all),
/// ignoring unknown tokens and surrounding spaces.
void parseChannels(const std::string& list, DiagState& state)
{
    std::size_t begin = 0;
    while (begin <= list.size()) {
        std::size_t end = list.find(',', begin);
        if (end == std::string::npos) {
            end = list.size();
        }
        std::size_t first = begin;
        std::size_t last = end;
        while (first < last && list[first] == ' ') {
            ++first;
        }
        while (last > first && list[last - 1] == ' ') {
            --last;
        }
        const std::string token = list.substr(first, last - first);
        for (std::size_t index = 0; index < std::size(ChannelNames); ++index) {
            if (token == "all" || token == ChannelNames[index]) {
                state.channels[index] = true;
            }
        }
        begin = end + 1;
    }
}

std::string defaultLogPath()
{
    return diagDirectory() + "/sidescopes-diag.log";
}

// The previous run's name keeps the extension - sidescopes-diag.prev.log -
// so the kept log opens like any other.
std::string previousLogPath(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return path + ".prev";
    }

    return path.substr(0, dot) + ".prev" + path.substr(dot);
}

// The run header: identifies the process and shows what the environment
// resolved to, so a mistyped channel list is visible in the file instead
// of silently logging nothing.
void writeHeader(std::FILE* sink, const std::string& requested, const DiagState& state)
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
    const int pid = _getpid();
#else
    localtime_r(&now, &local);
    const int pid = getpid();
#endif
    char started[32] = "unknown";
    (void)std::strftime(started, sizeof started, "%Y-%m-%dT%H:%M:%S", &local);
    std::string enabled;
    for (std::size_t index = 0; index < std::size(ChannelNames); ++index) {
        if (state.channels[index]) {
            enabled += enabled.empty() ? "" : ",";
            enabled += ChannelNames[index];
        }
    }
    std::fprintf(sink, "# sidescopes diagnostics pid=%d started=%s requested=\"%s\" channels=%s\n", pid, started,
                 requested.c_str(), enabled.empty() ? "(none)" : enabled.c_str());
    std::fflush(sink);
}

/// Closes any open sink and rebuilds the state from @p config: parses the
/// channel list, rotates the previous log to .prev, opens the new one, and
/// stamps the shared clock's zero.
void applyConfig(DiagState& state, const DiagConfig& config)
{
    if (state.sink) {
        std::fclose(state.sink);
    }
    state = DiagState{};
    if (config.channels.empty()) {
        return;
    }
    parseChannels(config.channels, state);
    const std::string path = config.filePath.empty() ? defaultLogPath() : config.filePath;
    // A custom path may name a directory that does not exist yet, like
    // the default location's own subfolder.
    std::error_code ignored;
    const std::filesystem::path file = utf8Path(path);
    std::filesystem::create_directories(file.parent_path(), ignored);
    const std::filesystem::path previous = utf8Path(previousLogPath(path));
    std::filesystem::remove(previous, ignored);
    std::filesystem::rename(file, previous, ignored);
    state.sink = openFile(file);
    if (!state.sink) {
        state = DiagState{};

        return;
    }
    state.flush = config.flush;
    state.start = std::chrono::steady_clock::now();
    state.path = path;
    writeHeader(state.sink, config.channels, state);
    if (std::none_of(std::begin(state.channels), std::end(state.channels), [](bool on) { return on; })) {
        // A list of only unknown names: the header stays behind as the
        // diagnosis, but no line can ever follow it, so the sink closes
        // and the recording state reports off instead of lying.
        std::fclose(state.sink);
        state.sink = nullptr;
    }
}

struct Diagnostics
{
    Diagnostics()
    {
        configure({environmentValue("SIDESCOPES_DIAG"), environmentValue("SIDESCOPES_DIAG_FILE"),
                   flushFromEnv(environmentValue("SIDESCOPES_DIAG_FLUSH"))});
    }

    ~Diagnostics()
    {
        if (state.sink) {
            std::fclose(state.sink);
        }
    }

    // Called under sinkMutex once construction has published this object.
    void configure(const DiagConfig& config)
    {
        enabled.store(0, std::memory_order_relaxed);
        applyConfig(state, config);
        std::uint32_t mask = 0;
        if (state.sink) {
            for (std::size_t index = 0; index < std::size(ChannelNames); ++index) {
                if (state.channels[index]) {
                    mask |= std::uint32_t{1} << index;
                }
            }
        }
        enabled.store(mask, std::memory_order_relaxed);
    }

    // Only configure/init take controlMutex. Emitters and report teardown
    // never need it, so callbacks may take subsystem locks and emit lines.
    std::mutex controlMutex;
    std::mutex sinkMutex;
    std::atomic<std::uint32_t> enabled{0};
    DiagState state;
};

Diagnostics& diagnostics()
{
    static Diagnostics instance;
    return instance;
}

/// Runs every registered state report against a freshly opened sink, once per
/// opening, so a recording starts with what is already true rather than only
/// with what changes next.
void reportState(Diagnostics& service)
{
    // The caller serializes recording changes with controlMutex.
    std::unique_lock sinkLock(service.sinkMutex);
    DiagState& state = service.state;
    if (!state.sink || state.reported) {
        return;
    }
    // Marked before the reports run: a report logs, and logging must not lead
    // back in here.
    state.reported = true;
    std::vector<std::shared_ptr<DiagReportState>> live;
    {
        StateReports& registered = stateReports();
        const std::lock_guard lock(registered.mutex);
        // The subsystems that report tend to outlive the process, so this
        // prunes little and stays short; doing it here keeps the registration
        // itself free of any bookkeeping.
        std::erase_if(registered.reports,
                      [](const std::weak_ptr<DiagReportState>& report) { return report.expired(); });
        for (const std::weak_ptr<DiagReportState>& report : registered.reports) {
            if (std::shared_ptr<DiagReportState> held = report.lock()) {
                live.push_back(std::move(held));
            }
        }
    }
    if (live.empty()) {
        return;
    }
    // Named, because these lines describe a past this recording did not see:
    // the timestamps are honest about when they were written, not about when
    // the state they carry came about.
    std::fprintf(state.sink, "# state when this recording opened\n");
    // A producer can hold a subsystem lock while emitting. Never retain the
    // sink lock while a report acquires that subsystem lock.
    sinkLock.unlock();
    for (const std::shared_ptr<DiagReportState>& report : live) {
        try {
            report->invoke();
        } catch (...) {  // NOLINT(bugprone-empty-catch): reporting must not disrupt recording
        }
    }
}

}  // namespace

bool diagEnabled(DiagChannel channel)
{
    const auto index = static_cast<std::size_t>(channel);
    return index < std::size(ChannelNames) &&
           (diagnostics().enabled.load(std::memory_order_relaxed) & (std::uint32_t{1} << index)) != 0;
}

void diagInit()
{
    Diagnostics& service = diagnostics();
    const std::lock_guard controlLock(service.controlMutex);
    // A run enabled from the environment opens its sink at whichever line
    // logs first, which can be before the application reaches here. The
    // reporting waits for this call regardless, so the subsystems built during
    // startup are all registered by the time it runs.
    reportState(service);
}

void diagConfigure(const DiagConfig& config)
{
    Diagnostics& service = diagnostics();
    const std::lock_guard controlLock(service.controlMutex);
    {
        const std::lock_guard sinkLock(service.sinkMutex);
        service.configure(config);
    }
    reportState(service);
}

DiagRegistration::~DiagRegistration()
{
    if (m_report) {
        m_report->disarm();
    }
}

DiagRegistration& DiagRegistration::operator=(DiagRegistration&& other) noexcept
{
    if (this != &other) {
        if (m_report) {
            m_report->disarm();
        }
        m_report = std::move(other.m_report);
    }
    return *this;
}

DiagRegistration diagAddStateReport(DiagStateReport report)
{
    auto held = std::make_shared<DiagReportState>(std::move(report));
    StateReports& registered = stateReports();
    const std::lock_guard lock(registered.mutex);
    registered.reports.push_back(held);

    return DiagRegistration(std::move(held));
}

bool diagRecording()
{
    Diagnostics& service = diagnostics();
    const std::lock_guard lock(service.sinkMutex);
    return service.state.sink != nullptr;
}

std::string diagLogPath()
{
    Diagnostics& service = diagnostics();
    const std::lock_guard lock(service.sinkMutex);
    const DiagState& state = service.state;
    if (!state.path.empty()) {
        return state.path;
    }
    const std::string configured = environmentValue("SIDESCOPES_DIAG_FILE");

    return configured.empty() ? defaultLogPath() : configured;
}

std::string diagDirectory()
{
    std::string base = environmentValue("TEMP");
    if (base.empty()) {
        base = environmentValue("TMPDIR");
    }
    if (base.empty()) {
        base = "/tmp";
    }
    const std::string directory = base + "/sidescopes";
    std::error_code ignored;
    std::filesystem::create_directories(utf8Path(directory), ignored);

    return directory;
}

void diagEmit(DiagChannel channel, const char* message) noexcept
{
    // Diagnostics are fire-and-forget: the only throwing surface is the sink's
    // one-time setup, which always runs before any emit, but a logging failure
    // must never disturb the caller regardless - so the whole path is contained.
    try {
        if (!diagEnabled(channel)) {
            return;
        }
        Diagnostics& service = diagnostics();
        const std::lock_guard lock(service.sinkMutex);
        DiagState& state = service.state;
        if (!state.sink || !state.channels[static_cast<std::size_t>(channel)]) {
            return;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
        std::fprintf(state.sink, "t=%.6f %s %s\n", seconds, ChannelNames[static_cast<std::size_t>(channel)], message);
        if (state.flush == DiagFlush::EveryLine ||
            (state.flush == DiagFlush::Interval && seconds - state.lastFlushSeconds >= FlushIntervalSeconds)) {
            std::fflush(state.sink);
            state.lastFlushSeconds = seconds;
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): a diagnostic must never propagate a failure
    }
}

DiagSpan::DiagSpan(DiagChannel channel, const char* name)
    : m_channel(channel),
      m_name(name),
      m_armed(diagEnabled(channel))
{
    if (m_armed) {
        m_begin = std::chrono::steady_clock::now();
    }
}

DiagSpan::~DiagSpan()
{
    if (!m_armed) {
        return;
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_begin).count();
    char message[128];
    (void)std::snprintf(message, sizeof message, "%s_ms=%.3f", m_name, elapsed);
    diagEmit(m_channel, message);
}

}  // namespace sidescopes
