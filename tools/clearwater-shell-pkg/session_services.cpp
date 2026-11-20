#include "session_services.h"

#include <dbus/dbus.h>
#include <png.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace brook {
namespace {
using Clock = std::chrono::steady_clock;
constexpr char Watcher[] = "org.kde.StatusNotifierWatcher";
constexpr char WatcherPath[] = "/StatusNotifierWatcher";
constexpr char ItemInterface[] = "org.kde.StatusNotifierItem";
constexpr char MenuInterface[] = "com.canonical.dbusmenu";
constexpr char Notifications[] = "org.freedesktop.Notifications";
constexpr char NotificationsPath[] = "/org/freedesktop/Notifications";
constexpr int32_t MenuBack = INT32_MIN;
constexpr unsigned ItemStorage = 32, NoticeStorage = 3;

using Message = std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)>;
Message Hold(DBusMessage* m) { return Message(m, dbus_message_unref); }
struct CancelPending {
    void operator()(DBusPendingCall* call) const
    {
        dbus_pending_call_cancel(call);
        dbus_pending_call_unref(call);
    }
};
using PendingCall = std::unique_ptr<DBusPendingCall, CancelPending>;

bool Same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

template<size_t N> void CopyText(char (&out)[N], const char* from)
{
    size_t i = 0;
    if (from) for (; i + 1 < N && from[i]; ++i)
        out[i] = static_cast<unsigned char>(from[i]) < 32 ? ' ' : from[i];
    out[i] = 0;
}

const char* Text(DBusMessageIter iter)
{
    const char* s = nullptr;
    const int type = dbus_message_iter_get_arg_type(&iter);
    if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH)
        dbus_message_iter_get_basic(&iter, &s);
    return s;
}
bool Boolean(DBusMessageIter iter, bool fallback = false)
{
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_BOOLEAN) return fallback;
    dbus_bool_t value;
    dbus_message_iter_get_basic(&iter, &value);
    return value;
}
int32_t Number(DBusMessageIter iter)
{
    int32_t value = 0;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32)
        dbus_message_iter_get_basic(&iter, &value);
    return value;
}
DBusMessageIter Into(DBusMessageIter& iter)
{
    DBusMessageIter nested{};
    dbus_message_iter_recurse(&iter, &nested);
    return nested;
}
template<typename Handle> void Properties(DBusMessageIter dict, Handle&& handle)
{
    if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_ARRAY) return;
    for (auto pair = Into(dict); dbus_message_iter_get_arg_type(&pair) == DBUS_TYPE_DICT_ENTRY;
         dbus_message_iter_next(&pair)) {
        auto entry = Into(pair);
        const auto* key = Text(entry);
        dbus_message_iter_next(&entry);
        if (key && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT)
            handle(key, Into(entry));
    }
}

bool Pixels(DBusMessageIter list, uint32_t (&out)[TrayIconWords])
{
    if (dbus_message_iter_get_arg_type(&list) != DBUS_TYPE_ARRAY) return false;
    for (auto image = Into(list); dbus_message_iter_get_arg_type(&image) == DBUS_TYPE_STRUCT;
         dbus_message_iter_next(&image)) {
        auto members = Into(image);
        const int width = Number(members);
        dbus_message_iter_next(&members);
        const int height = Number(members);
        dbus_message_iter_next(&members);
        if (width <= 0 || height <= 0 || width > 256 || height > 256 ||
            dbus_message_iter_get_arg_type(&members) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&members) != DBUS_TYPE_BYTE) continue;
        auto array = Into(members);
        const unsigned char* bytes = nullptr;
        int count = 0;
        dbus_message_iter_get_fixed_array(&array, &bytes, &count);
        if (!bytes || count != width * height * 4) continue;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 24; ++x) {
                const auto* pixel = bytes + 4 * ((y * height / 24) * width + x * width / 24);
                out[y * 24 + x] = uint32_t(pixel[0]) << 24 | uint32_t(pixel[1]) << 16 |
                                 uint32_t(pixel[2]) << 8 | pixel[3];
            }
        return true;
    }
    return false;
}

bool IconName(const std::string& name, uint32_t (&out)[TrayIconWords])
{
    if (name.empty() || name.size() > 120 || name.find("..") != std::string::npos) return false;
    for (char c : name)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '-' && c != '_' && c != '.') return false;
    for (const char* size : {"24x24", "32x32", "48x48", "22x22", "16x16", "64x64", "128x128"}) {
        const auto path = std::string("/nix/profile/share/icons/hicolor/") + size + "/apps/" + name + ".png";
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_file(&image, path.c_str())) continue;
        if (!image.width || !image.height || image.width > 256 || image.height > 256) {
            png_image_free(&image);
            continue;
        }
        image.format = PNG_FORMAT_RGBA;
        std::vector<unsigned char> bytes(PNG_IMAGE_SIZE(image));
        const unsigned width = image.width, height = image.height;
        const bool ok = png_image_finish_read(&image, nullptr, bytes.data(), 0, nullptr);
        png_image_free(&image);
        if (!ok) continue;
        for (unsigned y = 0; y < 24; ++y)
            for (unsigned x = 0; x < 24; ++x) {
                const auto* p = bytes.data() + 4 * ((y * height / 24) * width + x * width / 24);
                out[y * 24 + x] = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 |
                                 uint32_t(p[1]) << 8 | p[2];
            }
        return true;
    }
    return false;
}

struct Item {
    std::string bus, owner, path, menu;
    PendingCall refresh;
    ChromeTrayItem view{};
    bool visible = false;
    bool dirty = true;
};
struct Notice {
    ChromeNotice view{};
    std::array<std::string, NoticeActionLimit> keys;
    std::string owner;
    Clock::time_point deadline;
};

struct Session {
    explicit Session(DBusConnection* connection) : bus(connection) {}

    DBusConnection* bus;
    std::vector<Item> items;
    std::vector<Notice> notices;
    uint32_t nextId = 1;
    uint32_t menuOwner = 0;
    int32_t menuNode = 0;
    std::vector<ChromeTrayMenuRow> menu;
    ChromeTrayState last{};

    uint32_t Id() { if (!nextId) ++nextId; return nextId++; }

    void Send(Message message)
    {
        if (message) dbus_connection_send(bus, message.get(), nullptr);
    }
    Message Request(Message request)
    {
        return Hold(request ? dbus_connection_send_with_reply_and_block(bus, request.get(), 200, nullptr) : nullptr);
    }
    void Error(DBusMessage* m, const char* reason)
    {
        Send(Hold(dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS, reason)));
    }
    bool Empty(DBusMessage* m)
    {
        Send(Hold(dbus_message_new_method_return(m)));
        return true;
    }
    void SignalItem(const char* name, const Item& item)
    {
        auto m = Hold(dbus_message_new_signal(WatcherPath, Watcher, name));
        const std::string full = item.bus + item.path;
        const char* p = full.c_str();
        if (m && dbus_message_append_args(m.get(), DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID)) Send(std::move(m));
    }

    void Remove(const char* owner)
    {
        for (auto it = items.begin(); it != items.end();) {
            if (it->owner != owner) { ++it; continue; }
            if (menuOwner == it->view.id) CloseMenu();
            SignalItem("StatusNotifierItemUnregistered", *it);
            it = items.erase(it);
        }
    }

    bool RegisterItem(DBusMessage* m)
    {
        const char* supplied = nullptr;
        const char* sender = dbus_message_get_sender(m);
        if (!sender || !dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &supplied, DBUS_TYPE_INVALID) ||
            !supplied || !*supplied || std::strlen(supplied) > 240) {
            Error(m, "Expected a service name or object path");
            return true;
        }
        const bool object = supplied[0] == '/';
        const char* path = object ? supplied : "/StatusNotifierItem";
        const char* name = object ? sender : supplied;
        if (!dbus_validate_path(path, nullptr) || !dbus_validate_bus_name(name, nullptr)) {
            Error(m, "Invalid StatusNotifierItem address");
            return true;
        }
        if (!Same(sender, name)) {
            auto request = Hold(dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner"));
            if (request) dbus_message_append_args(request.get(), DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
            auto reply = Request(std::move(request));
            const char* owner = nullptr;
            if (!reply || !dbus_message_get_args(reply.get(), nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) ||
                !Same(owner, sender)) {
                Error(m, "Service name is not owned by the caller");
                return true;
            }
        }
        for (const auto& item : items)
            if (item.owner == sender && item.path == path) return Empty(m);
        if (items.size() == ItemStorage) { Error(m, "Too many StatusNotifierItems"); return true; }
        Item item;
        item.bus = name;
        item.owner = sender;
        item.path = path;
        item.view.id = Id();
        items.push_back(std::move(item));
        Empty(m);
        SignalItem("StatusNotifierItemRegistered", items.back());
        return true;
    }

    static const char* Signature(const char* key)
    {
        if (Same(key, "RegisteredStatusNotifierItems")) return "as";
        if (Same(key, "IsStatusNotifierHostRegistered")) return "b";
        if (Same(key, "ProtocolVersion")) return "i";
        return nullptr;
    }

    void WatcherProperty(DBusMessageIter& parent, const char* key)
    {
        DBusMessageIter value;
        dbus_message_iter_open_container(&parent, DBUS_TYPE_VARIANT, Signature(key), &value);
        if (Same(key, "RegisteredStatusNotifierItems")) {
            DBusMessageIter array;
            dbus_message_iter_open_container(&value, DBUS_TYPE_ARRAY, "s", &array);
            for (const auto& item : items) {
                const std::string full = item.bus + item.path;
                const char* p = full.c_str();
                dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &p);
            }
            dbus_message_iter_close_container(&value, &array);
        } else if (Same(key, "IsStatusNotifierHostRegistered")) {
            const dbus_bool_t yes = true;
            dbus_message_iter_append_basic(&value, DBUS_TYPE_BOOLEAN, &yes);
        } else {
            const int32_t version = 0;
            dbus_message_iter_append_basic(&value, DBUS_TYPE_INT32, &version);
        }
        dbus_message_iter_close_container(&parent, &value);
    }

    bool WatcherProperties(DBusMessage* m)
    {
        const bool all = dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll");
        const char* iface = nullptr;
        const char* key = nullptr;
        const bool ok = all ?
            dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID) :
            dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
        if (!ok || !Same(iface, Watcher) || (!all && !Signature(key))) {
            Error(m, "Unknown watcher property");
            return true;
        }
        auto reply = Hold(dbus_message_new_method_return(m));
        if (!reply) return true;
        DBusMessageIter output, dict;
        dbus_message_iter_init_append(reply.get(), &output);
        if (!all) WatcherProperty(output, key);
        else {
            dbus_message_iter_open_container(&output, DBUS_TYPE_ARRAY, "{sv}", &dict);
            for (const char* prop : {"RegisteredStatusNotifierItems", "IsStatusNotifierHostRegistered", "ProtocolVersion"}) {
                DBusMessageIter pair;
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &pair);
                dbus_message_iter_append_basic(&pair, DBUS_TYPE_STRING, &prop);
                WatcherProperty(pair, prop);
                dbus_message_iter_close_container(&dict, &pair);
            }
            dbus_message_iter_close_container(&output, &dict);
        }
        Send(std::move(reply));
        return true;
    }

    void RefreshItems()
    {
        for (auto& item : items) {
            if (item.refresh && dbus_pending_call_get_completed(item.refresh.get())) {
                auto reply = Hold(dbus_pending_call_steal_reply(item.refresh.get()));
                item.refresh.reset();
                UpdateItem(item, std::move(reply));
            }
            if (!item.dirty || item.refresh) continue;
            item.dirty = false;
            auto request = Hold(dbus_message_new_method_call(item.owner.c_str(), item.path.c_str(),
                                                            DBUS_INTERFACE_PROPERTIES, "GetAll"));
            const char* interface = ItemInterface;
            DBusPendingCall* pending = nullptr;
            // An app can synchronously ask the bus for another service
            // immediately after registering its item. Keep dispatching
            // those calls while its own GetAll request is outstanding.
            if (request && dbus_message_append_args(request.get(), DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID)
                && dbus_connection_send_with_reply(bus, request.get(), &pending, 2000))
                item.refresh.reset(pending);
        }
    }

    void UpdateItem(Item& item, Message reply)
    {
        DBusMessageIter dict{};
        if (!reply || !dbus_message_has_signature(reply.get(), "a{sv}")
            || !dbus_message_iter_init(reply.get(), &dict)) return;

        ChromeTrayItem view{};
        view.id = item.view.id;
        item.visible = true;
        item.menu.clear();
        std::string name, attention;
        uint32_t attentionPixels[TrayIconWords]{};
        bool hasAttention = false;
        Properties(dict, [&](const char* key, DBusMessageIter value) {
            const char* text = Text(value);
            if (Same(key, "Status")) {
                item.visible = !Same(text, "Passive");
                view.attention = Same(text, "NeedsAttention");
            } else if (Same(key, "Title")) CopyText(view.title, text);
            else if (Same(key, "Id") && !view.title[0]) CopyText(view.title, text);
            else if (Same(key, "IconName") && text) name = text;
            else if (Same(key, "AttentionIconName") && text) attention = text;
            else if (Same(key, "IconPixmap")) view.hasIcon = Pixels(value, view.icon);
            else if (Same(key, "AttentionIconPixmap")) hasAttention = Pixels(value, attentionPixels);
            else if (Same(key, "Menu") && text && dbus_validate_path(text, nullptr)) item.menu = text;
            else if (Same(key, "ToolTip") && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRUCT) {
                auto fields = Into(value);
                dbus_message_iter_next(&fields);
                dbus_message_iter_next(&fields);
                const char* title = Text(fields);
                dbus_message_iter_next(&fields);
                const char* detail = Text(fields);
                CopyText(view.tooltip, detail && *detail ? detail : title);
            }
        });
        if (view.attention) {
            if (!hasAttention) hasAttention = IconName(attention, attentionPixels);
            if (hasAttention) {
                std::copy(std::begin(attentionPixels), std::end(attentionPixels), view.icon);
                view.hasIcon = true;
            }
        }
        if (!view.hasIcon) view.hasIcon = IconName(name, view.icon);
        if (!view.title[0]) CopyText(view.title, item.bus.c_str());
        item.view = view;
        if (!item.visible && menuOwner == view.id) CloseMenu();
    }

    void CloseMenu() { menu.clear(); menuOwner = 0; menuNode = 0; }

    bool FetchMenu(const Item& item, int32_t node)
    {
        if (item.menu.empty() || item.menu == "/") return false;
        auto prepare = Hold(dbus_message_new_method_call(item.owner.c_str(), item.menu.c_str(), MenuInterface, "AboutToShow"));
        if (prepare) {
            dbus_message_append_args(prepare.get(), DBUS_TYPE_INT32, &node, DBUS_TYPE_INVALID);
            Send(std::move(prepare));
        }
        auto msg = Hold(dbus_message_new_method_call(item.owner.c_str(), item.menu.c_str(), MenuInterface, "GetLayout"));
        if (!msg) return false;
        DBusMessageIter append, array;
        dbus_message_iter_init_append(msg.get(), &append);
        const int32_t depth = 1;
        dbus_message_iter_append_basic(&append, DBUS_TYPE_INT32, &node);
        dbus_message_iter_append_basic(&append, DBUS_TYPE_INT32, &depth);
        dbus_message_iter_open_container(&append, DBUS_TYPE_ARRAY, "s", &array);
        dbus_message_iter_close_container(&append, &array);
        auto reply = Request(std::move(msg));
        DBusMessageIter output{};
        if (!reply || !dbus_message_has_signature(reply.get(), "u(ia{sv}av)") ||
            !dbus_message_iter_init(reply.get(), &output) || !dbus_message_iter_next(&output)) return false;
        auto root = Into(output);
        dbus_message_iter_next(&root);
        dbus_message_iter_next(&root);
        std::vector<ChromeTrayMenuRow> rows;
        if (node) {
            ChromeTrayMenuRow back{};
            back.enabled = true;
            back.id = MenuBack;
            CopyText(back.label, "Back");
            rows.push_back(back);
        }
        for (auto child = Into(root); dbus_message_iter_get_arg_type(&child) == DBUS_TYPE_VARIANT && rows.size() < TrayMenuLimit;
             dbus_message_iter_next(&child)) {
            auto v = Into(child);
            if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_STRUCT) continue;
            auto fields = Into(v);
            if (dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_INT32) continue;
            ChromeTrayMenuRow row{};
            row.id = Number(fields);
            row.enabled = true;
            bool visible = true;
            dbus_message_iter_next(&fields);
            Properties(fields, [&](const char* key, DBusMessageIter value) {
                if (Same(key, "label")) CopyText(row.label, Text(value));
                else if (Same(key, "type")) row.separator = Same(Text(value), "separator");
                else if (Same(key, "enabled")) row.enabled = Boolean(value, true);
                else if (Same(key, "visible")) visible = Boolean(value, true);
                else if (Same(key, "children-display")) row.submenu = Same(Text(value), "submenu");
            });
            if (!visible || row.id == MenuBack || (!row.label[0] && !row.separator)) continue;
            size_t at = 0;
            for (size_t j = 0; row.label[j]; ++j) if (row.label[j] != '_') row.label[at++] = row.label[j];
            row.label[at] = 0;
            rows.push_back(row);
        }
        if (rows.empty()) return false;
        menuOwner = item.view.id;
        menuNode = node;
        menu = std::move(rows);
        return true;
    }

    void ItemAction(const Item& item, const char* member, const TrayInput& event)
    {
        auto m = Hold(dbus_message_new_method_call(item.owner.c_str(), item.path.c_str(), ItemInterface, member));
        if (!m) return;
        if (event.kind == TrayInputKind::Scroll) {
            const char* orientation = "vertical";
            dbus_message_append_args(m.get(), DBUS_TYPE_INT32, &event.argument, DBUS_TYPE_STRING, &orientation, DBUS_TYPE_INVALID);
        } else dbus_message_append_args(m.get(), DBUS_TYPE_INT32, &event.x, DBUS_TYPE_INT32, &event.y, DBUS_TYPE_INVALID);
        dbus_message_set_no_reply(m.get(), true);
        Send(std::move(m));
    }

    void MenuSelect(const Item& item, int32_t id)
    {
        const auto it = std::find_if(menu.begin(), menu.end(), [id](const auto& row) { return row.id == id; });
        if (menuOwner != item.view.id || it == menu.end() || !it->enabled || it->separator) return;
        if (it->submenu || id == MenuBack) {
            if (!FetchMenu(item, id == MenuBack ? 0 : id)) CloseMenu();
            return;
        }
        auto msg = Hold(dbus_message_new_method_call(item.owner.c_str(), item.menu.c_str(), MenuInterface, "Event"));
        if (msg) {
            DBusMessageIter out, data;
            dbus_message_iter_init_append(msg.get(), &out);
            const char* click = "clicked";
            const int32_t zero = 0;
            const uint32_t timestamp = 0;
            dbus_message_iter_append_basic(&out, DBUS_TYPE_INT32, &id);
            dbus_message_iter_append_basic(&out, DBUS_TYPE_STRING, &click);
            dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "i", &data);
            dbus_message_iter_append_basic(&data, DBUS_TYPE_INT32, &zero);
            dbus_message_iter_close_container(&out, &data);
            dbus_message_iter_append_basic(&out, DBUS_TYPE_UINT32, &timestamp);
            dbus_message_set_no_reply(msg.get(), true);
            Send(std::move(msg));
        }
        CloseMenu();
    }

    void Closed(const Notice& notice, uint32_t reason)
    {
        auto msg = Hold(dbus_message_new_signal(NotificationsPath, Notifications, "NotificationClosed"));
        if (msg && dbus_message_set_destination(msg.get(), notice.owner.c_str()) &&
            dbus_message_append_args(msg.get(), DBUS_TYPE_UINT32, &notice.view.id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID))
            Send(std::move(msg));
    }

    void CloseNotice(uint32_t id, uint32_t reason, const char* owner = nullptr)
    {
        for (auto it = notices.begin(); it != notices.end(); ++it) {
            if (it->view.id != id || (owner && it->owner != owner)) continue;
            Closed(*it, reason);
            notices.erase(it);
            return;
        }
    }

    bool Notify(DBusMessage* m)
    {
        const char* sender = dbus_message_get_sender(m);
        DBusMessageIter fields;
        if (!sender || !dbus_message_has_signature(m, "susssasa{sv}i") || !dbus_message_iter_init(m, &fields)) {
            Error(m, "Invalid Notify arguments");
            return true;
        }
        Notice notice;
        notice.owner = sender;
        CopyText(notice.view.app, Text(fields));
        dbus_message_iter_next(&fields);
        uint32_t replace;
        dbus_message_iter_get_basic(&fields, &replace);
        dbus_message_iter_next(&fields); // app_icon
        dbus_message_iter_next(&fields);
        CopyText(notice.view.title, Text(fields));
        dbus_message_iter_next(&fields);
        CopyText(notice.view.body, Text(fields));
        dbus_message_iter_next(&fields);
        for (auto a = Into(fields); Text(a) && notice.view.actionCount < NoticeActionLimit;) {
            const char* key = Text(a);
            if (!dbus_message_iter_next(&a)) break;
            const char* label = Text(a);
            if (label && *label && *key && std::strlen(key) < 128) {
                const unsigned n = notice.view.actionCount++;
                notice.keys[n] = key;
                CopyText(notice.view.actions[n], label);
            }
            dbus_message_iter_next(&a);
        }
        dbus_message_iter_next(&fields); // hints
        dbus_message_iter_next(&fields);
        const auto timeout = Number(fields);
        notice.deadline = timeout == 0 ? Clock::time_point::max() :
            Clock::now() + std::chrono::milliseconds(timeout < 0 ? 6000 : timeout);
        auto it = std::find_if(notices.begin(), notices.end(), [&](const auto& n) {
            return n.view.id == replace && n.owner == sender;
        });
        const uint32_t id = notice.view.id = it == notices.end() ? Id() : replace;
        if (it == notices.end()) {
            if (notices.size() == NoticeStorage) {
                Closed(notices.front(), 4);
                notices.erase(notices.begin());
            }
            notices.push_back(std::move(notice));
        } else *it = std::move(notice);
        auto reply = Hold(dbus_message_new_method_return(m));
        if (reply && dbus_message_append_args(reply.get(), DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID))
            Send(std::move(reply));
        return true;
    }

    bool NotifyMethod(DBusMessage* m)
    {
        if (dbus_message_is_method_call(m, Notifications, "Notify")) return Notify(m);
        if (dbus_message_is_method_call(m, Notifications, "CloseNotification")) {
            uint32_t id;
            if (!dbus_message_get_args(m, nullptr, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) Error(m, "Expected notification id");
            else { CloseNotice(id, 3, dbus_message_get_sender(m)); Empty(m); }
            return true;
        }
        if (dbus_message_is_method_call(m, Notifications, "GetCapabilities")) {
            auto reply = Hold(dbus_message_new_method_return(m));
            if (reply) {
                DBusMessageIter out, array;
                dbus_message_iter_init_append(reply.get(), &out);
                dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "s", &array);
                for (const char* name : {"body", "actions"})
                    dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &name);
                dbus_message_iter_close_container(&out, &array);
                Send(std::move(reply));
            }
            return true;
        }
        if (dbus_message_is_method_call(m, Notifications, "GetServerInformation")) {
            auto reply = Hold(dbus_message_new_method_return(m));
            const char* name = "Clearwater", *vendor = "Brook", *version = "1", *spec = "1.2";
            if (reply && dbus_message_append_args(reply.get(), DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor,
                                                  DBUS_TYPE_STRING, &version, DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID))
                Send(std::move(reply));
            return true;
        }
        return false;
    }

    void Input(const TrayInput& e)
    {
        if (e.kind == TrayInputKind::NoticeAction || e.kind == TrayInputKind::CloseNotice) {
            if (notices.empty() || notices.front().view.id != e.owner) return;
            if (e.kind == TrayInputKind::NoticeAction) {
                const auto& n = notices.front();
                if (e.argument < 0 || static_cast<uint32_t>(e.argument) >= n.view.actionCount) return;
                const char* key = n.keys[e.argument].c_str();
                auto msg = Hold(dbus_message_new_signal(NotificationsPath, Notifications, "ActionInvoked"));
                if (msg && dbus_message_set_destination(msg.get(), n.owner.c_str()) &&
                    dbus_message_append_args(msg.get(), DBUS_TYPE_UINT32, &e.owner, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID))
                    Send(std::move(msg));
            }
            CloseNotice(e.owner, 2);
            return;
        }
        const auto item = std::find_if(items.begin(), items.end(), [&](const auto& i) { return i.view.id == e.owner && i.visible; });
        if (item == items.end()) return;
        switch (e.kind) {
        case TrayInputKind::Activate: ItemAction(*item, "Activate", e); break;
        case TrayInputKind::Context:
            if (!FetchMenu(*item, 0)) ItemAction(*item, "ContextMenu", e);
            break;
        case TrayInputKind::Scroll: ItemAction(*item, "Scroll", e); break;
        case TrayInputKind::MenuSelect: MenuSelect(*item, e.argument); break;
        case TrayInputKind::DismissMenu: if (menuOwner == e.owner) CloseMenu(); break;
        default: break;
        }
    }

    bool Publish(TrayPublish publish)
    {
        ChromeTrayState state{};
        for (const auto& item : items) {
            if (item.visible && state.itemCount < TrayItemLimit)
                state.items[state.itemCount++] = item.view;
        }
        if (menuOwner && std::any_of(state.items, state.items + state.itemCount,
                                     [&](const auto& i) { return i.id == menuOwner; })) {
            state.menuOwner = menuOwner;
            state.menuCount = menu.size();
            std::copy(menu.begin(), menu.end(), state.menu);
        }
        if (!notices.empty()) state.notice = notices.front().view;
        if (!std::memcmp(&last, &state, sizeof(state))) return true;
        if (!publish(state)) return false;
        last = state;
        return true;
    }

    bool Dispatch(DBusMessage* m)
    {
        if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
            Same(dbus_message_get_sender(m), DBUS_SERVICE_DBUS)) {
            const char *name, *old, *current;
            if (dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old,
                                      DBUS_TYPE_STRING, &current, DBUS_TYPE_INVALID) &&
                *name == ':' && *old && !*current) Remove(old);
            return true;
        }
        if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_SIGNAL) {
            const char* interface = dbus_message_get_interface(m);
            if (Same(interface, ItemInterface) || Same(interface, DBUS_INTERFACE_PROPERTIES))
                for (auto& item : items)
                    if (Same(dbus_message_get_sender(m), item.owner.c_str()) &&
                        Same(dbus_message_get_path(m), item.path.c_str())) item.dirty = true;
            if (menuOwner && Same(interface, MenuInterface))
                for (const auto& item : items)
                    if (item.view.id == menuOwner && Same(dbus_message_get_sender(m), item.owner.c_str()) &&
                        Same(dbus_message_get_path(m), item.menu.c_str()))
                        if (!FetchMenu(item, menuNode)) CloseMenu();
            return false;
        }
        if (dbus_message_has_path(m, WatcherPath)) {
            if (dbus_message_is_method_call(m, Watcher, "RegisterStatusNotifierItem")) return RegisterItem(m);
            if (dbus_message_is_method_call(m, Watcher, "RegisterStatusNotifierHost")) return Empty(m);
            if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Get") ||
                dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) return WatcherProperties(m);
        }
        if (dbus_message_has_path(m, NotificationsPath)) return NotifyMethod(m);
        return false;
    }
};

DBusHandlerResult Dispatch(DBusConnection*, DBusMessage* message, void* raw)
{
    return static_cast<Session*>(raw)->Dispatch(message) ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

} // namespace

int RunSessionServices(TrayPublish publish, TrayReadInput readInput, int parent)
{
    while (parent == 0 || getppid() == parent) {
        DBusConnection* connection = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
        if (!connection) { usleep(250000); continue; }
        dbus_connection_set_exit_on_disconnect(connection, false);
        dbus_connection_set_max_message_size(connection, 1024 * 1024);
        dbus_connection_set_max_received_size(connection, 2 * 1024 * 1024);
        Session service(connection);
        if (!dbus_connection_add_filter(connection, Dispatch, &service, nullptr)) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            return 1;
        }
        for (const char* name : {Watcher, "org.kde.StatusNotifierHost-brook", Notifications})
            dbus_bus_request_name(connection, name, DBUS_NAME_FLAG_DO_NOT_QUEUE, nullptr);
        for (const char* rule : {"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                                  "type='signal',interface='org.kde.StatusNotifierItem'",
                                  "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                                  "type='signal',interface='com.canonical.dbusmenu',member='LayoutUpdated'"})
            dbus_bus_add_match(connection, rule, nullptr);
        bool stopping = false;
        while (dbus_connection_read_write_dispatch(connection, 50)) {
            if (parent && getppid() != parent) { stopping = true; break; }
            service.RefreshItems();
            for (unsigned i = 0; i < 32; ++i) {
                TrayInput input{};
                const int status = readInput(input);
                if (status < 0) { stopping = true; break; }
                if (!status) break;
                service.Input(input);
            }
            for (auto it = service.notices.begin(); it != service.notices.end();) {
                if (it->deadline > Clock::now()) { ++it; continue; }
                service.Closed(*it, 1);
                it = service.notices.erase(it);
            }
            if (stopping || !service.Publish(publish)) { stopping = true; break; }
        }
        dbus_connection_remove_filter(connection, Dispatch, &service);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        if (stopping || !publish({})) return 0;
        usleep(250000);
    }
    return 0;
}

} // namespace brook
