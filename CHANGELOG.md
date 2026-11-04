# CHANGELOG

本项目版本格式：`主版本.次版本.修订`，按版本倒序分节。
版本号以 Release tag 为准。

## [1.0.1] - 2026-11-04

图标与桌面快捷方式。

### 新增

- 内嵌多尺寸图标（16 / 24 / 32 / 48 / 64 / 256，256 用 PNG 压缩）：资源 ID 101，窗口 `WM_SETICON`（16 / 32）与托盘图标复用同一套
- 托盘菜单新增「创建桌面快捷方式」：`IShellLinkW` + `IPersistFile` 生成 `桌面布局.lnk`，指向当前 exe 并复用内嵌图标（`SetIconLocation(exe, 0)`）
- 桌面路径用 `SHGetKnownFolderPath(FOLDERID_Desktop)` 解析，兼容桌面被重定向（如 `E:\desktop`），不硬编码
- 同名 `.lnk` 已存在且指向其它程序时不覆盖，状态栏提示；仅在确认是本程序生成的快捷方式时才覆盖

### 构建

- 图标源 `assets/app_icon.py`（Pillow，8× 超采样）与 `assets/app_icon.rc` / `app_icon.ico` / `app_icon.obj` 入库
- `build.bat` 直接链接 `.rsrc` COFF 对象（等价 `windres -O coff`），不依赖 `rc.exe`

### 体积

- exe 128,000 → 175,616 B（+47,616 B）：图标资源 +45,568 B，代码 +2,048 B（无图标对照构建 130,048 B）

### 启动

- 启动到窗口可见：加图标前 148.5 ms、加图标后 151.8 ms（各 5 次中位），差值在测量噪声内
- 常驻开销无变化：GDI / USER 对象、线程数、句柄数、空闲 CPU 与 v1.0.0 相同

## [1.0.0] - 2026-11-04

首个公开版本。

### 功能

- 保存 / 应用 / 撤销 / 重命名 / 删除桌面图标布局，布局存为可读 JSON
- 应用前自动快照，支持单级撤销
- 多显示器与 DPI 感知：记录图标所在显示器及坐标，按当前 DPI 还原
- 托盘常驻：关闭主窗口收进托盘，托盘菜单可保存、应用、撤销、打开、退出
- 开机自启：写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，带 `--tray` 静默启动
- 「删除前确认」开关：默认开启；关闭后删除立即生效并在状态栏提示，选择持久化到 `settings.json`
- 删除确认弹窗：分层窗口整窗绘制，四角 12 px 抗锯齿圆角（Win10 / Win11 同一实现）
- 命令行：`--save` / `--apply` / `--undo` / `--list` / `--rename` / `--delete` / `--swap-test` / `--tray`

### 修复

- 修复确认弹窗按钮不可见：`UpdateLayeredWindow` 接管整窗像素后子窗口不再合成，改为把按钮自绘进分层位图
- 弹窗高度改为按内容计算（152 → 134 px），去掉底部大片空白

### 说明

- 单文件 exe，128,000 B；无 .NET / Python / VC++ 运行库依赖
- 支持 Windows 10 / 11（x64）
