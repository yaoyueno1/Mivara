/**
 * WL_CAPTURE.H - Wayland screen capture via xdg-desktop-portal + PipeWire
 *
 * Used by the Linux sender when WAYLAND_DISPLAY is set. The portal shows
 * the desktop's screen-share dialog and hands back a PipeWire remote; frames
 * are copied out as RGB24. If this file was built without libpipewire and
 * libdbus, wlCaptureStart fails with a message that names the missing
 * libraries and the caller falls back to X11.
 *
 * Cursor: the portal request asks for an embedded cursor, so the compositor
 * draws the pointer. There is no second cursor composite on this path.
 */
#pragma once

#include <cstdint>
#include <string>

struct WlCaptureRequest {
    int  want_w = 0;            /* 0 = take the stream the user shared */
    int  want_h = 0;
    bool multiple = false;      /* let the dialog offer more than one monitor */
    bool include_virtual = false; /* also ask for portal virtual-monitor sources */
};

struct WlCaptureInfo {
    int width = 0;
    int height = 0;
    unsigned node_id = 0;
};

/** True when this binary was compiled with PipeWire and libdbus. */
bool wlCaptureBuilt();

/**
 * Open a portal screen cast and block until one frame has arrived
 * (or the dialog times out). On failure `err` is a single line safe
 * to print; it contains no '|' characters.
 */
bool wlCaptureStart(const WlCaptureRequest &req, WlCaptureInfo &info,
                    std::string &err);

/** Copy the latest RGB24 frame. False until the first frame, or if w/h differ. */
bool wlCaptureFrame(uint8_t *rgb, int w, int h);

/** Idempotent. Safe from atexit. */
void wlCaptureShutdown();
