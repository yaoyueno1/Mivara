# Virtual Display Driver

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/virtual-display.md)

On Windows, extend mode relies on an indirect display driver (IddCx) to provide a real virtual monitor. macOS has no equivalent today: the Extend button is disabled there, and `sender --mode extend` fails with an explicit error instead of streaming the primary screen as extend. Linux does not install a kernel driver; the paths it can enable are at the end of this page. The first half covers the Windows driver's provenance, the one-time installation steps and resolution configuration; for how the device is enabled, attached and cleaned up during each session, see [Architecture & Protocol](architecture.md).

## Driver Provenance

This project adapts **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)** (MIT license, properly signed via SignPath).

## Installation and Lifecycle

The driver **only needs to be installed once**. Afterwards no manual steps are needed — sender enables the device and attaches the virtual display when an extend session starts, and detaches + disables it when the session ends (so it disappears from Settings > Display completely). For the per-session enable/disable, CCD attach/detach and exit paths, see [Architecture & Protocol](architecture.md).

### Installation (Administrator PowerShell)

```powershell
# 1. Download from GitHub Releases (e.g. VDD.Control.25.7.23.zip) and extract

# 2. Stage the driver
pnputil /add-driver MttVDD.inf /install
# (note: the driver in the x86 directory actually targets NTamd64)

# 3. Create the Root\MttVDD device
devcon.exe install MttVDD.inf "Root\MttVDD"

# 4. Copy the settings file (resolution/refresh-rate list lives there)
Copy-Item vdd_settings.xml C:\Windows\System32\drivers\UMDF\vdd_settings.xml

# 5. Rescan; "Generic Monitor (VDD by MTT)" appearing means success
pnputil /scan-devices
```

### Resolution Notes

Once activated, the virtual display is automatically configured to **1920x1080@60** and placed right of the existing desktop. The resolution can be chosen at session start (interactive mode) or defaults to 1920x1080; to change the default, edit the `vddConfigure(dev, x, 0, w, h, 60)` call in `vddStartup()` in vdd.cpp, or add modes to the driver's `vdd_settings.xml` and rebuild.

## Linux virtual output

This is not a kernel driver, and the program does not load a kernel module. An extend session looks for a real second output in the order below, captures only that rectangle, and removes what this session created when it ends:

1. `WAYLAND_DISPLAY` is set and `swaymsg` runs: `swaymsg create_output`, then a resolution placed to the right of the primary output.
2. Otherwise, if `hyprctl` runs: `hyprctl output create headless`, then a monitor rule.
3. Otherwise `xrandr`: prefer a **disconnected** connector whose name starts with `VIRTUAL`. An already-connected physical output is never retargeted. An already-connected `VIRTUAL*` can be adopted.

| Variable | Effect |
|----------|--------|
| `MIVARA_VDD_OUTPUT` | Names an xrandr output (output-name characters only). An already-connected physical output that is not `VIRTUAL` is refused |
| `MIVARA_VDD_ALLOW_DISCONNECTED=1` | When there is no `VIRTUAL` connector, allow enabling any **disconnected** connector |
| `MIVARA_PORTAL_WAIT_MS` | How long to wait for the Wayland share dialog. Default 60000, range 200–180000 |

The xrandr path generates a CVT modeline in-process, runs `--newmode`, `--addmode`, and `--output --mode --right-of` the primary, then re-reads the geometry. On exit it runs `--off` only for the output this session enabled, and deletes a mode only when this session added it.

State is written to `$XDG_RUNTIME_DIR/mivara-vdd` (or `/tmp/mivara-vdd-<uid>`). After a hard kill the GUI uses that file to remove the output.

Wayland capture uses xdg-desktop-portal + PipeWire. A Sway / Hyprland extend output has to be the screen the portal shares; if the portal fails, capture does not switch to the XWayland primary. An xrandr output on X11 can fall back to X11 capture of that rectangle when the portal fails. A rectangle added with xrandr under XWayland is not necessarily a second desktop in the compositor; Sway and Hyprland prefer their own headless outputs.

**GNOME on Wayland** has no userspace virtual monitor. Install a kernel virtual display yourself (evdi, for example) so that `xrandr --query` lists a disconnected `VIRTUAL*`, and the xrandr path above will use it. This repository does not install or load that module.
