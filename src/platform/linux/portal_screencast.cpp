// The ScreenCast portal handshake, spoken in plain libdbus. Every call
// follows the portal's request pattern: the method returns a Request object
// path, and the actual result arrives as that object's Response signal. The
// expected path is subscribed before the call so a fast portal cannot slip
// the signal past the listener.

#include "platform/linux/portal_screencast.h"

#include <dbus/dbus.h>

#include <cstdio>

namespace sidescopes {
namespace {

constexpr const char* PortalService = "org.freedesktop.portal.Desktop";
constexpr const char* PortalObject = "/org/freedesktop/portal/desktop";
constexpr const char* ScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char* RequestInterface = "org.freedesktop.portal.Request";
constexpr const char* SessionInterface = "org.freedesktop.portal.Session";

// SelectSources option values, from the portal's ScreenCast interface.
constexpr uint32_t SourceTypeMonitor = 1;
constexpr uint32_t CursorModeHidden = 1;
constexpr uint32_t PersistUntilRevoked = 2;

// Response codes carried by org.freedesktop.portal.Request.Response.
constexpr uint32_t ResponseSuccess = 0;
constexpr uint32_t ResponseCancelled = 1;

/// The two places values cross libdbus's void* boundary. The C API reads and
/// writes every basic value through an untyped pointer whose real type is the
/// tag argument, so the double indirection the tidy check dislikes is that
/// API's own contract, stated once here.
void appendStringValue(DBusMessageIter* iter, int type, const char** value)
{
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    dbus_message_iter_append_basic(iter, type, static_cast<const void*>(value));
}

const char* readStringValue(DBusMessageIter* iter)
{
    const char* value = nullptr;
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    dbus_message_iter_get_basic(iter, static_cast<void*>(&value));
    return value;
}

void appendVardictString(DBusMessageIter* dict, const char* key, const char* value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    appendStringValue(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    appendStringValue(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void appendVardictUint32(DBusMessageIter* dict, const char* key, uint32_t value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    appendStringValue(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void appendVardictBool(DBusMessageIter* dict, const char* key, bool value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    const dbus_bool_t plain = value ? TRUE : FALSE;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    appendStringValue(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &plain);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

/// The results vardict of a Response signal, reduced to the fields the
/// handshake reads.
struct ResponseResults
{
    uint32_t code = ResponseCancelled;
    std::string sessionHandle;
    std::string restoreToken;
    uint32_t nodeId = 0;
    bool sawStream = false;
};

/// Reads the first stream's node id out of the streams field, a(ua{sv}).
void readStreams(DBusMessageIter* variant, ResponseResults& results)
{
    DBusMessageIter streams;
    dbus_message_iter_recurse(variant, &streams);
    if (dbus_message_iter_get_arg_type(&streams) != DBUS_TYPE_STRUCT) {
        return;
    }
    DBusMessageIter fields;
    dbus_message_iter_recurse(&streams, &fields);
    if (dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&fields, &results.nodeId);
        results.sawStream = true;
    }
}

void readResultEntry(DBusMessageIter* entry, ResponseResults& results)
{
    const char* key = readStringValue(entry);
    dbus_message_iter_next(entry);
    DBusMessageIter variant;
    dbus_message_iter_recurse(entry, &variant);
    const int type = dbus_message_iter_get_arg_type(&variant);
    if (type == DBUS_TYPE_STRING) {
        const char* value = readStringValue(&variant);
        if (std::string(key) == "session_handle") {
            results.sessionHandle = value;
        } else if (std::string(key) == "restore_token") {
            results.restoreToken = value;
        }
    } else if (type == DBUS_TYPE_ARRAY && std::string(key) == "streams") {
        readStreams(&variant, results);
    }
}

/// Parses a Response signal: (uint32 code, a{sv} results).
ResponseResults readResponse(DBusMessage* message)
{
    ResponseResults results;
    DBusMessageIter arguments;
    if (dbus_message_iter_init(message, &arguments) == FALSE ||
        dbus_message_iter_get_arg_type(&arguments) != DBUS_TYPE_UINT32) {
        return results;
    }
    dbus_message_iter_get_basic(&arguments, &results.code);
    dbus_message_iter_next(&arguments);
    if (dbus_message_iter_get_arg_type(&arguments) != DBUS_TYPE_ARRAY) {
        return results;
    }
    DBusMessageIter dict;
    dbus_message_iter_recurse(&arguments, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);
        readResultEntry(&entry, results);
        dbus_message_iter_next(&dict);
    }
    return results;
}

/// The sender part of a request object path: the bus name with the colon
/// dropped and dots turned to underscores, per the portal convention.
std::string senderPathToken(DBusConnection* connection)
{
    const char* unique = dbus_bus_get_unique_name(connection);
    std::string token = unique != nullptr ? unique : "";
    if (!token.empty() && token.front() == ':') {
        token.erase(token.begin());
    }
    for (char& piece : token) {
        if (piece == '.') {
            piece = '_';
        }
    }
    return token;
}

}  // namespace

PortalScreenCast::~PortalScreenCast()
{
    close();
}

void PortalScreenCast::close()
{
    if (m_connection == nullptr) {
        return;
    }
    if (!m_sessionHandle.empty() && !m_sessionClosed) {
        DBusMessage* message =
            dbus_message_new_method_call(PortalService, m_sessionHandle.c_str(), SessionInterface, "Close");
        if (message != nullptr) {
            dbus_connection_send(m_connection, message, nullptr);
            dbus_connection_flush(m_connection);
            dbus_message_unref(message);
        }
    }
    dbus_connection_close(m_connection);
    dbus_connection_unref(m_connection);
    m_connection = nullptr;
    m_sessionHandle.clear();
}

bool PortalScreenCast::pump(int timeoutMs)
{
    if (m_connection == nullptr || m_sessionClosed) {
        return false;
    }
    if (dbus_connection_read_write(m_connection, timeoutMs) == FALSE) {
        return false;
    }
    while (DBusMessage* message = dbus_connection_pop_message(m_connection)) {
        if (dbus_message_is_signal(message, SessionInterface, "Closed") != FALSE) {
            m_sessionClosed = true;
        }
        dbus_message_unref(message);
        if (m_sessionClosed) {
            return false;
        }
    }
    return true;
}

namespace {

/// Sends one portal method call and blocks for its reply, which for the
/// request-shaped calls is only the Request object path, not the result.
DBusMessage* callWithReply(DBusConnection* connection, DBusMessage* message)
{
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(connection, message, 5000, &error);
    dbus_message_unref(message);
    dbus_error_free(&error);
    return reply;
}

/// Waits for the Response signal on @p requestPath, dispatching in short
/// slices so @p abort stays honored; the consent dialog can sit open for
/// minutes, so there is deliberately no overall deadline.
std::optional<ResponseResults> waitForResponse(DBusConnection* connection, const std::string& requestPath,
                                               const std::atomic<bool>& abort)
{
    while (!abort.load()) {
        if (dbus_connection_read_write(connection, 200) == FALSE) {
            return std::nullopt;
        }
        while (DBusMessage* message = dbus_connection_pop_message(connection)) {
            const bool matches = dbus_message_is_signal(message, RequestInterface, "Response") != FALSE &&
                                 requestPath == dbus_message_get_path(message);
            if (matches) {
                ResponseResults results = readResponse(message);
                dbus_message_unref(message);
                return results;
            }
            dbus_message_unref(message);
        }
    }
    return std::nullopt;
}

/// The connection-scoped state the handshake steps thread through.
struct Handshake
{
    DBusConnection* connection = nullptr;
    std::string senderToken;
    std::string sessionHandle;
    uint32_t tokenCounter = 0;
};

std::string nextToken(Handshake& handshake)
{
    char token[32];
    std::snprintf(token, sizeof(token), "sidescopes%u", ++handshake.tokenCounter);
    return token;
}

/// Sends one request-shaped call whose options open with a handle_token, and
/// waits for its Response. @p fill appends the remaining arguments: object
/// paths before the vardict via @p arguments, options via @p options.
template <typename Fill>
std::optional<ResponseResults> requestCall(Handshake& handshake, const char* method, const std::atomic<bool>& abort,
                                           Fill fill)
{
    const std::string token = nextToken(handshake);
    const std::string requestPath = "/org/freedesktop/portal/desktop/request/" + handshake.senderToken + "/" + token;
    DBusMessage* message = dbus_message_new_method_call(PortalService, PortalObject, ScreenCastInterface, method);
    DBusMessageIter arguments;
    DBusMessageIter options;
    dbus_message_iter_init_append(message, &arguments);
    fill(&arguments, nullptr);
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}", &options);
    appendVardictString(&options, "handle_token", token.c_str());
    fill(nullptr, &options);
    dbus_message_iter_close_container(&arguments, &options);
    DBusMessage* reply = callWithReply(handshake.connection, message);
    if (reply == nullptr) {
        return std::nullopt;
    }
    dbus_message_unref(reply);
    return waitForResponse(handshake.connection, requestPath, abort);
}

std::optional<ResponseResults> createSession(Handshake& handshake, const std::atomic<bool>& abort)
{
    return requestCall(handshake, "CreateSession", abort, [&](DBusMessageIter*, DBusMessageIter* options) {
        if (options != nullptr) {
            appendVardictString(options, "session_handle_token", "sidescopes");
        }
    });
}

std::optional<ResponseResults> selectSources(Handshake& handshake, const std::string& restoreToken,
                                             const std::atomic<bool>& abort)
{
    return requestCall(handshake, "SelectSources", abort, [&](DBusMessageIter* arguments, DBusMessageIter* options) {
        if (arguments != nullptr) {
            const char* session = handshake.sessionHandle.c_str();
            appendStringValue(arguments, DBUS_TYPE_OBJECT_PATH, &session);
        }
        if (options != nullptr) {
            appendVardictUint32(options, "types", SourceTypeMonitor);
            appendVardictBool(options, "multiple", false);
            // Hidden: a cursor composited into the pixels would enter the
            // scopes' analysis; position metadata comes later for the probes.
            appendVardictUint32(options, "cursor_mode", CursorModeHidden);
            appendVardictUint32(options, "persist_mode", PersistUntilRevoked);
            if (!restoreToken.empty()) {
                appendVardictString(options, "restore_token", restoreToken.c_str());
            }
        }
    });
}

std::optional<ResponseResults> startSession(Handshake& handshake, const std::atomic<bool>& abort)
{
    return requestCall(handshake, "Start", abort, [&](DBusMessageIter* arguments, DBusMessageIter*) {
        if (arguments != nullptr) {
            const char* session = handshake.sessionHandle.c_str();
            const char* parent = "";
            appendStringValue(arguments, DBUS_TYPE_OBJECT_PATH, &session);
            appendStringValue(arguments, DBUS_TYPE_STRING, &parent);
        }
    });
}

int openPipeWireRemote(Handshake& handshake)
{
    DBusMessage* message =
        dbus_message_new_method_call(PortalService, PortalObject, ScreenCastInterface, "OpenPipeWireRemote");
    DBusMessageIter arguments;
    DBusMessageIter options;
    const char* session = handshake.sessionHandle.c_str();
    dbus_message_iter_init_append(message, &arguments);
    appendStringValue(&arguments, DBUS_TYPE_OBJECT_PATH, &session);
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}", &options);
    dbus_message_iter_close_container(&arguments, &options);
    DBusMessage* reply = callWithReply(handshake.connection, message);
    if (reply == nullptr) {
        return -1;
    }
    int fd = -1;
    DBusMessageIter result;
    if (dbus_message_iter_init(reply, &result) != FALSE &&
        dbus_message_iter_get_arg_type(&result) == DBUS_TYPE_UNIX_FD) {
        dbus_message_iter_get_basic(&result, &fd);
    }
    dbus_message_unref(reply);
    return fd;
}

}  // namespace

std::optional<PortalStream> PortalScreenCast::open(const std::string& restoreToken, const std::atomic<bool>& abort,
                                                   PortalError& error)
{
    error = PortalError::Unavailable;
    DBusError busError;
    dbus_error_init(&busError);
    m_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &busError);
    dbus_error_free(&busError);
    if (m_connection == nullptr) {
        return std::nullopt;
    }
    dbus_connection_set_exit_on_disconnect(m_connection, FALSE);

    // One match covers every Response and the session's Closed signal; the
    // waiters filter by object path.
    dbus_bus_add_match(m_connection, "type='signal',interface='org.freedesktop.portal.Request'", nullptr);
    dbus_bus_add_match(m_connection, "type='signal',interface='org.freedesktop.portal.Session'", nullptr);

    Handshake handshake;
    handshake.connection = m_connection;
    handshake.senderToken = senderPathToken(m_connection);

    const std::optional<ResponseResults> created = createSession(handshake, abort);
    if (!created || created->code != ResponseSuccess || created->sessionHandle.empty()) {
        return std::nullopt;
    }
    handshake.sessionHandle = created->sessionHandle;
    m_sessionHandle = created->sessionHandle;

    const std::optional<ResponseResults> selected = selectSources(handshake, restoreToken, abort);
    if (!selected || selected->code != ResponseSuccess) {
        return std::nullopt;
    }

    const std::optional<ResponseResults> started = startSession(handshake, abort);
    if (!started || started->code != ResponseSuccess || !started->sawStream) {
        if (started && started->code == ResponseCancelled) {
            error = PortalError::Declined;
        }
        return std::nullopt;
    }

    const int fd = openPipeWireRemote(handshake);
    if (fd < 0) {
        return std::nullopt;
    }

    error = PortalError::None;
    PortalStream stream;
    stream.pipewireFd = fd;
    stream.nodeId = started->nodeId;
    stream.restoreToken = started->restoreToken;
    return stream;
}

}  // namespace sidescopes
