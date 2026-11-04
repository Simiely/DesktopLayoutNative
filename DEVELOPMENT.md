# DEVELOPMENT

## 代码结构

单文件：`src/main.c`（约 145 KB），Win32 + Shell COM + GDI/GDI+，无第三方库。

| 区块 | 职责 |
|---|---|
| Shell COM | `IShellWindows` → `IServiceProvider` → `IShellBrowser` → `IFolderView` 枚举桌面图标与坐标 |
| JSON | 手写读写：布局文件、撤销快照、`settings.json` |
| 布局操作 | 保存 / 应用 / 撤销 / 重命名 / 删除，应用前写入快照并校验差异 |
| 界面 | 500 × 400 固定外框，自绘按钮/列表/状态栏，96–192 DPI 用 `S(v)=MulDiv(v,g_dpi,96)` 缩放 |
| 托盘 | `Shell_NotifyIconW` + 自建菜单，关闭主窗口隐藏到托盘；菜单含「创建桌面快捷方式」 |
| 快捷方式 | `IShellLinkW` + `IPersistFile`，桌面路径用 `SHGetKnownFolderPath(FOLDERID_Desktop)`，不硬编码 |
| 自启 | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，参数 `--tray` |
| 命令行 | `--save` / `--apply` / `--undo` / `--list` / `--rename` / `--delete` / `--swap-test` / `--shortcut-test <目录>` |

## 构建

```bat
build.bat                     :: PATH 中的 zig
build.bat D:\zig\zig.exe      :: 指定 zig
```

等价的原始命令：

```bat
zig cc -target x86_64-windows-gnu -Os -ffunction-sections -fdata-sections -s "-Wl,--subsystem,windows" ^
  -o dist\DesktopLayoutNative.exe src\main.c assets\app_icon.obj ^
  -lole32 -loleaut32 -luuid -lshell32 -luser32 -lgdi32 -lcomctl32 -ladvapi32 -lgdiplus -lmsimg32
```

注意：`-Wl,--subsystem,windows` 必须加引号，否则 PowerShell / cmd 会把逗号拆成参数，产出控制台子系统。
工具链实测 zig 0.17.0；更低版本可能缺少 `gdiplus` flat API 的导入库。

## 图标资源

图标源文件 `assets/app_icon.py`（Pillow，8× 超采样）生成：

- `assets/app_icon.ico`：单文件 6 个尺寸（16/24/32/48/64/256，256 用 PNG 压缩）
- `assets/app_icon.obj`：只含 `.rsrc` 段的 COFF 对象，`build.bat` 直接链接，无需 `rc.exe`

```bat
python assets/app_icon.py
```

`src/main.c` 用 `LoadImageW(g_inst, MAKEINTRESOURCEW(101), ...)` 读取同一套资源
（资源 ID 101，定义见 `assets/app_icon.rc`），分别设置 `WM_SETICON` 的
`ICON_SMALL(16)` / `ICON_BIG(32)`，托盘复用 32px 那张；资源缺失时退回运行时手绘。
`.res` 文件不被 zig 的 lld-link 识别（`unknown file type`），因此链接的是 COFF `.rsrc` 对象。

## 数据位置

```
%LOCALAPPDATA%\DesktopLayoutNative\data\
├── layouts\*.json          命名布局
├── undo\undo_snapshot.json 单级撤销快照
├── layout_current.json     最近一次保存
└── settings.json           { "confirm_delete": 0 | 1 }
```

`settings.json` 缺失或损坏时按默认值 `1`（开启删除确认）。

## 绘制要点

- 主界面按钮 / 列表 / 状态栏均为自绘，焦点环仅在键盘导航（Tab）时出现，鼠标点击不画。
- 删除确认弹窗是 `WS_POPUP + WS_EX_TOOLWINDOW` 无边框窗口，用 `WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA)` 整窗上屏：
  4× 超采样 GDI+ 圆角（半径 `S(12)`）→ 手工预乘 alpha → 双三次缩到 1× → GDI 画文字与按钮 → 圆角安全区强制不透明。
- **坑**：分层窗口的子控件不会参与合成，屏幕上看不见；弹窗按钮必须自绘进同一张分层位图（`dlg_layer_paint`），点击用 `WM_LBUTTONDOWN/UP` 命中测试。
- `DWMWA_WINDOW_CORNER_PREFERENCE` 不作用于无边框分层窗口；`SetWindowRgn` 会产生硬锯齿，均未采用。
