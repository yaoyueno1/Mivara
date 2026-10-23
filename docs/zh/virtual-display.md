# 虚拟显示器驱动

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/virtual-display.md)

Windows 上的扩展模式依赖间接显示驱动（IddCx）提供一块真实的虚拟显示器。macOS 目前没有对应实现：那一端的「扩展」按钮是禁用的，`sender --mode extend` 会明确失败，不会把主屏画面以扩展模式发出。Linux 不装内核驱动，能启用的路径写在文末。本页前半说明 Windows 驱动来源、一次性安装步骤与分辨率配置；设备在每次会话中的启用、附加与清理流程，见[架构与协议](architecture.md)。

## 驱动来源

本项目适配 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**（MIT 协议，SignPath 正规签名）。

## 安装与生命周期

驱动**只需安装一次**。装好后无需任何手动操作——sender 会在扩展会话开始时自动启用设备并激活虚拟显示器、结束时分离并禁用设备（彻底从「设置 → 屏幕」消失）。每次会话的具体启用/禁用、CCD 附加/分离与退出路径，见[架构与协议](architecture.md)。

### 安装步骤（管理员 PowerShell）

```powershell
# 1. 从 GitHub Releases 下载（如 VDD.Control.25.7.23.zip）并解压

# 2. 驱动入库
pnputil /add-driver MttVDD.inf /install
# （注意：x86 目录内的驱动实际面向 NTamd64）

# 3. 创建 Root\MttVDD 设备
devcon.exe install MttVDD.inf "Root\MttVDD"

# 4. 拷贝设置文件（分辨率/刷新率列表在此配置）
Copy-Item vdd_settings.xml C:\Windows\System32\drivers\UMDF\vdd_settings.xml

# 5. 扫描设备，出现 "Generic Monitor (VDD by MTT)" 即成功
pnputil /scan-devices
```

### 分辨率说明

虚拟显示器激活后由 sender 自动配置为 **1920x1080@60** 并置于现有桌面右侧。分辨率可在会话开始时选择（交互模式）或默认 1920x1080；如需修改默认值，编辑 vdd.cpp 中 `vddStartup()` 里的 `vddConfigure(dev, x, 0, w, h, 60)` 调用，或编辑驱动的 `vdd_settings.xml` 添加模式后重新编译。

## Linux 虚拟输出

这不是内核驱动，程序也不会加载内核模块。扩展会话按下面的顺序找一块真实的第二输出，只采集它的矩形，结束时拆掉本次创建的输出：

1. `WAYLAND_DISPLAY` 已设置且能运行 `swaymsg`：`swaymsg create_output`，再设分辨率并放到主输出右侧。
2. 否则能运行 `hyprctl`：`hyprctl output create headless`，再写一条 monitor 规则。
3. 否则 `xrandr`：优先选用**未连接**且名字以 `VIRTUAL` 开头的接口。已经连上的物理输出不会被改模式。已经连上的 `VIRTUAL*` 可以被接管。

| 变量 | 作用 |
|------|------|
| `MIVARA_VDD_OUTPUT` | 指定 xrandr 输出名（只接受输出名里的字符）。已连接的非 `VIRTUAL` 物理输出会被拒绝 |
| `MIVARA_VDD_ALLOW_DISCONNECTED=1` | 没有 `VIRTUAL` 时，允许启用任意一个**未连接**的接口 |
| `MIVARA_PORTAL_WAIT_MS` | Wayland 共享对话框的等待时间，默认 60000，范围 200–180000 |

xrandr 路径在进程内生成 CVT 模式行，执行 `--newmode`、`--addmode`、`--output --mode --right-of` 主输出，再重新查询几何。结束时只对本次启用的输出执行 `--off`，并且只删除本次添加的模式。

状态写在 `$XDG_RUNTIME_DIR/mivara-vdd`（没有则 `/tmp/mivara-vdd-<uid>`）。GUI 在发送端被强杀之后用这份状态拆除输出。

Wayland 采集走 xdg-desktop-portal + PipeWire。Sway / Hyprland 的扩展输出必须由门户采到那一块屏；门户失败时不会改去采 XWayland 主屏。X11 上的 xrandr 输出在门户失败时可以回退到 X11 矩形采集。在 XWayland 里用 xrandr 加出来的矩形，不一定是合成器桌面上的第二块屏；Sway 与 Hyprland 优先走它们自己的 headless 输出。

**GNOME Wayland** 没有用户态虚拟屏。自行安装内核虚拟显示器（例如 evdi），让 `xrandr --query` 出现断开的 `VIRTUAL*` 之后，上面的 xrandr 路径会用它。本仓库不执行安装，也不加载该模块。
