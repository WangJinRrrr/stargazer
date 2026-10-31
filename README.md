# Stargazer

便携式 Windows 文件盒子（Win32 + Direct2D，无第三方依赖，单文件 exe）。
把常用文件收成一组**路径引用**：打开、换位置、换盒子，源文件永远不被移动或删除。

MIT 许可证，见 [LICENSE](LICENSE)。

**下载**：到 [Releases](https://github.com/WangJinRrrr/stargazer/releases/latest) 取最新的 `Stargazer-<版本>-win64.zip`，
解压到任意可写目录双击 `stargazer.exe` 即可（免安装、免运行库）；想自己编译见下。

**更新**：用新的 `stargazer.exe` 覆盖旧的就行 —— 程序是单文件便携的，`data\` 是你的数据，不会被碰。
Releases 里那个 `Stargazer-<版本>-update.zip` 就是为此准备的（里面只有一个 exe，下载更小）。
替换前先把程序退出（托盘右键 → 退出），否则 exe 被占用写不进去。

## 构建

要求：Visual Studio（含“C++ 桌面开发”工作负载）+ CMake 3.20+。

```powershell
# 生成器名换成你本机 VS 对应的（VS 2022 是 "Visual Studio 17 2022"；本仓库开发机用 VS 18，即 "Visual Studio 18 2026"）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target stargazer
```

> 若 VS 实例没注册到 VS Installer（或装了多个版本），再加一个 `-DCMAKE_GENERATOR_INSTANCE="<VS 安装目录>,version=<版本>"`。

产物为 `build\Release\stargazer.exe`：单个文件、静态 CRT（`/MT`），不依赖 VC 运行库、.NET 或 WebView2。
程序是便携式的：所有数据写在 exe 同级的 `data\` 目录里，没写权限时会明确报错而不是默默丢数据。

没有 Visual Studio 时，可以用 MinGW-w64（posix 线程）做一次交叉编译，确认能编过：

```bash
bash scripts/mingw-build.sh
```

这只检查编译，不替代上面的 MSVC 发布构建（发布包仍是 `/MT` 的单文件）。

## 测试

```powershell
cmake --build build --config Release --target test_model test_io test_layout
.\build\Release\test_model.exe    # 数据层：纯 STL，无 Windows 头
.\build\Release\test_io.exe       # UTF-8 落盘、可写性探测、CF_HDROP
.\build\Release\test_layout.exe   # 网格与侧栏的布局/命中/导航
```

`test_model` 覆盖行格式（含 Windows 路径不被误转义的回归）、路径规范化与上一级路径、自然序比较、
数据序列化、收纳盒往返与坏行跳过、拖入与盒内换位、盒子重名检查、筛选下标（`box_visible`）、BGRA 预乘；
`test_io` 覆盖 UTF-8 原子读写、目录可写性探测、`CF_HDROP` 构造（逐字节 + 双 NUL）；
`test_layout` 覆盖网格（含侧栏让出的左缘）与窗口骨架按钮/侧栏行的布局与命中。

## 打包

```powershell
# cmake 不在 PATH 上时用 -Cmake 指到 VS 自带的那份（如 <VS 安装目录>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe）；
# VS 没注册到 VS Installer 时再补一个实例版本（取 devenv.exe 的文件版本，如 18.9.12112.369）
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1 `
  -Cmake "<VS 安装目录>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -ConfigureArgs '-DCMAKE_GENERATOR_INSTANCE="<VS 安装目录>,version=<实例版本>"'
```

会先跑三个测试套件当发布闸门（不过就不打包），产物在 `dist\Stargazer-<版本>-win64.zip`。
包内结构与 1.0.0 一致：顶层是 `Stargazer-<版本>-win64\`，里面放 `stargazer.exe`、
`使用说明.txt`（仓库根目录那份纯文本快速上手，与 README 分开维护）、`LICENSE`，以及一个空的 `data\`。
版本号只有一处：`resources\stargazer.rc` 的 VERSIONINFO，脚本构建后从 exe 里读回来，
不在别处再维护一份；同时打印 exe 的 SHA256 方便对照。

加 `-Update` 额外产出 `dist\Stargazer-<版本>-update.zip`：里面只有一个 exe，老用户解开覆盖即可，
`data\` 原地不动。

## 使用

窗口分成三块：顶上一条可以拖动的标题栏，左边是盒子，右边是这个盒子里的文件。
配色是暖纸色上的白卡片，间距留得比较开。

- 呼出热键 `Ctrl+Shift+Space`（**可在托盘菜单里改**：右键托盘图标 → 设置呼出热键…，
  也可以直接改 `config.txt` 的 `hotkey_mods` / `hotkey_key`）
- 收起：再按一次热键、按 `Esc`、点标题栏右侧「收起」，或托盘菜单。收起不等于退出，退出只在托盘菜单里
- **不做失焦自动隐藏**。窗口会一直浮在最上层，直到你明确收起。
  否则从资源管理器按住文件往窗口里拖时，窗口会在鼠标按下那一刻就消失
- **位置记忆**：收起后再呼出就停在原来那个地方（不会跳回屏幕中央）；位置与尺寸记在 `ui.txt`，
  首次启动才居中于鼠标所在显示器。显示器拔了 / 分辨率变了会自动夹回工作区
- 拖动：标题栏的空白处。四边四角 6px 内可缩放。双击标题栏不会最大化。
  窗口外框是 Win11 的圆角 + 系统投影（DWM 属性，失败则退为直角）

### 放进盒子

盒子就是一组**路径引用**。条目指向文件所在的位置，源文件永远不被移动或删除。

- 拖文件或文件夹到窗口里
- 「添加文件」或 `Ctrl+O`：系统文件框，可多选
- `Ctrl+V`：剪贴板里的文件（`CF_HDROP`），或者一行一个的绝对路径（`C:\...` 或 `\\server\...`）
- 还没有盒子时拖进来，会先建一个「新盒子」再把引用放进去

### 找、打开、拿走

- 搜索框在内容区上方。点它、按 `Ctrl+F`，或在格子外直接打字，都会把字送进搜索框。
  **输入即筛选**，只隐藏对不上的条目，不另存一份列表：打开、删除、换位置用的都是原来的下标。
  匹配看显示名，也看文件名。`Esc` 第一下清筛选，第二下才收起窗口
- 双击条目打开；选中后 `Enter` 同效。打得开就收起面板，免得置顶窗口挡住刚打开的程序
- `Del` 移除引用（**不碰磁盘**）；`F2` 改显示名（可以和真实文件名不同）。没选中文件时 `F2` 改的是盒子名
- `Ctrl+C` 复制路径：同时给 `CF_HDROP` 与纯文本，粘到资源管理器就是“粘贴文件”，粘到编辑器就是一行路径
- 右键还有「在资源管理器中显示」

### 整理

- 左边点一个盒子就切过去。`Ctrl+1..9` 跳到第 N 个，`Ctrl+Tab` / `Ctrl+Shift+Tab` 循环。
  切换时会清掉筛选，并重新校验这个盒子里的失效项
- `Ctrl+N` 或左下角「新盒子」。新建后立刻可以改名。双击左侧的名字也能改。
  重名会被拒绝：同名盒子在读回 `boxes.txt` 时会并成一个
- 删除盒子在右键菜单里。盒子里还有条目时会先确认，文案写明不动磁盘
- 按住一张卡片拖到**另一张卡片**上松开，就是换位置（落点即最终位置）
- 拖到**左侧另一个盒子**上松开，就是换盒子
- 拖出窗口则变成 OLE 拖出（对方拿到的是路径）。拖出不会删掉本地引用；
  若接收方选了“移动”，该条目会在下次呼出时变成失效项
- 点击和拖动是分开的：鼠标移动超过一小段才算拖，单击只是选中

### 失效项

呼出时由工作线程批量校验存在性。指向的文件被改名或删除后，该卡片会变淡并加删除线，
其余条目照常用；文件改回来就恢复。校验是异步的，断开的网盘不会卡住界面。

当前盒子里有失效项时，搜索框下面会出现一行提示和「清理」。
`Ctrl+Shift+D` 是同一件事：只从盒子里去掉这些引用，磁盘上什么都不删。

判定是**保守**的：只有明确的“文件/路径不存在”才标失效，断盘、无权限等一律当作存在。

## 数据

全部数据在 exe 同级的 `data\` 目录下，纯文本，可直接手改：

| 文件 | 内容 |
|---|---|
| `boxes.txt` | `盒子 \t 名称 \t 路径`（空盒子写一行“盒子名 + 两个空字段”的占位行） |
| `config.txt` | `键 \t 值`：`hotkey_mods` / `hotkey_key`（呼出热键的修饰键位与虚拟键码，托盘菜单可改） |
| `ui.txt` | 窗口尺寸（逻辑像素）、位置（物理像素）、上次打开的盒子名 |

UTF-8 编码、无 BOM，行内 Tab 分隔，字段内 `|` `Tab` `换行` 分别写作 `||` `|t` `|n`。

> 转义符是 `|` 而不是反斜杠：Windows 文件名里不允许出现 `|`，所以手写的路径**永远不需要转义**。
> 用反斜杠做转义符会让 `C:\temp`（含 `\t`）和 `D:\new folder`（含 `\n`）在读写时被拆坏。

格式不对的行会被跳过并计数，不影响其余记录；单行写坏不会让整个文件加载失败。
空盒子名或空路径的行会被当作坏行跳过（不会变成看得见却没什么可做的条目）。

`boxes.txt` 里的路径是纯引用：删条目、删盒子、清理失效项都只改这个文件，**磁盘上的文件一个也不会动**。

1.1 之前如果用过待办，磁盘上可能还留着 `todo.txt` 和 `data\images\`。程序不再读取也不再改写它们，确认不需要后可以自己删。
`config.txt` 里旧的 `browse_root` 同样会被忽略。`ui.txt` 里旧的 `view` 字段也会被忽略，下次保存窗口位置时不再写回去。

开机自启的真实状态在注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `stargazer` 值，不在 `config.txt`。
程序被移动到别的目录后，下次启动会自动修正该路径。

## 资源占用

两个关键手法仍然在：**懒加载**（面板窗口与 D2D 推迟到首次呼出才建，平时只驻留托盘）、
**收起时交还**（释放渲染目标与图标位图，并把常驻页赶回系统）。
待办缩略图和目录监视已经去掉，收起之后不必再为它们留缓存。

私有提交量里有一块是 D2D/DWrite 分配器的高水位，只有销毁工厂才能收回，代价是每次呼出都要重建设备，所以没有这么做。

## 已知限制（有意不做）

- 无设置界面：除了「呼出热键」可以在托盘菜单里改（`config.txt` 也能手改），其余没有可视化设置。
  配色是固定的浅色纸面，不跟随系统主题，写在 `render.h` 的 `Theme` 里
- 悬停有一段短淡入；呼出时内容从纸色淡入。没有滚动条（滚轮 / 键盘），没有平滑滚动
- 系统画的对话框（MessageBox、右键菜单、文件选择框）靠 uxtheme 的 `SetPreferredAppMode(ForceLight)` 保持浅色
- 无拼音搜索（子串匹配，中文按码点比较）
- 无磁盘图标缓存：图标统一按 48×48 提取，进程内 LRU 上限 300 项
- 只支持 `MAX_PATH` 以内的路径
- 无多选。添加文件的对话框可以一次选多个，放进盒子之后仍是逐条操作
- 「添加文件」对话框只选文件；文件夹用拖入或粘贴路径
- 拖出只提供 `CF_HDROP`（没有自定义剪贴板格式）
- 筛选开着时仍然可以换位置：落点是你看见的那一条在完整列表里的真实位置
- **拖放要求 OLE 初始化**：必须用 `OleInitialize`（只调 `CoInitializeEx` 时 `RegisterDragDrop` 返回
  `0x8007000E`，面板不会是拖放目标，症状是拖动时光标全程显示“禁止”）—— 踩过一次，记在这里
- `data\*.txt` 若是非 UTF-8 编码（例如被存成 ANSI），程序会备份成 `.bad` 并重新开始，不会静默改写乱码

## 目录结构

```
src/
  main.cpp          入口、单实例、消息循环
  app.{h,cpp}       窗口生命周期、呼出/收起、热键与托盘、输入转给盒子
  render.{h,cpp}    D2D/DWrite、浅色主题、设备丢失重建
  icons.{h,cpp}     Shell 图标三级提取、LRU 缓存、图标线程
  fs_work.{h,cpp}   存在性校验（单独的工作线程，慢盘不堵界面）
  edit.{h,cpp}      InlineEdit（原生 EDIT，中文 IME 可用）
  clipboard.{h,cpp} 剪贴板：路径的 CF_HDROP + 纯文本
  dragdrop.{h,cpp}  IDropTarget（拖入）、IDropSource + CF_HDROP（拖出）
  text_io.{h,cpp}   UTF-8 读写、原子替换
  persist.{h,cpp}   便携数据目录、可写性探测、注册表自启动
  viewapi.h         盒子的界面状态与 AppState
  views/            grid / shell（纯布局，可测）、box（唯一的界面）
  model/            纯数据层，不含任何 Windows 头，可被控制台测试
scripts/mingw-build.sh
```
