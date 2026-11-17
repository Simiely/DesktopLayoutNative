# AGENTS.md · 项目规则

> 给 AI / 未来的你：只记代码里看不出的关键信息。架构与构建细节见 [DEVELOPMENT.md](DEVELOPMENT.md)。

## 技术栈

- **纯 C + Win32**（无 C++、无第三方库）：`src/main.c` 单文件约 145 KB，GDI/GDI+ 自绘界面。
- 桌面协同走 Shell COM：`IShellWindows` → `IServiceProvider` → `IShellBrowser` → `IFolderView` 取图标坐标。
- **用 Zig 交叉编译**，不依赖 MSVC：`zig cc -target x86_64-windows-gnu`（见 `build.bat`）。
- 图标资源是**预生成的 COFF 对象** `assets/app_icon.obj`（由 `assets/app_icon.py` 生成并入库），
  构建时直接链接，不调用 `rc.exe`。

## 关键坑

1. **应用布局前必须关闭桌面「自动排列图标」**——否则系统拒绝移动，`IFolderView` 返回失败。
   代码里的提示语就是用户的实际解法，别改成技术术语。
2. **快照必须严格校验**：声明的 `count` 要与实际图标数一致，否则截断的快照会被"半应用"，
   桌面变成一半新一半旧。这是最难排查的一类状态。
3. **桌面路径不要硬编码**：用 `SHGetKnownFolderPath(FOLDERID_Desktop)`，兼容桌面被重定向到
   其它盘（如 `E:\desktop`）。
4. **创建快捷方式不覆盖别人的**：同名 `.lnk` 已存在且指向其它程序时，只提示不覆盖。
5. **撤销是单级快照**，不是无限历史。代码里预留了扩展点（`main.c` 撤销快照一节），
   真要做多级再改，别顺手改数据结构。
6. **DPI 用 `MulDiv` 等比缩放**（`S(v)=MulDiv(v,g_dpi,96)`），窗口外框锁 500×400；
   加控件前先确认 96 / 192 DPI 都不溢出。

## 约定

- 保持**单文件、零运行时依赖**：不引入 .NET / Python / VC++ 运行库。
- 布局文件、撤销快照、`settings.json` 都是**手写 JSON**（无第三方库），改格式必须同时写读旧格式的兼容分支。
- 版本号以 **Release tag 为准**；代码里不存版本常量，别为了显示版本号去加一个。

## 常用命令

```bat
build.bat                       :: 用 PATH 里的 zig 构建 → dist\DesktopLayoutNative.exe
build.bat D:\tools\zig.exe      :: 指定 zig 路径
```

命令行能力（写测试与脚本时用）：`--save` / `--apply` / `--undo` / `--list` /
`--rename` / `--delete` / `--swap-test` / `--shortcut-test <目录>`。
