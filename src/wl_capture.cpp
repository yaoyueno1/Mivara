/**
 * WL_CAPTURE.CPP - xdg-desktop-portal ScreenCast + PipeWire (see wl_capture.h)
 *
 * Portal (session bus, org.freedesktop.portal.Desktop):
 *   CreateSession → SelectSources → Start → OpenPipeWireRemote
 * The Start call is what raises the share dialog. MIVARA_PORTAL_WAIT_MS
 * (default 60000) bounds that wait so a missing portal cannot hang the
 * sender forever.
 *
 * PipeWire: the remote fd is connected with pw_context_connect_fd (it owns
 * the fd). The stream targets the portal node by PW_KEY_TARGET_OBJECT, not
 * by the deprecated numeric target id.
 */
#include "wl_capture.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

std::string scrub(std::string s)
{
    for (char &c : s) {
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

int portalWaitMs()
{
    const char *e = std::getenv("MIVARA_PORTAL_WAIT_MS");
    if (!e || !e[0]) return 60000;
    int n = std::atoi(e);
    if (n < 200) n = 200;
    if (n > 180000) n = 180000;
    return n;
}

}  // namespace

#ifndef MIVARA_HAS_PIPEWIRE

bool wlCaptureBuilt() { return false; }

bool wlCaptureStart(const WlCaptureRequest &, WlCaptureInfo &, std::string &err)
{
    err = "Wayland capture was not built (install libpipewire-0.3 and "
          "libdbus-1 development packages, then rebuild)";
    return false;
}

bool wlCaptureFrame(uint8_t *, int, int) { return false; }

void wlCaptureShutdown() {}

#else

extern "C" {
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <dbus/dbus.h>
}

namespace {

struct StreamChoice {
    uint32_t node = 0;
    int w = 0;
    int h = 0;
};

struct PortalResult {
    uint32_t code = 2;
    std::string session;
    std::vector<StreamChoice> streams;
};

struct WaitState {
    std::string path;
    bool got = false;
    DBusMessage *msg = nullptr;
};

struct PwState {
    std::mutex mu;
    int w = 0;
    int h = 0;
    uint32_t format = SPA_VIDEO_FORMAT_UNKNOWN;
    std::vector<uint8_t> rgb;
    bool have = false;
    std::string error;
    std::atomic<bool> streaming{false};
};

PwState            g_pw;
pw_thread_loop    *g_loop = nullptr;
pw_context        *g_ctx = nullptr;
pw_core           *g_core = nullptr;
pw_stream         *g_stream = nullptr;
DBusConnection    *g_bus = nullptr;
std::string        g_session_path;
bool               g_pw_inited = false;

void dbusAppendEntry(DBusMessageIter *dict, const char *key, int type, const void *val)
{
    DBusMessageIter ent, var;
    const char *k = key;
    char sig[2] = { (char)type, 0 };
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &ent);
    dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, sig, &var);
    dbus_message_iter_append_basic(&var, type, val);
    dbus_message_iter_close_container(&ent, &var);
    dbus_message_iter_close_container(dict, &ent);
}

void dbusAppendString(DBusMessageIter *dict, const char *key, const char *val)
{
    const char *v = val;
    dbusAppendEntry(dict, key, DBUS_TYPE_STRING, &v);
}

void dbusAppendU32(DBusMessageIter *dict, const char *key, uint32_t val)
{
    dbus_uint32_t v = val;
    dbusAppendEntry(dict, key, DBUS_TYPE_UINT32, &v);
}

void dbusAppendBool(DBusMessageIter *dict, const char *key, bool val)
{
    dbus_bool_t v = val ? 1 : 0;
    dbusAppendEntry(dict, key, DBUS_TYPE_BOOLEAN, &v);
}

std::string senderToken(DBusConnection *bus)
{
    const char *u = dbus_bus_get_unique_name(bus);
    std::string s;
    if (!u) return "mivara";
    for (const char *p = u; *p; ++p) {
        if (*p == ':') continue;
        s.push_back(*p == '.' ? '_' : *p);
    }
    return s;
}

DBusHandlerResult onPortalSignal(DBusConnection *, DBusMessage *msg, void *user)
{
    WaitState *w = static_cast<WaitState *>(user);
    if (!dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *path = dbus_message_get_path(msg);
    if (!path || w->path != path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (!w->got) {
        w->msg = dbus_message_ref(msg);
        w->got = true;
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

void parseStreamDict(DBusMessageIter *dict, StreamChoice &s)
{
    while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, var;
        dbus_message_iter_recurse(dict, &ent);
        const char *key = nullptr;
        if (dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&ent, &key);
        dbus_message_iter_next(&ent);
        if (key && std::strcmp(key, "size") == 0 &&
            dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&ent, &var);
            if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRUCT) {
                DBusMessageIter st;
                dbus_message_iter_recurse(&var, &st);
                dbus_int32_t w = 0, h = 0;
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(&st, &w);
                    dbus_message_iter_next(&st);
                    if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32)
                        dbus_message_iter_get_basic(&st, &h);
                }
                s.w = (int)w;
                s.h = (int)h;
            }
        }
        dbus_message_iter_next(dict);
    }
}

void parseResults(DBusMessageIter *dict, PortalResult &r)
{
    while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, var;
        dbus_message_iter_recurse(dict, &ent);
        const char *key = nullptr;
        if (dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&ent, &key);
        dbus_message_iter_next(&ent);
        if (!key || dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(dict);
            continue;
        }
        dbus_message_iter_recurse(&ent, &var);
        int vt = dbus_message_iter_get_arg_type(&var);
        if (std::strcmp(key, "session_handle") == 0 &&
            (vt == DBUS_TYPE_OBJECT_PATH || vt == DBUS_TYPE_STRING)) {
            const char *path = nullptr;
            dbus_message_iter_get_basic(&var, &path);
            if (path) r.session = path;
        } else if (std::strcmp(key, "streams") == 0 && vt == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr;
            dbus_message_iter_recurse(&var, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
                DBusMessageIter st, props;
                dbus_message_iter_recurse(&arr, &st);
                StreamChoice s;
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_UINT32) {
                    dbus_uint32_t node = 0;
                    dbus_message_iter_get_basic(&st, &node);
                    s.node = node;
                }
                dbus_message_iter_next(&st);
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&st, &props);
                    parseStreamDict(&props, s);
                }
                if (s.node) r.streams.push_back(s);
                dbus_message_iter_next(&arr);
            }
        }
        dbus_message_iter_next(dict);
    }
}

bool parseResponse(DBusMessage *msg, PortalResult &r, std::string &err)
{
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) {
        err = "empty portal response";
        return false;
    }
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
        err = "portal response has no status code";
        return false;
    }
    dbus_uint32_t code = 2;
    dbus_message_iter_get_basic(&it, &code);
    r.code = code;
    if (!dbus_message_iter_next(&it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
        return true;
    }
    DBusMessageIter dict;
    dbus_message_iter_recurse(&it, &dict);
    parseResults(&dict, r);
    return true;
}

bool waitResponse(DBusConnection *bus, const std::string &path, int timeout_ms,
                  PortalResult &r, std::string &err)
{
    WaitState w;
    w.path = path;
    dbus_connection_add_filter(bus, onPortalSignal, &w, nullptr);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (!w.got) {
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        if (left > 200) left = 200;
        if (!dbus_connection_read_write(bus, left)) break;
        while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
    }
    dbus_connection_remove_filter(bus, onPortalSignal, &w);
    if (!w.got || !w.msg) {
        err = "portal request timed out";
        return false;
    }
    bool ok = parseResponse(w.msg, r, err);
    dbus_message_unref(w.msg);
    if (!ok) return false;
    if (r.code == 1) {
        err = "portal dialog cancelled";
        return false;
    }
    if (r.code != 0) {
        err = "portal request failed (code " + std::to_string(r.code) + ")";
        return false;
    }
    return true;
}

bool portalCall(DBusConnection *bus, const char *method, const char *token,
                void (*fill)(DBusMessageIter *, void *), void *user,
                int timeout_ms, PortalResult &r, std::string &err)
{
    const std::string path =
        "/org/freedesktop/portal/desktop/request/" + senderToken(bus) + "/" + token;
    std::string rule =
        "type='signal',interface='org.freedesktop.portal.Request',"
        "member='Response',path='" + path + "'";
    DBusError de;
    dbus_error_init(&de);
    dbus_bus_add_match(bus, rule.c_str(), &de);
    if (dbus_error_is_set(&de)) {
        err = std::string("dbus match: ") + de.message;
        dbus_error_free(&de);
        return false;
    }
    dbus_connection_flush(bus);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        method);
    if (!msg) {
        err = "out of memory building portal call";
        dbus_bus_remove_match(bus, rule.c_str(), nullptr);
        return false;
    }
    DBusMessageIter args;
    dbus_message_iter_init_append(msg, &args);
    fill(&args, user);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, 8000, &de);
    dbus_message_unref(msg);
    if (!reply) {
        err = std::string("portal ") + method + ": " +
              (dbus_error_is_set(&de) ? de.message : "no reply");
        if (dbus_error_is_set(&de)) dbus_error_free(&de);
        dbus_bus_remove_match(bus, rule.c_str(), nullptr);
        return false;
    }
    const char *got = nullptr;
    if (!dbus_message_get_args(reply, &de, DBUS_TYPE_OBJECT_PATH, &got, DBUS_TYPE_INVALID) ||
        !got) {
        err = std::string("portal ") + method + " returned no request path";
        dbus_message_unref(reply);
        if (dbus_error_is_set(&de)) dbus_error_free(&de);
        dbus_bus_remove_match(bus, rule.c_str(), nullptr);
        return false;
    }
    const std::string wait_path = got;
    dbus_message_unref(reply);

    /* The spec path is the one we subscribed to. If the portal picked a
     * different request object, subscribe to that too before waiting. */
    std::string rule2;
    if (wait_path != path) {
        rule2 = "type='signal',interface='org.freedesktop.portal.Request',"
                "member='Response',path='" + wait_path + "'";
        dbus_bus_add_match(bus, rule2.c_str(), nullptr);
        dbus_connection_flush(bus);
    }
    bool ok = waitResponse(bus, wait_path, timeout_ms, r, err);
    dbus_bus_remove_match(bus, rule.c_str(), nullptr);
    if (!rule2.empty()) dbus_bus_remove_match(bus, rule2.c_str(), nullptr);
    return ok;
}

struct CreateArgs { const char *handle; const char *session; };
struct SelectArgs {
    const char *session;
    const char *handle;
    uint32_t types;
    bool multiple;
};
struct StartArgs { const char *session; const char *handle; };

void fillCreate(DBusMessageIter *args, void *user)
{
    CreateArgs *a = static_cast<CreateArgs *>(user);
    DBusMessageIter dict;
    dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbusAppendString(&dict, "handle_token", a->handle);
    dbusAppendString(&dict, "session_handle_token", a->session);
    dbus_message_iter_close_container(args, &dict);
}

void fillSelect(DBusMessageIter *args, void *user)
{
    SelectArgs *a = static_cast<SelectArgs *>(user);
    const char *session = a->session;
    dbus_message_iter_append_basic(args, DBUS_TYPE_OBJECT_PATH, &session);
    DBusMessageIter dict;
    dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbusAppendString(&dict, "handle_token", a->handle);
    dbusAppendU32(&dict, "types", a->types);
    dbusAppendBool(&dict, "multiple", a->multiple);
    dbusAppendU32(&dict, "cursor_mode", 2);   /* embedded: compositor draws the pointer */
    dbusAppendU32(&dict, "persist_mode", 0);
    dbus_message_iter_close_container(args, &dict);
}

void fillStart(DBusMessageIter *args, void *user)
{
    StartArgs *a = static_cast<StartArgs *>(user);
    const char *session = a->session;
    const char *parent = "";
    dbus_message_iter_append_basic(args, DBUS_TYPE_OBJECT_PATH, &session);
    dbus_message_iter_append_basic(args, DBUS_TYPE_STRING, &parent);
    DBusMessageIter dict;
    dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbusAppendString(&dict, "handle_token", a->handle);
    dbus_message_iter_close_container(args, &dict);
}

void closeSession()
{
    if (!g_bus || g_session_path.empty()) return;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        g_session_path.c_str(),
        "org.freedesktop.portal.Session",
        "Close");
    if (!msg) return;
    dbus_connection_send(g_bus, msg, nullptr);
    dbus_connection_flush(g_bus);
    dbus_message_unref(msg);
    g_session_path.clear();
}

int openPipeWireFd(DBusConnection *bus, const std::string &session, std::string &err)
{
    DBusError de;
    dbus_error_init(&de);
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "OpenPipeWireRemote");
    if (!msg) {
        err = "out of memory opening pipewire remote";
        return -1;
    }
    DBusMessageIter args, dict;
    const char *s = session.c_str();
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &s);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&args, &dict);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, 8000, &de);
    dbus_message_unref(msg);
    if (!reply) {
        err = std::string("OpenPipeWireRemote: ") +
              (dbus_error_is_set(&de) ? de.message : "no reply");
        if (dbus_error_is_set(&de)) dbus_error_free(&de);
        return -1;
    }
    int fd = -1;
    if (!dbus_message_get_args(reply, &de, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) ||
        fd < 0) {
        err = std::string("OpenPipeWireRemote fd: ") +
              (dbus_error_is_set(&de) ? de.message : "missing");
        if (dbus_error_is_set(&de)) dbus_error_free(&de);
        dbus_message_unref(reply);
        return -1;
    }
    dbus_message_unref(reply);
    return fd;
}

void onState(void *data, enum pw_stream_state, enum pw_stream_state state, const char *error)
{
    PwState *st = static_cast<PwState *>(data);
    if (state == PW_STREAM_STATE_STREAMING) st->streaming = true;
    if (state == PW_STREAM_STATE_ERROR && error) {
        std::lock_guard<std::mutex> lk(st->mu);
        st->error = error;
    }
}

void onParam(void *data, uint32_t id, const struct spa_pod *param)
{
    PwState *st = static_cast<PwState *>(data);
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_video_info_raw info;
    std::memset(&info, 0, sizeof(info));
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    if (info.size.width <= 0 || info.size.height <= 0) return;
    if (info.size.width > 8192 || info.size.height > 8192) {
        std::lock_guard<std::mutex> lk(st->mu);
        st->error = "portal frame is larger than 8192";
        return;
    }
    std::lock_guard<std::mutex> lk(st->mu);
    st->w = (int)info.size.width;
    st->h = (int)info.size.height;
    st->format = info.format;
    st->rgb.assign((size_t)st->w * (size_t)st->h * 3u, 0);
    st->have = false;
}

void packRgb(PwState *st, const uint8_t *src, int stride)
{
    const int w = st->w, h = st->h;
    uint8_t *dst = st->rgb.data();
    const uint32_t fmt = st->format;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * (size_t)stride;
        uint8_t *out = dst + (size_t)y * (size_t)w * 3u;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + x * 4;
            if (fmt == SPA_VIDEO_FORMAT_RGBx || fmt == SPA_VIDEO_FORMAT_RGBA) {
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
            } else {
                /* BGRx / BGRA and anything else 32-bit we accepted as B,G,R. */
                out[0] = p[2]; out[1] = p[1]; out[2] = p[0];
            }
            out += 3;
        }
    }
    st->have = true;
}

void onProcess(void *data)
{
    PwState *st = static_cast<PwState *>(data);
    pw_buffer *buf = pw_stream_dequeue_buffer(g_stream);
    if (!buf) return;
    spa_buffer *b = buf->buffer;
    spa_data *d = b ? &b->datas[0] : nullptr;
    if (d && d->data && d->chunk && d->chunk->size > 0) {
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->w > 0 && st->h > 0 && st->rgb.size() == (size_t)st->w * (size_t)st->h * 3u) {
            int stride = d->chunk->stride;
            if (stride <= 0) stride = st->w * 4;
            const uint8_t *src = static_cast<uint8_t *>(d->data) + d->chunk->offset;
            const size_t need = (size_t)stride * (size_t)st->h;
            const size_t avail = d->maxsize
                                     ? (size_t)d->maxsize
                                     : (size_t)d->chunk->offset + (size_t)d->chunk->size;
            if (stride >= st->w * 4 &&
                (size_t)d->chunk->offset + need <= avail)
                packRgb(st, src, stride);
        }
    }
    pw_stream_queue_buffer(g_stream, buf);
}

/* spa_hook must outlive the stream. */
spa_hook g_hook;

bool connectStreamReal(int fd, uint32_t node, std::string &err)
{
    if (!g_pw_inited) {
        pw_init(nullptr, nullptr);
        g_pw_inited = true;
    }
    g_loop = pw_thread_loop_new("mivara-pw", nullptr);
    if (!g_loop) {
        err = "pw_thread_loop_new failed";
        ::close(fd);
        return false;
    }
    pw_thread_loop_lock(g_loop);
    g_ctx = pw_context_new(pw_thread_loop_get_loop(g_loop), nullptr, 0);
    if (!g_ctx) {
        pw_thread_loop_unlock(g_loop);
        err = "pw_context_new failed";
        ::close(fd);
        return false;
    }
    g_core = pw_context_connect_fd(g_ctx, fd, nullptr, 0);
    if (!g_core) {
        err = std::string("pipewire connect failed: ") + std::strerror(errno);
        pw_thread_loop_unlock(g_loop);
        return false;
    }

    char node_buf[32];
    std::snprintf(node_buf, sizeof(node_buf), "%u", node);
    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_TARGET_OBJECT, node_buf,
        nullptr);
    static const pw_stream_events ev = []() {
        pw_stream_events e = {};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = onState;
        e.param_changed = onParam;
        e.process = onProcess;
        return e;
    }();
    g_stream = pw_stream_new(g_core, "mivara-capture", props);
    if (!g_stream) {
        pw_thread_loop_unlock(g_loop);
        err = "pw_stream_new failed";
        return false;
    }
    std::memset(&g_hook, 0, sizeof(g_hook));
    pw_stream_add_listener(g_stream, &g_hook, &ev, &g_pw);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_rectangle def = SPA_RECTANGLE(320, 240);
    spa_rectangle minr = SPA_RECTANGLE(1, 1);
    spa_rectangle maxr = SPA_RECTANGLE(8192, 8192);
    spa_fraction z = SPA_FRACTION(0, 1);
    spa_fraction maxf = SPA_FRACTION(240, 1);
    spa_pod_builder_add(&b,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &minr, &maxr),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&z, &z, &maxf),
        0);
    const spa_pod *param = static_cast<const spa_pod *>(spa_pod_builder_pop(&b, &f));
    int rc = pw_stream_connect(g_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                               (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                 PW_STREAM_FLAG_MAP_BUFFERS),
                               &param, 1);
    pw_thread_loop_unlock(g_loop);
    if (rc < 0) {
        err = std::string("pw_stream_connect failed: ") + std::strerror(-rc);
        return false;
    }
    pw_thread_loop_start(g_loop);
    return true;
}

const StreamChoice *pickStream(const std::vector<StreamChoice> &streams,
                               const WlCaptureRequest &req)
{
    if (streams.empty()) return nullptr;
    if (req.want_w > 0 && req.want_h > 0) {
        for (const auto &s : streams)
            if (s.w == req.want_w && s.h == req.want_h) return &s;
        return nullptr;
    }
    for (const auto &s : streams)
        if (s.w > 0 && s.h > 0) return &s;
    return &streams[0];
}

}  // namespace

bool wlCaptureBuilt() { return true; }

bool wlCaptureStart(const WlCaptureRequest &req, WlCaptureInfo &info, std::string &err)
{
    wlCaptureShutdown();
    info = WlCaptureInfo{};

    DBusError de;
    dbus_error_init(&de);
    g_bus = dbus_bus_get(DBUS_BUS_SESSION, &de);
    if (!g_bus) {
        err = std::string("session bus: ") +
              (dbus_error_is_set(&de) ? de.message : "unavailable");
        if (dbus_error_is_set(&de)) dbus_error_free(&de);
        err = scrub(err);
        return false;
    }

    const std::string pid = std::to_string((int)getpid());
    CreateArgs ca;
    ca.handle = "mivara_h1";
    std::string session_tok = "mivara_s" + pid;
    ca.session = session_tok.c_str();
    /* handle tokens must be unique per call. Bake the pid into each. */
    std::string h1 = "mivara" + pid + "a";
    std::string h2 = "mivara" + pid + "b";
    std::string h3 = "mivara" + pid + "c";
    ca.handle = h1.c_str();

    PortalResult created;
    if (!portalCall(g_bus, "CreateSession", h1.c_str(), fillCreate, &ca, 5000, created, err)) {
        err = scrub(err);
        wlCaptureShutdown();
        return false;
    }
    if (created.session.empty()) {
        err = "portal CreateSession returned no session";
        wlCaptureShutdown();
        return false;
    }
    g_session_path = created.session;

    SelectArgs sa;
    sa.session = g_session_path.c_str();
    sa.handle = h2.c_str();
    sa.types = req.include_virtual ? (1u | 4u) : 1u;  /* monitor, plus virtual when extending */
    sa.multiple = req.multiple;
    PortalResult selected;
    if (!portalCall(g_bus, "SelectSources", h2.c_str(), fillSelect, &sa, 5000, selected, err)) {
        err = scrub(err);
        wlCaptureShutdown();
        return false;
    }

    std::cerr << "Wayland: waiting for the screen-share dialog "
                 "(xdg-desktop-portal)...\n";
    StartArgs sta;
    sta.session = g_session_path.c_str();
    sta.handle = h3.c_str();
    PortalResult started;
    if (!portalCall(g_bus, "Start", h3.c_str(), fillStart, &sta, portalWaitMs(), started, err)) {
        err = scrub(err);
        wlCaptureShutdown();
        return false;
    }
    const StreamChoice *chosen = pickStream(started.streams, req);
    if (!chosen) {
        if (req.want_w > 0) {
            err = "portal stream does not match the virtual output " +
                  std::to_string(req.want_w) + "x" + std::to_string(req.want_h) +
                  " (refusing to stream another monitor as extend)";
        } else {
            err = "portal returned no screen stream";
        }
        wlCaptureShutdown();
        return false;
    }

    int fd = openPipeWireFd(g_bus, g_session_path, err);
    if (fd < 0) {
        err = scrub(err);
        wlCaptureShutdown();
        return false;
    }
    if (!connectStreamReal(fd, chosen->node, err)) {
        err = scrub(err);
        wlCaptureShutdown();
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        bool have = false, bad = false;
        int fw = 0, fh = 0;
        std::string pwerr;
        {
            std::lock_guard<std::mutex> lk(g_pw.mu);
            bad = !g_pw.error.empty();
            pwerr = g_pw.error;
            have = g_pw.have;
            fw = g_pw.w;
            fh = g_pw.h;
        }
        if (bad) {
            err = scrub(pwerr);
            wlCaptureShutdown();
            return false;
        }
        if (have && fw > 0 && fh > 0) {
            info.width = fw;
            info.height = fh;
            info.node_id = chosen->node;
            if (req.want_w > 0 && (fw != req.want_w || fh != req.want_h)) {
                err = "PipeWire frame " + std::to_string(fw) + "x" +
                      std::to_string(fh) + " does not match the virtual output";
                wlCaptureShutdown();
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    err = "PipeWire produced no frame";
    wlCaptureShutdown();
    return false;
}

bool wlCaptureFrame(uint8_t *rgb, int w, int h)
{
    if (!rgb || w <= 0 || h <= 0) return false;
    std::lock_guard<std::mutex> lk(g_pw.mu);
    if (!g_pw.have || g_pw.w != w || g_pw.h != h) return false;
    std::memcpy(rgb, g_pw.rgb.data(), g_pw.rgb.size());
    return true;
}

void wlCaptureShutdown()
{
    if (g_loop) pw_thread_loop_lock(g_loop);
    if (g_stream) {
        pw_stream_destroy(g_stream);
        g_stream = nullptr;
    }
    if (g_core) {
        pw_core_disconnect(g_core);
        g_core = nullptr;
    }
    if (g_ctx) {
        pw_context_destroy(g_ctx);
        g_ctx = nullptr;
    }
    if (g_loop) {
        pw_thread_loop_unlock(g_loop);
        pw_thread_loop_stop(g_loop);
        pw_thread_loop_destroy(g_loop);
        g_loop = nullptr;
    }
    closeSession();
    if (g_bus) {
        dbus_connection_unref(g_bus);
        g_bus = nullptr;
    }
    if (g_pw_inited) {
        pw_deinit();
        g_pw_inited = false;
    }
    std::lock_guard<std::mutex> lk(g_pw.mu);
    g_pw.have = false;
    g_pw.rgb.clear();
    g_pw.w = g_pw.h = 0;
}

#ifdef WL_CAPTURE_TEST
int main()
{
    WlCaptureRequest req;
    WlCaptureInfo info;
    std::string err;
    const bool ok = wlCaptureStart(req, info, err);
    std::cout << (ok ? "ok " : "fail ") << info.width << "x" << info.height
              << " " << err << "\n";
    wlCaptureShutdown();
    return ok ? 0 : 1;
}
#endif

#endif  /* MIVARA_HAS_PIPEWIRE */
