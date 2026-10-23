/**
 * VDD_LINUX.CPP - userspace virtual output for Linux extend mode.
 *
 * See vdd_linux.h. Commands are exec'd with an argument vector, never a
 * shell, and output names are restricted to a token charset before they
 * are passed through. A CVT modeline is generated in-process so `cvt`
 * does not have to be installed.
 */
#include "vdd_linux.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Modeline {
    int hdisp = 0, hss = 0, hse = 0, htot = 0;
    int vdisp = 0, vss = 0, vse = 0, vtot = 0;
    double pclk = 0;
};

struct Output {
    std::string name;
    bool connected = false;
    bool primary = false;
    bool has_geom = false;
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<std::string> modes;
};

struct MonRect {
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
};

int runArgv(const std::vector<std::string> &args, std::string &out)
{
    out.clear();
    if (args.empty()) return 127;
    int pipefd[2];
    if (pipe(pipefd) != 0) return 127;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 127;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char *> av;
        av.reserve(args.size() + 1);
        for (const auto &s : args) av.push_back(const_cast<char *>(s.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(pipefd[1]);
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (out.size() < 1024 * 1024)
            out.append(buf, buf + n);
    }
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128;
}

bool haveCmd(const char *name)
{
    std::string out;
    const int rc = runArgv({"/bin/sh", "-c", std::string("command -v ") + name}, out);
    return rc == 0 && !out.empty();
}

bool tokenName(const std::string &s)
{
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

bool startsWith(const std::string &s, const char *pfx)
{
    const size_t n = std::strlen(pfx);
    return s.size() >= n && s.compare(0, n, pfx) == 0;
}

std::string scrub(std::string s)
{
    for (char &c : s)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

/* VESA CVT, 60 Hz, non-reduced. Checked against cvt(1) for the modes the
 * interactive prompt offers (1920x1080, 1280x720, 2560x1440, 1366x768,
 * 800x600, 1024x768). Horizontal size is rounded to the 8-pixel cell. */
Modeline makeCvt(int h_pixels, int v_lines)
{
    const double cell = 8.0;
    const double clock_step = 0.25;
    const double min_v_bporch = 6.0;
    const double min_v_porch = 3.0;
    const double min_vsync_bp = 550.0;
    const double h_sync_per = 8.0;
    const double c_prime = 30.0;
    const double m_prime = 300.0;
    const double hz = 60.0;

    Modeline m;
    m.hdisp = (int)(std::floor(h_pixels / cell + 0.5) * cell);
    m.vdisp = v_lines > 0 ? v_lines : 1;
    if (m.hdisp < 8) m.hdisp = 8;

    int vsync = 10;
    if (m.vdisp * 4 == 3 * m.hdisp) vsync = 4;
    else if (m.vdisp * 16 == 9 * m.hdisp) vsync = 5;
    else if (m.vdisp * 16 == 10 * m.hdisp) vsync = 6;
    else if (m.vdisp * 5 == 4 * m.hdisp) vsync = 7;
    else if (m.vdisp * 15 == 9 * m.hdisp) vsync = 7;

    const double h_period_est =
        ((1000000.0 / hz) - min_vsync_bp) / (m.vdisp + min_v_porch);
    const double duty = c_prime - (m_prime * h_period_est / 1000.0);
    const int h_blank = (int)(std::floor(m.hdisp * duty / (100.0 - duty) / (2.0 * cell)) * (2.0 * cell));
    m.htot = m.hdisp + h_blank;
    const double pclk = m.htot / h_period_est;
    m.pclk = std::floor(pclk / clock_step) * clock_step;
    if (m.pclk < clock_step) m.pclk = clock_step;
    const double act_h = m.htot / m.pclk;
    int v_sync_bp = (int)(min_vsync_bp / act_h + 0.5);
    while (v_sync_bp * act_h < min_vsync_bp - 1e-6) v_sync_bp++;
    if (v_sync_bp < vsync + min_v_bporch)
        v_sync_bp = (int)(vsync + min_v_bporch);
    const int v_fp = (int)min_v_porch;
    const int v_bp = v_sync_bp - vsync;
    m.vss = m.vdisp + v_fp;
    m.vse = m.vss + vsync;
    m.vtot = m.vdisp + v_fp + vsync + v_bp;
    const int h_sync = (int)(std::floor((h_sync_per / 100.0) * m.htot / cell) * cell);
    int h_fp = h_blank / 2 - h_sync;
    if (h_fp < 0) h_fp = 0;
    m.hss = m.hdisp + h_fp;
    m.hse = m.hss + h_sync;
    return m;
}

std::string modeName(const Modeline &m)
{
    return "mivara_" + std::to_string(m.hdisp) + "x" + std::to_string(m.vdisp) + "_60";
}

std::string statePath()
{
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) return std::string(rt) + "/mivara-vdd";
    return "/tmp/mivara-vdd-" + std::to_string((int)getuid());
}

bool writeState(const std::string &body)
{
    const std::string path = statePath();
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    const size_t n = std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    if (n != body.size()) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

void clearState()
{
    std::remove(statePath().c_str());
}

bool readState(std::string &backend, std::string &output, std::string &mode,
               int &added, int &created)
{
    backend.clear();
    output.clear();
    mode.clear();
    added = 0;
    created = 0;
    FILE *f = std::fopen(statePath().c_str(), "r");
    if (!f) return false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        char *eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string key = line;
        std::string val = eq + 1;
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r'))
            val.pop_back();
        if (key == "backend") backend = val;
        else if (key == "output") output = val;
        else if (key == "mode") mode = val;
        else if (key == "added_mode") added = std::atoi(val.c_str());
        else if (key == "created") created = std::atoi(val.c_str());
    }
    std::fclose(f);
    return !backend.empty() && tokenName(output);
}

bool parseGeom(const std::string &t, int &w, int &h, int &x, int &y)
{
    int a = 0, b = 0, c = 0, d = 0;
    char sx = 0, sy = 0;
    if (std::sscanf(t.c_str(), "%dx%d%c%d%c%d", &a, &b, &sx, &c, &sy, &d) != 6)
        return false;
    if ((sx != '+' && sx != '-') || (sy != '+' && sy != '-')) return false;
    w = a;
    h = b;
    x = (sx == '-') ? -c : c;
    y = (sy == '-') ? -d : d;
    return w > 0 && h > 0;
}

std::vector<std::string> splitWs(const std::string &line)
{
    std::vector<std::string> t;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r') {
            if (!cur.empty()) { t.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) t.push_back(cur);
    return t;
}

bool parseXrandr(const std::string &text, std::vector<Output> &outs)
{
    outs.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        if (line.empty()) continue;
        const bool indented = (line[0] == ' ' || line[0] == '\t');
        if (indented) {
            if (outs.empty()) continue;
            auto tok = splitWs(line);
            if (!tok.empty()) {
                std::string name = tok[0];
                while (!name.empty() && (name.back() == '*' || name.back() == '+'))
                    name.pop_back();
                if (tokenName(name)) outs.back().modes.push_back(name);
            }
            continue;
        }
        auto tok = splitWs(line);
        if (tok.size() < 2) continue;
        if (!tokenName(tok[0])) continue;
        Output o;
        o.name = tok[0];
        for (size_t i = 1; i < tok.size(); i++) {
            if (tok[i] == "connected") o.connected = true;
            else if (tok[i] == "disconnected") o.connected = false;
            else if (tok[i] == "primary") o.primary = true;
            else if (!o.has_geom && parseGeom(tok[i], o.w, o.h, o.x, o.y))
                o.has_geom = true;
        }
        /* A header line such as "Screen 0:" is not an output. */
        bool is_out = false;
        for (size_t i = 1; i < tok.size(); i++)
            if (tok[i] == "connected" || tok[i] == "disconnected") is_out = true;
        if (is_out) outs.push_back(o);
    }
    return !outs.empty();
}

bool isVirtualName(const std::string &name)
{
    return startsWith(upper(name), "VIRTUAL");
}

const Output *findOutput(const std::vector<Output> &outs, const std::string &name)
{
    for (const auto &o : outs)
        if (o.name == name) return &o;
    return nullptr;
}

const Output *primaryOf(const std::vector<Output> &outs)
{
    const Output *any = nullptr;
    for (const auto &o : outs) {
        if (!o.connected || !o.has_geom) continue;
        if (o.primary) return &o;
        if (!any) any = &o;
    }
    return any;
}

/* Refuse to retarget the only real desktop. A connected VIRTUAL output can
 * be adopted; a disconnected named output can be enabled. */
const Output *chooseXrandr(const std::vector<Output> &outs, std::string &why)
{
    const char *env = std::getenv("MIVARA_VDD_OUTPUT");
    if (env && env[0]) {
        if (!tokenName(env)) {
            why = "MIVARA_VDD_OUTPUT is not a safe output name";
            return nullptr;
        }
        const Output *o = findOutput(outs, env);
        if (!o) {
            why = std::string("MIVARA_VDD_OUTPUT=") + env + " is not in xrandr --query";
            return nullptr;
        }
        if (o->connected && !isVirtualName(o->name)) {
            why = std::string(env) + " is an already-connected physical output; refusing to retarget it";
            return nullptr;
        }
        return o;
    }
    for (const auto &o : outs)
        if (!o.connected && isVirtualName(o.name)) return &o;
    for (const auto &o : outs)
        if (o.connected && isVirtualName(o.name)) return &o;
    const char *allow = std::getenv("MIVARA_VDD_ALLOW_DISCONNECTED");
    if (allow && allow[0] == '1') {
        for (const auto &o : outs)
            if (!o.connected) return &o;
    }
    why = "no disconnected VIRTUAL output (set MIVARA_VDD_OUTPUT to a disconnected connector, or MIVARA_VDD_ALLOW_DISCONNECTED=1)";
    return nullptr;
}

bool xrandrAddMode(const std::string &output, const Modeline &ml, const std::string &name, std::string &err)
{
    char clk[32];
    std::snprintf(clk, sizeof(clk), "%.2f", ml.pclk);
    const int rc = runArgv({
        "xrandr", "--newmode", name, clk,
        std::to_string(ml.hdisp), std::to_string(ml.hss),
        std::to_string(ml.hse), std::to_string(ml.htot),
        std::to_string(ml.vdisp), std::to_string(ml.vss),
        std::to_string(ml.vse), std::to_string(ml.vtot),
        "-hsync", "+vsync"
    }, err);
    /* "mode already exists" is fine; a hard failure still tries --addmode. */
    (void)rc;
    std::string add_err;
    const int arc = runArgv({"xrandr", "--addmode", output, name}, add_err);
    if (arc != 0) {
        err = scrub(add_err.empty() ? err : add_err);
        return false;
    }
    return true;
}

bool xrandrStartup(int reqW, int reqH, LinuxVddInfo &info, std::string &err)
{
    std::string query;
    if (runArgv({"xrandr", "--query"}, query) != 0) {
        err = "xrandr --query failed";
        return false;
    }
    std::vector<Output> outs;
    if (!parseXrandr(query, outs)) {
        err = "xrandr --query produced no outputs";
        return false;
    }
    std::string why;
    const Output *target = chooseXrandr(outs, why);
    if (!target) {
        err = why;
        return false;
    }
    const Output *primary = primaryOf(outs);
    if (primary && primary->name == target->name) primary = nullptr;

    const Modeline ml = makeCvt(reqW, reqH);
    const std::string name = modeName(ml);
    bool added = false;
    bool have_mode = false;
    for (const auto &m : target->modes)
        if (m == name || (m.find(std::to_string(ml.hdisp) + "x" + std::to_string(ml.vdisp)) == 0))
            have_mode = true;
    std::string use = name;
    if (!have_mode) {
        std::string merr;
        if (!xrandrAddMode(target->name, ml, name, merr)) {
            err = "could not add mode " + name + " on " + target->name + ": " + merr;
            return false;
        }
        added = true;
    } else {
        for (const auto &m : target->modes) {
            if (m.find(std::to_string(ml.hdisp) + "x" + std::to_string(ml.vdisp)) == 0) {
                use = m;
                break;
            }
        }
    }

    const bool was_connected = target->connected && target->has_geom;
    std::vector<std::string> cmd = {"xrandr", "--output", target->name, "--mode", use};
    if (primary && tokenName(primary->name)) {
        cmd.push_back("--right-of");
        cmd.push_back(primary->name);
    } else {
        cmd.push_back("--pos");
        cmd.push_back("0x0");
    }
    std::string enerr;
    if (runArgv(cmd, enerr) != 0) {
        if (added) {
            runArgv({"xrandr", "--delmode", target->name, name}, enerr);
            runArgv({"xrandr", "--rmmode", name}, enerr);
        }
        err = "xrandr failed to enable " + target->name + ": " + scrub(enerr);
        return false;
    }

    std::string again;
    runArgv({"xrandr", "--query"}, again);
    std::vector<Output> outs2;
    parseXrandr(again, outs2);
    const Output *now = findOutput(outs2, target->name);
    if (!now || !now->connected || !now->has_geom || now->w <= 0 || now->h <= 0) {
        runArgv({"xrandr", "--output", target->name, "--off"}, enerr);
        if (added) {
            runArgv({"xrandr", "--delmode", target->name, name}, enerr);
            runArgv({"xrandr", "--rmmode", name}, enerr);
        }
        err = target->name + " did not come up after xrandr --mode";
        return false;
    }

    const bool created = !was_connected;
    std::string body;
    body += "backend=xrandr\n";
    body += "output=" + now->name + "\n";
    body += "mode=" + use + "\n";
    body += std::string("added_mode=") + (added ? "1" : "0") + "\n";
    body += std::string("created=") + (created ? "1" : "0") + "\n";
    writeState(body);

    info.backend = "xrandr";
    info.output = now->name;
    info.x = now->x;
    info.y = now->y;
    info.w = now->w;
    info.h = now->h;
    std::cerr << "Linux extend: xrandr output " << info.output
              << " " << info.w << "x" << info.h
              << " at " << info.x << "," << info.y << "\n";
    const char *wl = std::getenv("WAYLAND_DISPLAY");
    if (wl && wl[0]) {
        std::cerr << "Note: on XWayland this rectangle is a real second desktop "
                     "only when the compositor exposes that connector. "
                     "Sway and Hyprland headless outputs are preferred there.\n";
    }
    return true;
}

int jsonIntAfter(const std::string &s, size_t pos, const char *key, int &out)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = s.find(pat, pos);
    if (k == std::string::npos || k > pos + 500) return 0;
    k = s.find(':', k + pat.size());
    if (k == std::string::npos) return 0;
    char *end = nullptr;
    long v = std::strtol(s.c_str() + k + 1, &end, 10);
    out = (int)v;
    return 1;
}

std::string jsonStringAfter(const std::string &s, size_t pos)
{
    size_t q1 = s.find('"', pos);
    if (q1 == std::string::npos) return {};
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return s.substr(q1 + 1, q2 - q1 - 1);
}

void parseMonitors(const std::string &text, std::vector<MonRect> &mons)
{
    mons.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t name = text.find("\"name\"", pos);
        if (name == std::string::npos) break;
        std::string n = jsonStringAfter(text, name + 6);
        size_t next = text.find("\"name\"", name + 6);
        size_t region = text.find("\"rect\"", name);
        size_t base = name;
        if (region != std::string::npos && (next == std::string::npos || region < next))
            base = region;
        int x = 0, y = 0, w = 0, h = 0;
        const int got = jsonIntAfter(text, base, "x", x) +
                        jsonIntAfter(text, base, "width", w) +
                        jsonIntAfter(text, base, "height", h);
        jsonIntAfter(text, base, "y", y);
        if (tokenName(n) && got >= 2 && w > 0 && h > 0)
            mons.push_back(MonRect{n, x, y, w, h});
        pos = name + 6;
    }
}

int rightEdge(const std::vector<MonRect> &mons)
{
    int edge = 0;
    for (const auto &m : mons) {
        const int r = m.x + m.w;
        if (r > edge) edge = r;
    }
    return edge;
}

const MonRect *findMon(const std::vector<MonRect> &mons, const std::string &name)
{
    for (const auto &m : mons)
        if (m.name == name) return &m;
    return nullptr;
}

bool swayStartup(int reqW, int reqH, LinuxVddInfo &info, std::string &err)
{
    const Modeline ml = makeCvt(reqW, reqH);
    const std::string name = "MivaraVDD";
    std::string out;
    runArgv({"swaymsg", "-t", "get_outputs"}, out);
    std::vector<MonRect> before;
    parseMonitors(out, before);
    const int edge = rightEdge(before);
    if (!findMon(before, name)) {
        std::string cerr_out;
        if (runArgv({"swaymsg", "create_output", name}, cerr_out) != 0) {
            err = "swaymsg create_output failed: " + scrub(cerr_out);
            return false;
        }
    }
    const std::string res = std::to_string(ml.hdisp) + "x" + std::to_string(ml.vdisp);
    std::string place;
    int rc = runArgv({"swaymsg", "output", name, "resolution", res,
                      "position", std::to_string(edge), "0"}, place);
    if (rc != 0)
        rc = runArgv({"swaymsg", "output", name, "mode", res,
                      "pos", std::to_string(edge), "0"}, place);
    if (rc != 0) {
        runArgv({"swaymsg", "output", name, "unplug"}, place);
        err = "swaymsg could not place " + name + ": " + scrub(place);
        return false;
    }
    std::string again;
    runArgv({"swaymsg", "-t", "get_outputs"}, again);
    std::vector<MonRect> after;
    parseMonitors(again, after);
    const MonRect *now = findMon(after, name);
    if (!now) {
        err = "sway created no output named " + name;
        return false;
    }
    writeState("backend=sway\noutput=" + name + "\nmode=" + res + "\nadded_mode=0\ncreated=1\n");
    info.backend = "sway";
    info.output = name;
    info.x = now->x;
    info.y = now->y;
    info.w = now->w;
    info.h = now->h;
    std::cerr << "Linux extend: sway output " << name << " " << info.w << "x" << info.h
              << " at " << info.x << "," << info.y << "\n";
    return true;
}

bool hyprStartup(int reqW, int reqH, LinuxVddInfo &info, std::string &err)
{
    const Modeline ml = makeCvt(reqW, reqH);
    std::string before_txt;
    runArgv({"hyprctl", "monitors", "-j"}, before_txt);
    if (before_txt.empty()) runArgv({"hyprctl", "monitors"}, before_txt);
    std::vector<MonRect> before;
    parseMonitors(before_txt, before);
    const int edge = rightEdge(before);

    std::string created_out;
    int rc = runArgv({"hyprctl", "output", "create", "headless", "MivaraVDD"}, created_out);
    if (rc != 0)
        rc = runArgv({"hyprctl", "output", "create", "headless"}, created_out);
    if (rc != 0) {
        err = "hyprctl output create headless failed: " + scrub(created_out);
        return false;
    }
    std::string after_txt;
    runArgv({"hyprctl", "monitors", "-j"}, after_txt);
    if (after_txt.empty()) runArgv({"hyprctl", "monitors"}, after_txt);
    std::vector<MonRect> after;
    parseMonitors(after_txt, after);

    const MonRect *now = findMon(after, "MivaraVDD");
    if (!now) {
        for (const auto &m : after) {
            if (!startsWith(upper(m.name), "HEADLESS")) continue;
            if (!findMon(before, m.name)) { now = &m; break; }
        }
    }
    if (!now) {
        err = "hyprctl created no headless monitor";
        return false;
    }
    const std::string mon = now->name;
    const std::string spec = mon + "," + std::to_string(ml.hdisp) + "x" +
                             std::to_string(ml.vdisp) + "@60," +
                             std::to_string(edge) + "x0,1";
    std::string kw;
    if (runArgv({"hyprctl", "keyword", "monitor", spec}, kw) != 0) {
        runArgv({"hyprctl", "output", "remove", mon}, kw);
        err = "hyprctl keyword monitor failed: " + scrub(kw);
        return false;
    }
    std::string final_txt;
    runArgv({"hyprctl", "monitors", "-j"}, final_txt);
    std::vector<MonRect> final_m;
    parseMonitors(final_txt, final_m);
    const MonRect *placed = findMon(final_m, mon);
    if (!placed) placed = now;
    writeState("backend=hyprland\noutput=" + mon + "\nmode=headless\nadded_mode=0\ncreated=1\n");
    info.backend = "hyprland";
    info.output = mon;
    info.x = placed->x;
    info.y = placed->y;
    info.w = placed->w > 0 ? placed->w : ml.hdisp;
    info.h = placed->h > 0 ? placed->h : ml.vdisp;
    std::cerr << "Linux extend: hyprland output " << info.output << " " << info.w << "x" << info.h
              << " at " << info.x << "," << info.y << "\n";
    return true;
}

bool tearDownRecorded(std::string &err)
{
    std::string backend, output, mode;
    int added = 0, created = 0;
    if (!readState(backend, output, mode, added, created)) return true;
    if (!tokenName(output) || (tokenName(mode) == false && !mode.empty() && mode != "headless")) {
        clearState();
        return true;
    }
    std::string out;
    bool gone = false;
    if (backend == "xrandr") {
        if (created)
            runArgv({"xrandr", "--output", output, "--off"}, out);
        if (added && tokenName(mode)) {
            runArgv({"xrandr", "--delmode", output, mode}, out);
            runArgv({"xrandr", "--rmmode", mode}, out);
        }
        std::string q;
        runArgv({"xrandr", "--query"}, q);
        std::vector<Output> outs;
        parseXrandr(q, outs);
        const Output *o = findOutput(outs, output);
        gone = !created || !o || !o->connected;
    } else if (backend == "sway") {
        int rc = runArgv({"swaymsg", "output", output, "unplug"}, out);
        if (rc != 0) rc = runArgv({"swaymsg", "destroy_output", output}, out);
        if (rc != 0) runArgv({"swaymsg", "output", output, "disable"}, out);
        std::string q;
        runArgv({"swaymsg", "-t", "get_outputs"}, q);
        std::vector<MonRect> mons;
        parseMonitors(q, mons);
        gone = findMon(mons, output) == nullptr;
    } else if (backend == "hyprland") {
        int rc = runArgv({"hyprctl", "output", "remove", output}, out);
        if (rc != 0) {
            runArgv({"hyprctl", "keyword", "monitor", output + ",disable"}, out);
        }
        std::string q;
        runArgv({"hyprctl", "monitors", "-j"}, q);
        std::vector<MonRect> mons;
        parseMonitors(q, mons);
        gone = findMon(mons, output) == nullptr;
    } else {
        clearState();
        return true;
    }
    if (!gone && created) {
        err = "virtual output " + output + " is still enabled";
        return false;
    }
    clearState();
    return true;
}

}  // namespace

bool linuxVddStartup(int reqW, int reqH, LinuxVddInfo &out, std::string &err)
{
    if (reqW < 320) reqW = 320;
    if (reqH < 240) reqH = 240;
    if (reqW > 7680) reqW = 7680;
    if (reqH > 4320) reqH = 4320;
    err.clear();
    std::string why;
    /* Drop a leftover from a killed sender before creating another one. */
    linuxVddForceDetach();

    const char *wl = std::getenv("WAYLAND_DISPLAY");
    const bool wayland = wl && wl[0];
    if (wayland && haveCmd("swaymsg")) {
        if (swayStartup(reqW, reqH, out, why)) return true;
        err += "sway: " + why + " ";
    }
    if (wayland && haveCmd("hyprctl")) {
        if (hyprStartup(reqW, reqH, out, why)) return true;
        err += "hyprland: " + why + " ";
    }
    if (haveCmd("xrandr")) {
        if (xrandrStartup(reqW, reqH, out, why)) return true;
        err += "xrandr: " + why + " ";
    } else {
        err += "xrandr is not installed. ";
    }
    if (wayland) {
        err += "GNOME Wayland has no userspace virtual monitor in this build. "
               "A kernel virtual display (evdi or similar) is not loaded here; "
               "install one yourself and it will be used when xrandr lists a "
               "disconnected VIRTUAL output. ";
    }
    err = scrub(err);
    return false;
}

void linuxVddShutdown()
{
    std::string err;
    tearDownRecorded(err);
}

bool linuxVddForceDetach()
{
    std::string err;
    return tearDownRecorded(err);
}

#ifdef LINUX_VDD_TEST
#include <iostream>

static int expectMode(int w, int h, double pclk,
                      int hss, int hse, int htot,
                      int vss, int vse, int vtot)
{
    Modeline m = makeCvt(w, h);
    auto bad = [&](const char *what, double got, double want) {
        std::cerr << w << "x" << h << " " << what << " got " << got
                  << " want " << want << "\n";
        return 1;
    };
    if (std::fabs(m.pclk - pclk) > 0.001) return bad("pclk", m.pclk, pclk);
    if (m.hss != hss) return bad("hss", m.hss, hss);
    if (m.hse != hse) return bad("hse", m.hse, hse);
    if (m.htot != htot) return bad("htot", m.htot, htot);
    if (m.vss != vss) return bad("vss", m.vss, vss);
    if (m.vse != vse) return bad("vse", m.vse, vse);
    if (m.vtot != vtot) return bad("vtot", m.vtot, vtot);
    return 0;
}

int main(int argc, char **argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "modeline";
    if (cmd == "modeline") {
        int rc = 0;
        rc |= expectMode(1920, 1080, 173.00, 2048, 2248, 2576, 1083, 1088, 1120);
        rc |= expectMode(1280, 720, 74.50, 1344, 1472, 1664, 723, 728, 748);
        rc |= expectMode(2560, 1440, 312.25, 2752, 3024, 3488, 1443, 1448, 1493);
        rc |= expectMode(800, 600, 38.25, 832, 912, 1024, 603, 607, 624);
        rc |= expectMode(1024, 768, 63.50, 1072, 1176, 1328, 771, 775, 798);
        rc |= expectMode(1366, 768, 85.25, 1440, 1576, 1784, 771, 781, 798);
        if (rc == 0) std::cout << "modeline ok\n";
        return rc == 0 ? 0 : 1;
    }
    if (cmd == "pick") {
        std::string text, line;
        while (std::getline(std::cin, line)) text += line + "\n";
        std::vector<Output> outs;
        parseXrandr(text, outs);
        std::string why;
        const Output *o = chooseXrandr(outs, why);
        if (!o) {
            std::cout << "NONE " << why << "\n";
            return 0;
        }
        std::cout << o->name << "\n";
        return 0;
    }
    if (cmd == "live") {
        LinuxVddInfo info;
        std::string err;
        const bool ok = linuxVddStartup(1920, 1080, info, err);
        std::cout << (ok ? "ok " : "fail ") << info.backend << " " << info.output
                  << " " << info.w << "x" << info.h << " " << err << "\n";
        if (ok) linuxVddShutdown();
        return ok ? 0 : 2;
    }
    std::cerr << "usage: modeline | pick | live\n";
    return 2;
}
#endif
