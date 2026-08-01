// The ScreenCast portal handshake, spoken in plain libdbus. Every call
// follows the portal's request pattern: the method returns a Request object
// path, and the actual result arrives as that object's Response signal. The
// expected path is subscribed before the call so a fast portal cannot slip
// the signal past the listener.

#include "platform/linux/portal_screencast.h"

#include <dbus/dbus.h>

#include <cstdio>

#include "core/diagnostics.h"
#include "platform/linux/portal_request_path.h"

namespace sidescopes {
namespace {

constexpr const char* PortalService = "org.freedesktop.portal.Desktop";
constexpr const char* PortalObject = "/org/freedesktop/portal/desktop";
constexpr const char* ScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char* RequestInterface = "org.freedesktop.portal.Request";
constexpr const char* SessionInterface = "org.freedesktop.portal.Session";

// A SelectSources option value; the source type and cursor mode are decided by
// portal_options.h, which is pure so both are held to by a test.
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
    /// Where the chosen source sits and what it covers. Both optional in the
    /// portal spec, so zero means "not stated" rather than "at the origin".
    double originX = 0.0;
    double originY = 0.0;
    double widthPoints = 0.0;
    double heightPoints = 0.0;
};

/// A stream's `position` or `size` property: an array of exactly two ints.
/// False leaves both outputs untouched, so a malformed pair reads as absent.
bool readIntPair(DBusMessageIter* variant, double& first, double& second)
{
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_ARRAY) {
        return false;
    }
    DBusMessageIter values;
    dbus_message_iter_recurse(variant, &values);
    int32_t pair[2] = {0, 0};
    int count = 0;
    while (count < 2 && dbus_message_iter_get_arg_type(&values) == DBUS_TYPE_INT32) {
        dbus_message_iter_get_basic(&values, &pair[count]);
        dbus_message_iter_next(&values);
        ++count;
    }
    if (count < 2) {
        return false;
    }
    first = pair[0];
    second = pair[1];

    return true;
}

void readStreamProperty(DBusMessageIter* entry, ResponseResults& results)
{
    const char* key = readStringValue(entry);
    dbus_message_iter_next(entry);
    DBusMessageIter variant;
    dbus_message_iter_recurse(entry, &variant);
    const std::string name = key != nullptr ? key : "";
    if (name == "position") {
        (void)readIntPair(&variant, results.originX, results.originY);
    } else if (name == "size") {
        (void)readIntPair(&variant, results.widthPoints, results.heightPoints);
    }
}

/// The a{sv} that follows a stream's node id.
void readStreamProperties(DBusMessageIter* fields, ResponseResults& results)
{
    if (dbus_message_iter_get_arg_type(fields) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter properties;
    dbus_message_iter_recurse(fields, &properties);
    while (dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&properties, &entry);
        readStreamProperty(&entry, results);
        dbus_message_iter_next(&properties);
    }
}

/// Reads the first stream's node id and placement out of the streams field,
/// a(ua{sv}).
void readStreams(DBusMessageIter* variant, ResponseResults& results)
{
    DBusMessageIter streams;
    dbus_message_iter_recurse(variant, &streams);
    if (dbus_message_iter_get_arg_type(&streams) != DBUS_TYPE_STRUCT) {
        return;
    }
    DBusMessageIter fields;
    dbus_message_iter_recurse(&streams, &fields);
    if (dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_UINT32) {
        return;
    }
    dbus_message_iter_get_basic(&fields, &results.nodeId);
    results.sawStream = true;
    dbus_message_iter_next(&fields);
    readStreamProperties(&fields, results);
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

/// This connection's unique bus name, or empty before it has one.
std::string uniqueBusName(DBusConnection* connection)
{
    const char* unique = dbus_bus_get_unique_name(connection);
    return unique != nullptr ? unique : "";
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
    std::string busName;
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
    const std::string requestPath = portalRequestPath(handshake.busName, token);
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

/// The AvailableCursorModes bitmask this portal advertises; zero when the
/// property cannot be read, which asks for nothing beyond the guaranteed mode.
uint32_t availableCursorModes(DBusConnection* connection)
{
    DBusMessage* message =
        dbus_message_new_method_call(PortalService, PortalObject, "org.freedesktop.DBus.Properties", "Get");
    if (message == nullptr) {
        return 0;
    }
    const char* interface = ScreenCastInterface;
    const char* property = "AvailableCursorModes";
    DBusMessageIter arguments;
    dbus_message_iter_init_append(message, &arguments);
    appendStringValue(&arguments, DBUS_TYPE_STRING, &interface);
    appendStringValue(&arguments, DBUS_TYPE_STRING, &property);
    DBusMessage* reply = callWithReply(connection, message);
    if (reply == nullptr) {
        return 0;
    }
    uint32_t modes = 0;
    DBusMessageIter result;
    if (dbus_message_iter_init(reply, &result) != FALSE &&
        dbus_message_iter_get_arg_type(&result) == DBUS_TYPE_VARIANT) {
        DBusMessageIter variant;
        dbus_message_iter_recurse(&result, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&variant, &modes);
        }
    }
    dbus_message_unref(reply);

    return modes;
}

std::optional<ResponseResults> selectSources(Handshake& handshake, PortalSourceKind kind,
                                             const std::string& restoreToken, const std::atomic<bool>& abort)
{
    const uint32_t availableModes = availableCursorModes(handshake.connection);
    const uint32_t cursorMode = portalCursorMode(availableModes);
    // What was asked for, beside what was on offer: "no cursor metadata
    // arrived" has two completely different causes and only this separates
    // them.
    SS_DIAG(Perf, "portal cursor modes available=%u asked=%u", availableModes, cursorMode);

    return requestCall(handshake, "SelectSources", abort, [&](DBusMessageIter* arguments, DBusMessageIter* options) {
        if (arguments != nullptr) {
            const char* session = handshake.sessionHandle.c_str();
            appendStringValue(arguments, DBUS_TYPE_OBJECT_PATH, &session);
        }
        if (options != nullptr) {
            appendVardictUint32(options, "types", portalSourceTypeMask(kind));
            appendVardictBool(options, "multiple", false);
            appendVardictUint32(options, "cursor_mode", cursorMode);
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

std::optional<PortalStream> PortalScreenCast::open(PortalSourceKind kind, const std::string& restoreToken,
                                                   const std::atomic<bool>& abort, PortalError& error)
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
    handshake.busName = uniqueBusName(m_connection);

    const std::optional<ResponseResults> created = createSession(handshake, abort);
    if (!created || created->code != ResponseSuccess || created->sessionHandle.empty()) {
        return std::nullopt;
    }
    handshake.sessionHandle = created->sessionHandle;
    m_sessionHandle = created->sessionHandle;

    const std::optional<ResponseResults> selected = selectSources(handshake, kind, restoreToken, abort);
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
    stream.originX = started->originX;
    stream.originY = started->originY;
    stream.widthPoints = started->widthPoints;
    stream.heightPoints = started->heightPoints;
    return stream;
}

}  // namespace sidescopes
