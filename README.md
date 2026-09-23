# Stargazer

便携式 Windows 桌面效率工具。三个视图均已实现：**文件收纳盒**、**待办**（随手一记）、**浏览**（读本地/网盘目录）。

> 原设计的“启动板”已按用户决定**彻底删除**（代码与 `data\launcher.txt` 一并移除，无迁移、无备份）：
> 找文件交给“浏览”，归档交给“收纳盒”，两者合起来取代了启动板的位置。

## 构建

要求：Visual Studio（含 C++ 桌面开发工作负载，本机为 VS 18 Community）+ 自带 CMake。

```powershell
$CMAKE = 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $CMAKE -S . -B build -G "Visual Studio 18 2026" -A x64 `
  "-DCMAKE_GENERATOR_INSTANCE=D:\Program Files\Microsoft Visual Studio\18\Community,version=18.0.0.0"
& $CMAKE --build build --config Release --target stargazer
```

> 若本机 VS 实例已正确注册在 VS Installer 里，可省掉 `-DCMAKE_GENERATOR_INSTANCE`，并把生成器换成对应版本。

产物为 `build\Release\stargazer.exe`，单个文件，静态 CRT，不依赖任何运行时，也不需要 .NET / WebView2。

## 测试

```powershell
& $CMAKE --build build --config Release --target test_model test_io test_layout
.\build\Release\test_model.exe
.\build\Release\test_io.exe
.\build\Release\test_layout.exe
```

`test_model` 覆盖行格式（含 Windows 路径不被误转义的回归）、路径规范化与上一级路径、自然序比较、
目录排序与新建文件夹命名、数据序列化与排序、收纳盒/待办（六字段、未知 kind→文字、副本归属）的往返与坏行跳过、
拖入的容器逻辑（`box_add_paths`、`box_move_item`、盒子重名检查）、BGRA 预乘；
`test_io` 覆盖 UTF-8 原子读写、目录可写性探测、`CF_HDROP` 构造（逐字节 + 双 NUL）、DIB→PNG 四种变体（24/32bpp × 两种行序）；
`test_layout` 覆盖共用网格与待办不等高行的布局/命中/导航。

## 使用

窗口顶部一行视图标签：`收纳盒` / `待办` / `浏览`（Win11 顶部标签：选中项带强调色下划指示条）。

- 切换视图：点标签，或 `Ctrl+1..3`，或 `Ctrl+Tab` 循环；上次的视图记在 `ui.txt`；
  首次启动（还没有 `ui.txt`）停在**收纳盒**
- 窗口拖动：顶部标签行的空白处按住拖动（标签本身是点击区，不会把点击变成拖窗口）；
  四边四角 6px 内可缩放。窗口外框是 Win11 的圆角 + 系统投影（DWM 属性，失败则退为直角）
- 呼出热键 `Ctrl+Shift+Space`（**可在托盘菜单里改**：右键托盘图标 → 设置呼出热键…，
  也可以直接改 `config.txt` 的 `hotkey_mods` / `hotkey_key`）；再次按同一热键、按 `Esc`、
  或托盘菜单均可隐藏
- **位置记忆**：隐藏后再呼出就停在原来那个地方（不会跳回屏幕中央）；位置与尺寸记在 `ui.txt`，
  首次启动才居中于鼠标所在显示器。显示器拔了 / 分辨率变了会自动夹回工作区

### 收纳盒

盒子就是一组**路径引用**：条目指向文件所在的位置，源文件永远不被移动或删除。

- 双击条目打开；`Enter` 同效
- `Del` 删除引用（**不碰磁盘**）；`F2` 改显示名（可与真实文件名不同）
- `Ctrl+C` 复制路径：同时给 `CF_HDROP` 与纯文本，粘到资源管理器就是“粘贴文件”，粘到编辑器就是一行路径
- 按住条目拖到别的盒子标签上松开即换盒；拖出窗口则变成 OLE 拖出（对方拿到的是路径）
- `Ctrl+Shift+D` 或右键菜单“清理失效项”：一次性删掉指向不存在文件的引用（与待办清空已完成同一个键）
- 右键菜单：新建盒子 / 重命名盒子 / 删除盒子 / 打开 / 复制路径 / 重命名条目 / 删除条目 / 清理失效项
- **失效检测**：呼出时由第二条工作线程批量校验存在性，指向的文件被改名或删除后该条目会灰显并加删除线，
  其余条目照常用；文件改回来就恢复。校验是异步的，断开的网盘不会卡住界面

顶部标题条可拖动移动窗口，四边与四角可缩放（尺寸与位置记在 `ui.txt`）；滚轮滚动网格。

### 待办（随手一记）

一个列表 + 底部常驻输入框。**类型不用选**：粘什么就是什么。

- 打字 / 粘文本 → **回车入列**（文本先落在输入框里，所以还能改错字、贴多行）
- 粘以 `http://` / `https://` 开头的文本 → **链接**（蓝色带下划线，`Enter` 或双击进默认浏览器）
- 粘截图（`Win+Shift+S` 等）→ **图片条目**，本体存成 `data\images\<id>.png`（便携，程序搬走图还在）
- 粘/拖一个磁盘上的图片文件 → **图片引用**（不复制内容；被外部改名/删除后该行灰显加删除线）
- 剪贴板里同时有文本与位图（Excel 单元格、Word 图文）→ 记成**文字**，不会变成一张图
- 键盘：`↑↓` 选、`Space` 勾选（沉底灰显）、`Enter` 打开、`Del` 删条目、`F2` 改文字、
  `Ctrl+C` 复制（图片给 `CF_HDROP`，可粘到资源管理器）、`Ctrl+V` 粘贴、`Ctrl+Shift+D` 清空已完成
- 右键菜单：打开 / 复制 / 编辑 / 切换完成 / 删除 / 在资源管理器中显示 / 粘贴 / 清空已完成
- 缩略图用系统缩略图服务，只对可见行取、LRU 32 张；取不到就画占位色块，不弹框

> 待办删条目时**只删自己存在 `data\images\` 里的那份副本**，且只当没有其他条目还在指向它时才删；
> 引用型的外部图片文件一根手指都不碰。

### 浏览（读本地目录 / 网盘目录）

- 键盘：`↑↓` 选行、`PgUp/PgDn`、`Home/End`、`Enter` 打开（目录 = 进入）、`Backspace` 上一级、
  `Alt+←/→` 后退/前进、`Ctrl+L` 地址栏、`Ctrl+C` 复制路径、`Ctrl+V` 把剪贴板里的文件复制到当前目录、
  `F2` 改名、`F5` 刷新、`F7` 新建文件夹、`Del` 删除到回收站、`Shift+Del` 永久删除
- 左手位浏览（不想把手从 `WASD` 上拿开时）：`Ctrl+W` / `Ctrl+S` 上下移动选中，
  `Ctrl+A` / `Ctrl+D` 后退 / 前进（与 `↑↓`、`Alt+←/→`、`Backspace` 并存）

**不做失焦自动隐藏**：窗口会一直浮在最上层，直到你按 `Esc`、再按一次热键或用托盘菜单关掉。
这是有意的取舍 —— 否则从资源管理器按住文件往窗口里拖时，窗口会在鼠标按下那一刻就消失。

## 数据

全部数据在 exe 同级的 `data\` 目录下，纯文本，可直接手改：

| 文件 | 内容 |
|---|---|
| `boxes.txt` | `盒子 \t 名称 \t 路径`（空盒子写一行“盒子名 + 两个空字段”的占位行） |
| `todo.txt` | `id \t done \t created \t kind \t text \t attach`（`kind` = `text`/`link`/`image`） |
| `config.txt` | `键 \t 值`：`browse_root`（浏览的根目录）、`hotkey_mods` / `hotkey_key`（呼出热键的修饰键位与虚拟键码，托盘菜单可改） |
| `ui.txt` | 窗口尺寸（逻辑像素）、位置（物理像素）、上次视图（`0` 收纳盒 / `1` 待办 / `2` 浏览）、上次盒子 |

UTF-8 编码、无 BOM，行内 Tab 分隔，字段内 `|` `Tab` `换行` 分别写作 `||` `|t` `|n`。

> 转义符是 `|` 而不是反斜杠：Windows 文件名里不允许出现 `|`，所以手写的路径**永远不需要转义**。
> 用反斜杠做转义符会让 `C:\temp`（含 `\t`）和 `D:\new folder`（含 `\n`）在读写时被拆坏。

格式不对的行会被跳过并计数，不影响其余记录；单行写坏不会让整个文件加载失败。
空盒子名或空路径的行会被当作坏行跳过（不会变成看得见却没什么可做的条目）。

`boxes.txt` 里的路径是纯引用：删条目、删盒子、清理失效项都只改这个文件，**磁盘上的文件一个也不会动**。

开机自启的真实状态在注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `stargazer` 值，不在 `config.txt`。
程序被移动到别的目录后，下次启动会自动修正该路径。

## 资源占用（本机实测，150% DPI，默认 960×620）

| 状态 | 工作集 |
|---|---|
| 启动后未呼出（仅托盘） | 11 MB |
| 隐藏态 | 6 MB |
| 呼出态（稳态） | 15–17 MB（三个视图都在，含待办缩略图缓存上限约 4.7 MB） |
| 空闲 CPU | 0 ms / 5 s |

两个关键手法：**懒加载**（面板窗口与 D2D 工厂推迟到首次呼出才建，否则仅驻留托盘就要 37 MB）、
**隐藏时交还**（释放渲染目标与图标位图，并把常驻页赶回系统，否则呼出过一次后永远停在 54 MB）。
私有提交量约 64 MB 且不下降——那是 D2D/DWrite 分配器的高水位，只有销毁工厂才能收回，代价是每次呼出都要重建设备。

## 已知限制（阶段 1 / 2 范围内有意不做）

- 无设置界面：除了「呼出热键」可以在托盘菜单里改（`config.txt` 也能手改），其余没有可视化设置；
  配色是 Win11 Fluent 深色（只做深色，不跟随系统主题），写死在 `render.h` 的 `Theme` 里
- 无滚动条（滚轮 / 键盘），无按压与焦点动画；圆角、悬停填充与下划指示条是静态的
- 系统画的对话框（MessageBox、右键菜单）靠 uxtheme 的 `SetPreferredAppMode(ForceDark)` 变深色；
  系统本身是浅色主题时它们仍会是浅色（本程序的自绘部分不受影响）
- 无拼音搜索（子串匹配，中文按码点比较）
- 无磁盘图标缓存：图标统一按 48×48 提取，进程内 LRU 上限 300 项（约 2.7 MB）
- 无缩略图（仅列表视图；待办图片行有缩略图）、无平滑滚动动画、无鼠标中键呼出
- 只支持 `MAX_PATH` 以内的路径
- 收纳盒：**格子间的排序未实现**（只支持拖到标签换盒）；无搜索过滤框；无多选
- 收纳盒：拖出只提供 `CF_HDROP`（没有自定义剪贴板格式）；拖出不会删掉本地引用，
  若接收方选了“移动”，该条目会在下次呼出时变成失效项
- 收纳盒的失效判定是**保守**的：只有明确的“文件/路径不存在”才标失效，断盘、无权限等一律当作存在
- 待办：无截止日期/优先级/提醒/子任务/标签/搜索/拖动排序/多选；一条只能是一种内容
- 待办：缩略图缓存上限 32 张；同一次呼出期间取不到图的条目不会自动重试（再呼出一次即可）
- **拖放要求 OLE 初始化**：必须用 `OleInitialize`（只调 `CoInitializeEx` 时 `RegisterDragDrop` 返回
  `0x8007000E`，面板不会是拖放目标，症状是拖动时光标全程显示“禁止”）—— 踩过一次，记在这里
- `data\*.txt` 若是非 UTF-8 编码（例如被存成 ANSI），程序会备份成 `.bad` 并重新开始，不会静默改写乱码

## 架构

```
src/
  main.cpp          入口、单实例、消息循环
  app.{h,cpp}       窗口生命周期、呼出/隐藏、视图分发、输入路由、数据加载与落盘
  render.{h,cpp}    D2D/DWrite 初始化、设备丢失重建、坐标系统一换算
  icons.{h,cpp}     Shell 图标三级提取、LRU 缓存、工作线程 A（图标）
  fs_work.{h,cpp}   存在性校验、工作线程 B（文件系统）—— 与图标线程分开，避免慢盘阻塞图标
  edit.{h,cpp}      InlineEdit（原生 EDIT 子控件，中文 IME 免费可用）
  clipboard.{h,cpp} 剪贴板：路径的 CF_HDROP + 纯文本 + 复制/移动意图（多处共用）
  png.{h,cpp}       DIB → PNG（WIC，工作线程里给截图落盘）
  images.{h,cpp}    Shell 缩略图 + LRU + 自己的工作线程（待办图片预览）
  dragdrop.{h,cpp}  IDropTarget（拖入）、IDropSource + CF_HDROP（拖出）
  text_io.{h,cpp}   UTF-8 读写、原子替换
  persist.{h,cpp}   便携数据目录、可写性探测、注册表自启动
  viewapi.h         视图枚举与 AppState
  views/            grid / grid_layout / todo_layout（纯布局，可测）、box（收纳盒）、browse（浏览）、todo（待办）
  model/            纯数据层，不含任何 Windows 头，可被控制台测试
```

设计文档：`docs/superpowers/specs/2026-09-22-stargazer-design.md`（总）、
`docs/superpowers/specs/2026-09-23-stargazer-todo-design.md`（待办）
实施计划：`docs/superpowers/plans/2026-09-22-stargazer-phase1-kernel-launcher.md`、
`docs/superpowers/plans/2026-09-22-stargazer-phase2-box.md`、
`docs/superpowers/plans/2026-09-23-stargazer-todo.md`
