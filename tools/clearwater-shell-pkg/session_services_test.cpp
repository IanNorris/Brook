#include "session_services.h"
#include <dbus/dbus.h>

#include <cassert>
#include <cerrno>
#include <initializer_list>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace brook;

namespace {
constexpr char Watcher[] = "org.kde.StatusNotifierWatcher";
constexpr char Item[] = "org.kde.StatusNotifierItem";
constexpr char Menu[] = "com.canonical.dbusmenu";
constexpr char Notes[] = "org.freedesktop.Notifications";
constexpr char NotesPath[] = "/org/freedesktop/Notifications";
int stateFd, inputFd;
bool scrolled = false, menuClicked = false, actionInvoked = false, closed = false;
bool itemIsMenu = false, rejectActivation = false;
unsigned activateCalls = 0, layoutCalls = 0, contextCalls = 0;
bool deferLayout = false;
DBusMessage* heldLayout = nullptr;
const char* menuLabel = "_Show Library";
const char* status = "Active";
const char* title = "Steam fixture";
DBusConnection* peer;
ChromeTrayState state{};
int received = 0;

bool Publish(const ChromeTrayState& view)
{
    const auto* p = reinterpret_cast<const char*>(&view);
    for (size_t n = 0; n < sizeof(view);) {
        const auto bytes = write(stateFd, p + n, sizeof(view) - n);
        if (bytes <= 0) return false;
        n += bytes;
    }
    return true;
}
int ReadInput(TrayInput& input)
{
    const auto n = read(inputFd, &input, sizeof(input));
    return n == sizeof(input) ? 1 : n < 0 && errno == EAGAIN ? 0 : -1;
}
void StringProperty(DBusMessageIter& dictionary, const char* key, const char* value)
{
    DBusMessageIter pair, variant;
    dbus_message_iter_open_container(&dictionary, DBUS_TYPE_DICT_ENTRY, nullptr, &pair);
    dbus_message_iter_append_basic(&pair, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&pair, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&pair, &variant);
    dbus_message_iter_close_container(&dictionary, &pair);
}
void IconPixmapProperty(DBusMessageIter& dictionary)
{
    DBusMessageIter pair, variant, images, image, pixels;
    const char* key = "IconPixmap";
    const int32_t size = 1;
    const unsigned char argb[] = {0xff, 0x12, 0x34, 0x56};
    const unsigned char* data = argb;
    assert(dbus_message_iter_open_container(&dictionary, DBUS_TYPE_DICT_ENTRY, nullptr, &pair));
    assert(dbus_message_iter_append_basic(&pair, DBUS_TYPE_STRING, &key));
    assert(dbus_message_iter_open_container(&pair, DBUS_TYPE_VARIANT, "a(iiay)", &variant));
    assert(dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(iiay)", &images));
    assert(dbus_message_iter_open_container(&images, DBUS_TYPE_STRUCT, nullptr, &image));
    assert(dbus_message_iter_append_basic(&image, DBUS_TYPE_INT32, &size));
    assert(dbus_message_iter_append_basic(&image, DBUS_TYPE_INT32, &size));
    assert(dbus_message_iter_open_container(&image, DBUS_TYPE_ARRAY, "y", &pixels));
    assert(dbus_message_iter_append_fixed_array(&pixels, DBUS_TYPE_BYTE, &data, sizeof(argb)));
    assert(dbus_message_iter_close_container(&image, &pixels));
    assert(dbus_message_iter_close_container(&images, &image));
    assert(dbus_message_iter_close_container(&variant, &images));
    assert(dbus_message_iter_close_container(&pair, &variant));
    assert(dbus_message_iter_close_container(&dictionary, &pair));
}
void MenuNode(DBusMessageIter& iter, int32_t id)
{
    DBusMessageIter node, properties, children;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr, &node);
    dbus_message_iter_append_basic(&node, DBUS_TYPE_INT32, &id);
    dbus_message_iter_open_container(&node, DBUS_TYPE_ARRAY, "{sv}", &properties);
    if (id) StringProperty(properties, "label", menuLabel);
    dbus_message_iter_close_container(&node, &properties);
    dbus_message_iter_open_container(&node, DBUS_TYPE_ARRAY, "v", &children);
    if (!id) {
        DBusMessageIter child;
        dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &child);
        MenuNode(child, 41);
        dbus_message_iter_close_container(&children, &child);
    }
    dbus_message_iter_close_container(&node, &children);
    dbus_message_iter_close_container(&iter, &node);
}

void ReplyLayout(DBusConnection* bus, DBusMessage* msg)
{
    auto* reply = dbus_message_new_method_return(msg);
    assert(reply);
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    uint32_t revision = layoutCalls;
    assert(dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &revision));
    MenuNode(iter, 0);
    assert(dbus_connection_send(bus, reply, nullptr));
    dbus_message_unref(reply);
}

void ReleaseLayout()
{
    assert(heldLayout);
    ReplyLayout(peer, heldLayout);
    dbus_message_unref(heldLayout);
    heldLayout = nullptr;
}

void LayoutUpdated()
{
    auto* msg = dbus_message_new_signal("/Menu", Menu, "LayoutUpdated");
    const uint32_t revision = layoutCalls + 1;
    const int32_t root = 0;
    assert(msg && dbus_message_append_args(msg, DBUS_TYPE_UINT32, &revision, DBUS_TYPE_INT32, &root, DBUS_TYPE_INVALID));
    assert(dbus_connection_send(peer, msg, nullptr));
    dbus_message_unref(msg);
}

DBusHandlerResult Fake(DBusConnection* bus, DBusMessage* msg, void*)
{
    auto send = [&](DBusMessage* m) {
        assert(m);
        assert(dbus_connection_send(bus, m, nullptr));
        dbus_message_unref(m);
    };
    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
        if (std::strncmp(dbus_message_get_path(msg), "/Unresponsive", 13) == 0)
            return DBUS_HANDLER_RESULT_HANDLED; // keep the connection open without answering
        auto* reply = dbus_message_new_method_return(msg);
        DBusMessageIter out, properties, pair, variant;
        dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &properties);
        StringProperty(properties, "Title", title);
        StringProperty(properties, "Status", status);
        IconPixmapProperty(properties);
        const char* key = "Menu", *value = "/Menu";
        dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr, &pair);
        dbus_message_iter_append_basic(&pair, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&pair, DBUS_TYPE_VARIANT, "o", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &value);
        dbus_message_iter_close_container(&pair, &variant);
        dbus_message_iter_close_container(&properties, &pair);
        const char* menuKey = "ItemIsMenu";
        const dbus_bool_t isMenu = itemIsMenu;
        assert(dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr, &pair));
        assert(dbus_message_iter_append_basic(&pair, DBUS_TYPE_STRING, &menuKey));
        assert(dbus_message_iter_open_container(&pair, DBUS_TYPE_VARIANT, "b", &variant));
        assert(dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &isMenu));
        assert(dbus_message_iter_close_container(&pair, &variant));
        assert(dbus_message_iter_close_container(&properties, &pair));
        dbus_message_iter_close_container(&out, &properties);
        send(reply);
    } else if (dbus_message_is_method_call(msg, Menu, "GetLayout")) {
        ++layoutCalls;
        if (deferLayout) {
            assert(!heldLayout);
            heldLayout = dbus_message_ref(msg);
        } else ReplyLayout(bus, msg);
    } else if (dbus_message_is_method_call(msg, Item, "ContextMenu")) {
        assert(dbus_message_has_signature(msg, "ii"));
        ++contextCalls;
    } else if (dbus_message_is_method_call(msg, Item, "Activate")) {
        assert(dbus_message_has_signature(msg, "ii"));
        ++activateCalls;
        send(rejectActivation ? dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_METHOD, "This app only provides a menu")
                              : dbus_message_new_method_return(msg));
    } else if (dbus_message_is_method_call(msg, Item, "Scroll")) {
        assert(dbus_message_has_signature(msg, "is"));
        scrolled = true;
    } else if (dbus_message_is_method_call(msg, Menu, "Event")) {
        assert(dbus_message_has_signature(msg, "isvu"));
        menuClicked = true;
    } else if (dbus_message_is_signal(msg, Notes, "ActionInvoked")) {
        const char* action;
        uint32_t id;
        assert(dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &action, DBUS_TYPE_INVALID));
        assert(std::strcmp(action, "open") == 0);
        actionInvoked = true;
    } else if (dbus_message_is_signal(msg, Notes, "NotificationClosed")) {
        closed = true;
    } else return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    return DBUS_HANDLER_RESULT_HANDLED;
}

DBusMessage* Ask(DBusMessage* message)
{
    assert(message);
    auto* reply = dbus_connection_send_with_reply_and_block(peer, message, 3000, nullptr);
    dbus_message_unref(message);
    assert(reply);
    return reply;
}

template<class Predicate> void Wait(Predicate condition)
{
    using namespace std::chrono;
    const auto timeLimit = steady_clock::now() + seconds(5);
    for (;;) {
        dbus_connection_read_write_dispatch(peer, 10);
        const auto n = read(stateFd, reinterpret_cast<char*>(&state) + received, sizeof(state) - received);
        if (n > 0) received += n;
        if (received == sizeof(state)) received = 0;
        if (!received && condition()) return;
        assert(steady_clock::now() < timeLimit);
    }
}

void Register(DBusConnection* sender, const char* path, bool accepted = true)
{
    auto* request = dbus_message_new_method_call(Watcher, "/StatusNotifierWatcher", Watcher, "RegisterStatusNotifierItem");
    assert(request && dbus_message_append_args(request, DBUS_TYPE_STRING, &path, DBUS_TYPE_INVALID));
    DBusError error = DBUS_ERROR_INIT;
    auto* reply = dbus_connection_send_with_reply_and_block(sender, request, 3000, &error);
    dbus_message_unref(request);
    if (accepted) assert(reply);
    else assert(!reply && dbus_error_has_name(&error, DBUS_ERROR_INVALID_ARGS));
    if (reply) dbus_message_unref(reply);
    dbus_error_free(&error);
}

unsigned RegisteredCount()
{
    auto* request = dbus_message_new_method_call(Watcher, "/StatusNotifierWatcher", DBUS_INTERFACE_PROPERTIES, "Get");
    const char* iface = Watcher;
    const char* key = "RegisteredStatusNotifierItems";
    assert(request && dbus_message_append_args(request, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID));
    auto* reply = Ask(request);
    DBusMessageIter value, array, items;
    assert(dbus_message_has_signature(reply, "v") && dbus_message_iter_init(reply, &value));
    dbus_message_iter_recurse(&value, &array);
    assert(dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&array, &items);
    unsigned count = 0;
    for (; dbus_message_iter_get_arg_type(&items) == DBUS_TYPE_STRING; dbus_message_iter_next(&items)) ++count;
    dbus_message_unref(reply);
    return count;
}

uint32_t Notify(int timeout, uint32_t replace = 0)
{
    auto* m = dbus_message_new_method_call(Notes, NotesPath, Notes, "Notify");
    DBusMessageIter args, array;
    dbus_message_iter_init_append(m, &args);
    const char* app = "Steam fixture";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replace);
    for (const char* text : {"", "Downloaded", "Library is ready"})
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &text);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &array);
    for (const char* text : {"open", "Open Library"})
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &text);
    dbus_message_iter_close_container(&args, &array);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &array);
    dbus_message_iter_close_container(&args, &array);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &timeout);
    auto* reply = Ask(m);
    uint32_t id;
    assert(dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID));
    dbus_message_unref(reply);
    return id;
}
void Action(TrayInputKind kind, uint32_t owner, int32_t n = 0)
{
    const TrayInput event{kind, owner, n, 50, 60};
    assert(write(inputFd, &event, sizeof(event)) == sizeof(event));
}
}

int main()
{
    int states[2], inputs[2];
    assert(pipe(states) == 0 && pipe(inputs) == 0);
    assert(fcntl(states[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(inputs[0], F_SETFL, O_NONBLOCK) == 0);
    const auto parent = getpid();
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        close(states[0]);
        close(inputs[1]);
        stateFd = states[1];
        inputFd = inputs[0];
        _exit(RunSessionServices(Publish, ReadInput, parent));
    }
    close(states[1]);
    close(inputs[0]);
    stateFd = states[0];
    inputFd = inputs[1];
    peer = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
    assert(peer);
    assert(dbus_connection_add_filter(peer, Fake, nullptr, nullptr));
    dbus_bus_add_match(peer, "type='signal',interface='org.freedesktop.Notifications'", nullptr);
    for (unsigned i = 0; !dbus_bus_name_has_owner(peer, Watcher, nullptr); ++i) {
        assert(i < 100);
        usleep(20000);
    }
    const char* path = "/StatusNotifierItem";
    Register(peer, path);
    // Properties.GetAll and a synchronous client request can cross on
    // the bus before the client begins dispatching incoming methods.
    auto* capabilities = Ask(dbus_message_new_method_call(Notes, NotesPath, Notes, "GetCapabilities"));
    assert(dbus_message_has_signature(capabilities, "as"));
    dbus_message_unref(capabilities);
    Wait([] { return state.itemCount == 1; });
    assert(std::strcmp(state.items[0].title, "Steam fixture") == 0);
    assert(state.items[0].hasIcon && state.items[0].icon[0] == 0xff123456u);
    const uint32_t id = state.items[0].id;
    Action(TrayInputKind::Activate, id);
    Action(TrayInputKind::Scroll, id, 120);
    Wait([] { return activateCalls == 1 && scrolled; });
    assert(state.menuCount == 0);
    Action(TrayInputKind::Context, id);
    Wait([] { return state.menuCount == 1; });
    assert(std::strcmp(state.menu[0].label, "Show Library") == 0);
    Action(TrayInputKind::MenuSelect, id, 41);
    Wait([] { return menuClicked && state.menuCount == 0; });

    itemIsMenu = true;
    title = "Steam menu-only fixture";
    auto* refresh = dbus_message_new_signal(path, Item, "NewTitle");
    assert(dbus_connection_send(peer, refresh, nullptr));
    dbus_message_unref(refresh);
    Wait([] { return std::strcmp(state.items[0].title, title) == 0; });
    Action(TrayInputKind::Activate, id);
    Wait([] { return state.menuCount == 1; });
    assert(activateCalls == 1);
    Action(TrayInputKind::DismissMenu, id);
    Wait([] { return state.menuCount == 0; });

    itemIsMenu = false;
    rejectActivation = true;
    title = "Steam no Activate fixture";
    refresh = dbus_message_new_signal(path, Item, "NewTitle");
    assert(dbus_connection_send(peer, refresh, nullptr));
    dbus_message_unref(refresh);
    Wait([] { return std::strcmp(state.items[0].title, title) == 0; });
    Action(TrayInputKind::Activate, id);
    Wait([] { return state.menuCount == 1; });
    assert(activateCalls == 2);
    Action(TrayInputKind::DismissMenu, id);
    Wait([] { return state.menuCount == 0; });

    // A peer holding GetLayout must be able to use another service on the
    // same bus before it replies, including a notification.
    deferLayout = true;
    Action(TrayInputKind::Context, id);
    Wait([] { return heldLayout != nullptr; });
    const uint32_t menuNotice = Notify(3000);
    Wait([&] { return state.notice.id == menuNotice; });
    assert(state.menuCount == 0 && heldLayout);
    Action(TrayInputKind::CloseNotice, menuNotice);
    deferLayout = false;
    ReleaseLayout();
    Wait([] { return state.menuCount == 1 && !state.notice.id; });

    // Hold a refresh open while its owner floods change signals. The
    // current menu remains usable, with one follow-up after the reply.
    deferLayout = true;
    const unsigned beforeBurst = layoutCalls;
    LayoutUpdated();
    Wait([] { return heldLayout != nullptr; });
    for (unsigned i = 0; i < 48; ++i) LayoutUpdated();
    dbus_message_unref(Ask(dbus_message_new_method_call(Notes, NotesPath, Notes, "GetCapabilities")));
    assert(layoutCalls == beforeBurst + 1 && state.menuCount == 1);
    menuLabel = "_Open updated library";
    deferLayout = false;
    ReleaseLayout();
    Wait([&] { return layoutCalls == beforeBurst + 2 && std::strcmp(state.menu[0].label, "Open updated library") == 0; });

    deferLayout = true;
    LayoutUpdated();
    Wait([] { return heldLayout != nullptr; });
    Action(TrayInputKind::DismissMenu, id);
    Wait([] { return state.menuCount == 0; });
    deferLayout = false;
    ReleaseLayout(); // The superseded reply must never reopen a menu.
    const unsigned beforeTimeout = layoutCalls;
    deferLayout = true;
    Action(TrayInputKind::Context, id);
    Wait([&] { return layoutCalls == beforeTimeout + 1 && heldLayout; });
    Wait([] { return contextCalls == 1; }); // Timeout falls back to the item's ContextMenu.
    assert(state.menuCount == 0);
    dbus_message_unref(heldLayout);
    heldLayout = nullptr;
    deferLayout = false;

    for (const char* value : {"NeedsAttention", "Passive"}) {
        status = value;
        auto* signal = dbus_message_new_signal(path, Item, "NewStatus");
        assert(dbus_connection_send(peer, signal, nullptr));
        dbus_message_unref(signal);
        if (std::strcmp(value, "Passive") == 0) Wait([] { return state.itemCount == 0; });
        else Wait([] { return state.items[0].attention != 0; });
    }

    // The valid Passive item still counts, but a peer cannot consume
    // every slot or claim another peer's bus name.
    assert(RegisteredCount() == 1);
    for (const char* fixture : {"/Unresponsive1", "/Unresponsive2", "/Unresponsive3"}) Register(peer, fixture);
    Register(peer, "/Unresponsive4", false);
    Register(peer, path); // Re-registration is idempotent even at the cap.
    auto* second = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
    assert(second);
    Register(second, dbus_bus_get_unique_name(peer), false);
    Register(second, "/UnresponsivePeer");
    assert(RegisteredCount() == 5);
    Wait([] { return RegisteredCount() == 1; });
    Register(peer, "/Unresponsive4"); // The unanswered registrations freed capacity.
    dbus_connection_close(second);
    dbus_connection_unref(second);

    const uint32_t note = Notify(2000);
    assert(Notify(2000, note) == note);
    Wait([] { return state.notice.id && state.notice.actionCount == 1; });
    assert(state.notice.id == note);
    Action(TrayInputKind::NoticeAction, note);
    Wait([] { return actionInvoked && closed && !state.notice.id; });
    closed = false;
    Notify(300);
    Wait([] { return state.notice.id != 0; });
    Wait([] { return closed && !state.notice.id; });
    closed = false;
    const uint32_t explicitClose = Notify(0);
    Wait([] { return state.notice.id != 0; });
    auto* m = dbus_message_new_method_call(Notes, NotesPath, Notes, "CloseNotification");
    dbus_message_append_args(m, DBUS_TYPE_UINT32, &explicitClose, DBUS_TYPE_INVALID);
    dbus_message_unref(Ask(m));
    Wait([] { return closed && !state.notice.id; });

    dbus_connection_close(peer);
    dbus_connection_unref(peer);
    close(inputFd);
    close(stateFd);
    int exitStatus;
    assert(waitpid(child, &exitStatus, 0) == child && WIFEXITED(exitStatus) && WEXITSTATUS(exitStatus) == 0);
    std::puts("CLEARWATER_SESSION_SERVICES_PASS");
}
