# Stargazer 阶段 2（文件收纳盒 Box）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在阶段 1 的启动器上加入第二个视图"文件收纳盒"：把任意文件/文件夹的**路径引用**分盒陈列在网格里，支持失效检测、拖入拖出、复制路径、盒子的新建/改名/删除。

**Architecture:** 沿用阶段 1 的单进程单窗口与 `AppState`。新增三块基础设施：①把 launcher 里的网格布局/命中/键盘导航抽成 `views/grid`（布局与命中必须共用同一份算法，否则会出现"点得到但画不出"）；②顶层视图标签行 + `Ctrl+1..4` 视图切换；③第二个工作线程（文件系统）做批量存在性校验，与图标线程分开，避免慢盘阻塞图标提取。数据层完全复用：`Box`/`BoxItem`/`serialize_boxes`/`parse_boxes` 在阶段 1 已实现并被 `test_model` 覆盖。

**Tech Stack:** 沿用阶段 1：C++20、Win32、Direct2D 1.1 + DirectWrite、CMake + MSVC、零第三方依赖、`/MT`。

**Spec:** `docs/superpowers/specs/2026-09-22-stargazer-design.md`（§11.2 是本阶段的行为定义；§9 拖放；§15 阶段 2 验收）

**前置阅读（执行者必读，不要凭记忆）:**
- `src/viewapi.h`（`View` 枚举、`AppState`、`LauncherState`）
- `src/views/launcher.{h,cpp}`（网格布局/命中/渲染/键盘导航的现有实现，本阶段要把它抽出来）
- `src/app.cpp`（面板消息处理、`WM_COMMAND`/`EN_CHANGE`、内部拖拽状态机、`launch_selected`）
- `src/dragdrop.{h,cpp}`（`IDropTarget`，本阶段要加 `IDropSource`）
- `src/icons.{h,cpp}`（工作线程 A 的完整范式：请求队列 + key 查表 + 不携带索引的通知）
- `src/edit.{h,cpp}`（`InlineEdit`：`on_key` / `keep_open_on_blur` 两个钩子的语义）
- `.superpowers/sdd/2026-09-22-stargazer-phase1-kernel-launcher/progress.md`（阶段 1 的裁决与推迟项，尤其"分组改名/删除未实现"要在本阶段一并补）

## Global Constraints

（与阶段 1 相同，逐条照旧，别重新发明）
- C++20；`/W4 /utf-8 /permissive-`；Release `/O2`；静态 CRT `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`
- 零第三方依赖；只链接 `d2d1 dwrite shell32 ole32 shlwapi comctl32 dwmapi user32 gdi32`
- **`src/model/` 下不得出现任何 Windows 头**，只用 STL（这条让数据层可被控制台测试）
- 数据文件 UTF-8 无 BOM，Tab 分隔，字段内 `|` `Tab` `换行` 写作 `||` `|t` `|n`
- 坐标约定：D2D 用逻辑 DIP（`Renderer::client_logical()`），鼠标 lParam 与子 HWND 用物理像素，**换算只能经过 `Renderer::to_logical()` / `to_physical()`**，视图层不得自己乘除 DPI
- 空闲时不得有定时器/轮询；窗口隐藏时不重绘
- 便携：数据只在 `<exe>\data\`；注册表是 autostart 的唯一真相
- 鼠标消息在本机可能被真实用户操作干扰，**自动化验证一律走键盘/程序化路径**，不要靠投递 click 来判定

## Review Focus

阶段 2 最可能伤到使用者的五类输入/故障。每条都要落到宿主任务的测试或验收步骤里。

1. **收纳盒里指向的文件被外部改名/删除/移走** —— 该条目应灰显并标记为失效，其余条目照常可用，清理失效项要能一键完成，且**绝不因此删除或移动任何磁盘文件**
2. **跨盘/断开的网盘路径**（`D:\netdisk\...` 而 D 盘是 CD2 挂载点且当前离线）—— 校验必须异步、不得阻塞 UI；断盘不应让程序卡死或把整盒标成失效后自动清空
3. **拖出到资源管理器**（把盒子里的条目拖到桌面/文件夹窗口）—— 应产生副本/移动，且 `DoDragDrop` 的嵌套消息循环不得让窗口状态错乱（`m_dragging` 期间不响应某些输入）
4. **手改 `boxes.txt`**（字段数不对、盒子名重复、路径为空、含 `|` 转义）—— 坏行跳过并计数，空盒子名/空路径的行不产生"幽灵条目"
5. **同一路径在多个盒子里 / 同一盒子内重复添加** —— 不得因此崩溃或产生无法删除的重复项；拖入重复路径的去重行为要明确（本计划裁决：允许重复但不自动去重，因为"同一文件在两个盒子里"是合法用法）

---

## 任务总览

| # | 任务 | 新增/改动 |
|---|---|---|
| 1 | 抽出共用网格模块 `views/grid` | `views/grid.{h,cpp}`，launcher 改用 |
| 2 | 视图切换框架（顶层标签行 + `Ctrl+1..4` + ui.txt 记住视图） | `viewapi.h`、`app.cpp`、`views/grid` |
| 3 | Box 视图渲染与导航 | `views/box.{h,cpp}` |
| 4 | 失效检测（工作线程 B）+ 灰显 + 清理 | `fs_work.{h,cpp}`、`views/box.cpp` |
| 5 | Box 条目与盒子管理（Del/F2/Ctrl+C/双击/新建盒子） | `views/box.cpp`、`app.cpp` |
| 6 | 拖入按当前视图分发 | `app.cpp` |
| 7 | 拖出（`IDropSource`）+ 内部拖拽排序 | `dragdrop.{h,cpp}`、`views/box.cpp`、`app.cpp` |
| 8 | 阶段 2 验收 + 文档更新 | `README.md`、spec §16 |

---

### Task 1: 抽出共用网格模块

**为什么先做这个**：Box 的网格与 Launcher 的网格在**布局、命中、滚动、键盘导航**四处必须完全一致。复制一份出来，两边迟早漂移，症状是"能点到的格子和画出来的格子对不上"。

**Files:**
- Create: `src/views/grid.h`、`src/views/grid.cpp`
- Modify: `src/views/launcher.h`、`src/views/launcher.cpp`（改为调用 grid）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `sg::Renderer`（`client_logical` / `to_logical` / `to_physical` / `format` / `fill_round_rect` / `text` / `rt`）、`sg::icons_get`
- Produces（`views/grid.h`）:
  ```cpp
  namespace sg {
  constexpr float kCell = 96.f;
  constexpr float kGap = 8.f;
  constexpr float kPad = 16.f;
  constexpr float kTabsH = 36.f;    // 分组/盒子标签行
  constexpr float kSearchH = 34.f;  // 搜索框行

  // 网格需要知道的所有东西都由调用方喂进来，grid 不认识 Launcher 也不认识 Box
  struct GridItem {
      std::wstring label;      // 显示名
      std::wstring icon_src;   // 取图标的路径（空则用 label 画占位）
      bool is_dir = false;
      bool selected = false;
      bool hovered = false;
      bool missing = false;    // 失效项：灰显 + 删除线
  };

  // 网格区域在窗口里的纵向起点（视图可以用标签行/搜索框/两者）
  struct GridLayout {
      float top = 0.f;      // 网格区上沿（逻辑 DIP）
      float bottom = 0.f;   // 网格区下沿
      int cols = 1;
      int rows_visible = 1;
  };

  GridLayout grid_measure(float client_w, float client_h, float top);
  D2D1_RECT_F grid_cell_rect(const GridLayout& gl, int index, int scroll);
  // 返回 index，未命中返回 -1
  int grid_hittest(const GridLayout& gl, int count, int scroll, D2D1_POINT_2F pt);
  // 滚动夹紧；返回夹紧后的 scroll
  int grid_clamp_scroll(const GridLayout& gl, int count, int scroll);
  // 键盘导航；返回 true 表示按键被消费。移动 sel/scroll（sel<0 = 焦点不在网格）
  bool grid_keydown(const GridLayout& gl, int count, int& sel, int& scroll, UINT vk);

  // 把一屏可见的格子画出来（虚拟化：只画可见行）
  void grid_render(Renderer& r, const GridLayout& gl, const std::vector<GridItem>& items,
                   int scroll, D2D1_COLOR_F accent, D2D1_COLOR_F hover_color,
                   D2D1_COLOR_F text_color, D2D1_COLOR_F missing_color);

  // 按扩展名给占位块配色（从 launcher 搬过来，两边共用）
  D2D1_COLOR_F ext_color(const std::wstring& path);
  }
  ```

- [ ] **Step 1: 先把 launcher 的行为钉死再搬**

在搬代码之前，先给现有 Launcher 的行为留一份可对照的证据，否则搬完无法判断是否等价：

```powershell
# 手工写下 data\launcher.txt（8 条已在阶段 1 用过），然后
& powershell -NoProfile -ExecutionPolicy Bypass -File `
  ".superpowers\sdd\2026-09-22-stargazer-phase1-kernel-launcher\gui-probe.ps1" -Ascii "0,0,1440,420"
```

把输出贴进 ledger（`Task 1: 搬迁前的 launcher 网格 ASCII 基线`）。搬完以后用**同一份数据、同一个区域**再跑一次，两次的 ASCII 图必须逐字符一致（允许图标像素因为异步到达而不同，但格子位置、选中高亮位置必须一致）。

- [ ] **Step 2: 新建 `views/grid.h`，把上面的接口原样写进去**

只写声明与常量，不写实现。`GridItem` 刻意不含 `LaunchItem`/`BoxItem`：视图负责把各自的数据翻译成 `GridItem`，网格因此不需要认识任何数据模型。

- [ ] **Step 3: 从 launcher.cpp 搬运实现**

`views/grid.cpp` 的实现从 `launcher.cpp` 逐段搬，**只改签名不改算法**：
- `launcher_layout` 的列数/行数计算 → `grid_measure`
- `launcher_cell_rect` → `grid_cell_rect`（加 `scroll` 参数，去掉对 `LauncherState` 的依赖）
- `launcher_hittest` 的格子命中部分 → `grid_hittest`
- `launcher_render` 里 `for (size_t i = 0; i < ls.filtered.size(); ++i)` 那段（虚拟化 + 图标/占位 + 名字）→ `grid_render`
- `launcher_keydown` 里 `VK_DOWN/UP/LEFT/RIGHT/PRIOR/NEXT` 那段 → `grid_keydown`
- `ext_color` 平移过来

`missing` 为真时：先画正常底与图标，再把名字换成 `missing_color`，并在名字中段画一条 1px 横线（删除线）。删除线的 y 取名字矩形的垂直中点。

- [ ] **Step 4: launcher 改为调用 grid**

`launcher_render` 里改成：构造 `std::vector<GridItem>`（把 `filtered` 里的条目翻译过来，填 `selected`/`hovered`/`missing=false`），然后调 `grid_render`。`launcher_hittest`/`launcher_keydown` 同理转发。

`launcher.h` 保留 `kTabsH`/`kSearchH` 的引用（分组标签行与搜索框仍是 launcher 自己的），但把它们指向 `grid.h` 里的常量，避免两处各写一份。

- [ ] **Step 5: 构建并跑两套测试**

```powershell
$CMAKE = 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $CMAKE --build build --config Release --target stargazer test_model test_io
.\build\Release\test_model.exe ; .\build\Release\test_io.exe
```
预期：`BUILD_EXIT=0`，两个测试都是 `OK`。

- [ ] **Step 6: 与基线逐字符对照**

重跑 Step 1 的探针命令，把 ASCII 图与基线对比。预期：**完全一致**。不一致就是搬错了，回去查，不要改基线。

- [ ] **Step 7: 提交**

```powershell
git add src/views/grid.h src/views/grid.cpp src/views/launcher.h src/views/launcher.cpp CMakeLists.txt
git commit -m "refactor(views): 抽出共用网格模块，launcher 改用它"
```

---

### Task 2: 视图切换框架

**Files:**
- Modify: `src/viewapi.h`（`AppState::view` 已存在；新增视图标签行的常量）
- Modify: `src/views/grid.{h,cpp}`（新增 `grid_view_tabs_rect` / `grid_view_hittest`，与分组标签行同款算法）
- Modify: `src/app.cpp`（`WM_LBUTTONDOWN` 命中视图标签、`WM_KEYDOWN` 处理 `Ctrl+1..4` 与 `Ctrl+Tab`）
- Modify: `src/views/launcher.cpp`（launcher 的标签行下移到视图标签行之下）

**Interfaces:**
- Produces:
  ```cpp
  constexpr float kViewTabsH = 28.f;
  D2D1_RECT_F view_tabs_rect(D2D1_SIZE_F client);
  // 返回视图下标（0..3），未命中 -1
  int view_tab_hittest(D2D1_SIZE_F client, D2D1_POINT_2F pt);
  void view_tabs_render(Renderer& r, D2D1_SIZE_F client, int active);
  const wchar_t* view_name(int v);   // L"启动板" / L"收纳盒" / L"待办" / L"浏览"
  ```

- [ ] **Step 1: 视图标签行**

高度 `kViewTabsH`，置于窗口最顶部（y 0..28 逻辑）。四个标签等宽或按名字宽度排布（照 `launcher_tabs_rect` 的做法）。活动项用 `theme.accent` 底 + 白字，其余用 `theme.card` 底 + `theme.text_dim` 字。

**这一步要动 launcher 的纵向偏移**：launcher 的分组标签行与搜索框整体下移 `kViewTabsH`。改 `launcher_tabs_rect`/`launcher_search_rect` 的 top，并把它们传给 `grid_measure` 的 `top` 一并加上。改完必须重跑 Task 1 的 ASCII 基线——**这次不一致是预期的**（整体下移 28 逻辑像素），在 ledger 里记下新的基线。

- [ ] **Step 2: 鼠标与键盘切换**

`WM_LBUTTONDOWN`：先查视图标签（在分组标签之前），命中就 `state.view = ...`、重置该视图的选中态、`InvalidateRect`。
`WM_KEYDOWN`：`Ctrl+1..4` 直接切；`Ctrl+Tab` 循环切。切换时若搜索框开着，先 `search.close()`，再按新视图决定是否重开（Launcher 需要，Box 视情况）。

`ui.txt` 增加 `view` 键（存视图序号），`app_load` 读取、`app_save_ui` 写入。

- [ ] **Step 3: 空 Box 视图占位**

Box 视图此时还没实现，先画一行"收纳盒：尚未实现"的灰字，确认切换时窗口内容真的换了（用探针看画面变化即可）。

- [ ] **Step 4: 验证与提交**

验证：程序化投递 `Ctrl+2`（或用 `PostMessage` 直接发 `WM_KEYDOWN` 带 `Ctrl` 状态不可靠，改用**视图标签的鼠标命中**——但鼠标投递在本机不可靠，因此实际采用的方式是：临时加一个 `--view=2` 命令行参数用于验收，或直接改 `ui.txt` 的 `view=1` 后重启）。取证：探针 ASCII 图在 `view=0` 与 `view=1` 下不同。

提交信息：`feat(app): 视图切换框架（顶层标签行 + Ctrl+1..4 + ui.txt 记忆）`

---

### Task 3: Box 视图渲染与导航

**Files:**
- Create: `src/views/box.h`、`src/views/box.cpp`
- Modify: `src/viewapi.h`（新增 `BoxState`）
- Modify: `src/app.cpp`（`WM_PAINT` 按 `state.view` 分发；`WM_KEYDOWN`/`WM_MOUSEMOVE`/`WM_LBUTTONDOWN` 同理）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  struct BoxState {
      int box = 0;            // 当前盒子下标
      int sel = -1;           // 网格选中下标；-1 = 无
      int hover = -1;
      int scroll = 0;
      int drag_over_tab = -1;
  };
  // AppState 新增：BoxState box_view;
  ```
  视图分发：`void box_render(App&)` / `bool box_keydown(App&, UINT vk)` / `int box_hittest(App&, D2D1_POINT_2F)` —— 与 launcher 的对应函数同签名风格，由 `app.cpp` 用 `switch (state.view)` 调用。

- [ ] **Step 1: 盒子标签行 + 网格**

盒子标签行与搜索框行**不同**：Box 没有搜索框（本阶段不做过滤），标签行下面是网格。所以 `grid_measure(client_w, client_h, top = kViewTabsH + kPad + kTabsH + kPad)`。

`BoxItem` 翻译成 `GridItem`：`label = item.name`，`icon_src = item.path`，`is_dir = path 以 '\\' 结尾`，`missing = item.missing`（Task 4 才填，现在恒 false）。

- [ ] **Step 2: 键盘与鼠标**

`VK_DELETE` 删引用、`VK_F2` 改显示名、方向键走 `grid_keydown`、`Ctrl+C` 复制路径、`VK_RETURN` 打开（Task 5 实现具体动作，本步先接上分发）。鼠标与 launcher 同款（悬停只在变化时重绘、点击命中格子）。

- [ ] **Step 3: 验收与提交**

验证：手工写 `data/boxes.txt` 若干行（**路径里要包含 `C:\temp` 与 `C:\new folder` 这两类会踩转义坑的写法**），启动后切到 `view=1`，用探针取 ASCII 图确认盒子标签、格子、图标都出现。取证贴进 ledger。

---

### Task 4: 失效检测（工作线程 B）

**Files:**
- Create: `src/fs_work.h`、`src/fs_work.cpp`
- Modify: `src/model/store.h`（`BoxItem` 增加 `bool missing = false;` —— 运行期状态，**不参与序列化**）
- Modify: `src/views/box.cpp`（呼出时投递校验请求；`WM_APP_FS_CHECKED` 后标脏重绘）
- Modify: `src/app.cpp`（处理 `WM_APP_FS_CHECKED`）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 阶段 1 图标线程的范式（`icons.cpp` 的队列/锁/通知结构照抄，但**不共享锁**）
- Produces:
  ```cpp
  // 投递一批需要校验存在性的路径；完成后向 notify_hwnd 投 WM_APP_FS_CHECKED
  void fs_init(HWND notify_hwnd);
  void fs_shutdown();
  // 请求校验（去重：同一路径只排一次）；结果通过回调拿
  void fs_check_paths(const std::vector<std::wstring>& paths,
                      std::function<void(const std::wstring& path, bool exists)> on_result);
  ```
  新增消息 `constexpr UINT WM_APP_FS_CHECKED = WM_APP + 4;`

- [ ] **Step 1: 为什么单开一条线程**

图标线程做的是 Shell 图标提取（可能几毫秒一次），文件系统校验做的是 `GetFileAttributesW`（网盘上可能几秒一次）。放同一条队列里，一个断盘的路径会把它后面所有图标请求堵住。这是 spec §4「线程 A：图标；线程 B：文件系统」的理由，照做。

- [ ] **Step 2: 实现**

照 `src/icons.cpp` 的结构写：`std::mutex` + `condition_variable` + `deque<Request>` + `std::set` 去重 + `g_quit` + `join`。
校验函数用 `GetFileAttributesW`：`INVALID_FILE_ATTRIBUTES` 且 `GetLastError() == ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND` → 不存在；其它错误（断盘、权限）→ **当作存在**（保守：宁可显示正常也不要误标失效，避免用户一键清理时误删引用）。

- [ ] **Step 3: 呼出时校验**

`app_show` 里、`ensure_panel` 之后：把**当前视图**需要校验的路径收集起来一次性投递。不要一次投递所有盒子的所有条目——大盒子会拖慢。只校验当前盒子。

回调在 UI 线程（`WM_APP_FS_CHECKED` 里）执行：按 path 找到对应 `BoxItem` 并置 `missing`。**不能按索引回填**：投递到结果回来之间用户可能删了条目或换了盒子（这与图标缓存"不用索引、按 key 查表"是同一个理由）。

- [ ] **Step 4: 灰显 + 删除线 + 清理**

`grid_render` 的 `missing` 分支已在 Task 1 实现。清理入口：`Ctrl+Shift+Delete` 或在右键菜单加一项"清理失效项"，实现为一次 `std::remove_if` + `data_dirty = true`。**清理只删引用，不碰磁盘**。

- [ ] **Step 5: 验收与提交**

验证（可自动化）：写一个 `boxes.txt` 指向一个确实不存在的路径 + 一个存在的路径，启动，等待 3 秒，用探针确认：失效项的名字区域出现删除线像素、有效项没有。再把路径改回存在，重启后删除线消失。

预期不可自动化：断网盘的行为（需要真断 CD2 挂载）。列入 Task 8 人工验收。

---

### Task 5: Box 条目与盒子管理

**Files:**
- Modify: `src/views/box.cpp`、`src/views/box.h`
- Modify: `src/app.cpp`（右键菜单按视图分发；`launcher_context_menu` 改名为 `view_context_menu`）

**Interfaces:**
- Produces:
  ```cpp
  void box_delete_selected(App&);      // 只删引用
  void box_rename_selected(App&);      // InlineEdit 叠在格子上
  void box_add_box(App&);              // 连续编号命名
  void box_rename_box(App&, int index);
  void box_delete_box(App&, int index);  // 空盒子直接删；非空要确认
  void box_copy_selected(App&);          // Ctrl+C：同时给 CF_HDROP 与 CF_UNICODETEXT
  void box_open_selected(App&);          // ShellExecute 打开
  void box_clear_missing(App&);
  ```

- [ ] **Step 1: 顺手把阶段 1 欠的补上**

阶段 1 账本里有两项**未实现**：`launcher_rename_group` 与 `launcher_delete_group`（计划承诺了但步骤里没写）。本任务一并补：`launcher_rename_group` 用 `InlineEdit` 叠在分组标签上；`launcher_delete_group` 非空时先问一次，删掉后把 `group` 夹紧。补完更新 README 的"已知限制"，把那条删掉。

- [ ] **Step 2: `Ctrl+C` 复制路径**

同时提供两种格式，粘贴到哪里都能用：
```cpp
// CF_UNICODETEXT：纯文本路径
// CF_HDROP：粘贴到资源管理器里等于"粘贴文件"
```
用 `OpenClipboard(hwnd)` → `EmptyClipboard()` → 两次 `SetClipboardData` → `CloseClipboard()`。`CF_HDROP` 的构造见 Task 7 的 `make_hdrop`（与拖出共用同一个函数，不要写两遍）。

- [ ] **Step 3: 验收与提交**

验证（可自动化）：`view=1`、选中一项、投 `VK_DELETE` → 网格少一格且 `boxes.txt` 少一行、**原文件仍在磁盘**；投 `VK_F2` → 输入框出现在该格子上（用 `GetWindowRect` 对比格子矩形）。

---

### Task 6: 拖入按当前视图分发

**Files:**
- Modify: `src/app.cpp`（`dragdrop_set_hook` 的回调里按 `state.view` 分发）

- [ ] **Step 1: 分发**

`kbHit` 现在写死"加入当前 Launcher 分组"。改成：
```cpp
if (s.view == View::Launcher) { ...现有逻辑... }
else if (s.view == View::Box) {
    if (s.boxes.empty()) s.boxes.push_back(Box{ L"新盒子", {} });
    auto& b = s.boxes[std::clamp(s.box_view.box, 0, (int)s.boxes.size() - 1)];
    for (const auto& p : paths) b.items.push_back(BoxItem{ file_name(p), p });
    s.data_dirty = true;
    box_refilter_or_clamp(s);   // 无过滤，只需夹紧 sel/scroll
}
```
去重决策（见 Review Focus 5）：**不自动去重**。同一文件可以同时出现在两个盒子里，也可以在一个盒子里出现两次（用户可能故意如此）。删除是按条目删，不会有"删不掉的重复项"。

- [ ] **Step 2: 验收与提交**

验证：拖入无法自动化（OLE 拖放不可编程模拟），改为**人工验收项**，在 Task 8 列出。本步只验证编译通过 + 分发逻辑被走到（可用一个临时的 `--drop-test <path>` 命令行参数直接调用 hook，验证分发结果写进了正确的盒子）。

---

### Task 7: 拖出（`IDropSource`）+ 内部拖拽排序

**Files:**
- Modify: `src/dragdrop.{h,cpp}`（新增 `IDropSource` 实现与 `make_hdrop`）
- Modify: `src/views/box.cpp`、`src/app.cpp`（左键按住条目移动超阈值 → `DoDragDrop`）

**Interfaces:**
- Produces:
  ```cpp
  // 构造 CF_HDROP（全局内存），返回 HDROP；失败返回 nullptr。
  // 调用方负责在 DoDragDrop/SetClipboardData 之后不要再碰它（所有权已转移）
  HGLOBAL make_hdrop(const std::vector<std::wstring>& paths);
  // 发起拖出。effect 允许 DROPEFFECT_COPY | DROPEFFECT_MOVE
  bool dragdrop_begin_drag(HWND owner, const std::vector<std::wstring>& paths);
  ```

- [ ] **Step 1: `make_hdrop`**

`DROPFILES` 头 + 以 `\0` 分隔、双 `\0` 结尾的路径串，`fWide = TRUE`。长度 `sizeof(DROPFILES) + (总字符数 + 1) * sizeof(wchar_t)`。`GlobalAlloc(GHND)`。写完 `GlobalUnlock`。这是与资源管理器互通的关键格式，**路径必须是绝对路径**。

- [ ] **Step 2: `IDropSource`**

只用 `QueryContinueDrag`：`MK_LBUTTON` 未按下且 `Esc` 未按下 → `DRAGDROP_S_DROP`；`Esc` → `DRAGDROP_S_CANCEL`；否则 `S_OK`。`GiveFeedback` 返回 `DRAGDROP_S_USEDEFAULTCURSORS`。

`DoDragDrop` 会跑嵌套消息循环，期间会重入 `WM_PAINT`。用 `app.in_drag = true` 抑制悬停更新（阶段 1 已有该标志），并在 `finally` 语义上确保复位（用一个小 RAII 或确保所有分支都复位）。

- [ ] **Step 3: 内部拖拽排序**

Box 的内部拖拽**不走 OLE**（照阶段 1 Launcher 拖到标签换组那套状态机扩展）：按住条目移动，落在**另一个格子**上 → 交换位置；落在标签上 → 换盒子。为避免状态机复杂化，本阶段只做**换盒子**（与 launcher 一致），**格子间排序推迟**，并在 README 写明。

- [ ] **Step 4: 验收与提交**

验证：`dragdrop_begin_drag` 无法自动验证（需要交互式拖拽）；但 `make_hdrop` **可以**自动验证——加一条 `test_io` 用例：构造 3 个路径 → 读回 `DROPFILES` 头与串 → 断言逐字节一致且是双 `\0` 结尾。这条测试能挡住最容易错的部分（结尾的 NUL 数量、`fWide`、偏移）。

---

### Task 8: 阶段 2 验收 + 文档

**Files:**
- Modify: `README.md`（新增收纳盒用法、更新"已知限制"、补内存数字）
- Modify: `docs/superpowers/specs/2026-09-22-stargazer-design.md`（§16 取舍表补阶段 2 的新增项）

- [ ] **Step 1: 可自动化的验收**

沿用 `.superpowers/sdd/2026-09-22-stargazer-phase1-kernel-launcher/acceptance.ps1` 的做法（把它复制成阶段 2 的版本）：

| # | 项 | 期望 |
|---|---|---|
| 1 | `Del` 删条目后原文件仍在 | 是 |
| 2 | 指向不存在路径的条目 | 灰显 + 删除线 |
| 3 | 把文件改名后重启 | 该条目失效；改回后恢复 |
| 4 | 清理失效项 | 只少引用，磁盘文件一个不少 |
| 5 | 手改 `boxes.txt` 混入坏行 | 坏行跳过，其余正常，托盘气泡提示 |
| 6 | 路径含 `C:\temp` / `C:\new folder` | 读写往返一字不差 |
| 7 | 同一路径在两个盒子里 | 都正常显示，删其中一个不影响另一个 |
| 8 | `Ctrl+C` 后剪贴板内容 | `CF_HDROP` 与 `CF_UNICODETEXT` 都在，路径正确 |
| 9 | 内存 | 隐藏态仍 ≤10MB，呼出态不因为多了 Box 视图而翻倍 |

- [ ] **Step 2: 人工验收（必须由人来做的）**

| # | 项 | 说明 |
|---|---|---|
| 10 | 从资源管理器拖文件到 Box | 条目出现在当前盒子 |
| 11 | 把条目拖出到桌面/文件夹 | 产生副本或移动，源程序不崩 |
| 12 | 断开盘符（CD2 网盘离线）后打开该盒子 | UI 不冻结；条目不被误标失效后自动清空 |
| 13 | 拖到盒子标签换盒 | 条目换盒且标签高亮 |
| 14 | 右键菜单各项 | 外观与动作正确 |

- [ ] **Step 3: 更新文档并提交**

README 补：收纳盒用法、`boxes.txt` 格式、已知限制（格子间排序未做、无搜索框、无缩略图）。spec §16 补：`missing` 只存运行期不进文件、拖出只支持 `CF_HDROP`、内部排序推迟。

---

## 完成后的状态

- `stargazer.exe` 同时具备启动板与文件收纳盒，`Ctrl+1/2` 切换
- 收纳盒是**纯引用**：删除条目、清理失效项都不碰磁盘
- 新增两个可复用的东西：`views/grid`（布局与命中唯一的实现）与 `fs_work`（第二条工作线程的范式）
- 阶段 3（Todo）、4（网盘浏览）各自另写计划
