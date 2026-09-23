# Stargazer 桌面效率工具 — 设计文档

日期：2026-09-22
状态：待评审

## 1. 目标

一个常驻 Windows 的桌面效率工具，按需呼出的单一窗口，内含四个模块：

1. **Launcher** — 快捷启动板（类似 Rolan 的启动板 / 应用库）
2. **Box** — 文件收纳盒（引用式，类似 Fences 的陈列，但以弹窗形态呈现）
3. **Todo** — 待办清单
4. **Explorer** — 网盘目录浏览面板（主要用途：浏览 CD2 挂载到本地的网盘目录结构，并把条目归档到 Box）

### 使用者与场景

个人自用，Windows 单机。日常动作是：按热键呼出 → 打字搜索或浏览 → 启动程序 / 打开文件 / 勾掉待办 → `Esc` 消失。`Explorer` 专门用于把网盘目录结构"暴露"出来以便快速归档。

### 成功标准

| 指标 | 目标 |
|---|---|
| 空闲常驻内存 | ≤ 5 MB（托盘 + 热键，无窗口） |
| 呼出态内存 | ≤ 15 MB（含图标缓存） |
| 空闲 CPU | 0%（不跑定时器、不轮询） |
| 冷启动到可用 | < 100 ms |
| 产物 | 单个 `stargazer.exe`，零外部依赖，拷到 U 盘即可用 |
| 代码总量 | ≤ 4000 行 C++（不含测试） |

## 2. 非目标（明确不做）

- 不做完整文件资源管理器（多标签、目录树、缩略图、预览、文件操作队列、批量重命名）
- 不做文件真实移动（收纳盒只存路径引用，永不移动或删除用户文件）
- 不做云同步、多机共享、账本式迁移框架
- 不做插件系统、不做主题市场、不做皮肤编辑器
- 不做安装器、不做自动更新
- 不做多进程、不做 DLL 模块边界
- 不做平滑滚动动画、不做过渡特效

## 3. 技术选型

| 项 | 选择 | 理由 |
|---|---|---|
| 语言 | C++20 | 内存与启动速度目标要求原生 |
| UI | Win32 + Direct2D + DirectWrite | 单一窗口，自绘；D2D 提供抗锯齿圆角与清晰缩放 |
| 构建 | CMake + MSVC v143，`/O2 /MT` | 静态 CRT 是便携的前提 |
| 第三方依赖 | **无**（仅 Windows SDK） | 零依赖才叫绿色单文件 |
| 目标系统 | Win10 1809+ / Win11 | 用到 per-monitor-v2 DPI 与 D2D 1.1 |

被否决的方案：Electron/Tauri（WebView2 多进程，实测常驻 150MB+，与核心要求冲突）；WPF（常驻 80–150MB，且需要 .NET 运行时）；多进程拆分（窗口本就不常驻，收益近零却引入 IPC）；DLL 插件式（只有 4 个固定模块）。

## 4. 架构

单进程、单窗口、单一渲染路径。`AppState` 持有全部数据，四个视图共用渲染与输入内核。

```
src/
  main.cpp          入口、单实例、消息循环、热键、托盘、自启动
  app.h/.cpp        AppState、窗口呼出/隐藏、视图分发、输入路由
  render.h/.cpp     D2D/DWrite 初始化、设备丢失、绘制原语、文本格式缓存
  icons.h/.cpp      Shell 图标提取、LRU 缓存、图标工作线程
  fs_work.h/.cpp    目录枚举、存在性校验、文件系统工作线程
  dragdrop.h/.cpp   IDropTarget、IDropSource、内部拖拽状态机
  edit.h/.cpp       InlineEdit（原生 EDIT 子控件封装）
  persist.h/.cpp    行格式读写、数据目录解析、注册表自启动
  views/
    launcher.cpp
    box.cpp
    todo.cpp
    explorer.cpp
  model/            无任何 Win32 依赖
    rowformat.h/.cpp  行格式序列化/反序列化
    paths.h/.cpp      路径规范化
    search.h/.cpp     子串匹配 + 自写自然序比较
    todo_model.h/.cpp 排序与状态变更
    store.h/.cpp      四个模块的内存模型
tests/
  test_model.cpp    控制台 assert 测试
```

**线程模型**：1 个 UI 线程 + 2 个工作线程。

- 线程 A：Shell 图标提取（COM MTA）
- 线程 B：文件系统操作（目录枚举、存在性校验）

分两条是为了让慢速网盘枚举不阻塞图标提取。工作线程完成后 `PostMessage` 回 UI 线程，携带 `requestId`；UI 线程比对 `requestId` 丢弃过期结果。UI 线程 COM 为 STA。

**视图分发**：`enum class View { Launcher, Box, Todo, Explorer }`，每个视图提供 `Render(view, ctx)` / `HitTest(view, pt)` / `OnKey(view, msg)` 三组自由函数，用 `switch` 分发。不使用虚函数、不使用观察者模式、不使用消息总线。

**组件边界**：`model/` 完全无 Win32 依赖，可独立编译测试；`icons` / `fs_work` / `persist` 只通过数据结构（`std::vector`、`std::wstring`）与其他部分通信，不暴露 D2D 或窗口概念；`views/` 依赖渲染与模型，但不直接调用 Shell API。

## 5. 进程与窗口模型

**单实例**：`CreateMutexW(L"Local\\stargazer")`；已存在则 `FindWindowW` 找到主窗口，`PostMessage(WM_APP_SHOW)`，自身退出。

**主窗口**：单一窗口类，`WS_POPUP` 无边框。圆角用 Win11 的 `DWMWA_WINDOW_CORNER_PREFERENCE`；Win10 上为直角。标题条区域在 `WM_NCHITTEST` 返回 `HTCAPTION` 以支持拖动。默认尺寸 960×620（按 DPI 缩放），记忆上次尺寸与位置。

**呼出与隐藏**

| 触发 | 实现 |
|---|---|
| 全局热键 | `RegisterHotKey(hwnd, 1, MOD_CONTROL\|MOD_SHIFT, VK_SPACE)`，可在设置中修改 |
| 托盘 | `Shell_NotifyIcon` + `TrackPopupMenu` 右键菜单 |
| 隐藏 | `Esc`、再次按热键、托盘菜单 |

**不做失焦自动隐藏。** 早期设计里有 `WM_ACTIVATE(WA_INACTIVE)` → 隐藏，已删除，因为它在真实使用中会造成故障：
从资源管理器按住文件拖向本工具时，窗口在鼠标按下的那一刻就失去焦点并隐藏，用户根本拖不到目标上。
“拖放期间禁用隐藏”补不了这个洞——那个标志只在 `DragEnter` 时才置位，而窗口在更早的 `WM_ACTIVATE` 就已经消失了。

代价：窗口会一直浮在其它窗口之上直到被主动关掉（`WS_EX_TOPMOST`）。这是有意的取舍：它是“呼出型”面板，
宁可你不想要时按一下 `Esc`，也不要你想要时它自己不见了。

呼出定位：`GetCursorPos` → `MonitorFromPoint` → `GetMonitorInfo().rcWork`，窗口居中于鼠标所在显示器，clamp 到工作区避免压任务栏。提供"跟随鼠标位置"选项。

**空闲开销**：窗口隐藏后消息循环阻塞在 `GetMessage`，无定时器、无轮询、无重绘。这是内存与 CPU 指标的实现基础。

**DPI**：`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`，所有尺寸乘以 `dpi/96`。多显示器切换时重新计算。

## 6. 渲染层

- `ID2D1Factory1` + `ID2D1HwndRenderTarget`。不使用 DXGI swap chain。
- **设备丢失**：`EndDraw` 返回 `D2DERR_RECREATE_TARGET` 时释放全部设备相关资源并重建。所有位图资源经 `ReleaseDeviceResources()` / `CreateDeviceResources()` 一对函数管理。
- 文本用 `IDWriteTextFormat`，按 (字号, 字重, 对齐) 缓存复用，不每次创建。
- **按需重绘**：维护 `m_dirty`。`WM_MOUSEMOVE` 仅在**悬停项索引变化**时标脏（不是每次移动都重绘）。`WM_PAINT` 先检查脏标记，不脏则 `ValidateRect` 直接返回。
- **无动画**：滚动直接跳变。视觉质量靠圆角、抗锯齿、配色与留白，不靠动效。
- 颜色集中在一个 `Theme` 结构体（深色/浅色两套，启动时读注册表 `AppsUseLightTheme` 选择）。

## 7. 文本输入

**所有文本编辑使用原生 `EDIT` 子控件，绝不自绘。** 封装为 `InlineEdit`：

- 按需 `CreateWindowExW(L"EDIT", ...)`，`WS_CHILD | ES_AUTOHSCROLL`，`SetWindowSubclass` 拦截 `VK_RETURN`（提交）、`VK_ESCAPE`（取消）、`VK_TAB`（切换）
- `WM_SETFONT` 设置与 UI 一致的微软雅黑，字号按 DPI 换算
- `EM_SETMARGINS` 调整内边距，使其与自绘背景无缝
- 位置由视图提供，随滚动与 DPI 同步

子 HWND 天然位于父窗口客户区之上，无需特殊处理层级。这套方案免费获得：中文 IME 候选窗、光标、选区、Shift 选择、`Ctrl+A/C/V/X`、右键菜单。

用于：Todo 新增与编辑、Box 重命名、Explorer 路径栏、Launcher 搜索框。

## 8. Shell 集成与图标

**三级提取，从最便宜的开始，前一级失败才降级：**

| 级别 | API | 代价 | 阶段 |
|---|---|---|---|
| 1 | `SHGetFileInfoW(SHGFI_SYSICONINDEX)` + `SHGetImageList(SHIL_EXTRALARGE)` | 约 0.1ms，查表 | 1 |
| 2 | `SHGetFileInfoW` + `SHGFI_USEFILEATTRIBUTES`（按扩展名推断，不触盘） | 约 0 | 1 |
| 3 | `IShellItemImageFactory::GetImage` | 10ms 至数秒 | 4，仅网盘可见项 |

**硬约束：`WM_PAINT` 中不调用任何 Shell API。** 渲染遇到未加载项时绘制占位（扩展名哈希 → 固定 8 色调色板取色 + 首字母），把请求投递到线程 A，完成后 `PostMessage(WM_APP_ICON_READY)` 标脏重绘。效果为"骨架先出，图标后到"。

**缓存**：进程内 `std::unordered_map`，key 为规范化路径（小写、去尾斜杠）或图标系统索引，上限 300 项，LRU 淘汰。不做磁盘缓存——窗口按需呼出，进程存活期间缓存即在，冷启动重取数百图标仅数十毫秒。

**只取一档图标尺寸**（48 物理像素 × DPI 对应档），在 D2D 中缩放，不做多档提取。

**`.lnk` 解析**：`IShellLinkW` + `IPersistFile::Load` 取 target / args / workdir / icon。解析失败则按扩展名处理并提示。

## 9. 拖放

三种拖放，只有两种走 OLE：

| 场景 | 实现 | 量级 |
|---|---|---|
| 外部拖入 | `IDropTarget` + `RegisterDragDrop`，只接受 `CF_HDROP` | 约 80 行 |
| 拖出到外部 | `IDropSource` + `DoDragDrop` 提供 `CF_HDROP`；`WM_LBUTTONDOWN` 后移动超阈值才启动 | 约 100 行 |
| 内部拖拽（项在盒子间移动、调整顺序） | 自写鼠标状态机，**不走 OLE** | 约 60 行 |

内部拖拽走 OLE 需要自定义剪贴板格式、序列化与处理嵌套消息循环重入，自写状态机更短且完全可控。

必须处理的三个细节：

1. 拖放期间置 `m_inDrag = true` 抑制悬停高亮更新（外部拖拽悬停时鼠标位置不代表选择意图）。
2. `DoDragDrop` 跑嵌套消息循环会重入 `WM_PAINT`，用 `m_dragging` 标志保护状态机。
3. 从网盘拖出给外部程序时传递的是路径，"按需下载"由网盘客户端负责，本工具不介入。

## 10. 便携与数据持久化

**便携是唯一模式**：数据固定在 `exe目录\data\`，不使用 `%APPDATA%`。启动时探测可写性（创建再删除 `.writetest`）；不可写则明确提示"请将 stargazer.exe 放到可写目录后重试"并退出，不静默回落到 AppData（那会让用户找不到自己的数据）。

**格式**：每个模块一个 UTF-8 文本文件，一行一条记录，Tab 分隔字段，值内转义 `\t` `\n` `\\`。

```
data/launcher.txt   group \t name \t target \t args \t workdir \t icon
data/boxes.txt      box   \t name \t path
data/todo.txt       id \t done(0/1) \t created \t due \t prio \t text
data/config.txt     key  \t value
data/ui.txt         key  \t value      （窗口位置尺寸、上次视图、上次盒子）
```

选择理由：四个模块的数据全是扁平列表，无嵌套结构。自制行格式约 60 行含转义，无第三方依赖、人类可读、可手改、可 diff。引入 JSON 库是 25k 行头文件换一个用不上的嵌套能力。

**写入**：变更即写，原子替换（写 `.tmp` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`）。数据量最多数千行，单次写入 < 1ms，不需要防抖或后台写线程。

**读取**：全程 UTF-8（`WideCharToMultiByte(CP_UTF8)`），中文路径天然安全。**格式错误的行跳过并计数**，不使整个文件失败——此文件是用户可手改的信任边界。整体解析失败时备份为 `.bad` 并重建空文件，不静默丢数据。

**开机自启**：写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值为 `"<exe绝对路径>" --autostart`。不需要管理员、不需要安装器、不使用任务计划程序。三条配套要求：

1. `--autostart` 时直接进托盘，不显示窗口、不抢焦点。
2. **路径自愈**：每次启动比对该注册表项与当前 exe 路径，不一致就改写。便携程序会被移动，不自愈会产生"开机自启莫名失效"的幽灵问题。
3. **注册表是唯一真相**：`config.txt` 不存 `autostart`，托盘勾选状态实时读注册表，避免双份真相不同步。

**不做**：长路径 `\\?\` 前缀、数据库、数据加密、版本迁移框架（格式变更时在加载处按字段数分支，一处 `if`）。

## 11. 视图设计

呼出后焦点默认落在当前视图的主输入控件。`Esc` 一律隐藏窗口。顶部一行视图标签，可点击或 `Ctrl+Tab` / `Ctrl+1..4` 切换。

### 11.1 Launcher 快捷启动板

- **布局**：顶部分组标签行 + 常驻搜索框；主体为自动换行的图标网格，每行个数按可用宽度计算。
- **交互**：打字即搜索（子串匹配，拼音首字母后续再加）；`↓` 从搜索框进入网格；方向键移动；`Enter` 启动；`Del` 删除条目（不删文件）。
- **启动**：`ShellExecuteExW`（可获取错误码）。
- **添加**：从 Explorer 拖入即添加，自动解析 `.lnk` 取出目标与图标；右键菜单支持手工新建（名称 + 目标即可，其余字段可选）。
- **分组**：新建 / 改名 / 删除；把条目拖到分组标签即移动分组。

### 11.2 Box 文件收纳盒

- **布局**：盒子切换标签 + 图标网格。
- **数据语义**：盒子 = 一组路径引用，源文件位置不变，删除盒子或条目永不触及文件。
- **失效检测**：呼出时由线程 B 批量 `GetFileAttributesW` 校验，失效项灰显加删除线，点击提示"原文件已不存在"，支持一键清理失效项。
- **交互**：双击打开；`Del` 删引用；`F2` 改显示名；`Ctrl+C` 复制路径（同时提供 `CF_HDROP` 与 `CF_UNICODETEXT`）；拖入添加；拖出为 `CF_HDROP`；内部拖拽排序；拖到盒子标签即移动到该盒子。

### 11.3 Todo

- **布局**：底部**常驻** `EDIT`，回车新增并清空输入框且保持焦点；上下为任务列表。
- **交互**：自绘小复选框点击切换完成状态；双击进 `InlineEdit` 编辑；`Del` 删除；方向键移动选择。
- **排序**：未完成在上（优先级降序 → 创建时间降序），已完成沉底灰显。
- **明确不做**：重复任务、通知提醒、子任务、标签、日历、拖拽排序。

### 11.4 Explorer 网盘浏览

- **状态**：当前路径、历史栈（后退/前进）、项列表、加载中标志、滚动位置、`requestId`。
- **异步枚举**：线程 B 执行 `FindFirstFileW` / `FindNextFileW`，完成后 `PostMessage(WM_APP_DIR_LOADED, requestId)`；UI 线程比对 `requestId` 丢弃过期结果。
- **虚拟化**：只渲染可见行（固定行高 28×DPI）。网盘目录可达数千项，不虚拟化会同时压垮枚举、图标提取与渲染。
- **排序**：自写自然序比较（约 20 行，位于 `model/search`，无 Win32 依赖因而可测），效果为 `1.txt < 2.txt < 10.txt`，目录优先。使用自写实现而非 `StrCmpLogicalW` 是为了让排序规则落在可测试的 `model/` 内。
- **刷新策略**：手动 `F5` 为基准；`FindFirstChangeNotification` 注册成功才附加自动刷新——网盘挂载点常不支持，正确性不依赖它。
- **键盘**：`Enter` 进入/打开，`Backspace` 后退，`Alt+←/→` 前进后退，`F5` 刷新，`Ctrl+C` 复制路径。
- **路径栏**：常驻 `EDIT`，可直接粘贴路径跳转。
- **右键菜单**：自绘小菜单（打开 / 复制路径 / 添加到盒子 / 删除）。系统原生 `IContextMenu` 作为后续可选项。
- **核心价值**：拖条目到 Box 标签即添加引用。

## 12. 错误处理

- `SetUnhandledExceptionFilter` 写 `crash.log` 到 exe 目录；写失败则 `OutputDebugString`。不做自动重启。
- Shell 调用失败一律降级（图标取不到 → 通用图标；`.lnk` 解析失败 → 按扩展名），**不弹 MessageBox 打断操作流**，用托盘气泡提示。
- 目录枚举失败 → 列表内显示错误行 + `F5` 重试，不弹框。
- 数据文件写失败 → 托盘警告一次（便携版位于只读目录时的唯一反馈）。
- 数据文件单行格式错误 → 跳过并计数，加载完成后若计数 > 0 用托盘气泡告知行数。

## 13. 测试策略

`model/` 不依赖任何 Win32 头（仅 STL），单独编译为控制台 `test_model.exe`，用 `assert` 覆盖：

- 行格式转义 / 反解往返（含 `\t`、`\n`、中文、空字段、超长字段）
- 格式错误的行被跳过而非整文件失败
- `paths` 规范化（大小写、尾斜杠、`..`、重复分隔符、UNC）
- Box / 分组增删改与引用失效标记
- Todo 排序规则（优先级 → 创建时间 → 已完成沉底）
- 搜索匹配与自然序比较（`1.txt < 2.txt < 10.txt`、中文与大小写边界）

不引入测试框架（几行 `assert` 足够）。**UI 层不写自动化测试**，改为每阶段一份手工验收清单。

## 14. 工具链与构建

- CMake ≥ 3.20，生成器 Visual Studio 17 2022，MSVC v143
- C++20，`/O2 /MT`（静态 CRT，便携前提），`/W4`
- 零第三方依赖，仅链接 Windows SDK 与系统库（`d2d1` `dwrite` `shell32` `ole32` `shlwapi` `comctl32` `dwmapi`）
- 产物：单个 `stargazer.exe`
- 可选：UPX 压缩（不默认启用，可能触发杀软误报）

## 15. 阶段划分与验收

每个阶段结束时都是可用状态。

### 阶段 1 — 内核 + Launcher

单实例、托盘、全局热键、窗口呼出/隐藏、D2D 渲染循环、图标提取与 LRU 缓存、InlineEdit、行格式持久化、便携目录与可写性探测、注册表开机自启、Launcher 完整功能。

**验收**：冷启动 < 100ms；隐藏时任务管理器内存 ≤ 5MB；`Ctrl+Shift+Space` 呼出并在 `Esc` 后消失；从桌面拖 `.lnk` 进来自动解析并显示真实图标；双击能启动；重启后配置保留；搬到另一个目录后仍能开机自启。

### 阶段 2 — Box

引用式收纳盒、失效检测与灰显、拖入拖出、内部拖拽排序、`Ctrl+C` 复制路径、盒子管理。

**验收**：删条目后原文件仍在；把原文件改名后该条目灰显并在刷新后恢复；拖出到资源管理器能产生副本；拖拽过程中窗口不因失焦而消失。

### 阶段 3 — Todo

列表、常驻输入框、自绘复选框、编辑、排序。

**验收**：中文输入法正常唤起候选窗；连续回车新增多条；重启后完成状态与顺序保留；手工把 `todo.txt` 改坏一行，其余任务仍正常加载并给出提示。

### 阶段 4 — Explorer

异步目录枚举、虚拟化列表、历史栈、路径栏、排序、自绘右键菜单、`F5` 刷新、拖到 Box。

**验收**：浏览数千项的网盘目录时 UI 不冻结、滚动流畅；快速连续切换目录不出现旧目录结果覆盖新目录；断开网盘后打开该路径显示错误行而非崩溃或卡死。

## 16. 明确的设计取舍与升级路径

| 取舍 | 现在的做法 | 何时升级 |
|---|---|---|
| 无平滑动画 | 滚动直接跳变 | 观感被明确抱怨时 |
| 无鼠标中键呼出 | 只用热键与托盘 | 需要 `WH_MOUSE_LL` 全局钩子，会被杀软关注，按需再加 |
| 无失焦自动隐藏 | 窗口只能靠 `Esc`/热键/托盘关掉，且始终 TOPMOST | 若“一直浮在最上层”被抱怨，改成“失焦后去掉 TOPMOST”而不是隐藏 |
| 无拼音搜索 | 子串匹配 | 需要按拼音首字母搜索时加拼音表 |
| 无磁盘图标缓存 | 仅进程内 LRU 300 项 | 冷启动图标加载可感知偏慢时（需引入 PNG 编码） |
| 无缩略图 | 只用 Shell 图标 | 阶段 4 后按需对可见项启用 `IShellItemImageFactory` |
| 内部拖拽不走 OLE | 自写状态机 | 除非需要跨进程拖拽 |
| 无系统右键菜单 | 自绘小菜单 | 需要"用其他程序打开"等完整动词时接 `IContextMenu` |
| 单工作线程池 | 2 条固定线程 | 出现第三类慢任务时再统一调度 |
| 无长路径支持 | 标准 `MAX_PATH` | 遇到真实超长路径时加 `\\?\` 前缀处理 |
