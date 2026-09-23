# Stargazer

便携式 Windows 桌面效率工具。阶段 1 已实现**快捷启动板**，其余三个模块（文件收纳盒、Todo、网盘浏览）按计划在后续阶段加入。

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
& $CMAKE --build build --config Release --target test_model test_io
.\build\Release\test_model.exe
.\build\Release\test_io.exe
```

`test_model` 覆盖行格式（含 Windows 路径不被误转义的回归）、路径规范化、自然序比较、数据序列化与排序、BGRA 预乘；
`test_io` 覆盖 UTF-8 原子读写、目录可写性探测、`.lnk` 解析。

## 使用

- `Ctrl+Shift+Space` 呼出/隐藏（可在托盘菜单里改？——尚未提供设置界面，见「已知限制」）
- 呼出后光标在搜索框，直接打字即过滤（名称与目标路径都参与匹配）
- `↓` 进网格，方向键移动，`Enter` 启动，`Esc` 隐藏
- `Del` 删除条目（**只删引用，永不删磁盘上的文件**）
- `F2` 重命名条目；在网格里打字会自动回到搜索框继续过滤
- 从桌面或资源管理器拖文件/快捷方式进窗口即添加（`.lnk` 会自动解析出目标与参数）
- 右键菜单：新建条目 / 新建分组 / 重命名 / 删除 / 打开所在位置
- 按住条目拖到顶部分组标签上松开即换组

**不做失焦自动隐藏**：窗口会一直浮在最上层，直到你按 `Esc`、再按一次热键或用托盘菜单关掉。
这是有意的取舍 —— 否则从资源管理器按住文件往窗口里拖时，窗口会在鼠标按下那一刻就消失。

## 数据

全部数据在 exe 同级的 `data\` 目录下，纯文本，可直接手改：

| 文件 | 内容 |
|---|---|
| `launcher.txt` | `分组 \t 名称 \t 目标 \t 参数 \t 工作目录 \t 图标` |
| `config.txt` | `键 \t 值` |
| `ui.txt` | 窗口尺寸（逻辑像素）与上次分组 |

UTF-8 编码、无 BOM，行内 Tab 分隔，字段内 `|` `Tab` `换行` 分别写作 `||` `|t` `|n`。

> 转义符是 `|` 而不是反斜杠：Windows 文件名里不允许出现 `|`，所以手写的路径**永远不需要转义**。
> 用反斜杠做转义符会让 `C:\temp`（含 `\t`）和 `D:\new folder`（含 `\n`）在读写时被拆坏。

格式不对的行会被跳过并计数，不影响其余记录；单行写坏不会让整个文件加载失败。

开机自启的真实状态在注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `stargazer` 值，不在 `config.txt`。
程序被移动到别的目录后，下次启动会自动修正该路径。

## 资源占用（本机实测，150% DPI，默认 960×620）

| 状态 | 工作集 |
|---|---|
| 启动后未呼出（仅托盘） | 11 MB |
| 隐藏态 | 6 MB |
| 呼出态（稳态） | 15 MB |
| 空闲 CPU | 0 ms / 5 s |

两个关键手法：**懒加载**（面板窗口与 D2D 工厂推迟到首次呼出才建，否则仅驻留托盘就要 37 MB）、
**隐藏时交还**（释放渲染目标与图标位图，并把常驻页赶回系统，否则呼出过一次后永远停在 54 MB）。
私有提交量约 64 MB 且不下降——那是 D2D/DWrite 分配器的高水位，只有销毁工厂才能收回，代价是每次呼出都要重建设备。

## 已知限制（阶段 1 范围内有意不做）

- 无设置界面：热键固定为 `Ctrl+Shift+Space`，窗口尺寸以外观感项写死在 `Theme` 结构里
- 无拼音搜索（子串匹配，中文按码点比较）
- 无磁盘图标缓存：图标统一按 48×48 提取，进程内 LRU 上限 300 项（约 2.7 MB）
- 无缩略图、无平滑滚动动画、无鼠标中键呼出
- 只支持 `MAX_PATH` 以内的路径

## 架构

```
src/
  main.cpp          入口、单实例、消息循环
  app.{h,cpp}       窗口生命周期、呼出/隐藏、视图分发、输入路由、数据加载与落盘
  render.{h,cpp}    D2D/DWrite 初始化、设备丢失重建、坐标系统一换算
  icons.{h,cpp}     Shell 图标三级提取、LRU 缓存、工作线程
  edit.{h,cpp}      InlineEdit（原生 EDIT 子控件，中文 IME 免费可用）
  dragdrop.{h,cpp}  IDropTarget（外部拖入）
  launch.{h,cpp}    ShellExecuteEx 启动、.lnk 解析
  text_io.{h,cpp}   UTF-8 读写、原子替换
  persist.{h,cpp}   便携数据目录、可写性探测、注册表自启动
  viewapi.h         视图枚举与 AppState
  views/            各视图（当前只有 launcher）
  model/            纯数据层，不含任何 Windows 头，可被控制台测试
```

设计文档：`docs/superpowers/specs/2026-09-22-stargazer-design.md`
实施计划：`docs/superpowers/plans/2026-09-22-stargazer-phase1-kernel-launcher.md`
