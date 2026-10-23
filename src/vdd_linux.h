/**
 * VDD_LINUX.H - Linux extend-mode virtual output
 *
 * Not a kernel driver. The sender enables a real second output the
 * desktop already knows how to create, captures only that rectangle,
 * and removes it on every exit path:
 *   - X11 / XWayland: a disconnected RANDR output named VIRTUAL*
 *     (Intel and similar), or MIVARA_VDD_OUTPUT
 *   - Sway: `swaymsg create_output`
 *   - Hyprland: `hyprctl output create headless`
 *
 * GNOME on Wayland has no userspace virtual monitor. A kernel virtual
 * display (evdi and the like) is not loaded by this code; if the user
 * installs one and it shows up as VIRTUAL*, the X11 path will use it.
 *
 * State is written to $XDG_RUNTIME_DIR/mivara-vdd (or /tmp/mivara-vdd-<uid>)
 * so the GUI can linuxVddForceDetach() after the sender is SIGKILL'd.
 * Windows MttVDD stays in vdd.cpp / vdd.h.
 */
#pragma once

#include <string>

struct LinuxVddInfo {
    std::string backend;   /* "xrandr", "sway", or "hyprland" */
    std::string output;
    int x = 0, y = 0, w = 0, h = 0;
};

/** Create or adopt the virtual output and fill its rectangle on the desktop.
 *  On failure nothing that was already enabled is turned off, and `err`
 *  explains which backends were tried. */
bool linuxVddStartup(int reqW, int reqH, LinuxVddInfo &out, std::string &err);

/** Disable an output this process created. Idempotent. */
void linuxVddShutdown();

/** Disable whatever the state file points at, including after a hard kill. */
bool linuxVddForceDetach();
