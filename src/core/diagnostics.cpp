#include "core/diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>

#ifdef _WIN32
#include <process.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace sidescopes {

namespace {

// Indexed by DiagChannel; the env-list parser and the line prefix share it.
constexpr const char* ChannelNames[] = {"attach", "border", "suggestions"};
static_assert(std::size(ChannelNames) == static_cast<std::size_t>(DiagChannel::Count));

// The secure-CRT deprecations make std::getenv and std::fopen hard errors
// under MSVC's warnings-as-errors, so environment and file access go
// through the annexes Microsoft accepts.
std::string envValue(const char* name)
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::string();
    }
    std::string result(value);
    std::free(value);

    return result;
#else
    const char* value = std::getenv(name);

    return value ? std::string(value) : std::string();
#endif
}

std::FILE* openFile(const char* path, const char* mode)
{
#ifdef _MSC_VER
    // Not fopen_s: that denies all sharing, and a live tail of the log
    // while the application runs is the point of the sink.
    return _fsopen(path, mode, _SH_DENYWR);
#else
    return std::fopen(path, mode);
#endif
}

struct DiagState
{
    std::FILE* sink = nullptr;
    bool channels[static_cast<std::size_t>(DiagChannel::Count)] = {};
    bool flushEachLine = true;
    std::chrono::steady_clock::time_point start;
    std::string path;
};

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
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
    const std::string previous = previousLogPath(path);
    std::remove(previous.c_str());
    std::rename(path.c_str(), previous.c_str());
    state.sink = openFile(path.c_str(), "w");
    if (!state.sink) {
        state = DiagState{};

        return;
    }
    state.flushEachLine = config.flushEachLine;
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

DiagState& diagState()
{
    static DiagState state = []() {
        DiagState fresh;
        applyConfig(fresh, DiagConfig{envValue("SIDESCOPES_DIAG"), envValue("SIDESCOPES_DIAG_FILE"),
                                      envValue("SIDESCOPES_DIAG_FLUSH") != "0"});

        return fresh;
    }();

    return state;
}

}  // namespace

bool diagEnabled(DiagChannel channel)
{
    return diagState().channels[static_cast<std::size_t>(channel)];
}

void diagInit()
{
    (void)diagState();
}

void diagConfigure(const DiagConfig& config)
{
    applyConfig(diagState(), config);
}

bool diagRecording()
{
    return diagState().sink != nullptr;
}

std::string diagLogPath()
{
    const DiagState& state = diagState();
    if (!state.path.empty()) {
        return state.path;
    }
    const std::string configured = envValue("SIDESCOPES_DIAG_FILE");

    return configured.empty() ? defaultLogPath() : configured;
}

std::string diagDirectory()
{
    std::string base = envValue("TEMP");
    if (base.empty()) {
        base = envValue("TMPDIR");
    }
    if (base.empty()) {
        base = "/tmp";
    }
    const std::string directory = base + "/sidescopes";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);

    return directory;
}

void diagEmit(DiagChannel channel, const char* message)
{
    DiagState& state = diagState();
    if (!state.sink || !state.channels[static_cast<std::size_t>(channel)]) {
        return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
    std::fprintf(state.sink, "t=%.6f %s %s\n", seconds, ChannelNames[static_cast<std::size_t>(channel)], message);
    if (state.flushEachLine) {
        std::fflush(state.sink);
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
