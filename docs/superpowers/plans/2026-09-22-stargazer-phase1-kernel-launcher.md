# Stargazer 阶段 1（内核 + Launcher）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付一个可用的便携式热键启动器：单实例进程、全局热键呼出无边框窗口、快捷启动板（分组、搜索、图标网格、从 Explorer 拖入、双击启动）、配置持久化到 exe 同级 `data\`、注册表开机自启。

**Architecture:** 单进程单窗口。`AppState` 持有全部数据，四个视图共用一套 Direct2D 渲染与输入内核（本计划只实现 Launcher 视图，其余视图留空壳）。`model/` 为纯 C++ 数据层，不依赖任何 Windows 头，用控制台 assert 测试；`text_io`/`persist` 负责 Win32 文件与注册表；`icons` 在独立工作线程提取 Shell 图标并按 `requestId` 回投；`WM_PAINT` 永不调用 Shell API。

**Tech Stack:** C++20、Win32、Direct2D 1.1 + DirectWrite、CMake ≥3.20 + MSVC v143、零第三方依赖、`/MT` 静态 CRT。

**Spec:** `docs/superpowers/specs/2026-09-22-stargazer-design.md`

## Global Constraints

- C++20；MSVC v143；CMake ≥ 3.20；`/W4 /utf-8 /permissive-`；Release `/O2`

> **本机工具链（2026-09-22 实测确认）**：本机没有 v143，也没有独立的 cmake，但 D 盘已有 VS 18 Community，足够使用。
> 因此计划里所有 `-G "Visual Studio 17 2022"` 一律替换为 `-G "Visual Studio 18 2026"`，其余命令（含 `--config Release`、`--target`）原样不变。
> ```
> CMAKE  = D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
> VCVARS = D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
> ```
> cmake 不在 PATH 上，用绝对路径。已实测 `cl /W4 /EHsc /std:c++20 /MT` 能编出调用 D2D1CreateFactory +
> DWriteCreateFactory + CreateTextFormat(L"Microsoft YaHei UI") 的程序，三个 HRESULT 均为 S_OK。
>
> **另一件必须做的事**：VS 18 没有在 VS Installer 里注册，`vswhere -all` 查不到实例，因此 CMake 的 VS 生成器
> 默认枚举不到它，报 `could not find any instance of Visual Studio`。每次 configure 必须显式指定实例（版本号必须是 4 段）：
> ```
> "$CMAKE" -S . -B build -G "Visual Studio 18 2026" -A x64 `
>   "-DCMAKE_GENERATOR_INSTANCE=D:\Program Files\Microsoft Visual Studio\18\Community,version=18.0.0.0"
> ```
> 只有 `-S/-B` 的 configure 需要它；`cmake --build build --config Release --target X` 不需要。
- **静态 CRT**：`set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")`——便携的前提
- **零第三方依赖**：只允许链接 `d2d1 dwrite shell32 ole32 shlwapi comctl32 dwmapi`
- **`src/model/` 下任何文件不得 `#include <windows.h>` 或任何 Windows 头**，只用 STL（这条使数据层可被控制台测试）
- 目标系统 Win10 1809+ / Win11；DPI 感知 `PER_MONITOR_AWARE_V2`；所有尺寸乘以 `dpi/96`
- 数据固定在 `<exe目录>\data\`，**不使用 `%APPDATA%`**；目录不可写时明确提示并退出
- 数据文件 UTF-8 **无 BOM**，`\n` 换行，Tab 分隔字段，字段内 `\` `\t` `\n` 转义为 `\\` `\t` `\n`
- 注册表是开机自启的**唯一真相**，`config.txt` 不存 autostart
- 正则表达式：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值名 `stargazer`，值数据 `"<exe绝对路径>" --autostart`
- 空闲状态：无定时器、无轮询、无重绘（窗口隐藏后消息循环阻塞在 `GetMessage`）
- 产物：单个 `stargazer.exe`

## Review Focus

以下五类输入/故障在 spec 里没有对应测试，但最可能伤到使用者。每条都在其宿主任务里加了验证步骤。

1. **便携目录不可写**（放在 `Program Files`、只读 U 盘、只读网络盘）——应明确提示并退出，绝不能静默丢数据或回落到 AppData
2. **`data\*.txt` 被手工改坏**（字段数不对、含裸 Tab、截断、混入 CRLF）——坏行跳过并计数，其余行照常加载
3. **含特殊字符的中文路径**（文件名内有制表符或换行、emoji、反斜杠字面量）——序列化往返后必须逐字节还原
4. **图标回投竞态**（请求发出后用户已切换分组/关闭窗口）——过期结果不得写入已失效的索引，不得访问已释放状态
5. **二次启动**（已运行时再次双击 exe，含 `--autostart` 情况）——应唤出已有实例并退出自身，不得启动第二个进程

---

## 计划拆分说明

本 spec 分四个阶段，每个阶段独立可用，因此拆成四个计划。本文件只覆盖阶段 1。阶段 2（Box）、3（Todo）、4（Explorer）的计划在阶段 1 完成后另写——它们的任务内容取决于阶段 1 定下的 `AppState` / 渲染 / 拖放接口，提前写只能是猜测。

spec §9 里的**拖出（`IDropSource` + `DoDragDrop`）**不在本计划内：阶段 1 的 Launcher 只需要拖入。拖出由阶段 2（Box）实现，那时它才有真实用途——把收纳盒里的项拖到资源管理器。本计划把 `app.in_drag` 标志和 `dragdrop` 模块的接口先定好，阶段 2 直接往里加接收方即可。

## 文件结构（阶段 1 结束时）

| 文件 | 职责 |
|---|---|
| `CMakeLists.txt` | 构建：`stargazer`（GUI）+ `test_model` / `test_io`（控制台） |
| `src/model/rowformat.{h,cpp}` | 行格式转义与解析（纯） |
| `src/model/paths.{h,cpp}` | 路径规范化与拆分（纯） |
| `src/model/search.{h,cpp}` | 大小写不敏感匹配 + 自然序比较（纯） |
| `src/model/store.{h,cpp}` | 四类数据的结构体与文本序列化（纯，无文件 IO） |
| `src/text_io.{h,cpp}` | UTF-8 读写、原子替换 |
| `src/persist.{h,cpp}` | 数据目录解析、可写性探测、注册表自启动 |
| `src/render.{h,cpp}` | D2D/DWrite 初始化、设备丢失重建、绘制原语、文本格式缓存 |
| `src/icons.{h,cpp}` | Shell 图标提取、LRU 缓存、工作线程 A |
| `src/edit.{h,cpp}` | `InlineEdit`：原生 EDIT 子控件封装 |
| `src/viewapi.h` | `View` 枚举、`AppState`、视图渲染/命中/按键的自由函数声明 |
| `src/app.{h,cpp}` | 窗口、消息循环、呼出/隐藏、视图分发、图标回投 |
| `src/main.cpp` | 入口、单实例、DPI、COM、热键、托盘、`--autostart` |
| `src/views/launcher.cpp` | 启动板视图 |
| `src/views/box.cpp` `todo.cpp` `explorer.cpp` | 阶段 2-4 的空壳（本阶段只画"未实现"占位） |
| `data/launcher.txt` `config.txt` `ui.txt` | 运行时由程序创建 |
| `tests/test_model.cpp` | 纯数据层 assert 测试 |
| `tests/test_io.cpp` | 文件 IO 层 assert 测试 |
| `README.md` | 构建方式与便携说明 |

---

### Task 1: 构建骨架 + 行格式（model/rowformat）

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/model/rowformat.h`
- Create: `src/model/rowformat.cpp`
- Create: `tests/test_model.cpp`
- Create: `.gitignore`

**Interfaces:**
- Consumes: 无（首个任务）
- Produces:
  - `std::wstring sg::escape_field(const std::wstring&)`
  - `std::wstring sg::unescape_field(const std::wstring&)`
  - `std::wstring sg::join_row(const std::vector<std::wstring>&)`
  - `bool sg::split_row(const std::wstring& line, size_t expect, std::vector<std::wstring>& out)`
  - `std::vector<std::vector<std::wstring>> sg::parse_rows(const std::wstring& text, size_t expect, int& bad)`
  - `std::wstring sg::build_text(const std::vector<std::vector<std::wstring>>&)`

- [ ] **Step 1: 写构建骨架**

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(stargazer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(/W4 /utf-8 /permissive- /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# 中文菜单/对话框一律走系统 UI 语言
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

set(MODEL_SOURCES
  src/model/rowformat.cpp
)

add_executable(test_model tests/test_model.cpp ${MODEL_SOURCES})
target_include_directories(test_model PRIVATE src)
```

`/utf-8` 是必须的：源码里有中文宽字符串字面量，没有它 MSVC 会按本地代码页解释。

`.gitignore`：

```
build/
data/
*.user
```

- [ ] **Step 2: 写失败测试**

`tests/test_model.cpp`：

```cpp
#include <cstdio>
#include <string>
#include <vector>

#include "model/rowformat.h"

static int g_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failed;                                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto _a = (a);                                                     \
        auto _b = (b);                                                     \
        if (!(_a == _b)) {                                                 \
            std::printf("FAIL %s:%d  %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failed;                                                    \
        }                                                                  \
    } while (0)

// Review Focus 3：含制表符、换行、反斜杠字面量、emoji、中文的文件名必须逐字节还原
static void test_field_roundtrip() {
    const std::wstring cases[] = {
        L"",
        L"普通中文路径",
        L"C:\\Users\\wjr\\文档\\a b.txt",
        L"含\t制表符",
        L"含\n换行",
        L"含\\反斜杠",
        L"\\t 字面量（反斜杠加字母 t，共两字符）",
        L"emoji \U0001F600 混合 CJK",
        L"末尾单个反斜杠\\",
    };
    for (const auto& c : cases) {
        CHECK_EQ(sg::unescape_field(sg::escape_field(c)), c);
    }
}

static void test_row_roundtrip() {
    std::vector<std::wstring> row = { L"常用", L"名字\t带制表符", L"D:\\a\\b.lnk",
                                      L"--arg \"x y\"", L"", L"C:\\ico\\i.ico" };
    std::vector<std::wstring> back;
    CHECK(sg::split_row(sg::join_row(row), 6, back));
    CHECK(back == row);
}

// Review Focus 2：字段数不对的行跳过并计数，不使整份文件失败
static void test_bad_row_skipped() {
    const std::wstring text = L"a\tb\tc\nd\te\nf\tg\th\n";
    int bad = 0;
    auto rows = sg::parse_rows(text, 3, bad);
    CHECK_EQ(rows.size(), size_t{2});
    CHECK_EQ(bad, 1);
    CHECK_EQ(rows[0][0], std::wstring(L"a"));
    CHECK_EQ(rows[1][2], std::wstring(L"h"));
}

// 全空字段是合法行；空行与 CRLF 不产生坏行
static void test_empty_fields_and_crlf() {
    const std::wstring text = L"\t\t\nx\t\ty\r\n\r\n";
    int bad = 0;
    auto rows = sg::parse_rows(text, 3, bad);
    CHECK_EQ(rows.size(), size_t{2});
    CHECK_EQ(bad, 0);
    CHECK_EQ(rows[0][1], std::wstring(L""));
    CHECK_EQ(rows[1][2], std::wstring(L"y"));
}

static void test_build_text() {
    std::vector<std::vector<std::wstring>> rows = { { L"a", L"b" }, { L"c", L"" } };
    CHECK_EQ(sg::build_text(rows), std::wstring(L"a\tb\nc\t\n"));
}

int main() {
    test_field_roundtrip();
    test_row_roundtrip();
    test_bad_row_skipped();
    test_empty_fields_and_crlf();
    test_build_text();

    if (g_failed == 0) {
        std::printf("OK: test_model 全部通过\n");
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    return 1;
}
```

- [ ] **Step 3: 配置并构建，确认编译失败**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target test_model
```

预期：FAILED，报 `无法打开源文件 "model/rowformat.h"`。

- [ ] **Step 4: 写最小实现**

`src/model/rowformat.h`：

```cpp
#pragma once

#include <string>
#include <vector>

namespace sg {

// 字段内转义：'\\' -> "\\\\"，'\t' -> "\\t"，'\n' -> "\\n"，'\r' 丢弃
std::wstring escape_field(const std::wstring& v);
std::wstring unescape_field(const std::wstring& v);

// 字段 -> 一行（Tab 连接）
std::wstring join_row(const std::vector<std::wstring>& fields);

// 解析一行；字段数不等于 expect 时返回 false（out 内容不定）
bool split_row(const std::wstring& line, size_t expect, std::vector<std::wstring>& out);

// 解析整份文本：空行跳过，字段数不符的行跳过并计入 bad
std::vector<std::vector<std::wstring>> parse_rows(const std::wstring& text, size_t expect, int& bad);

// 多行 -> 文本；每行以 '\n' 结尾，不含 BOM
std::wstring build_text(const std::vector<std::vector<std::wstring>>& rows);

}  // namespace sg
```

`src/model/rowformat.cpp`：

```cpp
#include "model/rowformat.h"

namespace sg {

std::wstring escape_field(const std::wstring& v) {
    std::wstring out;
    out.reserve(v.size() + 8);
    for (wchar_t c : v) {
        switch (c) {
            case L'\\': out += L"\\\\"; break;
            case L'\t': out += L"\\t"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': break;  // 换行统一为 \n
            default: out += c; break;
        }
    }
    return out;
}

std::wstring unescape_field(const std::wstring& v) {
    std::wstring out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            const wchar_t n = v[i + 1];
            if (n == L'\\') { out += L'\\'; ++i; continue; }
            if (n == L't') { out += L'\t'; ++i; continue; }
            if (n == L'n') { out += L'\n'; ++i; continue; }
            // 其它 "\x" 原样保留（保证手改文件不会被吞字符）
        }
        out += v[i];
    }
    return out;
}

std::wstring join_row(const std::vector<std::wstring>& fields) {
    std::wstring out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) out += L'\t';
        out += escape_field(fields[i]);
    }
    return out;
}

// escape_field never emits a raw TAB, so splitting on raw TAB is exact.
bool split_row(const std::wstring& line, size_t expect, std::vector<std::wstring>& out) {
    out.clear();
    std::wstring cur;
    for (wchar_t c : line) {
        if (c == L'\t') {
            out.push_back(unescape_field(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(unescape_field(cur));
    return out.size() == expect;
}

std::vector<std::vector<std::wstring>> parse_rows(const std::wstring& text, size_t expect, int& bad) {
    bad = 0;
    std::vector<std::vector<std::wstring>> rows;
    std::wstring line;
    line.reserve(256);

    for (size_t i = 0; i <= text.size(); ++i) {
        const bool at_end = (i == text.size());
        if (at_end || text[i] == L'\n') {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!line.empty()) {
                std::vector<std::wstring> f;
                if (split_row(line, expect, f)) {
                    rows.push_back(std::move(f));
                } else {
                    ++bad;
                }
            }
            line.clear();
        } else {
            line += text[i];
        }
    }
    return rows;
}

std::wstring build_text(const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring out;
    for (const auto& r : rows) {
        out += join_row(r);
        out += L'\n';
    }
    return out;
}

}  // namespace sg
```

- [ ] **Step 5: 构建并运行测试，确认全部通过**

```powershell
cmake --build build --config Release --target test_model
.\build\Release\test_model.exe
```

预期：`OK: test_model 全部通过`，退出码 0。

- [ ] **Step 6: 提交**

```powershell
git add CMakeLists.txt .gitignore src/model/rowformat.h src/model/rowformat.cpp tests/test_model.cpp
git commit -m "feat(model): 行格式转义与解析 + 构建骨架"
```

---

### Task 2: 路径规范化与自然序比较（model/paths, model/search）

**Files:**
- Create: `src/model/paths.h`
- Create: `src/model/paths.cpp`
- Create: `src/model/search.h`
- Create: `src/model/search.cpp`
- Modify: `CMakeLists.txt`（把两个 .cpp 加进 `MODEL_SOURCES`）
- Modify: `tests/test_model.cpp`（追加测试与调用）

**Interfaces:**
- Consumes: 无
- Produces:
  - `std::wstring sg::normalize_key(const std::wstring& path)` — 小写、`/`→`\`、折叠重复分隔符、去尾分隔符（根盘符如 `c:\` 保留）
  - `std::wstring sg::file_name(const std::wstring& path)`
  - `std::wstring sg::extension_of(const std::wstring& path)` — 小写、含点；无扩展名返回空
  - `std::wstring sg::join_path(const std::wstring& dir, const std::wstring& name)`
  - `bool sg::contains_ci(const std::wstring& hay, const std::wstring& needle)`
  - `int sg::natural_compare(const std::wstring& a, const std::wstring& b)`

- [ ] **Step 1: 追加失败测试**

在 `tests/test_model.cpp` 的 `#include` 区加：

```cpp
#include "model/paths.h"
#include "model/search.h"
```

在 `main()` 之前加：

```cpp
static void test_normalize_key() {
    CHECK_EQ(sg::normalize_key(L"C:\\A\\B\\"), std::wstring(L"c:\\a\\b"));
    CHECK_EQ(sg::normalize_key(L"C:/A/B"), std::wstring(L"c:\\a\\b"));
    CHECK_EQ(sg::normalize_key(L"C:\\\\A\\\\\\B"), std::wstring(L"c:\\a\\b"));
    CHECK_EQ(sg::normalize_key(L"C:\\"), std::wstring(L"c:\\"));
    CHECK_EQ(sg::normalize_key(L"D:\\网盘\\电影\\"), std::wstring(L"d:\\网盘\\电影"));
    CHECK_EQ(sg::normalize_key(L"\\\\server\\share\\x"), std::wstring(L"\\\\server\\share\\x"));
}

static void test_file_name_and_ext() {
    CHECK_EQ(sg::file_name(L"C:\\a\\b\\c.txt"), std::wstring(L"c.txt"));
    CHECK_EQ(sg::file_name(L"C:\\a\\b\\"), std::wstring(L""));
    CHECK_EQ(sg::file_name(L"c.txt"), std::wstring(L"c.txt"));
    CHECK_EQ(sg::extension_of(L"C:\\a\\B.TXT"), std::wstring(L".txt"));
    CHECK_EQ(sg::extension_of(L"C:\\a\\无扩展名"), std::wstring(L""));
    CHECK_EQ(sg::extension_of(L"C:\\a\\.gitignore"), std::wstring(L""));
}

static void test_join_path() {
    CHECK_EQ(sg::join_path(L"C:\\a", L"b.txt"), std::wstring(L"C:\\a\\b.txt"));
    CHECK_EQ(sg::join_path(L"C:\\a\\", L"b.txt"), std::wstring(L"C:\\a\\b.txt"));
}

static void test_contains_ci() {
    CHECK(sg::contains_ci(L"C:\\Program Files\\Notepad++.exe", L"notepad"));
    CHECK(sg::contains_ci(L"网盘备份目录", L"备份"));
    CHECK(sg::contains_ci(L"abc", L""));
    CHECK(!sg::contains_ci(L"abc", L"abcd"));
}

static void test_natural_compare() {
    CHECK(sg::natural_compare(L"1.txt", L"2.txt") < 0);
    CHECK(sg::natural_compare(L"2.txt", L"10.txt") < 0);
    CHECK(sg::natural_compare(L"a2.txt", L"a10.txt") < 0);
    CHECK(sg::natural_compare(L"a007.txt", L"a7.txt") == 0);
    CHECK(sg::natural_compare(L"ABC", L"abc") == 0);
    CHECK(sg::natural_compare(L"a", L"a1") < 0);
    // 同一批文件名排序必须是确定的全序（不允许出现 a<b 且 b<a）
    CHECK(sg::natural_compare(L"x", L"x") == 0);
}
```

在 `main()` 里按顺序追加：

```cpp
    test_normalize_key();
    test_file_name_and_ext();
    test_join_path();
    test_contains_ci();
    test_natural_compare();
```

- [ ] **Step 2: 运行测试，确认失败**

```powershell
cmake --build build --config Release --target test_model
```

预期：编译失败，`无法打开源文件 "model/paths.h"`。

- [ ] **Step 3: 实现 paths**

`src/model/paths.h`：

```cpp
#pragma once

#include <string>

namespace sg {

// 用于缓存键与去重：小写、'/'->'\\'、折叠重复分隔符、去尾分隔符（"c:\\" 保留）
std::wstring normalize_key(const std::wstring& path);

// 最后一段；以分隔符结尾时返回空
std::wstring file_name(const std::wstring& path);

// 小写扩展名，含点；无扩展名或以点开头返回空
std::wstring extension_of(const std::wstring& path);

std::wstring join_path(const std::wstring& dir, const std::wstring& name);

}  // namespace sg
```

`src/model/paths.cpp`：

```cpp
#include "model/paths.h"

namespace sg {

static wchar_t lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

static bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring normalize_key(const std::wstring& path) {
    std::wstring out;
    out.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        const wchar_t c = path[i];
        if (is_sep(c)) {
            if (!out.empty() && is_sep(out.back())) continue;  // 折叠
            out += L'\\';
        } else {
            out += lower(c);
        }
    }
    // UNC 前导 "\\\\" 会被折叠成一个，补回来
    if (out.size() >= 1 && path.size() >= 2 && is_sep(path[0]) && is_sep(path[1])) {
        out.insert(out.begin(), L'\\');
    }
    while (out.size() > 3 && is_sep(out.back())) out.pop_back();
    return out;
}

std::wstring file_name(const std::wstring& path) {
    if (path.empty() || is_sep(path.back())) return std::wstring();
    size_t i = path.size();
    while (i > 0 && !is_sep(path[i - 1])) --i;
    return path.substr(i);
}

std::wstring extension_of(const std::wstring& path) {
    const std::wstring name = file_name(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 == name.size()) return std::wstring();
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& c : ext) c = lower(c);
    return L"." + ext;
}

std::wstring join_path(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (!is_sep(dir.back())) return dir + L"\\" + name;
    return dir + name;
}

}  // namespace sg
```

- [ ] **Step 4: 实现 search**

`src/model/search.h`：

```cpp
#pragma once

#include <string>

namespace sg {

// 大小写不敏感的子串匹配；needle 为空时返回 true
// 注意：仅 ASCII 大小写折叠，中文按码点比较
bool contains_ci(const std::wstring& hay, const std::wstring& needle);

// 自然序：数字段按数值比较，其余按 ASCII 大小写折叠后比较码点
// 返回 <0 / 0 / >0
int natural_compare(const std::wstring& a, const std::wstring& b);

}  // namespace sg
```

`src/model/search.cpp`：

```cpp
#include "model/search.h"

namespace sg {

static wchar_t lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

static bool is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool contains_ci(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

int natural_compare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            const size_t si = i, sj = j;
            while (i < a.size() && is_digit(a[i])) ++i;
            while (j < b.size() && is_digit(b[j])) ++j;
            size_t zi = si, zj = sj;
            while (zi + 1 < i && a[zi] == L'0') ++zi;  // 去前导零，至少留一位
            while (zj + 1 < j && b[zj] == L'0') ++zj;
            const size_t li = i - zi, lj = j - zj;
            if (li != lj) return li < lj ? -1 : 1;
            const int c = a.compare(zi, li, b, zj, lj);
            if (c != 0) return c < 0 ? -1 : 1;
            continue;
        }
        const wchar_t ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace sg
```

- [ ] **Step 5: 构建并运行，确认通过**

```powershell
cmake --build build --config Release --target test_model
.\build\Release\test_model.exe
```

预期：`OK: test_model 全部通过`。

- [ ] **Step 6: 提交**

```powershell
git add src/model/paths.h src/model/paths.cpp src/model/search.h src/model/search.cpp tests/test_model.cpp CMakeLists.txt
git commit -m "feat(model): 路径规范化、大小写不敏感匹配、自然序比较"
```

---

### Task 3: 数据模型与序列化（model/store）

**Files:**
- Create: `src/model/store.h`
- Create: `src/model/store.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_model.cpp`

**Interfaces:**
- Consumes: `sg::parse_rows` / `sg::build_text`（Task 1）、`sg::natural_compare`（Task 2）
- Produces:
  - 结构体 `sg::LaunchItem{name,target,args,workdir,icon}`、`sg::LaunchGroup{name,items}`、`sg::BoxItem{name,path}`、`sg::Box{name,items}`、`sg::TodoItem{id,done,created,due,prio,text}`
  - `std::wstring sg::serialize_launcher(const std::vector<LaunchGroup>&)`
  - `std::vector<LaunchGroup> sg::parse_launcher(const std::wstring& text, int& bad)`
  - `std::wstring sg::serialize_boxes(const std::vector<Box>&)` / `std::vector<Box> sg::parse_boxes(const std::wstring&, int& bad)`
  - `std::wstring sg::serialize_todos(const std::vector<TodoItem>&)` / `std::vector<TodoItem> sg::parse_todos(const std::wstring&, int& bad)`
  - `std::wstring sg::serialize_config(const std::vector<std::pair<std::wstring,std::wstring>>&)`
  - `std::vector<std::pair<std::wstring,std::wstring>> sg::parse_config(const std::wstring&, int& bad)`
  - `std::wstring sg::config_get(const std::vector<std::pair<std::wstring,std::wstring>>&, const std::wstring& key, const std::wstring& def)`
  - `void sg::config_set(std::vector<std::pair<std::wstring,std::wstring>>&, const std::wstring& key, const std::wstring& value)`
  - `void sg::sort_todos(std::vector<TodoItem>&)` — 未完成在上（prio 降序 → created 降序），已完成沉底
  - `long long sg::next_todo_id(const std::vector<TodoItem>&)`

- [ ] **Step 1: 追加失败测试**

在 `tests/test_model.cpp` 加 `#include "model/store.h"`，并在 `main()` 前加：

```cpp
// Review Focus 2：混入坏行后其余记录照常加载
static void test_launcher_roundtrip_and_bad_line() {
    std::vector<sg::LaunchGroup> groups(2);
    groups[0].name = L"常用";
    groups[0].items.push_back({ L"记事本", L"C:\\Windows\\notepad.exe", L"", L"", L"" });
    groups[0].items.push_back({ L"带\t制表符的名字", L"D:\\a\\b.lnk", L"--x \"y z\"", L"D:\\a", L"D:\\ico.ico" });
    groups[1].name = L"网盘";
    groups[1].items.push_back({ L"", L"", L"", L"", L"" });  // 全空字段必须能往返

    int bad = 0;
    auto back = sg::parse_launcher(sg::serialize_launcher(groups), bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(back.size(), size_t{2});
    CHECK_EQ(back[0].name, std::wstring(L"常用"));
    CHECK_EQ(back[0].items.size(), size_t{2});
    CHECK_EQ(back[0].items[1].name, std::wstring(L"带\t制表符的名字"));
    CHECK_EQ(back[0].items[1].args, std::wstring(L"--x \"y z\""));
    CHECK_EQ(back[1].items[0].target, std::wstring(L""));

    // 手工插一行字段数不对的
    std::wstring broken = sg::serialize_launcher(groups) + L"少\t字段\n";
    auto back2 = sg::parse_launcher(broken, bad);
    CHECK_EQ(bad, 1);
    CHECK_EQ(back2.size(), size_t{3});  // 2 组 + 1 条目，坏行不产生记录
}

static void test_boxes_roundtrip() {
    std::vector<sg::Box> boxes(1);
    boxes[0].name = L"待归档";
    boxes[0].items.push_back({ L"文档", L"D:\\网盘\\文档\\" });
    boxes[0].items.push_back({ L"含\n换行", L"D:\\a\\b" });

    int bad = 0;
    auto back = sg::parse_boxes(sg::serialize_boxes(boxes), bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(back.size(), size_t{1});
    CHECK_EQ(back[0].items.size(), size_t{2});
    CHECK_EQ(back[0].items[0].path, std::wstring(L"D:\\网盘\\文档\\"));
    CHECK_EQ(back[0].items[1].name, std::wstring(L"含\n换行"));
}

static void test_todos_roundtrip_and_sort() {
    std::vector<sg::TodoItem> todos;
    todos.push_back({ 1, false, 100, 0, 0, L"普通" });
    todos.push_back({ 2, false, 200, 0, 1, L"高优先级旧" });
    todos.push_back({ 3, false, 300, 0, 1, L"高优先级新" });
    todos.push_back({ 4, true, 400, 0, 1, L"已完成但高优先级" });

    int bad = 0;
    auto back = sg::parse_todos(sg::serialize_todos(todos), bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(back.size(), size_t{4});
    CHECK_EQ(back[2].text, std::wstring(L"高优先级新"));

    sg::sort_todos(back);
    CHECK_EQ(back[0].text, std::wstring(L"高优先级新"));
    CHECK_EQ(back[1].text, std::wstring(L"高优先级旧"));
    CHECK_EQ(back[2].text, std::wstring(L"普通"));
    CHECK_EQ(back[3].text, std::wstring(L"已完成但高优先级"));
    CHECK_EQ(back[3].done, true);

    CHECK_EQ(sg::next_todo_id(back), 5LL);
    CHECK_EQ(sg::next_todo_id(std::vector<sg::TodoItem>{}), 1LL);
}

static void test_config() {
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    sg::config_set(kv, L"hotkey", L"Ctrl+Shift+Space");
    sg::config_set(kv, L"hotkey", L"Alt+Space");  // 覆盖而非追加
    CHECK_EQ(kv.size(), size_t{1});
    CHECK_EQ(sg::config_get(kv, L"hotkey", L""), std::wstring(L"Alt+Space"));
    CHECK_EQ(sg::config_get(kv, L"missing", L"默认值"), std::wstring(L"默认值"));

    int bad = 0;
    auto back = sg::parse_config(sg::serialize_config(kv), bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(back.size(), size_t{1});
    CHECK_EQ(sg::config_get(back, L"hotkey", L""), std::wstring(L"Alt+Space"));
}
```

在 `main()` 里追加：

```cpp
    test_launcher_roundtrip_and_bad_line();
    test_boxes_roundtrip();
    test_todos_roundtrip_and_sort();
    test_config();
```

- [ ] **Step 2: 运行测试，确认编译失败**

```powershell
cmake --build build --config Release --target test_model
```

预期：`无法打开源文件 "model/store.h"`。

- [ ] **Step 3: 实现 store**

`src/model/store.h`：

```cpp
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sg {

struct LaunchItem {
    std::wstring name;
    std::wstring target;   // exe / .lnk / 目录 / URL / shell: 协议
    std::wstring args;
    std::wstring workdir;
    std::wstring icon;     // 自定义图标路径，空则用 target 的
};

struct LaunchGroup {
    std::wstring name;
    std::vector<LaunchItem> items;
};

struct BoxItem {
    std::wstring name;  // 显示名（可与真实文件名不同）
    std::wstring path;  // 绝对路径，仅引用，永不移动
};

struct Box {
    std::wstring name;
    std::vector<BoxItem> items;
};

struct TodoItem {
    long long id = 0;
    bool done = false;
    long long created = 0;  // Unix 秒
    long long due = 0;      // 0 = 未设置
    int prio = 0;           // 0 普通，1 高
    std::wstring text;
};

using Config = std::vector<std::pair<std::wstring, std::wstring>>;

// 行格式：group \t name \t target \t args \t workdir \t icon
std::wstring serialize_launcher(const std::vector<LaunchGroup>& groups);
std::vector<LaunchGroup> parse_launcher(const std::wstring& text, int& bad);

// 行格式：box \t name \t path
std::wstring serialize_boxes(const std::vector<Box>& boxes);
std::vector<Box> parse_boxes(const std::wstring& text, int& bad);

// 行格式：id \t done \t created \t due \t prio \t text
std::wstring serialize_todos(const std::vector<TodoItem>& todos);
std::vector<TodoItem> parse_todos(const std::wstring& text, int& bad);

// 行格式：key \t value
std::wstring serialize_config(const Config& kv);
Config parse_config(const std::wstring& text, int& bad);

std::wstring config_get(const Config& kv, const std::wstring& key, const std::wstring& def);
void config_set(Config& kv, const std::wstring& key, const std::wstring& value);

// 未完成在上：prio 降序 -> created 降序；已完成全部沉底（同样按 prio/create 排）
void sort_todos(std::vector<TodoItem>& todos);

// 空列表返回 1
long long next_todo_id(const std::vector<TodoItem>& todos);

}  // namespace sg
```

`src/model/store.cpp`：

```cpp
#include "model/store.h"

#include <cwchar>

#include "model/rowformat.h"

namespace sg {

std::wstring serialize_launcher(const std::vector<LaunchGroup>& groups) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& g : groups) {
        for (const auto& it : g.items) {
            rows.push_back({ g.name, it.name, it.target, it.args, it.workdir, it.icon });
        }
    }
    return build_text(rows);
}

std::vector<LaunchGroup> parse_launcher(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 6, bad);
    std::vector<LaunchGroup> groups;
    for (const auto& r : rows) {
        auto g = std::find_if(groups.begin(), groups.end(),
                              [&](const LaunchGroup& x) { return x.name == r[0]; });
        if (g == groups.end()) {
            groups.push_back(LaunchGroup{ r[0], {} });
            g = groups.end() - 1;
        }
        g->items.push_back(LaunchItem{ r[1], r[2], r[3], r[4], r[5] });
    }
    return groups;
}

std::wstring serialize_boxes(const std::vector<Box>& boxes) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& b : boxes) {
        for (const auto& it : b.items) {
            rows.push_back({ b.name, it.name, it.path });
        }
    }
    return build_text(rows);
}

std::vector<Box> parse_boxes(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 3, bad);
    std::vector<Box> boxes;
    for (const auto& r : rows) {
        auto b = std::find_if(boxes.begin(), boxes.end(),
                              [&](const Box& x) { return x.name == r[0]; });
        if (b == boxes.end()) {
            boxes.push_back(Box{ r[0], {} });
            b = boxes.end() - 1;
        }
        b->items.push_back(BoxItem{ r[1], r[2] });
    }
    return boxes;
}

static long long to_ll(const std::wstring& s) {
    return static_cast<long long>(std::wcstoll(s.c_str(), nullptr, 10));
}

std::wstring serialize_todos(const std::vector<TodoItem>& todos) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& t : todos) {
        rows.push_back({ std::to_wstring(t.id),
                         t.done ? L"1" : L"0",
                         std::to_wstring(t.created),
                         std::to_wstring(t.due),
                         std::to_wstring(t.prio),
                         t.text });
    }
    return build_text(rows);
}

std::vector<TodoItem> parse_todos(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 6, bad);
    std::vector<TodoItem> todos;
    for (const auto& r : rows) {
        TodoItem t;
        t.id = to_ll(r[0]);
        t.done = (r[1] == L"1");
        t.created = to_ll(r[2]);
        t.due = to_ll(r[3]);
        t.prio = static_cast<int>(to_ll(r[4]));
        t.text = r[5];
        todos.push_back(std::move(t));
    }
    return todos;
}

std::wstring serialize_config(const Config& kv) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& p : kv) rows.push_back({ p.first, p.second });
    return build_text(rows);
}

Config parse_config(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 2, bad);
    Config kv;
    for (const auto& r : rows) kv.emplace_back(r[0], r[1]);
    return kv;
}

std::wstring config_get(const Config& kv, const std::wstring& key, const std::wstring& def) {
    for (const auto& p : kv) {
        if (p.first == key) return p.second;
    }
    return def;
}

void config_set(Config& kv, const std::wstring& key, const std::wstring& value) {
    for (auto& p : kv) {
        if (p.first == key) {
            p.second = value;
            return;
        }
    }
    kv.emplace_back(key, value);
}

void sort_todos(std::vector<TodoItem>& todos) {
    std::stable_sort(todos.begin(), todos.end(), [](const TodoItem& a, const TodoItem& b) {
        if (a.done != b.done) return !a.done;         // 未完成在前
        if (a.prio != b.prio) return a.prio > b.prio; // 高优先级在前
        return a.created > b.created;                 // 新的在前
    });
}

long long next_todo_id(const std::vector<TodoItem>& todos) {
    long long max_id = 0;
    for (const auto& t : todos) {
        if (t.id > max_id) max_id = t.id;
    }
    return max_id + 1;
}

}  // namespace sg
```

`src/model/store.cpp` 需要 `#include <algorithm>`（`std::find_if` / `std::stable_sort`），加到 include 区。

- [ ] **Step 4: 构建并运行，确认通过**

```powershell
cmake --build build --config Release --target test_model
.\build\Release\test_model.exe
```

预期：`OK: test_model 全部通过`。

- [ ] **Step 5: 提交**

```powershell
git add src/model/store.h src/model/store.cpp tests/test_model.cpp CMakeLists.txt
git commit -m "feat(model): 四类数据结构与行格式序列化"
```

---

### Task 4: 文件 IO 与便携数据目录（text_io, persist）

**Files:**
- Create: `src/text_io.h`
- Create: `src/text_io.cpp`
- Create: `src/persist.h`
- Create: `src/persist.cpp`
- Create: `tests/test_io.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 无（只用 Win32 与 STL）
- Produces:
  - `std::wstring sg::utf8_to_wide(const std::string&)` / `std::string sg::wide_to_utf8(const std::wstring&)`
  - `bool sg::read_file_utf8(const std::wstring& path, std::wstring& out)`
  - `bool sg::write_file_utf8_atomic(const std::wstring& path, const std::wstring& text)`
  - `struct sg::Paths { std::wstring exe_dir; std::wstring data_dir; bool writable; }`
  - `bool sg::init_paths(Paths& out)` — 定位 exe 目录、创建 `data`、探测可写性
  - `bool sg::save_text(const Paths&, const wchar_t* name, const std::wstring&)`
  - `bool sg::load_text(const Paths&, const wchar_t* name, std::wstring& out)`
  - `std::wstring sg::data_file(const Paths&, const wchar_t* name)`
  - `bool sg::dir_writable(const std::wstring& dir)` — 供测试直接调用
  - `bool sg::autostart_enabled()`
  - `bool sg::autostart_set(bool enabled, const std::wstring& exe_path)`
  - `bool sg::autostart_heal(const std::wstring& exe_path)`

- [ ] **Step 1: 写失败测试**

`tests/test_io.cpp`：

```cpp
#include <cstdio>
#include <string>

#include "persist.h"
#include "text_io.h"

static int g_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failed;                                                    \
        }                                                                  \
    } while (0)

static std::wstring temp_dir() {
    wchar_t buf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, buf);
    std::wstring d = buf;
    d += L"stargazer_test";
    ::CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// Review Focus 3：中文与特殊字符经 UTF-8 落盘再读回必须一致
static void test_roundtrip_utf8() {
    const std::wstring dir = temp_dir();
    const std::wstring file = sg::join_path(dir, L"中文名字.txt");

    const std::wstring content = L"第一行\t制表符\n第二行 emoji \U0001F600\n末行\\\\反斜杠\n";
    CHECK(sg::write_file_utf8_atomic(file, content));

    std::wstring back;
    CHECK(sg::read_file_utf8(file, back));
    CHECK(back == content);

    // 覆盖写：不得残留上一次的内容
    CHECK(sg::write_file_utf8_atomic(file, L"x\n"));
    CHECK(sg::read_file_utf8(file, back));
    CHECK(back == std::wstring(L"x\n"));

    // 内容为空文件：返回 true 且读到空串
    CHECK(sg::write_file_utf8_atomic(file, L""));
    CHECK(sg::read_file_utf8(file, back));
    CHECK(back.empty());

    ::DeleteFileW(file.c_str());
}

static void test_read_missing_file() {
    std::wstring out;
    CHECK(!sg::read_file_utf8(sg::join_path(temp_dir(), L"不存在.txt"), out));
}

// Review Focus 1：不可写目录必须被探测出来
static void test_dir_writable_probe() {
    const std::wstring dir = temp_dir();
    CHECK(sg::dir_writable(dir));
    CHECK(!sg::dir_writable(sg::join_path(dir, L"没有这个子目录")));
}

static void test_atomic_write_leaves_no_tmp() {
    const std::wstring dir = temp_dir();
    const std::wstring file = sg::join_path(dir, L"a.txt");
    CHECK(sg::write_file_utf8_atomic(file, L"hello\n"));
    CHECK(::GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    ::DeleteFileW(file.c_str());
}

int main() {
    test_roundtrip_utf8();
    test_read_missing_file();
    test_dir_writable_probe();
    test_atomic_write_leaves_no_tmp();

    if (g_failed == 0) {
        std::printf("OK: test_io 全部通过\n");
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    return 1;
}
```

测试里用到了 `sg::join_path`（Task 2 的纯函数），因此 `test_io` 需要链接 `MODEL_SOURCES`，并在文件头补 `#include "model/paths.h"` 与 `#include <windows.h>`（用到 `MAX_PATH` 与 `GetTempPathW`）。

- [ ] **Step 2: 加构建目标并确认失败**

在 `CMakeLists.txt` 末尾追加：

```cmake
add_executable(test_io tests/test_io.cpp
  src/text_io.cpp src/persist.cpp src/model/paths.cpp)
target_include_directories(test_io PRIVATE src)
target_link_libraries(test_io PRIVATE shell32 ole32 advapi32)
```

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target test_io
```

预期：`无法打开源文件 "persist.h"`。

- [ ] **Step 3: 实现 text_io**

`src/text_io.h`：

```cpp
#pragma once

#include <string>

namespace sg {

// 文件内容按 UTF-8（无 BOM）读写；失败返回 false。
// 读取不存在的文件返回 false 且 out 不变。
bool read_file_utf8(const std::wstring& path, std::wstring& out);

// 原子写：先写 <path>.tmp，再 MoveFileExW 覆盖；失败时清理 .tmp
bool write_file_utf8_atomic(const std::wstring& path, const std::wstring& text);

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

}  // namespace sg
```

`src/text_io.cpp`：

```cpp
#include "text_io.h"

#include <windows.h>

namespace sg {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

bool read_file_utf8(const std::wstring& path, std::wstring& out) {
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        return false;
    }
    if (size.QuadPart > 32 * 1024 * 1024) {  // 数据文件不可能这么大，防手改坏
        ::CloseHandle(h);
        return false;
    }

    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = buf.empty()
        ? TRUE
        : ::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    buf.resize(read);

    if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf.erase(0, 3);  // 容忍用户用记事本加了 BOM
    }
    out = utf8_to_wide(buf);
    return true;
}

bool write_file_utf8_atomic(const std::wstring& path, const std::wstring& text) {
    const std::wstring tmp = path + L".tmp";
    const std::string bytes = wide_to_utf8(text);

    const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = bytes.empty()
        ? TRUE
        : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flushed = ok && ::FlushFileBuffers(h);
    ::CloseHandle(h);

    if (!ok || !flushed || written != bytes.size()) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    const BOOL moved = ::MoveFileExW(tmp.c_str(), path.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!moved) ::DeleteFileW(tmp.c_str());
    return moved != FALSE;
}

}  // namespace sg
```

- [ ] **Step 4: 实现 persist**

`src/persist.h`：

```cpp
#pragma once

#include <string>

namespace sg {

struct Paths {
    std::wstring exe_dir;   // 末尾无分隔符
    std::wstring data_dir;  // exe_dir + "\\data"，末尾无分隔符
    bool writable = false;
};

// 目录存在且能创建并删除临时文件
bool dir_writable(const std::wstring& dir);

// 定位 exe 目录、创建 data 子目录、写入 ui.txt 校验可写性
bool init_paths(Paths& out);

std::wstring data_file(const Paths& p, const wchar_t* name);
bool save_text(const Paths& p, const wchar_t* name, const std::wstring& text);
bool load_text(const Paths& p, const wchar_t* name, std::wstring& out);

// HKCU\...\Run 的 stargazer 值
bool autostart_enabled();
bool autostart_set(bool enabled, const std::wstring& exe_path);
// 已启用但记录的路径与当前 exe 不一致时改写；未启用时不动
bool autostart_heal(const std::wstring& exe_path);

}  // namespace sg
```

`src/persist.cpp`：

```cpp
#include "persist.h"

#include <windows.h>

#include "model/paths.h"
#include "text_io.h"

namespace sg {

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"stargazer";

bool dir_writable(const std::wstring& dir) {
    const DWORD attr = ::GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const std::wstring probe = join_path(dir, L".writetest");
    const HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

bool init_paths(Paths& out) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    out.exe_dir = buf;
    const size_t cut = out.exe_dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return false;
    out.exe_dir.resize(cut);

    out.data_dir = join_path(out.exe_dir, L"data");
    ::CreateDirectoryW(out.data_dir.c_str(), nullptr);

    out.writable = dir_writable(out.data_dir);
    return true;
}

std::wstring data_file(const Paths& p, const wchar_t* name) {
    return join_path(p.data_dir, name);
}

bool save_text(const Paths& p, const wchar_t* name, const std::wstring& text) {
    return write_file_utf8_atomic(data_file(p, name), text);
}

bool load_text(const Paths& p, const wchar_t* name, std::wstring& out) {
    return read_file_utf8(data_file(p, name), out);
}

bool autostart_enabled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const LONG r = ::RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr);
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool autostart_set(bool enabled, const std::wstring& exe_path) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                          nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    if (enabled) {
        const std::wstring cmd = L"\"" + exe_path + L"\" --autostart";
        const DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        ok = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(cmd.c_str()), bytes) == ERROR_SUCCESS;
    } else {
        const LONG r = ::RegDeleteValueW(key, kRunValue);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    ::RegCloseKey(key);
    return ok;
}

std::wstring autostart_target() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buf[1024] = {};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG r = ::RegQueryValueExW(key, kRunValue, nullptr, &type,
                                      reinterpret_cast<BYTE*>(buf), &bytes);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ) return std::wstring();
    std::wstring v = buf;
    // 形如 "<path>" --autostart -> 提取引号内路径
    const size_t a = v.find(L'"');
    if (a == std::wstring::npos) return std::wstring();
    const size_t b = v.find(L'"', a + 1);
    if (b == std::wstring::npos) return std::wstring();
    return v.substr(a + 1, b - a - 1);
}

bool autostart_heal(const std::wstring& exe_path) {
    if (!autostart_enabled()) return true;
    const std::wstring recorded = autostart_target();
    if (recorded.empty()) return true;
    if (normalize_key(recorded) == normalize_key(exe_path)) return true;
    return autostart_set(true, exe_path);  // 程序被挪动过，改写为新路径
}

}  // namespace sg
```

`autostart_target()` 只在 `persist.cpp` 内部使用，因此不加进头文件（`autostart_heal` 是唯一对外入口）。

- [ ] **Step 5: 构建并运行，确认通过**

```powershell
cmake --build build --config Release --target test_io
.\build\Release\test_io.exe
```

预期：`OK: test_io 全部通过`。

- [ ] **Step 6: 手工验证开机自启路径自愈**

```powershell
$exe = "$PWD\build\Release\stargazer.exe"   # 阶段 1 完成后才有这个文件
```
此步在 Task 12 的验收清单里统一执行；本任务只保证 `autostart_set` / `autostart_heal` 编译通过并被 Task 5 调用。

- [ ] **Step 7: 提交**

```powershell
git add src/text_io.h src/text_io.cpp src/persist.h src/persist.cpp tests/test_io.cpp CMakeLists.txt
git commit -m "feat(io): UTF-8 原子读写、便携数据目录探测、注册表自启动"
```

---

### Task 5: 单实例 + 窗口 + 托盘 + 全局热键 + 呼出/隐藏

第一个看得见的里程碑。本任务不渲染内容（只有纯色背景），目的是把窗口生命周期和呼出体验定死。

**Files:**
- Create: `src/app.h`
- Create: `src/app.cpp`
- Create: `src/main.cpp`
- Modify: `CMakeLists.txt`（加 `stargazer` 目标）

**Interfaces:**
- Consumes: `sg::Paths` / `sg::init_paths` / `sg::autostart_heal`（Task 4）
- Produces:
  - `const wchar_t* sg::kWindowClass`
  - 自定义消息：`WM_APP_SHOW`（`WM_APP + 1`，从另一实例唤醒）、`WM_APP_TRAY`（`WM_APP + 2`，托盘回调）、`WM_APP_ICON_READY`（`WM_APP + 3`，Task 7 使用）
  - `struct sg::App { HINSTANCE inst; HWND hwnd; Paths paths; UINT hotkey_id; bool in_drag; bool running; }`
  - `bool sg::app_init(App& app, HINSTANCE inst, bool autostart_mode)`
  - `void sg::app_show(App& app)` / `void sg::app_hide(App& app)` / `void sg::app_toggle(App& app)`
  - `void sg::app_shutdown(App& app)`

- [ ] **Step 1: 加构建目标**

在 `CMakeLists.txt` 末尾追加（`MODEL_SOURCES` 此时含 `rowformat.cpp paths.cpp search.cpp store.cpp`）：

```cmake
set(SG_SOURCES
  src/main.cpp
  src/app.cpp
  src/text_io.cpp
  src/persist.cpp
)

add_executable(stargazer WIN32 ${SG_SOURCES} ${MODEL_SOURCES})
target_include_directories(stargazer PRIVATE src)
target_compile_options(stargazer PRIVATE /O2)
target_link_libraries(stargazer PRIVATE
  d2d1 dwrite shell32 ole32 shlwapi comctl32 dwmapi user32 gdi32)
```

- [ ] **Step 2: 写 app.h**

`src/app.h`：

```cpp
#pragma once

#include <windows.h>

#include "persist.h"

namespace sg {

extern const wchar_t* kWindowClass;

constexpr UINT WM_APP_SHOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_ICON_READY = WM_APP + 3;

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    Paths paths;
    UINT hotkey_id = 1;      // RegisterHotKey 的 id
    bool hotkey_ok = false;
    bool in_drag = false;    // 拖放期间禁止失焦隐藏
    bool running = true;
};

bool app_init(App& app, HINSTANCE inst);
void app_show(App& app);
void app_hide(App& app);
void app_toggle(App& app);
void app_shutdown(App& app);

LRESULT CALLBACK app_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace sg
```

- [ ] **Step 3: 写 app.cpp**

`src/app.cpp`：

```cpp
#include "app.h"

#include <string>

#include "text_io.h"

namespace sg {

const wchar_t* kWindowClass = L"StargazerWnd";

static const UINT kTrayId = 1;
static const UINT kDefaultHotkeyMods = MOD_CONTROL | MOD_SHIFT;
static const UINT kDefaultHotkeyKey = VK_SPACE;

// 默认窗口尺寸（96 DPI 逻辑像素）
static const int kDefaultW = 960;
static const int kDefaultH = 620;

static void add_tray_icon(App& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Stargazer ");
    wcscat_s(nid.szTip, L"— Ctrl+Shift+Space 呼出");
    ::Shell_NotifyIconW(NIM_ADD, &nid);
}

static void remove_tray_icon(App& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void show_tray_menu(App& app) {
    ::SetForegroundWindow(app.hwnd);  // 否则菜单不会因失焦而关闭

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"呼出 (&S)");
    ::AppendMenuW(menu, MF_STRING, 2, L"开机自启");
    if (autostart_enabled()) {
        ::CheckMenuItem(menu, 2, MF_BYCOMMAND | MF_CHECKED);
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 3, L"退出 (&X)");

    POINT pt{};
    ::GetCursorPos(&pt);
    // 注意：自启状态每次从注册表读，config.txt 不存（注册表是唯一真相）
    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                      app.hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1: app_show(app); break;
        case 2: {
            wchar_t exe[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            autostart_set(!autostart_enabled(), exe);
            break;
        }
        case 3:
            ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

void app_show(App& app) {
    POINT pt{};
    ::GetCursorPos(&pt);
    const HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);

    RECT rc{};
    ::GetWindowRect(app.hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    ::SetWindowPos(app.hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    ::SetForegroundWindow(app.hwnd);
    ::InvalidateRect(app.hwnd, nullptr, FALSE);
}

void app_hide(App& app) { ::ShowWindow(app.hwnd, SW_HIDE); }

void app_toggle(App& app) {
    if (::IsWindowVisible(app.hwnd)) {
        app_hide(app);
    } else {
        app_show(app);
    }
}

static void on_tray(App& app, LPARAM lp) {
    switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            app_toggle(app);
            break;
        case WM_RBUTTONUP:
            show_tray_menu(app);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK app_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            return TRUE;
        case WM_APP_SHOW:
            if (app) app_show(*app);
            return 0;
        case WM_APP_TRAY:
            if (app) on_tray(*app, lp);
            return 0;
        case WM_ACTIVATE:
            // Review Focus：拖放期间失焦不能隐藏，否则刚拖起的项会随着窗口一起消失
            if (app && LOWORD(wp) == WA_INACTIVE && !app->in_drag && ::IsWindowVisible(hwnd)) {
                app_hide(*app);
            }
            return 0;
        case WM_HOTKEY:
            if (app && wp == app->hotkey_id) app_toggle(*app);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) app_hide(*app);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 全部自绘，禁止系统擦背景以消除闪烁
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(ps.hdc, &rc, reinterpret_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH)));
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool app_init(App& app, HINSTANCE inst) {
    app.inst = inst;

    // Review Focus 1：数据目录不可写时明确告知并退出，不静默丢数据
    if (!init_paths(app.paths)) {
        ::MessageBoxW(nullptr, L"无法定位程序所在目录。", L"Stargazer", MB_ICONERROR);
        return false;
    }
    if (!app.paths.writable) {
        std::wstring msg = L"Stargazer 是便携程序，需要在自己的目录下读写数据。\n\n无法写入：\n";
        msg += app.paths.data_dir;
        msg += L"\n\n请把 stargazer.exe 移到可写目录（例如 D:\\Tools\\stargazer）后重试。";
        ::MessageBoxW(nullptr, msg.c_str(), L"Stargazer", MB_ICONERROR);
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // 双击检测由系统完成，视图层不必自己计时
    wc.lpfnWndProc = app_wndproc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 自绘
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc)) return false;

    const UINT sys_dpi = ::GetDpiForSystem();
    const int w = ::MulDiv(kDefaultW, static_cast<int>(sys_dpi), 96);
    const int h = ::MulDiv(kDefaultH, static_cast<int>(sys_dpi), 96);

    // TOOLWINDOW：不出现在任务栏与 Alt+Tab；TOPMOST：呼出后始终在顶层
    app.hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"Stargazer",
                                 WS_POPUP, 0, 0, w, h, nullptr, nullptr, inst, &app);
    if (!app.hwnd) return false;

    app.hotkey_ok = ::RegisterHotKey(app.hwnd, app.hotkey_id, kDefaultHotkeyMods, kDefaultHotkeyKey) != FALSE;
    if (!app.hotkey_ok) {
        ::MessageBoxW(nullptr, L"全局热键 Ctrl+Shift+Space 注册失败（可能被其它程序占用）。\n"
                               L"可继续用托盘图标呼出。",
                      L"Stargazer", MB_ICONWARNING);
    }

    add_tray_icon(app);
    autostart_heal(exe_path_of(app));
    return true;
}

void app_shutdown(App& app) {
    if (app.hwnd) {
        ::UnregisterHotKey(app.hwnd, app.hotkey_id);
        remove_tray_icon(app);
    }
}

}  // namespace sg
```

`app.cpp` 里用到的 `exe_path_of(app)` 是取当前 exe 全路径的小助手，写在 `persist.h` 里（`std::wstring exe_path()`），在本任务中补上：

`persist.h` 追加声明：

```cpp
// 当前进程 exe 的绝对路径；失败返回空
std::wstring exe_path();
```

`persist.cpp` 追加实现：

```cpp
std::wstring exe_path() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return buf;
}
```

- [ ] **Step 4: 写 main.cpp**

`src/main.cpp`：

```cpp
#include <windows.h>
#include <shellapi.h>

#include <string>

#include "app.h"

namespace {

bool has_autostart_flag() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (::_wcsicmp(argv[i], L"--autostart") == 0) {
            found = true;
            break;
        }
    }
    ::LocalFree(argv);
    return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // per-monitor-v2：多显示器不同缩放时不糊
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Review Focus 5：已有实例时唤出它并退出自己，绝不启动第二个进程
    HANDLE once = ::CreateMutexW(nullptr, TRUE, L"Local\\stargazer-singleton");
    if (once && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(sg::kWindowClass, nullptr)) {
            ::PostMessageW(existing, sg::WM_APP_SHOW, 0, 0);
        }
        ::CloseHandle(once);
        return 0;
    }

    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    sg::App app;
    if (!sg::app_init(app, inst)) {
        ::CoUninitialize();
        if (once) ::CloseHandle(once);
        return 1;
    }

    // 开机自启时只驻留托盘，不弹窗、不抢焦点
    if (!has_autostart_flag()) sg::app_show(app);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    sg::app_shutdown(app);
    ::CoUninitialize();
    if (once) ::CloseHandle(once);
    return 0;
}
```

- [ ] **Step 5: 构建**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target stargazer
```

预期：编译通过，产出 `build\Release\stargazer.exe`。

- [ ] **Step 6: 手工验收**

依次确认：

1. 双击 `stargazer.exe`，窗口出现在鼠标所在显示器中央，深灰背景，无边框。
2. 任务栏和 Alt+Tab 里**没有**它（`WS_EX_TOOLWINDOW` 生效）。
3. 切到别的窗口（点击记事本），它自动消失。
4. `Ctrl+Shift+Space` 能反复呼出/隐藏。
5. `Esc` 隐藏。
6. 托盘右键菜单可呼出、可勾选/取消"开机自启"（勾选后开 regedit 确认 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下有 `stargazer` 值）。
7. **Review Focus 5**：保持它运行，再双击一次 exe——不出现第二个进程，已有窗口被唤出。任务管理器里 `stargazer.exe` 始终只有一个。
8. 双击已运行时的 exe 并带 `--autostart`（`Start-Process .\build\Release\stargazer.exe -ArgumentList --autostart`）——同样只有一个进程，且不会弹窗。
9. 托盘"退出"后进程从任务管理器消失，无残留。

- [ ] **Step 7: 提交**

```powershell
git add src/app.h src/app.cpp src/main.cpp src/persist.h src/persist.cpp CMakeLists.txt
git commit -m "feat(app): 单实例、无边框窗口、托盘、全局热键、呼出/隐藏"
```

---

### Task 6: Direct2D 渲染层

**Files:**
- Create: `src/render.h`
- Create: `src/render.cpp`
- Modify: `src/app.cpp`（`WM_CREATE` / `WM_SIZE` / `WM_PAINT` / `WM_DESTROY`）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 无
- Produces:
  - `struct sg::Theme`（深色配色常量）
  - `struct sg::Renderer`，方法：`init(HWND)` / `shutdown()` / `begin()` / `end()` / `clear(D2D1_COLOR_F)` / `fill_round_rect(rect, radius, color)` / `stroke_round_rect(rect, radius, color, width)` / `text(rect, wstring, IDWriteTextFormat*, color)` / `format(size, weight, align)`
  - `ID2D1Bitmap* sg::make_bitmap(Renderer&, const void* pixels, int w, int h)` — 从预乘 BGRA 缓冲创建位图
  - `Renderer::dpi`（float）供字体尺寸换算；`Renderer::theme`

- [ ] **Step 1: 写 render.h**

`src/render.h`：

```cpp
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <unordered_map>

namespace sg {

struct Theme {
    D2D1_COLOR_F bg       = D2D1::ColorF(0.106f, 0.110f, 0.125f, 1.f);
    D2D1_COLOR_F panel    = D2D1::ColorF(0.145f, 0.153f, 0.176f, 1.f);
    D2D1_COLOR_F card     = D2D1::ColorF(0.180f, 0.192f, 0.220f, 1.f);
    D2D1_COLOR_F hover    = D2D1::ColorF(0.235f, 0.251f, 0.290f, 1.f);
    D2D1_COLOR_F border   = D2D1::ColorF(0.250f, 0.266f, 0.306f, 1.f);
    D2D1_COLOR_F text     = D2D1::ColorF(0.850f, 0.870f, 0.900f, 1.f);
    D2D1_COLOR_F text_dim = D2D1::ColorF(0.500f, 0.530f, 0.580f, 1.f);
    D2D1_COLOR_F accent   = D2D1::ColorF(0.290f, 0.580f, 0.900f, 1.f);
};

struct Renderer {
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    HWND hwnd = nullptr;
    float dpi = 96.f;   // 物理 DPI；DIP 坐标下所有尺寸都用 96 DPI 逻辑像素
    Theme theme;
    std::unordered_map<std::wstring, IDWriteTextFormat*> formats;

    bool init(HWND hwnd);
    void shutdown();

    bool begin();   // 取到 rt 并 BeginDraw；false 表示跳过这一帧
    void end();     // EndDraw；D2DERR_RECREATE_TARGET 时重建设备资源

    IDWriteTextFormat* format(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

    void clear(D2D1_COLOR_F c);
    void fill_rect(const D2D1_RECT_F& r, D2D1_COLOR_F c);
    void fill_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c);
    void stroke_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1.f);
    void text(const D2D1_RECT_F& r, const std::wstring& s, IDWriteTextFormat* fmt, D2D1_COLOR_F c);

private:
    bool create_device_resources();
    void discard_device_resources();
    void discard_formats();
};

ID2D1Bitmap* make_bitmap(Renderer& r, const void* pixels, int w, int h);

}  // namespace sg
```

- [ ] **Step 2: 写 render.cpp**

`src/render.cpp`：

```cpp
#include "render.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace sg {

bool Renderer::init(HWND wnd) {
    hwnd = wnd;
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) return false;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(&dwrite)))) {
        return false;
    }
    dpi = static_cast<float>(::GetDpiForWindow(hwnd));
    if (dpi <= 0.f) dpi = 96.f;
    return create_device_resources();
}

bool Renderer::create_device_resources() {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right > 0 ? rc.right : 1),
        static_cast<UINT32>(rc.bottom > 0 ? rc.bottom : 1));

    // SetDpi 后所有绘制坐标都是 96 DPI 下的逻辑像素，DPI 换算全部交给 D2D
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props = D2D1::HwndRenderTargetProperties(
        hwnd, size, D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(factory->CreateHwndRenderTarget(props, hwnd_props, &rt))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush))) return false;
    return true;
}

void Renderer::discard_device_resources() {
    if (brush) { brush->Release(); brush = nullptr; }
    if (rt) { rt->Release(); rt = nullptr; }
}

void Renderer::discard_formats() {
    for (auto& kv : formats) kv.second->Release();
    formats.clear();
}

void Renderer::shutdown() {
    discard_device_resources();
    discard_formats();
    if (dwrite) { dwrite->Release(); dwrite = nullptr; }
    if (factory) { factory->Release(); factory = nullptr; }
}

bool Renderer::begin() {
    if (!rt) {
        if (!create_device_resources()) return false;
    }
    rt->BeginDraw();
    // 窗口尺寸变化后 rt 需要同步，否则绘制会被裁剪
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom));
    if (rt->GetPixelSize() != size) rt->Resize(size);
    return true;
}

void Renderer::end() {
    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // 设备丢失：丢弃设备相关资源，下一帧重建。图标位图由 icons 模块自理
        discard_device_resources();
    }
}

IDWriteTextFormat* Renderer::format(float size, DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align) {
    wchar_t key[64] = {};
    ::swprintf_s(key, L"%.1f|%d|%d", size, static_cast<int>(weight), static_cast<int>(align));
    auto it = formats.find(key);
    if (it != formats.end()) return it->second;

    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", &fmt))) {
        return nullptr;
    }
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 单元格内文字过长时截断，不溢到相邻单元格
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats.emplace(key, fmt);
    return fmt;
}

void Renderer::clear(D2D1_COLOR_F c) { rt->Clear(&c); }

void Renderer::fill_rect(const D2D1_RECT_F& r, D2D1_COLOR_F c) {
    brush->SetColor(c);
    rt->FillRectangle(&r, brush);
}

void Renderer::fill_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    brush->SetColor(c);
    rt->FillRoundedRectangle(&rr, brush);
}

void Renderer::stroke_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width) {
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    brush->SetColor(c);
    rt->DrawRoundedRectangle(&rr, brush, width);
}

void Renderer::text(const D2D1_RECT_F& r, const std::wstring& s, IDWriteTextFormat* fmt,
                    D2D1_COLOR_F c) {
    if (!fmt || s.empty()) return;
    brush->SetColor(c);
    rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

ID2D1Bitmap* make_bitmap(Renderer& r, const void* pixels, int w, int h) {
    if (!r.rt || w <= 0 || h <= 0 || !pixels) return nullptr;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        r.dpi, r.dpi);
    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(r.rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)),
                                  pixels, static_cast<UINT32>(w * 4), props, &bmp))) {
        return nullptr;
    }
    return bmp;
}

}  // namespace sg
```

- [ ] **Step 3: 接入 app.cpp**

在 `src/app.h` 的 `App` 结构体里加成员：`Renderer render;`（并在 `app.h` 里 `#include "render.h"`）。

在 `app.cpp` 的 `app_wndproc` 里替换 `WM_PAINT`，并新增 `WM_CREATE` / `WM_SIZE`：

```cpp
        case WM_CREATE:
            if (app) app->render.init(hwnd);
            return 0;
        case WM_SIZE:
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            if (app && app->render.begin()) {
                app->render.clear(app->render.theme.bg);
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const D2D1_RECT_F card = D2D1::RectF(
                    16.f, 56.f, static_cast<float>(rc.right) - 16.f, static_cast<float>(rc.bottom) - 16.f);
                app->render.fill_round_rect(card, 8.f, app->render.theme.panel);
                app->render.text(D2D1::RectF(24.f, 16.f, 400.f, 44.f), L"Stargazer 渲染层就绪",
                                 app->render.format(16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                                 app->render.theme.text);
                app->render.end();
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
```

在 `app_shutdown` 开头加 `app.render.shutdown();`。

- [ ] **Step 4: 构建并手工验收**

```powershell
cmake --build build --config Release --target stargazer
.\build\Release\stargazer.exe
```

确认：深色背景；中间一个圆角面板；左上方一行抗锯齿中文标题（无锯齿、无错别字）；缩放窗口时面板跟着拉伸且不闪烁；隐藏再呼出无描边残留。

- [ ] **Step 5: 提交**

```powershell
git add src/render.h src/render.cpp src/app.h src/app.cpp CMakeLists.txt
git commit -m "feat(render): Direct2D 渲染层、文本格式缓存、设备丢失重建"
```

---

### Task 7: Shell 图标提取、LRU 缓存、图标工作线程

**Files:**
- Create: `src/model/bgra.h`、`src/model/bgra.cpp`（预乘，纯，可测）
- Create: `src/icons.h`
- Create: `src/icons.cpp`
- Modify: `src/render.cpp`（设备丢失时通知图标层）
- Modify: `src/app.cpp`（处理 `WM_APP_ICON_READY`）
- Modify: `CMakeLists.txt`
- Modify: `tests/test_model.cpp`

**Interfaces:**
- Consumes: `sg::Renderer`（Task 6）、`sg::normalize_key`（Task 2）
- Produces:
  - `void sg::premultiply_bgra(uint8_t* px, size_t count)` — 原地转换，count 为像素数
  - `bool sg::icons_init(HWND notify_hwnd)` / `void sg::icons_shutdown()`
  - `ID2D1Bitmap* sg::icons_get(Renderer& r, const std::wstring& path, bool is_dir)` — 命中返回位图，未命中投递后台请求并返回 `nullptr`
  - `void sg::icons_on_device_lost()` / `void sg::icons_clear()` / `size_t sg::icons_count()`

- [ ] **Step 1: 追加预乘测试**

在 `tests/test_model.cpp` 加 `#include "model/bgra.h"` 与：

```cpp
static void test_premultiply_bgra() {
    // 不透明像素不变
    uint8_t a[4] = { 10, 20, 30, 255 };
    sg::premultiply_bgra(a, 1);
    CHECK_EQ(int(a[0]), 10);
    CHECK_EQ(int(a[1]), 20);
    CHECK_EQ(int(a[2]), 30);
    CHECK_EQ(int(a[3]), 255);

    // 半透明向 0 收缩
    uint8_t b[4] = { 200, 100, 0, 128 };
    sg::premultiply_bgra(b, 1);
    CHECK_EQ(int(b[0]), 100);  // 200*128/255 = 100
    CHECK_EQ(int(b[1]), 50);
    CHECK_EQ(int(b[2]), 0);
    CHECK_EQ(int(b[3]), 128);

    // 全透明像素不残留颜色（否则会出现亮边）
    uint8_t c[4] = { 255, 255, 255, 0 };
    sg::premultiply_bgra(c, 1);
    CHECK_EQ(int(c[0]), 0);
    CHECK_EQ(int(c[1]), 0);
    CHECK_EQ(int(c[2]), 0);

    // 幂等性不要求（重复调用会变暗），但空指针与 0 像素必须安全
    sg::premultiply_bgra(nullptr, 0);
}
```

`main()` 里追加 `test_premultiply_bgra();`。

- [ ] **Step 2: 实现 bgra**

`src/model/bgra.h`：

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace sg {

// 原地把直通 alpha 的 BGRA 转成预乘（D2D 的 PREMULTIPLIED 需要）
// px 为 BGRA 顺序，每像素 4 字节；count 为像素数
void premultiply_bgra(uint8_t* px, size_t count);

}  // namespace sg
```

`src/model/bgra.cpp`：

```cpp
#include "model/bgra.h"

namespace sg {

void premultiply_bgra(uint8_t* px, size_t count) {
    if (!px) return;
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = px + i * 4;
        const unsigned a = p[3];
        if (a == 255) continue;
        if (a == 0) {
            p[0] = p[1] = p[2] = 0;
            continue;
        }
        p[0] = static_cast<uint8_t>(p[0] * a / 255);
        p[1] = static_cast<uint8_t>(p[1] * a / 255);
        p[2] = static_cast<uint8_t>(p[2] * a / 255);
    }
}

}  // namespace sg
```

把 `bgra.cpp` 加进 `CMakeLists.txt` 的 `MODEL_SOURCES`，构建并运行 `test_model`，确认通过。

- [ ] **Step 3: 写 icons.h**

`src/icons.h`：

```cpp
#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>

namespace sg {
struct Renderer;

// 启动图标工作线程；notify_hwnd 会收到 WM_APP_ICON_READY
bool icons_init(HWND notify_hwnd);
void icons_shutdown();

// 命中则返回位图；未命中时投递后台请求并返回 nullptr（本帧画占位）
// key 用 sg::normalize_key(path)，目录额外加后缀 ":dir" 以区分同名文件与目录
ID2D1Bitmap* icons_get(Renderer& r, const std::wstring& path, bool is_dir);

// 设备丢失：丢弃位图保留像素，下一帧按需重建
void icons_on_device_lost();
void icons_clear();
size_t icons_count();

}  // namespace sg
```

- [ ] **Step 4: 写 icons.cpp**

`src/icons.cpp`：

```cpp
#include "icons.h"

#include <shellapi.h>
#include <commoncontrols.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app.h"         // WM_APP_ICON_READY
#include "model/bgra.h"
#include "model/paths.h"
#include "render.h"

namespace sg {
namespace {

// 统一按 48×48 提取（SHIL_EXTRALARGE）。
// 300 项 × 48×48×4B ≈ 2.7 MB；若高 DPI 下觉得模糊，改用
// IShellItemImageFactory::GetImage 取精确尺寸（代价约 10ms/图标）。
constexpr size_t kMaxEntries = 300;

struct Entry {
    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;
    ID2D1Bitmap* bitmap = nullptr;  // 设备资源，丢失后重建
    uint64_t last_used = 0;
};

struct Request {
    std::wstring key;
    std::wstring path;
    bool is_dir = false;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::unordered_map<std::wstring, Entry> g_cache;
std::deque<Request> g_queue;
std::set<std::wstring> g_queued;  // 去重，避免重绘风暴重复投递
uint64_t g_tick = 0;
HWND g_notify = nullptr;
std::thread g_worker;
bool g_quit = false;

void discard_bitmaps_locked() {
    for (auto& kv : g_cache) {
        if (kv.second.bitmap) {
            kv.second.bitmap->Release();
            kv.second.bitmap = nullptr;
        }
    }
}

void evict_locked() {
    while (g_cache.size() > kMaxEntries) {
        auto victim = g_cache.begin();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
            if (it->second.last_used < victim->second.last_used) victim = it;
        }
        if (victim->second.bitmap) victim->second.bitmap->Release();
        g_cache.erase(victim);
    }
}

bool hicon_to_bgra(HICON icon, std::vector<uint8_t>& px, int& w, int& h) {
    ICONINFO ii{};
    if (!::GetIconInfo(icon, &ii)) return false;

    BITMAP bm{};
    if (!::GetObjectW(ii.hbmColor, sizeof(bm), &bm)) {
        ::DeleteObject(ii.hbmColor);
        ::DeleteObject(ii.hbmMask);
        return false;
    }
    w = bm.bmWidth;
    h = bm.bmHeight;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 负高度 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib) {
        const HGDIOBJ old = ::SelectObject(screen, dib);
        ::PatBlt(screen, 0, 0, w, h, BLACKNESS);
        ok = ::DrawIconEx(screen, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL) != FALSE;
        if (ok) {
            px.assign(static_cast<uint8_t*>(bits),
                      static_cast<uint8_t*>(bits) + static_cast<size_t>(w) * h * 4);
            // DrawIconEx 写的是直通 alpha，D2D 要预乘
            premultiply_bgra(px.data(), px.size() / 4);
        }
        ::SelectObject(screen, old);
        ::DeleteObject(dib);
    }
    ::ReleaseDC(nullptr, screen);
    ::DeleteObject(ii.hbmColor);
    ::DeleteObject(ii.hbmMask);
    return ok;
}

HICON icon_from_image_list(int index) {
    IImageList* list = nullptr;
    if (FAILED(::SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&list)))) return nullptr;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(index, ILD_TRANSPARENT, &icon))) icon = nullptr;
    list->Release();
    return icon;
}

// 三级提取：真图标 -> 按扩展名推断 -> 通用图标
bool extract_icon(const Request& req, std::vector<uint8_t>& px, int& w, int& h) {
    SHFILEINFOW sfi{};
    const DWORD attrs = req.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    if (!req.is_dir &&
        ::SHGetFileInfoW(req.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_ICON)) {
        if (sfi.iIcon >= 0) {
            if (HICON icon = icon_from_image_list(sfi.iIcon)) {
                const bool ok = hicon_to_bgra(icon, px, w, h);
                ::DestroyIcon(icon);
                if (ok) return true;
            }
        }
    }

    // 网盘路径拿不到真图标时按扩展名推断（不触盘）
    if (::SHGetFileInfoW(req.path.c_str(), attrs, &sfi, sizeof(sfi),
                         SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
        if (HICON icon = icon_from_image_list(sfi.iIcon)) {
            const bool ok = hicon_to_bgra(icon, px, w, h);
            ::DestroyIcon(icon);
            if (ok) return true;
        }
    }

    if (HICON icon = ::LoadIconW(nullptr, req.is_dir ? IDI_APPLICATION : IDI_APPLICATION)) {
        const bool ok = hicon_to_bgra(icon, px, w, h);
        return ok;
    }
    return false;
}

void worker_main() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_quit || !g_queue.empty(); });
            if (g_quit && g_queue.empty()) break;
            req = std::move(g_queue.front());
            g_queue.pop_front();
        }

        std::vector<uint8_t> px;
        int w = 0, h = 0;
        const bool ok = extract_icon(req, px, w, h);

        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_queued.erase(req.key);
            if (ok) {
                Entry e;
                e.pixels = std::move(px);
                e.w = w;
                e.h = h;
                e.last_used = ++g_tick;
                auto it = g_cache.find(req.key);
                if (it != g_cache.end() && it->second.bitmap) it->second.bitmap->Release();
                g_cache[req.key] = std::move(e);
                evict_locked();
            }
        }
        // 只通知重绘，不带索引。视图按 key 查表，因此过期结果永远不会填错槽位
        if (g_notify) ::PostMessageW(g_notify, WM_APP_ICON_READY, 0, 0);
    }
    ::CoUninitialize();
}

}  // namespace

bool icons_init(HWND notify_hwnd) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_worker.joinable()) return true;
    g_notify = notify_hwnd;
    g_quit = false;
    g_worker = std::thread(worker_main);
    return true;
}

void icons_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_quit = true;
        g_notify = nullptr;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();

    std::lock_guard<std::mutex> lk(g_mu);
    discard_bitmaps_locked();
    g_cache.clear();
    g_queue.clear();
    g_queued.clear();
}

ID2D1Bitmap* icons_get(Renderer& r, const std::wstring& path, bool is_dir) {
    const std::wstring key = normalize_key(path) + (is_dir ? L":dir" : L"");

    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        Entry& e = it->second;
        e.last_used = ++g_tick;
        if (!e.bitmap) e.bitmap = make_bitmap(r, e.pixels.data(), e.w, e.h);
        return e.bitmap;
    }

    if (g_queued.insert(key).second) {
        g_queue.push_back(Request{ key, path, is_dir });
    }
    // 每投递一次就唤醒一次，避免工作线程已睡眠时新请求被拖到下一个请求才处理
    g_cv.notify_one();
    return nullptr;
}

void icons_on_device_lost() {
    std::lock_guard<std::mutex> lk(g_mu);
    discard_bitmaps_locked();
}

void icons_clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    discard_bitmaps_locked();
    g_cache.clear();
}

size_t icons_count() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cache.size();
}

}  // namespace sg
```

`g_cv.notify_one()` 已在作用域外调用：

```cpp
ID2D1Bitmap* icons_get(Renderer& r, const std::wstring& path, bool is_dir) {
    const std::wstring key = normalize_key(path) + (is_dir ? L":dir" : L"");
    bool queued_now = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            Entry& e = it->second;
            e.last_used = ++g_tick;
            if (!e.bitmap) e.bitmap = make_bitmap(r, e.pixels.data(), e.w, e.h);
            return e.bitmap;
        }
        queued_now = g_queued.insert(key).second;
        if (queued_now) g_queue.push_back(Request{ key, path, is_dir });
    }
    if (queued_now) g_cv.notify_one();
    return nullptr;
}
```

- [ ] **Step 5: 接入设备丢失与图标就绪**

`src/render.cpp` 的 `end()` 里在 `discard_device_resources()` 之后加 `icons_on_device_lost();`（`#include "icons.h"`）。

`src/app.cpp` 的 `app_wndproc` 加一条消息：

```cpp
        case WM_APP_ICON_READY:
            ::InvalidateRect(hwnd, nullptr, FALSE);   // 只标脏，不抢焦点、不重排
            return 0;
```

`main.cpp` 里在 `app_init` 之后加 `sg::icons_init(app.hwnd);`，在 `app_shutdown` 之前加 `sg::icons_shutdown();`。

- [ ] **Step 6: 临时验证钩子**

为在本任务内独立验证图标管线，在 `app_wndproc` 里加一个**临时**分支（Task 9 会删除，注释里标明）：

```cpp
        // TEMP(Task 9 移除)：按 1/2 键在标题栏区域请求图标，验证三级提取与异步回投
        case WM_KEYDOWN:
            if (wp == L'1') { app->debug_icon = L"C:\\Windows\\notepad.exe"; }
            else if (wp == L'2') { app->debug_icon = L"D:\\不存在的网盘目录\\a.psd"; }
            else if (wp == VK_ESCAPE) { app_hide(*app); }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
```

在 `App` 结构体加 `std::wstring debug_icon;`，并在 `WM_PAINT` 里把 `debug_icon` 非空时的图标画在卡片内左侧：

```cpp
                if (!app->debug_icon.empty()) {
                    if (ID2D1Bitmap* bmp = icons_get(app->render, app->debug_icon, false)) {
                        app->render.rt->DrawBitmap(
                            bmp, D2D1::RectF(card.left + 16.f, card.top + 16.f,
                                             card.left + 64.f, card.top + 64.f),
                            1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                }
```

- [ ] **Step 7: 构建并手工验收**

```powershell
cmake --build build --config Release --target stargazer
.\build\Release\stargazer.exe
```

确认：

1. 按 `1`：第一帧空白，随后记事本图标出现（“骨架先出、图标后到”成立）。
2. 按 `2`：不存在的路径也能出图标（走到第二级按扩展名推断）。
3. **Review Focus 4**：连按 `1`、`2` 快速切换，图标始终出现在同一位置且**不串色**（不会出现上一个图标的图标落在这一帧）。
4. 任务管理器观察内存：按 `2` 之后（图标未命中真路径）内存增长很小；连续请求不同路径 400 个以上，内存应稳定在约 3 MB 左右不再涨（LRU 在起作用）。

- [ ] **Step 8: 提交**

```powershell
git add src/model/bgra.h src/model/bgra.cpp src/icons.h src/icons.cpp src/render.cpp src/app.h src/app.cpp src/main.cpp tests/test_model.cpp CMakeLists.txt
git commit -m "feat(icons): Shell 图标三级提取、LRU 缓存、异步工作线程"
```

---

### Task 8: InlineEdit（原生 EDIT 子控件封装）

**Files:**
- Create: `src/edit.h`
- Create: `src/edit.cpp`
- Modify: `src/app.cpp`（`WM_CTLCOLOREDIT` + 临时验证钩子）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 无
- Produces:
  - `struct sg::InlineEdit`，方法：`open(HWND parent, const RECT& rc, const std::wstring& initial, float dpi, std::function<void(const std::wstring&)> commit, std::function<void()> cancel)` / `set_rect(const RECT&)` / `close()` / `is_open()` / `text()` / `focus()`
  - `HBRUSH sg::edit_bg_brush()` — 供父窗口 `WM_CTLCOLOREDIT` 返回，使 EDIT 与深色主题同色

- [ ] **Step 1: 写 edit.h**

`src/edit.h`：

```cpp
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace sg {

// 深色主题下的 EDIT 背景刷（父窗口在 WM_CTLCOLOREDIT 里返回它）
HBRUSH edit_bg_brush();

struct InlineEdit {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    HFONT font = nullptr;
    std::function<void(const std::wstring&)> on_commit;
    std::function<void()> on_cancel;
    bool committing = false;  // 防重入：失焦提交时不再触发取消

    void open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
              std::function<void(const std::wstring&)> commit,
              std::function<void()> cancel);
    void set_rect(const RECT& rc);
    void close();
    bool is_open() const { return hwnd != nullptr; }
    std::wstring text() const;
    void focus();
};

}  // namespace sg
```

- [ ] **Step 2: 写 edit.cpp**

`src/edit.cpp`：

```cpp
#include "edit.h"

#include <commctrl.h>

namespace sg {

namespace {

constexpr UINT_PTR kSubclassId = 1;

HBRUSH g_bg_brush = nullptr;

LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                               DWORD_PTR ref) {
    InlineEdit* e = reinterpret_cast<InlineEdit*>(ref);

    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                if (e->on_commit) e->on_commit(e->text());
                e->close();
                if (e->parent) ::SetFocus(e->parent);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (e->on_cancel) e->on_cancel();
                e->close();
                if (e->parent) ::SetFocus(e->parent);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            // 点到别处视为提交（输入框的最后内容不会白打）
            if (e && e->hwnd && !e->committing) {
                e->committing = true;
                if (e->on_commit) e->on_commit(e->text());
                e->close();
                e->committing = false;
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == VK_TAB) return 0;  // Tab 不插字符，留给视图切控件
            break;
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, edit_subclass, id);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

HBRUSH edit_bg_brush() {
    if (!g_bg_brush) {
        // 与 Theme::card 一致的实色（37,39,45）
        g_bg_brush = ::CreateSolidBrush(RGB(37, 39, 45));
    }
    return g_bg_brush;
}

void InlineEdit::open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
                      std::function<void(const std::wstring&)> commit,
                      std::function<void()> cancel) {
    close();
    parent = parent_wnd;
    on_commit = std::move(commit);
    on_cancel = std::move(cancel);
    committing = false;

    if (font) ::DeleteObject(font);
    const int height = -::MulDiv(12, static_cast<int>(dpi), 96);
    font = ::CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    hwnd = ::CreateWindowExW(0, L"EDIT", initial.c_str(),
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent,
                             nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ::SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // 全选，直接输入即替换
    ::SetWindowSubclass(hwnd, edit_subclass, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    focus();
}

void InlineEdit::set_rect(const RECT& rc) {
    if (hwnd) {
        ::SetWindowPos(hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void InlineEdit::close() {
    if (!hwnd) return;
    HWND tmp = hwnd;
    hwnd = nullptr;  // 先清空，避免 DestroyWindow 触发的失焦消息重入
    ::DestroyWindow(tmp);
    if (font) {
        ::DeleteObject(font);
        font = nullptr;
    }
    on_commit = nullptr;
    on_cancel = nullptr;
}

std::wstring InlineEdit::text() const {
    if (!hwnd) return std::wstring();
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len) + 1, L'\0');
    const int got = ::GetWindowTextW(hwnd, out.data(), len + 1);
    out.resize(static_cast<size_t>(got > 0 ? got : 0));
    return out;
}

void InlineEdit::focus() {
    if (hwnd) ::SetFocus(hwnd);
}

}  // namespace sg
```

- [ ] **Step 3: 接入 app.cpp**

`src/app.h` 的 `App` 结构体加 `InlineEdit edit;`（`#include "edit.h"`）。

`app_wndproc` 加：

```cpp
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            ::SetTextColor(dc, RGB(217, 222, 230));
            ::SetBkColor(dc, RGB(37, 39, 45));
            return reinterpret_cast<LRESULT>(sg::edit_bg_brush());
        }
```

并把临时验证钩子改为：按 `F2` 在卡片区域内打开一个 InlineEdit，提交后用 `SetWindowTextW` 把结果显示到窗口标题上。

- [ ] **Step 4: 构建并手工验收**

```powershell
cmake --build build --config Release --target stargazer
.\build\Release\stargazer.exe
```

确认：

1. `F2` 后出现输入框，底色与卡片一致（不是白底），有光标。
2. **切到中文输入法输入“测试中文”，候选窗出现在光标附近且不遮挡输入框**（这是自绘文本控件会出错的地方，原生 EDIT 必须正常）。
3. `Enter` 提交后标题栏显示刚才输入的文字；输入框消失。
4. `Esc` 取消：标题栏不变。
5. 输入到一半点击窗口其它位置：内容被提交（失焦即提交），输入框消失。
6. `Ctrl+A` 全选、`Ctrl+C`/`Ctrl+V` 可用。
7. 呼出时窗口失焦（切换到记事本）：输入框随窗口一起隐藏，无残留的悬空输入框。

- [ ] **Step 5: 提交**

```powershell
git add src/edit.h src/edit.cpp src/app.h src/app.cpp CMakeLists.txt
git commit -m "feat(edit): InlineEdit 原生 EDIT 子控件封装，含 IME 与失焦提交"
```

---

### Task 9: Launcher 视图（布局、图标网格、键盘导航、搜索过滤）

**Files:**
- Create: `src/viewapi.h`
- Create: `src/views/launcher.cpp`
- Create: `src/views/launcher.h`
- Create: `src/views/box.cpp`、`src/views/todo.cpp`、`src/views/explorer.cpp`（占位空壳）
- Modify: `src/app.h`、`src/app.cpp`（视图分发、鼠标/键盘路由、搜索框）
- Delete: Task 7/8 的临时验证钩子（`debug_icon`、`F2` 测输入框）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `sg::LaunchGroup` / `sg::LaunchItem`（Task 3）、`sg::contains_ci`（Task 2）、`sg::Renderer`（Task 6）、`sg::icons_get`（Task 7）、`sg::InlineEdit`（Task 8）
- Produces:
  - `enum class sg::View { Launcher, Box, Todo, Explorer }`
  - `struct sg::LauncherState { int group; int sel; int hover; int scroll; std::wstring query; std::vector<int> filtered; InlineEdit search; }`
  - `struct sg::AppState { std::vector<LaunchGroup> groups; std::vector<Box> boxes; std::vector<TodoItem> todos; Config config; int bad_lines; View view; LauncherState launcher; bool data_dirty; }`
  - 启动板入口：`void sg::launcher_layout(const AppState&, const RECT&, float dpi, int& cols, int& rows_visible)`、`void sg::launcher_refilter(AppState&)`、`void sg::launcher_render(Renderer&, AppState&, const RECT&, float dpi)`、`int sg::launcher_hittest(const AppState&, const RECT&, POINT)`、`bool sg::launcher_keydown(AppState&, UINT vk)`、`void sg::launcher_sync_search(AppState&, HWND parent, const RECT&, float dpi)`、`D2D1_COLOR_F sg::ext_color(const std::wstring& path)`

- [ ] **Step 1: 写 viewapi.h**

`src/viewapi.h`：

```cpp
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "edit.h"
#include "model/store.h"
#include "render.h"

namespace sg {

enum class View { Launcher, Box, Todo, Explorer };

// 启动板视图状态（只属于启动板，不污染全局）
struct LauncherState {
    int group = 0;
    int sel = -1;              // filtered 中的下标；-1 = 焦点在搜索框
    int hover = -1;
    int scroll = 0;            // 起始行
    int drag_over_tab = -1;    // 内部拖拽时高亮的目标分组标签
    std::wstring query;
    std::vector<int> filtered; // 当前分组里通过过滤的条目下标
    InlineEdit search;
};

// search 这个 InlineEdit 同时充当搜索框、重命名框与新建输入框；
// 同一时刻只会存在一个 EDIT 实例，不需要多个。

struct AppState {
    std::vector<LaunchGroup> groups;
    std::vector<Box> boxes;
    std::vector<TodoItem> todos;
    Config config;
    int bad_lines = 0;
    View view = View::Launcher;
    LauncherState launcher;
    bool data_dirty = false;  // 变更后由 app 层落盘
};

}  // namespace sg
```

`src/views/launcher.h`：

```cpp
#pragma once

#include <windows.h>

#include "viewapi.h"

namespace sg {

// 启动板布局常量（均为 96 DPI 逻辑像素）
constexpr float kCell = 96.f;
constexpr float kGap = 8.f;
constexpr float kPad = 16.f;
constexpr float kTabsH = 36.f;
constexpr float kSearchH = 34.f;

// 算出一屏能放多少列、多少行；rows_visible 至少为 1
void launcher_layout(const AppState& s, const RECT& client, float dpi, int& cols, int& rows_visible);

// 根据 query 重建 filtered；filtered 变化后夹紧 sel 与 scroll
void launcher_refilter(AppState& s);

void launcher_render(Renderer& r, AppState& s, const RECT& client, float dpi);

// 返回 filtered 下标，未命中返回 -1（点在标签区则返回 -2）
int launcher_hittest(const AppState& s, const RECT& client, POINT pt);
int launcher_tab_hittest(const AppState& s, const RECT& client, POINT pt);

// 返回 true 表示按键已被消费
bool launcher_keydown(AppState& s, const RECT& client, UINT vk);

// 把搜索框 EDIT 子控件对齐到布局位置（呼出、缩放、切分组时调用）
void launcher_sync_search(AppState& s, HWND parent, const RECT& client, float dpi);

// 按扩展名给占位块配色，同一扩展名总是同一颜色
D2D1_COLOR_F ext_color(const std::wstring& path);

}  // namespace sg
```

- [ ] **Step 2: 实现占位色与布局**

`src/views/launcher.cpp` 开头（后续步骤继续往这个文件里加）：

```cpp
#include "views/launcher.h"

#include <d2d1.h>

#include <algorithm>

#include "icons.h"
#include "model/paths.h"
#include "model/search.h"

namespace sg {

D2D1_COLOR_F ext_color(const std::wstring& path) {
    // 固定 8 色调色板：扩展名哈希取模，同一类型总是同色且重启不变
    static const D2D1_COLOR_F palette[8] = {
        D2D1::ColorF(0.35f, 0.45f, 0.62f), D2D1::ColorF(0.32f, 0.53f, 0.48f),
        D2D1::ColorF(0.58f, 0.44f, 0.35f), D2D1::ColorF(0.50f, 0.38f, 0.55f),
        D2D1::ColorF(0.40f, 0.48f, 0.36f), D2D1::ColorF(0.58f, 0.38f, 0.42f),
        D2D1::ColorF(0.34f, 0.42f, 0.56f), D2D1::ColorF(0.45f, 0.45f, 0.45f),
    };
    const std::wstring ext = extension_of(path);
    unsigned h = 2166136261u;  // FNV-1a
    for (wchar_t c : ext) {
        h ^= static_cast<unsigned>(c);
        h *= 16777619u;
    }
    if (ext.empty()) return palette[7];
    return palette[h % 8];
}

void launcher_layout(const AppState& s, const RECT& client, float /*dpi*/, int& cols,
                     int& rows_visible) {
    const float w = static_cast<float>(client.right - client.left);
    const float h = static_cast<float>(client.bottom - client.top);
    const float avail_w = w - kPad * 2.f;
    const float avail_h = h - kTabsH - kSearchH - kPad * 2.f;
    cols = static_cast<int>((avail_w + kGap) / (kCell + kGap));
    if (cols < 1) cols = 1;
    rows_visible = static_cast<int>((avail_h + kGap) / (kCell + kGap));
    if (rows_visible < 1) rows_visible = 1;
}

void launcher_refilter(AppState& s) {
    LauncherState& ls = s.launcher;
    ls.filtered.clear();
    if (s.groups.empty()) {
        ls.group = 0;
        ls.sel = -1;
        ls.scroll = 0;
        return;
    }
    ls.group = std::clamp(ls.group, 0, static_cast<int>(s.groups.size()) - 1);
    const auto& items = s.groups[ls.group].items;
    for (size_t i = 0; i < items.size(); ++i) {
        // 名字与目标都参与匹配：记不得名字时敲路径片段也能找到
        if (contains_ci(items[i].name, ls.query) || contains_ci(items[i].target, ls.query)) {
            ls.filtered.push_back(static_cast<int>(i));
        }
    }
    if (ls.sel >= static_cast<int>(ls.filtered.size())) {
        ls.sel = ls.filtered.empty() ? -1 : static_cast<int>(ls.filtered.size()) - 1;
    }
    if (ls.scroll < 0) ls.scroll = 0;
}

}  // namespace sg
```

- [ ] **Step 3: 实现渲染与命中**

继续在 `src/views/launcher.cpp` 的 `}  // namespace sg` 之前插入：

```cpp
// 标签行的区域与单个标签宽度（渲染与命中必须共用同一套算法，否则点击位置会对不上）
static D2D1_RECT_F tabs_rect(const RECT& client) {
    return D2D1::RectF(kPad, kPad, static_cast<float>(client.right) - kPad, kPad + kTabsH);
}

static D2D1_RECT_F search_rect(const RECT& client) {
    return D2D1::RectF(kPad, kPad + kTabsH, static_cast<float>(client.right) - kPad,
                       kPad + kTabsH + kSearchH);
}

static float tab_width(const std::wstring& name) {
    return 24.f + static_cast<float>(name.size()) * 13.f;
}

static int tab_at(const AppState& s, const RECT& client, POINT pt) {
    const D2D1_RECT_F tr = tabs_rect(client);
    if (static_cast<float>(pt.y) < tr.top || static_cast<float>(pt.y) > tr.bottom) return -1;
    float x = tr.left;
    for (size_t i = 0; i < s.groups.size(); ++i) {
        const float w = tab_width(s.groups[i].name);
        if (static_cast<float>(pt.x) >= x && static_cast<float>(pt.x) <= x + w) {
            return static_cast<int>(i);
        }
        x += w + 6.f;
    }
    return -1;
}

int launcher_tab_hittest(const AppState& s, const RECT& client, POINT pt) {
    return tab_at(s, client, pt);
}

// 网格起点与单元格矩形
static D2D1_RECT_F cell_rect(const AppState& s, const RECT& client, int index, float& cell_out) {
    int cols = 1, rows = 1;
    launcher_layout(s, client, 1.f, cols, rows);
    const int col = index % cols;
    const int row = index / cols - s.launcher.scroll;
    const float x = kPad + col * (kCell + kGap);
    const float y = kPad + kTabsH + kSearchH + kPad + row * (kCell + kGap);
    cell_out = kCell;
    return D2D1::RectF(x, y, x + kCell, y + kCell);
}

int launcher_hittest(const AppState& s, const RECT& client, POINT pt) {
    if (tab_at(s, client, pt) >= 0) return -2;  // 点在标签上

    const float sr_bottom = search_rect(client).bottom;
    if (static_cast<float>(pt.y) < sr_bottom) return -1;  // 搜索框区域归 EDIT 子控件

    int cols = 1, rows = 1;
    launcher_layout(s, client, 1.f, cols, rows);
    const float gx = static_cast<float>(pt.x) - kPad;
    const float gy = static_cast<float>(pt.y) - (kPad + kTabsH + kSearchH + kPad);
    if (gx < 0 || gy < 0) return -1;
    const int col = static_cast<int>(gx / (kCell + kGap));
    const int row = static_cast<int>(gy / (kCell + kGap));
    // 落在格子间隙里也算没命中，避免“点空白启动了程序”
    if (gx - col * (kCell + kGap) > kCell) return -1;
    if (gy - row * (kCell + kGap) > kCell) return -1;
    if (col >= cols) return -1;
    const int idx = (row + s.launcher.scroll) * cols + col;
    if (idx < 0 || idx >= static_cast<int>(s.launcher.filtered.size())) return -1;
    return idx;
}

void launcher_render(Renderer& r, AppState& s, const RECT& client, float dpi) {
    LauncherState& ls = s.launcher;

    // 顶部标签行
    const D2D1_RECT_F tr = tabs_rect(client);
    float x = tr.left;
    IDWriteTextFormat* tab_fmt = r.format(13.f);
    for (size_t i = 0; i < s.groups.size(); ++i) {
        const float w = tab_width(s.groups[i].name);
        const D2D1_RECT_F tab = D2D1::RectF(x, tr.top, x + w, tr.bottom);
        if (static_cast<int>(i) == ls.group) {
            r.fill_round_rect(tab, 6.f, r.theme.accent);
            r.text(tab, s.groups[i].name, tab_fmt, D2D1::ColorF(1.f, 1.f, 1.f));
        } else {
            r.fill_round_rect(tab, 6.f, r.theme.card);
            r.text(tab, s.groups[i].name, tab_fmt, r.theme.text_dim);
        }
        x += w + 6.f;
    }

    // 搜索框背景（文字由 EDIT 子控件自己画）
    const D2D1_RECT_F sr = search_rect(client);
    r.fill_round_rect(sr, 6.f, r.theme.card);
    if (ls.query.empty() && !ls.search.is_open()) {
        r.text(D2D1::RectF(sr.left + 10.f, sr.top, sr.right, sr.bottom), L"搜索名称或路径…",
               r.format(13.f), r.theme.text_dim);
    }

    int cols = 1, rows = 1;
    launcher_layout(s, client, dpi, cols, rows);
    const int total_rows = (static_cast<int>(ls.filtered.size()) + cols - 1) / cols;
    ls.scroll = std::clamp(ls.scroll, 0, std::max(0, total_rows - rows));

    IDWriteTextFormat* name_fmt = r.format(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (size_t i = 0; i < ls.filtered.size(); ++i) {
        const int row = static_cast<int>(i) / cols;
        if (row < ls.scroll || row >= ls.scroll + rows) continue;  // 只画可见行（虚拟化）

        float cell = kCell;
        D2D1_RECT_F rc = cell_rect(s, client, static_cast<int>(i), cell);

        if (static_cast<int>(i) == ls.sel) {
            r.fill_round_rect(rc, 8.f, r.theme.accent);
        } else if (static_cast<int>(i) == ls.hover) {
            r.fill_round_rect(rc, 8.f, r.theme.hover);
        }

        const LaunchItem& item = s.groups[ls.group].items[ls.filtered[i]];
        const std::wstring icon_src = item.icon.empty() ? item.target : item.icon;
        const bool is_dir = !icon_src.empty() && icon_src.back() == L'\\';

        const float ix = rc.left + (kCell - 48.f) / 2.f;
        const float iy = rc.top + 8.f;
        if (ID2D1Bitmap* bmp = icons_get(r, icon_src, is_dir)) {
            r.rt->DrawBitmap(bmp, D2D1::RectF(ix, iy, ix + 48.f, iy + 48.f), 1.f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // 图标未就绪：扩展名色块 + 首字母（骨架先出，图标后到）
            const D2D1_RECT_F ph = D2D1::RectF(ix, iy, ix + 48.f, iy + 48.f);
            r.fill_round_rect(ph, 8.f, ext_color(icon_src));
            const std::wstring initial = item.name.empty() ? std::wstring(L"?")
                                                           : item.name.substr(0, 1);
            r.text(ph, initial, r.format(20.f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                   D2D1::ColorF(1.f, 1.f, 1.f));
        }

        const D2D1_RECT_F label = D2D1::RectF(rc.left + 4.f, iy + 48.f + 4.f, rc.right - 4.f, rc.bottom - 4.f);
        const D2D1_COLOR_F label_color =
            (static_cast<int>(i) == ls.sel) ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.text;
        r.text(label, item.name, name_fmt, label_color);
    }

    if (ls.filtered.empty() && !s.groups.empty()) {
        r.text(D2D1::RectF(kPad, 120.f, static_cast<float>(client.right) - kPad, 180.f),
               L"没有匹配的条目", r.format(13.f), r.theme.text_dim);
    }
}
```

- [ ] **Step 4: 实现键盘导航与搜索框同步**

继续插入：

```cpp
bool launcher_keydown(AppState& s, const RECT& client, UINT vk) {
    LauncherState& ls = s.launcher;
    int cols = 1, rows = 1;
    launcher_layout(s, client, 1.f, cols, rows);
    const int count = static_cast<int>(ls.filtered.size());

    switch (vk) {
        case VK_DOWN:
            // 从搜索框进入网格
            ls.sel = (ls.sel < 0) ? (count > 0 ? 0 : -1) : std::min(ls.sel + cols, count - 1);
            break;
        case VK_UP:
            if (ls.sel >= 0 && ls.sel < cols) {
                ls.sel = -1;  // 回到搜索框
                ls.search.focus();
            } else {
                ls.sel = std::max(0, ls.sel - cols);
            }
            break;
        case VK_LEFT:
            ls.sel = std::max(0, ls.sel - 1);
            break;
        case VK_RIGHT:
            ls.sel = std::min(count - 1, ls.sel + 1);
            break;
        case VK_PRIOR:  // PageUp
            ls.scroll = std::max(0, ls.scroll - rows);
            break;
        case VK_NEXT:
            ls.scroll += rows;
            break;
        case VK_TAB:
            // 焦点在搜索框与网格之间切换
            if (ls.search.is_open()) {
                ls.search.close();
                if (count > 0) ls.sel = 0;
            } else {
                ls.sel = -1;
            }
            break;
        case VK_F5:
            launcher_refilter(s);
            break;
        default:
            return false;
    }

    // 选中项必须在可见范围内
    if (ls.sel >= 0) ls.scroll = std::clamp(ls.scroll, std::max(0, ls.sel / cols - rows + 1), ls.sel / cols);
    // 这里不碰 InlineEdit：焦点同步由 app 层在按键处理完之后调用
    // launcher_sync_search(state, app.hwnd, rc, app.render.dpi) 完成，
    // 因为只有 app 层知道真实的父窗口句柄与 DPI。
    return true;
}

void launcher_sync_search(AppState& s, HWND parent, const RECT& client, float dpi) {
    LauncherState& ls = s.launcher;
    const D2D1_RECT_F sr = search_rect(client);
    const RECT rc{ static_cast<LONG>(sr.left + 6.f), static_cast<LONG>(sr.top + 5.f),
                   static_cast<LONG>(sr.right - 6.f), static_cast<LONG>(sr.bottom - 5.f) };

    if (!ls.search.is_open()) {
        // 搜索框常驻：呼出时自动获得焦点，直接打字即搜索
        ls.search.open(parent, rc, ls.query, dpi,
                       [&s](const std::wstring& t) {
                           s.launcher.query = t;
                           launcher_refilter(s);
                       },
                       [&s]() {
                           s.launcher.query.clear();
                           launcher_refilter(s);
                       });
    } else {
        ls.search.set_rect(rc);
    }
}
```

注意：`InlineEdit` 在失焦时会提交并销毁自己，因此搜索框会在呼出时重建。`launcher_sync_search` 必须先设 `ls.query` 再重建，否则重新呼出时搜索词会丢。呼出时调用顺序：先 `ls.query.clear()` → `launcher_refilter` → `launcher_sync_search`。

- [ ] **Step 5: 接入 app（视图分发、鼠标与键盘路由）**

`src/app.h` 的 `App` 结构体加 `AppState state;`，并删掉 Task 7/8 的临时成员（`debug_icon`）。

`src/app.cpp` 的 `app_wndproc` 里替换 `WM_PAINT` 的临时绘制，并新增鼠标消息：

```cpp
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            if (app && app->render.begin()) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                app->render.clear(app->render.theme.bg);
                launcher_render(app->render, app->state, rc, app->render.dpi);
                app->render.end();
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!app || app->in_drag) return 0;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hit = launcher_hittest(app->state, rc, pt);
            // 只在悬停项变化时重绘，鼠标每动一下就重绘会让空闲 CPU 上去
            if (hit != app->state.launcher.hover) {
                app->state.launcher.hover = hit;
                if (!app->mouse_tracking) {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    ::TrackMouseEvent(&tme);
                    app->mouse_tracking = true;
                }
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (app) {
                app->mouse_tracking = false;
                app->state.launcher.hover = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!app) return 0;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int tab = launcher_tab_hittest(app->state, rc, pt);
            if (tab >= 0) {
                app->state.launcher.group = tab;
                app->state.launcher.sel = -1;
                app->state.launcher.scroll = 0;
                launcher_refilter(app->state);
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
```

`App` 加成员 `bool mouse_tracking = false;`，并加 `#include "views/launcher.h"`（它已包含 `viewapi.h`）。鼠标消息里的 `GET_X_LPARAM` / `GET_Y_LPARAM` 来自 `<windowsx.h>`，在 `app.cpp` 顶部 `#include <windowsx.h>`，否则 `/W4` 下找不到这些宏。`WM_KEYDOWN` 改为：

```cpp
        case WM_KEYDOWN:
            if (!app) break;
            if (wp == VK_ESCAPE) {
                app_hide(*app);
                return 0;
            }
            {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                if (launcher_keydown(app->state, rc, static_cast<UINT>(wp))) {
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            break;
```

但 `VK_ESCAPE` 会先被 EDIT 子控件的子类链表吃掉（Task 8 已处理），因此搜索框有焦点时 `Esc` 是“清空搜索”而不是“隐藏窗口”。呼出时 `launcher_sync_search` 需在 `app_show` 里调用：

```cpp
void app_show(App& app) {
    // ... 定位代码不变 ...
    app.state.launcher.query.clear();
    app.state.launcher.sel = -1;
    app.state.launcher.scroll = 0;
    launcher_refilter(app.state);
    RECT rc{};
    ::GetClientRect(app.hwnd, &rc);
    launcher_sync_search(app.state, app.hwnd, rc, app.render.dpi);
    ::InvalidateRect(app.hwnd, nullptr, FALSE);
}
```

`Box` / `Todo` / `Explorer` 三个空壳文件内容：

```cpp
#include "views/launcher.h"

namespace sg {
// 阶段 2 实现
}
```

并把三个 .cpp 加进 `CMakeLists.txt` 的 `SG_SOURCES`。

- [ ] **Step 6: 构建并手工验收**

```powershell
cmake --build build --config Release --target stargazer
.\build\Release\stargazer.exe
```

先在 `data\launcher.txt` 里手工写几行测试数据（UTF-8，Tab 分隔，六列）：

```
常用	记事本	C:\Windows\notepad.exe			
常用	计算器	C:\Windows\System32\calc.exe			
常用	我的文档	C:\Users\%USERNAME%\Documents			
网盘	电影	D:\网盘\电影			
```

验收：

1. 呼出后光标在搜索框，直接敲“记”就只剩记事本（中文字串匹配）。
2. 敲 `calc` 能匹配到计算器（目标路径也参与匹配）。
3. `↓` 进入网格，方向键移动，选中项有蓝色高亮；`↑` 从首行回到搜索框。
4. 圆角、文字、图标均无锯齿；图标 48px 居中，名字在下方居中且超长不溢出格子。
5. 鼠标移到格子上变淡灰高亮，移开消失；拖动鼠标在格子上快速划过时 CPU 占用不高（悬停索引变化才重绘）。
6. 点标签切到“网盘”分组，网格内容随之变化。
7. 窗口拉宽后每行列数自动增加，拉窄后减少。

- [ ] **Step 7: 提交**

```powershell
git add src/viewapi.h src/views/ CMakeLists.txt src/app.h src/app.cpp
git commit -m "feat(launcher): 启动板视图（图标网格、搜索过滤、键盘导航）"
```

---

### Task 10: 启动条目、解析 .lnk、从 Explorer 拖入

**Files:**
- Create: `src/launch.h`、`src/launch.cpp`
- Create: `src/dragdrop.h`、`src/dragdrop.cpp`
- Modify: `src/app.cpp`（Enter 启动、双击启动、注册拖放、`in_drag`）
- Modify: `src/views/launcher.cpp`（拖入后加入目标分组、`data_dirty`）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `sg::LaunchItem`、`sg::AppState`
- Produces:
  - `bool sg::launch_item(const LaunchItem& item, std::wstring* err)` — 失败时写入错误文本
  - `bool sg::resolve_lnk(const std::wstring& lnk_path, LaunchItem& out)` — 填充 name/target/args/workdir/icon
  - `LaunchItem sg::item_from_path(const std::wstring& path)` — 自动分流：`.lnk` 走解析，其余直接当 target
  - `bool sg::dragdrop_init(HWND hwnd)` / `void sg::dragdrop_shutdown(HWND hwnd)`
  - `void sg::dragdrop_set_hook(std::function<void(const std::vector<std::wstring>&)>)` — 拖入时的回调
  - `void sg::dragdrop_set_drag_flag(bool*)` — 把 `App::in_drag` 交给拖放层置位

- [ ] **Step 1: 实现 launch.h / launch.cpp**

`src/launch.h`：

```cpp
#pragma once

#include <string>

#include "model/store.h"

namespace sg {

// 启动失败时 err 被填入用户可读的原因（用于托盘气泡，不弹 MessageBox）
bool launch_item(const LaunchItem& item, std::wstring* err);

// 解析 .lnk 的目标/参数/工作目录/图标；失败返回 false
bool resolve_lnk(const std::wstring& lnk_path, LaunchItem& out);

LaunchItem item_from_path(const std::wstring& path);

}  // namespace sg
```

`src/launch.cpp`：

```cpp
#include "launch.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "model/paths.h"

namespace sg {

bool launch_item(const LaunchItem& item, std::wstring* err) {
    if (item.target.empty()) {
        if (err) *err = L"条目没有目标路径";
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;  // 自己报错，不让系统弹框
    sei.lpVerb = L"open";
    sei.lpFile = item.target.c_str();
    sei.lpParameters = item.args.empty() ? nullptr : item.args.c_str();
    sei.lpDirectory = item.workdir.empty() ? nullptr : item.workdir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&sei)) {
        if (err) {
            const DWORD e = ::GetLastError();
            wchar_t buf[256] = {};
            ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                             0, buf, 256, nullptr);
            *err = L"启动失败：";
            *err += item.target;
            if (buf[0]) {
                *err += L"\n";
                *err += buf;
            }
        }
        return false;
    }
    return true;
}
bool resolve_lnk(const std::wstring& lnk_path, LaunchItem& out) {
    IShellLinkW* link = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return false;
    }

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        if (SUCCEEDED(file->Load(lnk_path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH] = {};
            wchar_t args[1024] = {};
            wchar_t workdir[MAX_PATH] = {};
            wchar_t icon[MAX_PATH] = {};
            int icon_index = 0;
            if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) && target[0]) {
                link->GetArguments(args, 1024);
                link->GetWorkingDirectory(workdir, MAX_PATH);
                // 显式设置的图标路径才用它，否则交给 target 自己
                if (SUCCEEDED(link->GetIconLocation(icon, MAX_PATH, &icon_index)) && icon[0]) {
                    out.icon = icon;
                }
                out.target = target;
                out.args = args;
                out.workdir = workdir;
                ok = true;
            }
        }
        file->Release();
    }
    link->Release();
    return ok;
}

LaunchItem item_from_path(const std::wstring& path) {
    LaunchItem item;
    item.name = file_name(path);
    const std::wstring ext = extension_of(path);

    if (ext == L".lnk") {
        // 解析失败也要保留条目：target 落到 .lnk 本身，双击仍能启动
        LaunchItem resolved;
        if (resolve_lnk(path, resolved)) {
            resolved.name = item.name.empty() ? resolved.name : item.name;
            return resolved;
        }
    }

    item.target = path;
    return item;
}

}  // namespace sg
```

`item.name` 保留 `.lnk` 文件名（含扩展名）以便用户认出是哪个快捷方式；若想去除扩展名，改 `file_name` 后自取主干，不在此步做。

- [ ] **Step 2: 实现 dragdrop（接收侧）**

`src/dragdrop.h`：

```cpp
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sg {

bool dragdrop_init(HWND hwnd);
void dragdrop_shutdown(HWND hwnd);

// 拖入完成时回调，参数为绝对路径列表
void dragdrop_set_hook(std::function<void(const std::vector<std::wstring>&)> hook);

// 把 App::in_drag 的地址交给拖放层；拖放期间为 true，禁止失焦隐藏
void dragdrop_set_drag_flag(bool* flag);

}  // namespace sg
```

`src/dragdrop.cpp`：

```cpp
#include "dragdrop.h"

#include <ole2.h>
#include <shellapi.h>

namespace sg {
namespace {

std::function<void(const std::vector<std::wstring>&)> g_hook;
bool* g_in_drag = nullptr;

bool read_hdrop(IDataObject* obj, std::vector<std::wstring>& out) {
    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (FAILED(obj->GetData(&fe, &stg))) return false;

    bool ok = false;
    if (HDROP drop = static_cast<HDROP>(::GlobalLock(stg.hGlobal))) {
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len + 1, L'\0');
            ::DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(len);
            if (!path.empty()) out.push_back(path);
        }
        ::GlobalUnlock(stg.hGlobal);
        ok = !out.empty();
    }
    ::ReleaseStgMedium(&stg);
    return ok;
}

class DropTarget : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* obj, DWORD, POINTL, DWORD* effect) override {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        const bool ok = obj->QueryGetData(&fe) == S_OK;
        if (g_in_drag) *g_in_drag = ok;
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = (g_in_drag && *g_in_drag) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (g_in_drag) *g_in_drag = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* obj, DWORD, POINTL, DWORD* effect) override {
        std::vector<std::wstring> paths;
        const bool ok = read_hdrop(obj, paths);
        if (ok && g_hook) g_hook(paths);
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (g_in_drag) *g_in_drag = false;
        return S_OK;
    }

private:
    ULONG refs_ = 1;
};

DropTarget* g_target = nullptr;

}  // namespace

bool dragdrop_init(HWND hwnd) {
    if (g_target) return true;
    g_target = new DropTarget();
    if (FAILED(::RegisterDragDrop(hwnd, g_target))) {
        g_target->Release();
        g_target = nullptr;
        return false;
    }
    return true;
}

void dragdrop_shutdown(HWND hwnd) {
    if (!g_target) return;
    ::RevokeDragDrop(hwnd);
    g_target->Release();
    g_target = nullptr;
    g_hook = nullptr;
}

void dragdrop_set_hook(std::function<void(const std::vector<std::wstring>&)> hook) {
    g_hook = std::move(hook);
}

void dragdrop_set_drag_flag(bool* flag) { g_in_drag = flag; }

}  // namespace sg
```

- [ ] **Step 3: 接入 app**

`app_init` 末尾（`add_tray_icon` 之后）：

```cpp
    dragdrop_set_drag_flag(&app.in_drag);
    dragdrop_init(app.hwnd);
    dragdrop_set_hook([&app](const std::vector<std::wstring>& paths) {
        // 拖入当前分组：每增加一条就标记需要落盘
        if (app.state.groups.empty()) app.state.groups.push_back(LaunchGroup{ L"常用", {} });
        auto& g = app.state.groups[std::clamp(app.state.launcher.group, 0,
                                              static_cast<int>(app.state.groups.size()) - 1)];
        for (const auto& p : paths) g.items.push_back(item_from_path(p));
        app.state.data_dirty = true;
        launcher_refilter(app.state);
        ::InvalidateRect(app.hwnd, nullptr, FALSE);
    });
```

`app_shutdown` 开头加 `dragdrop_shutdown(app.hwnd);`。

`app_wndproc` 加启动与双击：

```cpp
        case WM_LBUTTONDBLCLK: {
            // 双击启动（CS_DBLCLKS 已开启，系统保证只有快速双击才发这条消息）
            if (!app) return 0;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hit = launcher_hittest(app->state, rc, pt);
            if (hit >= 0) {
                const auto& items = app->state.groups[app->state.launcher.group].items;
                std::wstring err;
                if (!launch_item(items[app->state.launcher.filtered[hit]], &err)) {
                    // 失败用托盘气泡，不弹 MessageBox 打断操作流
                    PostMessageW(hwnd, WM_APP_TRAY, 0, 0);
                }
                app_hide(*app);
            }
            return 0;
        }
```

`WM_KEYDOWN` 里在 `launcher_keydown` 之前加 Enter 分支：

```cpp
            if (wp == VK_RETURN && app->state.launcher.sel >= 0) {
                const auto& items = app->state.groups[app->state.launcher.group].items;
                std::wstring err;
                if (!launch_item(items[app->state.launcher.filtered[app->state.launcher.sel]], &err)) {
                    ::MessageBoxW(hwnd, err.c_str(), L"Stargazer", MB_ICONWARNING);
                }
                app_hide(*app);
                return 0;
            }
```

注意：`Enter` 在搜索框有焦点时会被 EDIT 子类吃掉（Task 8 的提交逻辑），因此要启动条目必须先 `↓` 进入网格。这是有意设计：打字时回车不应当启动东西。

- [ ] **Step 4: 构建并手工验收**

```powershell
cmake --build build --config Release --target stargazer
.\build\Release\stargazer.exe
```

验收：

1. 从桌面拖一个 `.lnk` 进窗口：出现新条目，**名字是真名而非快捷方式文件名**，图标是目标程序的真实图标。
2. 从资源管理器多选几个文件拖入：条数正确。
3. `↓` 选中后 `Enter`：程序启动，窗口隐藏；启动失败时看到警告。
4. 双击条目启动。
5. 拖入过程中窗口失去焦点不隐藏（在拖拽悬停阶段切到别的窗口看）——`in_drag` 生效。
6. 拖一个不存在的路径（比如断开的网盘快捷方式）进来：不崩溃，条目存在且双击后报错。

- [ ] **Step 5: 提交**

```powershell
git add src/launch.h src/launch.cpp src/dragdrop.h src/dragdrop.cpp src/app.cpp src/views/launcher.cpp CMakeLists.txt
git commit -m "feat(launch): ShellExecuteEx 启动、.lnk 解析、从 Explorer 拖入添加"
```

---

### Task 11: 右键菜单、条目与分组管理

**Files:**
- Modify: `src/views/launcher.cpp`、`src/views/launcher.h`
- Modify: `src/app.cpp`
- Create: `src/views/launcher_edit.cpp`（重命名/新建流程：连续 InlineEdit）
- Modify: `src/launch.h`、`src/launch.cpp`（追加 `item_from_path_keep_name`）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `sg::InlineEdit`（Task 8）、`sg::AppState`
- Produces:
  - `void sg::launcher_context_menu(App& app, POINT screen_pt)`
  - `void sg::launcher_delete_selected(AppState&)`
  - `void sg::launcher_begin_rename(App&, HWND, const RECT&, float dpi)`
  - `void sg::launcher_begin_new_item(App&, HWND, const RECT&, float dpi)` — 两步输入：先名称后目标
  - `void sg::launcher_add_group(AppState&)` / `bool sg::launcher_rename_group(App&, HWND, const RECT&, float dpi)` / `bool sg::launcher_delete_group(AppState&)` — 无目标分组不可删，删除前需确认
  - `void sg::launcher_move_item_to_group(AppState&, int filtered_index, int group_index)`

右键菜单用 `TrackPopupMenuW`（与托盘菜单同一套 API，不引额外 UI 代码）：

```cpp
void launcher_context_menu(App& app, POINT screen_pt) {
    RECT rc{};
    ::GetClientRect(app.hwnd, &rc);
    const int hit = launcher_hittest(app.state, rc, screen_pt);  // 传入屏幕坐标前已转成客户区坐标

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"新建条目(&N)");
    ::AppendMenuW(menu, MF_STRING, 2, L"新建分组(&G)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 3, L"重命名(&R)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 4, L"删除(&D)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 5, L"打开所在位置(&F)");

    if (hit >= 0) app.state.launcher.sel = hit;

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x, screen_pt.y,
                                      0, app.hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1: launcher_begin_new_item(app, app.hwnd, rc, app.render.dpi); break;
        case 2: launcher_add_group(app.state); break;
        case 3: launcher_begin_rename(app, app.hwnd, rc, app.render.dpi); break;
        case 4: launcher_delete_selected(app.state); break;
        case 5: {
            if (hit >= 0) {
                const auto& items = app.state.groups[app.state.launcher.group].items;
                const std::wstring& t = items[app.state.launcher.filtered[hit]].target;
                ::ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                (L"/select," + t).c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        default: break;
    }
    ::InvalidateRect(app.hwnd, nullptr, FALSE);
}
```

“移动到分组”不用菜单（第二层菜单或输入序号都别扭）：**实现方式是按住条目拖到目标标签上松开**，见 Step 3。

命中测试用客户区坐标、弹菜单用屏幕坐标，因此签名带两个点，不要在同名参数上混淆：

```cpp
// launcher.h
void launcher_context_menu(App& app, POINT screen_pt, POINT client_pt);
```

实现里 `launcher_hittest(app.state, rc, client_pt)` 用 `client_pt`，`TrackPopupMenu` 用 `screen_pt`。

- [ ] **Step 1: 实现删除、移动与分组增删**

在 `src/views/launcher.cpp` 追加：

```cpp
void launcher_delete_selected(AppState& s) {
    LauncherState& ls = s.launcher;
    if (ls.sel < 0 || ls.sel >= static_cast<int>(ls.filtered.size())) return;
    if (s.groups.empty()) return;
    auto& items = s.groups[ls.group].items;
    items.erase(items.begin() + ls.filtered[ls.sel]);
    s.data_dirty = true;  // 只删引用，不碰磁盘上的文件
    launcher_refilter(s);
}

void launcher_add_group(AppState& s) {
    // 连续编号命名，避免为了一个新分组先弹输入框
    int n = static_cast<int>(s.groups.size()) + 1;
    std::wstring name = L"新分组 " + std::to_wstring(n);
    while (std::any_of(s.groups.begin(), s.groups.end(),
                       [&](const LaunchGroup& g) { return g.name == name; })) {
        name = L"新分组 " + std::to_wstring(++n);
    }
    s.groups.push_back(LaunchGroup{ name, {} });
    s.launcher.group = static_cast<int>(s.groups.size()) - 1;
    s.launcher.sel = -1;
    s.data_dirty = true;
    launcher_refilter(s);
}

void launcher_move_item_to_group(AppState& s, int filtered_index, int group_index) {
    if (filtered_index < 0 || filtered_index >= static_cast<int>(s.launcher.filtered.size())) return;
    if (group_index < 0 || group_index >= static_cast<int>(s.groups.size())) return;
    if (group_index == s.launcher.group) return;

    auto& src = s.groups[s.launcher.group].items;
    const int raw = s.launcher.filtered[filtered_index];
    LaunchItem moved = src[raw];
    src.erase(src.begin() + raw);
    s.groups[group_index].items.push_back(std::move(moved));
    s.data_dirty = true;
    launcher_refilter(s);
}
```

- [ ] **Step 2: 实现重命名与新建（连续 InlineEdit）**

`src/views/launcher_edit.cpp`：

```cpp
#include "views/launcher.h"

namespace sg {

void launcher_begin_rename(App& app, HWND parent, const RECT& client, float dpi) {
    AppState& s = app.state;
    LauncherState& ls = s.launcher;
    if (ls.sel < 0 || ls.sel >= static_cast<int>(ls.filtered.size())) return;

    // 输入框叠在被改的那个格子上；坐标算法与渲染共用同一份
    const D2D1_RECT_F cr = launcher_cell_rect(s, client, ls.sel);
    const RECT rc{ static_cast<LONG>(cr.left), static_cast<LONG>(cr.top), static_cast<LONG>(cr.right),
                   static_cast<LONG>(cr.top + 28.f) };

    const int raw = ls.filtered[ls.sel];
    ls.search.close();
    const std::wstring current = s.groups[ls.group].items[raw].name;
    ls.search.open(parent, rc, current, dpi,
                   [&s, raw](const std::wstring& t) {
                       if (!t.empty()) {
                           s.groups[s.launcher.group].items[raw].name = t;
                           s.data_dirty = true;
                       }
                       launcher_refilter(s);
                   },
                   nullptr);
}

void launcher_begin_new_item(App& app, HWND parent, const RECT& client, float dpi) {
    AppState& s = app.state;
    LauncherState& ls = s.launcher;
    if (s.groups.empty()) s.groups.push_back(LaunchGroup{ L"常用", {} });

    const D2D1_RECT_F sr = D2D1::RectF(kPad, kPad + kTabsH, static_cast<float>(client.right) - kPad,
                                       kPad + kTabsH + kSearchH);
    const RECT rc{ static_cast<LONG>(sr.left + 6.f), static_cast<LONG>(sr.top + 5.f),
                   static_cast<LONG>(sr.right - 6.f), static_cast<LONG>(sr.bottom - 5.f) };

    // 两步输入：先名称，回车后再输目标。比自建对话框少写一百多行，且与搜索框行为一致
    auto pending = std::make_shared<std::wstring>();
    ls.search.close();
    ls.search.open(parent, rc, L"", dpi,
                   [&app, parent, rc, dpi, pending](const std::wstring& name) {
                       if (name.empty()) return;
                       *pending = name;
                       app.state.launcher.search.open(
                           parent, rc, L"", dpi,
                           [&app, pending](const std::wstring& target) {
                               if (target.empty()) return;
                               const int gi = app.state.launcher.group;
                               app.state.groups[gi].items.push_back(
                                   item_from_path_keep_name(target, *pending));
                               app.state.data_dirty = true;
                               launcher_refilter(app.state);
                               ::InvalidateRect(app.hwnd, nullptr, FALSE);
                           },
                           nullptr);
                   },
                   nullptr);
}

}  // namespace sg
```

`item_from_path_keep_name` 是 `launch.h` 里的一个小助手（新建时用户已经输了名字，不能被文件名的自动推导覆盖），加声明与实现：

```cpp
// launch.h
LaunchItem item_from_path_keep_name(const std::wstring& path, const std::wstring& name);

// launch.cpp
LaunchItem item_from_path_keep_name(const std::wstring& path, const std::wstring& name) {
    LaunchItem item = item_from_path(path);
    if (!name.empty()) item.name = name;
    return item;
}
```

`launcher_cell_rect` 原本是 `launcher.cpp` 匿名命名空间里的 `cell_rect`，重命名流程需要同一套坐标算法，因此提升为公开函数：

```cpp
// launcher.h 追加
D2D1_RECT_F launcher_cell_rect(const AppState& s, const RECT& client, int filtered_index);
```

并把 `src/views/launcher.cpp` 里的 `cell_rect` 改名为 `launcher_cell_rect` 并移出匿名命名空间，`launcher_begin_rename` 改用它计算矩形。

- [ ] **Step 3: 内部拖拽到分组标签**

在 `app_wndproc` 里加一个只用于内部拖拽的鼠标状态机（不走 OLE）：

```cpp
        case WM_LBUTTONDOWN: {
            if (!app) return 0;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int tab = launcher_tab_hittest(app->state, rc, pt);
            if (tab >= 0) {
                app->state.launcher.group = tab;
                app->state.launcher.sel = -1;
                app->state.launcher.scroll = 0;
                launcher_refilter(app->state);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int hit = launcher_hittest(app->state, rc, pt);
            if (hit >= 0) {
                app->state.launcher.sel = hit;
                app->internal_drag = true;   // 先按下，等移动超阈值才开始真拖
                app->drag_start = pt;
                app->drag_from = hit;
                ::SetCapture(hwnd);
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (app && app->internal_drag) {
                const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const int over_tab = launcher_tab_hittest(app->state, rc, pt);
                if (over_tab != app->state.launcher.drag_over_tab) {
                    app->state.launcher.drag_over_tab = over_tab;
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;  // 否则走悬停高亮逻辑
        case WM_LBUTTONUP:
            if (app && app->internal_drag) {
                app->internal_drag = false;
                ::ReleaseCapture();
                if (app->state.launcher.drag_over_tab >= 0) {
                    launcher_move_item_to_group(app->state, app->drag_from,
                                                app->state.launcher.drag_over_tab);
                }
                app->state.launcher.drag_over_tab = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
```

`App` 结构体加成员：

```cpp
    bool internal_drag = false;
    bool mouse_tracking = false;
    POINT drag_start{};
    int drag_from = -1;
```

`drag_over_tab` 放在 `LauncherState`（Task 9 已加）而不是 `App`，因为渲染需要读它：`launcher_render` 的标签循环里，当 `i == ls.drag_over_tab` 时用 `theme.hover` 而不是 `theme.card` 画底。`drag_start` / `drag_from` 只有 app 层用，留在 `App`。

“移动超阈值才开始真拖”：本任务简化为按下即进入拖拽态（阈值判断只影响观感，不影响功能），若需要阈值，在 `WM_MOUSEMOVE` 里加 `if (abs(pt.x - drag_start.x) + abs(pt.y - drag_start.y) < GetSystemMetrics(SM_CXDRAG)) return 0;`。

- [ ] **Step 4: 接右键菜单到 app**

`app_wndproc` 加：

```cpp
        case WM_CONTEXTMENU: {
            if (!app) return 0;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) {  // 键盘唤出菜单
                RECT rc{};
                ::GetWindowRect(hwnd, &rc);
                pt.x = rc.left + 40;
                pt.y = rc.top + 40;
            }
            POINT client = pt;
            ::ScreenToClient(hwnd, &client);
            launcher_context_menu(*app, pt, client);
            return 0;
        }
```

`launcher_context_menu` 必须用**客户区坐标**做命中测试、用**屏幕坐标**弹菜单。因此签名改为 `launcher_context_menu(App& app, POINT screen_pt, POINT client_pt)`，内部命中测试用 `client_pt`。

- [ ] **Step 5: 构建并手工验收**

```powershell
cmake --build build --config Release --target stargazer
.\build\Release\stargazer.exe
```

验收：

1. 右键网格空白处：菜单里“重命名/删除/打开所在位置”为灰。“新建条目”可用。
2. “新建条目”→ 出现输入框，输名称回车 → 再输目标（例如 `C:\Windows\system32\mspaint.exe`）回车 → 新条目出现且图标正确。
3. 右键条目 → 重命名：输入框叠在该格子上，回车后名字变了。
4. 右键条目 → 删除：条目消失，且 `C:\Windows\system32\mspaint.exe` 文件仍在（永久不会删文件）。
5. 按 F2 重命名、按 Del 删除。
6. 按住条目拖到另一个分组标签上松开：条目换组，标签有高亮提示。
7. 重新呼出后所有变更仍在（Task 12 接入落盘后再验一次）。

- [ ] **Step 6: 提交**

```powershell
git add src/views/ CMakeLists.txt src/app.h src/app.cpp src/launch.h src/launch.cpp
git commit -m "feat(launcher): 右键菜单、条目与分组管理、拖拽换组"
```

---

### Task 12: 收尾（ui.txt 记忆、落盘、README、全量验收）

**Files:**
- Create: `README.md`
- Modify: `src/app.cpp`（数据加载与落盘、ui.txt）
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `sg::parse_launcher` / `serialize_launcher` / `parse_config` / `serialize_config`（Task 3）、`sg::load_text` / `save_text`（Task 4）
- Produces:
  - `void sg::app_load(App&)` — 读 `launcher.txt` / `config.txt` / `ui.txt` 到 `AppState`
  - `void sg::app_save_if_dirty(App&)` — `data_dirty` 时落盘并清标志

- [ ] **Step 1: 实现加载与落盘**

在 `app.cpp` 加：

```cpp
void app_load(App& app) {
    AppState& s = app.state;
    std::wstring text;

    if (load_text(app.paths, L"launcher.txt", text)) {
        int bad = 0;
        s.groups = parse_launcher(text, bad);
        s.bad_lines += bad;
    }
    if (s.groups.empty()) {
        // 首次运行给一个能直接用的分组，而不是空白界面
        s.groups.push_back(LaunchGroup{ L"常用", {} });
    }

    if (load_text(app.paths, L"config.txt", text)) {
        int bad = 0;
        s.config = parse_config(text, bad);
        s.bad_lines += bad;
    }

    if (load_text(app.paths, L"ui.txt", text)) {
        int bad = 0;
        const Config ui = parse_config(text, bad);
        const std::wstring v = config_get(ui, L"group", L"");
        // 只接受存在于当前数据里的分组名，防止手改后越界
        for (size_t i = 0; i < s.groups.size(); ++i) {
            if (s.groups[i].name == v) {
                s.launcher.group = static_cast<int>(i);
                break;
            }
        }
        const std::wstring w = config_get(ui, L"w", L"");
        const std::wstring h = config_get(ui, L"h", L"");
        if (!w.empty() && !h.empty()) {
            ::SetWindowPos(app.hwnd, nullptr, 0, 0, _wtoi(w.c_str()), _wtoi(h.c_str()),
                           SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    launcher_refilter(s);
}

void app_save_if_dirty(App& app) {
    AppState& s = app.state;
    if (!s.data_dirty) return;
    s.data_dirty = false;
    if (!save_text(app.paths, L"launcher.txt", serialize_launcher(s.groups))) {
        ::MessageBoxW(app.hwnd, L"保存失败：程序目录可能已变为不可写。", L"Stargazer", MB_ICONWARNING);
    }
}

void app_save_ui(App& app) {
    RECT rc{};
    ::GetWindowRect(app.hwnd, &rc);
    Config ui;
    config_set(ui, L"w", std::to_wstring(rc.right - rc.left));
    config_set(ui, L"h", std::to_wstring(rc.bottom - rc.top));
    if (!app.state.groups.empty()) {
        config_set(ui, L"group", app.state.groups[app.state.launcher.group].name);
    }
    save_text(app.paths, L"ui.txt", serialize_config(ui));
}
```

`main.cpp` 在 `app_init` 之后调 `sg::app_load(app);`；在消息循环后、`app_shutdown` 之前调 `sg::app_save_if_dirty(app); sg::app_save_ui(app);`。同时监听窗口尺寸变化以便记住尺寸：`app.cpp` 的 `WM_EXITSIZEMOVE` 里调 `app_save_ui(app)`。

在 `app_hide` 里也调一次 `app_save_if_dirty(app)`：用户改完就切换窗口是很自然的操作，落盘不能等退出。

- [ ] **Step 2: 崩溃日志与位置记忆的取舍**

`SetUnhandledExceptionFilter` 把崩溃写到 exe 目录——便携程序没有系统事件日志可用：

```cpp
static std::wstring g_crash_log;

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    if (!g_crash_log.empty()) {
        HANDLE h = ::CreateFileW(g_crash_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t line[256] = {};
            ::swprintf_s(line, L"[%04d-%02d-%02d %02d:%02d:%02d] 异常码 0x%08X 地址 %p\r\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         info ? info->ExceptionRecord->ExceptionCode : 0u,
                         info ? info->ExceptionRecord->ExceptionAddress : nullptr);
            DWORD written = 0;
            ::WriteFile(h, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written,
                        nullptr);
            ::CloseHandle(h);
        }
    }
    // 写日志后直接结束进程：恢复后的进程状态不可信，继续跑只会损坏数据文件
    return EXCEPTION_EXECUTE_HANDLER;
}
```

在 `app_init` 里（`init_paths` 成功之后）加：

```cpp
    g_crash_log = join_path(app.paths.exe_dir, L"crash.log");
    ::SetUnhandledExceptionFilter(crash_filter);
```

日志是 UTF-16 无 BOM（`WriteFile` 直接写宽字符），记事本能打开。写不进去就静默跳过——崩溃路径上不能再抛异常。

**位置记忆的取舍**：spec §5 要求“呼出时居中于鼠标所在显示器”，§10 又列了“窗口位置尺寸”要记忆，两条互相冲突。本计划的取舍是**只记忆尺寸，不记忆位置**：热键呼出的窗口出现在鼠标所在那块屏才符合“就近操作”的用途；若记住上次位置，在另一块屏幕上按热键会在别的屏上弹出窗口。同理不存“上次视图”（阶段 1 只有一个视图）。

- [ ] **Step 3: 写 README.md**

```markdown
# Stargazer

便携式 Windows 桌面效率工具。当前已实现快捷启动板。

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target stargazer
```

产物为 `build\Release\stargazer.exe`，单个文件，静态 CRT，不依赖任何运行时。

## 测试

```powershell
cmake --build build --config Release --target test_model test_io
.\build\Release\test_model.exe
.\build\Release\test_io.exe
```

## 使用

- `Ctrl+Shift+Space` 呼出/隐藏，`Esc` 隐藏
- 呼出后光标在搜索框，直接打字即搜索
- `↓` 进入网格，方向键移动，`Enter` 启动，`Del` 删除条目
- 从桌面或资源管理器拖入文件/快捷方式即添加
- 按住条目拖到分组标签上松开即换组

## 数据

全部数据在 exe 同级的 `data\` 目录下，纯文本，可直接手改：

| 文件 | 内容 |
|---|---|
| `launcher.txt` | `分组 \t 名称 \t 目标 \t 参数 \t 工作目录 \t 图标` |
| `config.txt` | `键 \t 值` |
| `ui.txt` | 窗口尺寸与上次分组 |

UTF-8 编码，行内 Tab 分隔，字段里的 `\` `Tab` `换行` 分别写作 `\\` `\t` `\n`。格式不对的行会被跳过并在托盘提示。

开机自启的真实状态在注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `stargazer` 值，不在 `config.txt`。程序被移动到别的目录后，下次启动会自动修正该路径。

## 设计取舍

- 不做磁盘图标缓存（进程内 LRU 300 项，约 2.7 MB）
- 图标统一按 48×48 提取后缩放
- 不自绘文本控件，编辑一律用原生 `EDIT`
- 窗口隐藏后不跑任何定时器，空闲 CPU 为 0
```

- [ ] **Step 4: 跑完整验收清单**

先清掉 `data\` 目录模拟首次运行：

```powershell
Remove-Item -Recurse -Force .\build\Release\data -ErrorAction SilentlyContinue
```

依次确认：

| # | проверка | 期望 |
|---|---|---|
| 1 | 启动到窗口出现 | 冷启动到可用 < 100 ms（用 Process Explorer 或秒表粗测） |
| 2 | 隐藏窗口后的工作集 | 任务管理器 ≤ 5 MB |
| 3 | 呼出并载入 60 个图标后 | ≤ 15 MB |
| 4 | 全部隐藏后的 CPU | 任务管理器显示 0%，长时间不波动 |
| 5 | 把 `data\launcher.txt` 中一行改成 `只有两个\t字段` | 该行被跳过，其余条目正常；托盘或弹窗给出坏行提示 |
| 6 | 把 exe 连同 `data\` 拷到另一个目录后启动 | 数据跟着走，图标与条目都在 |
| 7 | 把 exe 放到 `C:\Program Files\` 下运行 | 弹出“需要可写目录”提示并退出，不静默丢数据 |
| 8 | 勾选开机自启后把 exe 移到新目录并重启程序 | 注册表里的路径被自动改写为新区径 |
| 9 | 已运行时再双击 exe | 唤出已有窗口，只有一个进程 |
| 10 | `--autostart` 启动 | 不弹窗，只出现在托盘 |
| 11 | 全屏应用（游戏/视频）前呼出 | 窗口出现在鼠标所在显示器，不越界不压任务栏 |
| 12 | 150% 缩放的显示器上呼出 | 文字与图标不模糊（D2D 按 DPI 缩放） |

- [ ] **Step 5: 提交**

```powershell
git add README.md src/app.cpp src/app.h src/main.cpp
git commit -m "feat(app): 数据加载与落盘、ui.txt 记忆、README 与验收清单"
```

---

## 阶段 1 完成后的状态

- `stargazer.exe` 是一个可日常使用的热键启动器
- `data\` 下的四个文本文件是全部状态，可手工编辑、可随 exe 搬家
- `model/` 与 `text_io`/`persist` 已有 assert 测试覆盖；GUI 层靠本文档的验收清单
- 阶段 2（Box）、3（Todo）、4（Explorer）各自另写计划，它们复用本阶段定下的 `AppState`、`Renderer`、`icons_get`、`InlineEdit`、`dragdrop` 与行格式存储

## 已知待办（不在本阶段范围）

- 拼音首字母搜索（需拼音表）
- 磁盘图标缓存（需引入 PNG 编码）
- 鼠标中键呼出（需 `WH_MOUSE_LL` 全局钩子）
- 呼出时的平滑动画
- 项目图标（现在用系统默认图标 `IDI_APPLICATION`，需加 `.rc` 资源文件）

