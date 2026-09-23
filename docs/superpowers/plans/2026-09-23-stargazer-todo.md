# Stargazer 待办（Todo）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 Stargazer 加上“随手一记”的待办视图：`Ctrl+V` 粘文字 / 链接 / 图片（或顺手拖入），类型自动判定，图片有缩略图预览，勾掉的沉底并可一键清空。

**Architecture:** 沿用现有单进程单窗口 + `AppState` + 视图标签行。待办只新增三样东西：①纯逻辑的模型（6 字段 `todo.txt` + 类型判定 + 副本归属）②一个独立的 Shell 缩略图模块（照搬 `icons.cpp` 的“工作线程取像素 → UI 建位图 → LRU”范式，`WM_PAINT` 里绝不调 Shell）③一个视图 `views/todo`。图片落盘与存在性校验复用线程 B（`fs_work`），剪贴板复用 `clipboard`，行格式复用 `model/rowformat`。

**Tech Stack:** 沿用：C++20、Win32、Direct2D 1.1 + DirectWrite、CMake + MSVC、零第三方依赖、`/MT`。图片编码用系统自带的 WIC（`windowscodecs.lib`），缩略图用 `IShellItemImageFactory`。

**Spec:** `docs/superpowers/specs/2026-09-23-stargazer-todo-design.md`（本计划实现它；其中一条顺序在计划里被修正，见 Task 6 的 Ruling 提示）

## Global Constraints

（与阶段 1/2 相同，逐条照旧，别重新发明）
- C++20；`/W4 /utf-8 /permissive-`；Release `/O2`；静态 CRT `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`
- 零第三方依赖；只链接系统库（现在还要加 `windowscodecs`）
- **`src/model/` 下不得出现任何 Windows 头**，只用 STL（这条让数据层可被控制台测试）
- 数据文件 UTF-8 无 BOM，Tab 分隔，字段内 `|` `Tab` `换行` 写作 `||` `|t` `|n`
- 坐标约定：D2D 用逻辑 DIP（`Renderer::client_logical()`），鼠标 lParam 与子 HWND 用物理像素，**换算只能经过 `Renderer::to_logical()` / `to_physical()`**
- 空闲时不得有定时器/轮询；窗口隐藏时不重绘
- 便携：数据只在 `<exe>\data\`；注册表是 autostart 的唯一真相
- 鼠标消息在本机可能被真实用户操作干扰，**自动化验证一律走键盘/程序化路径**；探针脚本必须纯 ASCII（PowerShell 5.1 会把 BOM-less .ps1 按 GBK 误读，中文字面量已被坑过三次）
- 工具链（阶段 1 账本裁决，照旧）：`CMAKE = 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'`，生成器 `"Visual Studio 18 2026"`，`-DCMAKE_GENERATOR_INSTANCE="D:\Program Files\Microsoft Visual Studio\18\Community,version=18.0.0.0"`
- 每个任务的构建命令都是：
  `& $CMAKE --build build --config Release --target test_model test_io test_layout stargazer`

## Review Focus

待办最可能伤到使用者的六类输入/故障。每条都要落到宿主任务的测试或验收步骤里：

1. **粘进来的是哪种位图**：截图工具给的可能是 32bpp（带/不带 alpha）、24bpp、行序自下而上、头是 `BITMAPINFOHEADER` 或 `BITMAPV5HEADER` —— 落盘的 PNG 必须是所见即所得（不能黑块、不能上下颠倒、不能负片）。（Task 2 用 4 种组合钉死）
2. **剪贴板里同时有文本与位图**（Excel 复制单元格、Word 复制图文）—— 必须记成**文字**，不能把单元格变成一张图。（Task 6 的判定测试 + 一条“同时存在取文本”的用例）
3. **同一张图被两条条目引用** —— 删掉其中一条，另一条的图片必须还在（副本清理必须问“还有别的引用吗”，不是问“路径在 data\images 下吗”）。（Task 1 的纯函数测试 + Task 5 的验收）
4. **引用型图片在外部被改名/删除/移走** —— 该行灰显 + 删除线 + 文案，其余条目照常可用，界面不冻结（校验走线程 B、结果按 path 回填）。（Task 7 验收）
5. **中文/emoji/多行文本** —— 存进 `todo.txt` 往返一字不差（`|t` `|n` 转义），渲染不崩、不把多行撑破行高。（Task 1 往返测试 + Task 4 的 ASCII 验收）
6. **边界态**：空列表、全部已完成、几千条（滚动/命中不能错位、内存不爆）、`data\images\` 被手删 —— 退化成“图片已不存在”而不是崩溃。（Task 3 的纯布局测试 + Task 7 验收）

---

### Task 1: 模型层（6 字段 todo + 类型判定 + 排序 + 副本归属）

**Files:**
- Create: `src/model/todo_kind.h`、`src/model/todo_kind.cpp`
- Modify: `src/model/store.h`、`src/model/store.cpp`
- Test: `tests/test_model.cpp`
- Modify: `CMakeLists.txt`（`MODEL_SOURCES` 加 `src/model/todo_kind.cpp`）

**Interfaces:**
- Produces（`model/todo_kind.h`，纯 STL）:
  ```cpp
  namespace sg {
  enum class TodoKind { Text, Link, Image };
  std::wstring todo_kind_to_string(TodoKind k);      // L"text" / L"link" / L"image"
  TodoKind todo_kind_from_string(const std::wstring& s);  // 未知值 → Text（宽松，不丢内容）
  // 去掉首尾空白后以 http:// 或 https:// 开头（大小写不敏感）→ Link
  TodoKind todo_kind_from_text(const std::wstring& text);
  // 扩展名白名单：png jpg jpeg gif bmp webp ico tif tiff（大小写不敏感）
  bool is_image_path(const std::wstring& path);
  // path 是否位于 images_dir 下（大小写不敏感的前缀比较，且必须是 images_dir + 分隔符 开头）
  bool todo_is_owned_copy(const std::wstring& images_dir, const std::wstring& path);
  // 该副本是否还被别的条目引用（other_attaches = 除待删条目外其余条目的 attach）
  bool todo_copy_still_used(const std::vector<std::wstring>& other_attaches,
                            const std::wstring& path);
  }
  ```
- Produces（`model/store.h`）:
  ```cpp
  struct TodoItem {
      long long id = 0;
      bool done = false;
      long long created = 0;   // Unix 秒
      TodoKind kind = TodoKind::Text;
      std::wstring text;       // 文字内容；link 时是 URL；image 时可为空
      std::wstring attach;     // image 时的图片来源路径，其余为空
  };
  // 行格式：id \t done \t created \t kind \t text \t attach
  std::wstring serialize_todos(const std::vector<TodoItem>& todos);
  std::vector<TodoItem> parse_todos(const std::wstring& text, int& bad);
  // 未完成在前 → created 降序 → id 降序（同秒连记多条不会乱跳）
  void sort_todos(std::vector<TodoItem>& todos);
  long long next_todo_id(const std::vector<TodoItem>& todos);   // 空列表返回 1（已存在）
  ```
- Consumes: `model/rowformat.h` 的 `parse_rows` / `build_text`（已有）

- [ ] **Step 1: 写失败的测试**

在 `tests/test_model.cpp` 的 `test_todos_roundtrip_and_sort()` 之后新增四个测试，并在 `main()` 里按顺序加调用（`test_todo_kind`、`test_todo_roundtrip`、`test_todo_sort`、`test_todo_copy_ownership`）：

```cpp
// Review Focus 2/5：类型判定与转义往返
static void test_todo_kind() {
    CHECK(sg::todo_kind_from_text(L"https://example.com") == sg::TodoKind::Link);
    CHECK(sg::todo_kind_from_text(L"  HTTP://EXAMPLE.COM  ") == sg::TodoKind::Link);  // 空白 + 大小写
    CHECK(sg::todo_kind_from_text(L"http://") == sg::TodoKind::Link);
    CHECK(sg::todo_kind_from_text(L"www.example.com") == sg::TodoKind::Text);   // 不是链接
    CHECK(sg::todo_kind_from_text(L"ftp://x") == sg::TodoKind::Text);           // 不是链接
    CHECK(sg::todo_kind_from_text(L"买牛奶\n第二行") == sg::TodoKind::Text);
    CHECK(sg::todo_kind_from_text(L"") == sg::TodoKind::Text);

    CHECK(sg::is_image_path(L"C:\\a\\b.PNG"));
    CHECK(sg::is_image_path(L"D:/x/y.jpeg"));
    CHECK(sg::is_image_path(L"shot.webp"));
    CHECK(!sg::is_image_path(L"C:\\a\\b.txt"));
    CHECK(!sg::is_image_path(L"C:\\a\\b"));           // 无扩展名
    CHECK(!sg::is_image_path(L"C:\\pics\\"));         // 目录

    // 未知 kind 值 → Text（宽松原则）
    CHECK(sg::todo_kind_from_string(L"foo") == sg::TodoKind::Text);
    CHECK(sg::todo_kind_from_string(L"image") == sg::TodoKind::Image);
    CHECK_EQ(sg::todo_kind_to_string(sg::TodoKind::Link), std::wstring(L"link"));
}

static void test_todo_roundtrip() {
    std::vector<sg::TodoItem> todos(3);
    todos[0].id = 1; todos[0].created = 100; todos[0].kind = sg::TodoKind::Text;
    todos[0].text = L"买牛奶 |t 与 |n 与 ||";   // 转义三件套
    todos[1].id = 2; todos[1].created = 200; todos[1].kind = sg::TodoKind::Link;
    todos[1].text = L"https://example.com/a?b=1";
    todos[2].id = 3; todos[2].created = 300; todos[2].done = true;
    todos[2].kind = sg::TodoKind::Image;
    todos[2].attach = L"C:\\Users\\wjr\\AppData\\Local\\Temp\\sg\\images\\3.png";

    int bad = 0;
    auto back = sg::parse_todos(sg::serialize_todos(todos), bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(back.size(), size_t{3});
    CHECK_EQ(back[0].text, std::wstring(L"买牛奶 |t 与 |n 与 ||"));
    CHECK(back[1].kind == sg::TodoKind::Link);
    CHECK(back[2].kind == sg::TodoKind::Image);
    CHECK(back[2].done);
    CHECK_EQ(back[2].attach, todos[2].attach);

    // 空 kind + 空 text + 空 attach 的行 = 坏行（不产生幽灵条目）
    auto back2 = sg::parse_todos(L"9\t0\t1\t\t\t\n", bad);
    CHECK_EQ(bad, 1);
    CHECK_EQ(back2.size(), size_t{0});
    // 字段数不对的行也是坏行
    auto back3 = sg::parse_todos(L"9\t0\t1\ttext\n", bad);
    CHECK_EQ(bad, 2);
    CHECK_EQ(back3.size(), size_t{0});
    // 非数字的 created（手改）→ 当作 0，不崩、不丢条目
    auto back4 = sg::parse_todos(L"9\t0\tabc\ttext\t\u4e2d\u6587\t\n", bad);
    CHECK_EQ(back4.size(), size_t{1});
    CHECK_EQ(back4[0].created, 0LL);
    CHECK_EQ(back4[0].text, std::wstring(L"\u4e2d\u6587"));
}

static void test_todo_sort() {
    std::vector<sg::TodoItem> t(4);
    t[0].id = 1; t[0].created = 100; t[0].text = L"old";
    t[1].id = 2; t[1].created = 300; t[1].text = L"new";
    t[2].id = 3; t[2].created = 200; t[2].text = L"mid";
    t[3].id = 4; t[3].created = 999; t[3].done = true; t[3].text = L"done-newest";
    sg::sort_todos(t);
    CHECK_EQ(t[0].text, std::wstring(L"new"));
    CHECK_EQ(t[1].text, std::wstring(L"mid"));
    CHECK_EQ(t[2].text, std::wstring(L"old"));
    CHECK_EQ(t[3].text, std::wstring(L"done-newest"));   // 已完成沉底，哪怕 created 最大

    // 同一秒连记多条：id 大的在前（稳定，不随排序实现漂移）
    std::vector<sg::TodoItem> same(3);
    for (int i = 0; i < 3; ++i) { same[i].id = i + 1; same[i].created = 500; }
    sg::sort_todos(same);
    CHECK_EQ(same[0].id, 3);
    CHECK_EQ(same[1].id, 2);
    CHECK_EQ(same[2].id, 1);

    std::vector<sg::TodoItem> empty;
    sg::sort_todos(empty);              // 空列表不崩
    CHECK_EQ(sg::next_todo_id(empty), 1LL);
}

// Review Focus 3：副本归属与“还有别的引用吗”
static void test_todo_copy_ownership() {
    const std::wstring images = L"D:\\Tools\\stargazer\\data\\images";
    CHECK(sg::todo_is_owned_copy(images, L"D:\\Tools\\stargazer\\data\\images\\7.png"));
    CHECK(sg::todo_is_owned_copy(images, L"d:\\tools\\stargazer\\DATA\\IMAGES\\7.png"));  // 大小写
    CHECK(!sg::todo_is_owned_copy(images, L"D:\\Tools\\stargazer\\data\\images2\\7.png")); // 前缀不算
    CHECK(!sg::todo_is_owned_copy(images, L"D:\\Tools\\other\\7.png"));
    CHECK(!sg::todo_is_owned_copy(images, L"D:\\Tools\\stargazer\\data\\images"));          // 目录本身
    CHECK(!sg::todo_is_owned_copy(images, L""));

    const std::wstring p = L"D:\\Tools\\stargazer\\data\\images\\7.png";
    CHECK(sg::todo_copy_still_used({ p, L"C:\\other.png" }, p));        // 还有别人在用
    CHECK(sg::todo_copy_still_used({ L"D:\\TOOLS\\STARGAZER\\DATA\\IMAGES\\7.PNG" }, p));  // 大小写
    CHECK(!sg::todo_copy_still_used({ L"C:\\other.png" }, p));          // 没人用了
    CHECK(!sg::todo_copy_still_used({}, p));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `& $CMAKE --build build --config Release --target test_model`
Expected: 编译失败，`error C2039: "TodoKind": 不是 "sg" 的成员` 或 `LNK2019: 无法解析的外部符号 sg::todo_kind_from_text`（RED 应当是“符号不存在”，不是断言失败）

- [ ] **Step 3: 写 `src/model/todo_kind.h` / `.cpp`**

`todo_kind.h`：把 Interfaces 里的声明原样抄进去（`#include <string>`、`#include <vector>`）。
`todo_kind.cpp`：实现要点 ——
- `todo_kind_from_text`：先跳过首尾空白（`iswspace`），再用**不区分大小写**的比较判断 `http://` / `https://` 前缀。
- `is_image_path`：取 `file_name(path)` 的 `extension_of`，转小写后和白名单比；`extension_of` 已存在于 `model/paths.h`（无扩展名/以点开头返回空）。
- `todo_is_owned_copy`：把 `images_dir` 与 `path` 都做 `normalize_key()`（`model/paths.h` 已有：小写、`/`→`\\`、去尾分隔符），然后要求 `path` 以 `images_dir + L"\\"` 开头且长度更大（**前缀 + 分隔符**，否则 `images2\` 会被误判）。
- `todo_copy_still_used`：对 `other_attaches` 做 `normalize_key` 比较，任一相等即返回 true。

- [ ] **Step 4: 改 `src/model/store.h` / `.cpp` 到新模型**

`store.h`：删掉 `TodoItem` 的 `due`/`prio`，加上 `kind`/`attach`（`#include "model/todo_kind.h"`），并按 Interfaces 更新四个声明。
`store.cpp`：
- `serialize_todos`：每行 6 个字段，`kind` 用 `todo_kind_to_string`，`created` 用 `std::to_wstring`。
- `parse_todos`：`parse_rows(text, 6, bad)`；`kind` 空且 `text` 空且 `attach` 空 → `++bad; continue;`（占位/坏行）；其余字段按现状解析，`kind` 用 `todo_kind_from_string`（未知值 → Text）。
- `sort_todos`：`std::stable_sort`，比较器先比 `done`（未完成在前），再比 `created` 降序，再比 `id` 降序。
- `next_todo_id`：保持不变（已有实现）。
- `to_ll` 已存在，复用。

- [ ] **Step 5: 跑测试确认通过**

Run: `.\build\Release\test_model.exe`
Expected: `OK: test_model 全部通过`
（若有 FAIL：`kind` 未知值那条容易被写成“跳过”，注意宽松原则是**按 Text 收下**，不是丢弃。）

- [ ] **Step 6: 跑另外两个测试目标确认没被带坏**

Run: `& $CMAKE --build build --config Release --target test_io test_layout stargazer; .\build\Release\test_io.exe; .\build\Release\test_layout.exe`
Expected: 两个都 `OK`（`store.cpp` 是 `test_io` 与 `stargazer` 的公共依赖，现在还没人用新字段）

- [ ] **Step 7: 提交**

```powershell
git add src/model/todo_kind.h src/model/todo_kind.cpp src/model/store.h src/model/store.cpp tests/test_model.cpp CMakeLists.txt
git commit -m "feat(todo): 六字段待办模型（kind/attach）、类型判定、稳定排序与副本归属"
```

---

### Task 2: 剪贴板位图读取 + DIB→PNG（WIC）

**Files:**
- Create: `src/png.h`、`src/png.cpp`
- Modify: `src/clipboard.h`、`src/clipboard.cpp`
- Test: `tests/test_io.cpp`
- Modify: `CMakeLists.txt`（`test_io` 与 `SG_SOURCES` 加 `src/png.cpp`；两者链接加 `windowscodecs`）

**Interfaces:**
- Produces（`src/png.h`）:
  ```cpp
  namespace sg {
  // 把一段 DIB 编码成 PNG 文件。dib = BITMAPINFOHEADER/BITMAPV5HEADER 起、后跟像素数据
  // （32bpp 或 24bpp；行序自下而上或自上而下由头里的 biHeight 正负决定）。
  // 失败返回 false 并填 error（中文，可直接进托盘气泡）。
  bool png_encode_dib(const std::wstring& path, const std::vector<uint8_t>& dib,
                      std::wstring& error);
  }
  ```
- Produces（`src/clipboard.h` 追加）:
  ```cpp
  // 读剪贴板里的位图（CF_DIBV5 优先，退回 CF_DIB），拿到 DIB 原始字节；没有位图返回 false
  bool clipboard_get_image_dib(std::vector<uint8_t>& out);
  // 读剪贴板里的纯文本（CF_UNICODETEXT）；没有文本返回 false
  bool clipboard_get_text(std::wstring& out);
  ```
- Consumes: `clipboard.cpp` 已能读 `CF_HDROP`/文本；`png.cpp` 用 WIC（`CoCreateInstance(CLSID_WICImagingFactory)`，UI 线程或工作线程都可，调用方负责已初始化 COM）

- [ ] **Step 1: 写失败的测试**

在 `tests/test_io.cpp` 加两个测试（并加 `#include "png.h"`、`#include <wincodec.h>`、在 `main()` 里调用）：

```cpp
// 造一小段 DIB（自下而上），编码成 PNG，再用 WIC 解回来验证尺寸与像素
static std::vector<uint8_t> make_dib(int w, int h, int bpp, bool top_down) {
    const int stride = ((w * bpp + 31) / 32) * 4;
    std::vector<uint8_t> dib(sizeof(BITMAPINFOHEADER) + size_t(stride) * h, 0);
    auto* bi = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = w;
    bi->biHeight = top_down ? -h : h;   // 负 = 自上而下
    bi->biPlanes = 1;
    bi->biBitCount = static_cast<WORD>(bpp);
    bi->biCompression = BI_RGB;
    uint8_t* px = dib.data() + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 纯红（BGR 顺序）
            uint8_t* p = px + size_t(y) * stride + size_t(x) * (bpp / 8);
            p[0] = 0; p[1] = 0; p[2] = 255;
            if (bpp == 32) p[3] = 255;
        }
    }
    return dib;
}

static void test_png_encode_dib() {
    const std::wstring dir = temp_dir();
    // Review Focus 1：四种组合都要能编出“能看”的图
    const struct { int bpp; bool top_down; const wchar_t* name; } cases[] = {
        { 32, false, L"t32_bottom.png" }, { 32, true, L"t32_top.png" },
        { 24, false, L"t24_bottom.png" }, { 24, true, L"t24_top.png" },
    };
    for (const auto& c : cases) {
        const std::wstring path = sg::join_path(dir, c.name);
        ::DeleteFileW(path.c_str());
        std::wstring err;
        const std::vector<uint8_t> dib = make_dib(8, 4, c.bpp, c.top_down);
        CHECK(sg::png_encode_dib(path, dib, err));
        CHECK(::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);

        // 头 8 字节必须是 PNG 签名
        std::vector<uint8_t> head(8);
        FILE* f = nullptr;
        CHECK(::_wfopen_s(&f, path.c_str(), L"rb") == 0);
        if (f) { CHECK(::fread(head.data(), 1, 8, f) == 8); ::fclose(f); }
        const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        CHECK(::memcmp(head.data(), sig, 8) == 0);

        // 用 WIC 解回来：尺寸对，且左上角像素是红的（证明行序没搞反）
        IWICImagingFactory* fac = nullptr;
        CHECK(SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&fac))));
        if (fac) {
            IWICBitmapDecoder* dec = nullptr;
            CHECK(SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &dec)));
            IWICBitmapFrameDecode* frame = nullptr;
            if (dec) CHECK(SUCCEEDED(dec->GetFrame(0, &frame)));
            UINT w = 0, h = 0;
            if (frame) CHECK(SUCCEEDED(frame->GetSize(&w, &h)));
            CHECK_EQ(w, 8u);
            CHECK_EQ(h, 4u);
            if (frame) {
                std::vector<uint8_t> rgba(8u * 4u * 4u);
                CHECK(SUCCEEDED(frame->CopyPixels(nullptr, 8 * 4, static_cast<UINT>(rgba.size()),
                                                  rgba.data())));
                CHECK(rgba[2] > 200);   // R
                CHECK(rgba[1] < 60);    // G
                CHECK(rgba[0] < 60);    // B
            }
            if (frame) frame->Release();
            if (dec) dec->Release();
            fac->Release();
        }
        ::DeleteFileW(path.c_str());
    }
    // 坏输入不能崩：头都不够长
    std::wstring err;
    CHECK(!sg::png_encode_dib(sg::join_path(dir, L"bad.png"), std::vector<uint8_t>{ 1, 2, 3 }, err));
    CHECK(!err.empty());
}

// 剪贴板里没有位图时返回 false（同一台机器上跑，不依赖外部状态）
static void test_clipboard_image_absent() {
    std::vector<uint8_t> dib;
    if (sg::clipboard_get_image_dib(dib)) { /* 剪贴板里恰好有位图也算通过 */ }
    else { CHECK(dib.empty()); }
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `& $CMAKE --build build --config Release --target test_io`
Expected: `error C1083: 无法打开包括文件: "png.h"`（RED = 文件不存在）

- [ ] **Step 3: 实现 `src/png.cpp`**

要点（WIC 编码，约 70 行）：
1. 校验 `dib.size() >= sizeof(BITMAPINFOHEADER)`、`biSize` 合法、`biBitCount` 是 24 或 32、`biCompression == BI_RGB`、`biWidth/biHeight != 0`；不合法就填 `error`（中文）返回 false。
2. `CoCreateInstance(CLSID_WICImagingFactory)`；`CreateStream`（`IWICStream::InitializeFromFilename(path, GENERIC_WRITE)`）→ `CreateEncoder(GUID_ContainerFormatPng)` → `Initialize` → `CreateNewFrame` → `Initialize`。
3. 像素：把 DIB 转成 32bpp **自上而下** 的 BGRA（用 `IWICBitmap` + `WritePixels`，或先建 `IWICBitmap` 再 `SetPixel` 太慢 —— 用 `WritePixels`）：
   - 32bpp 且非 top-down 时逐行倒序拷；
   - 24bpp 时把 BGR 补齐成 BGRA（alpha 填 255），并按 `biHeight` 的正负决定行序；
   - `stride` 用 `((width * bpp + 31) / 32) * 4`，与 `biSizeImage` 无关（很多工具不填它）。
4. `SetSize(w, h)`、`SetPixelFormat(GUID_WICPixelFormat32bppBGRA)`、`WritePixels(h, stride, bytes, size)`、`Commit`、`Release` 全部资源。
5. 设备/编码失败时填 `error = L"图片编码失败（错误码 0x…）"` 并返回 false。

- [ ] **Step 4: 实现 `clipboard_get_image_dib`**

`clipboard.cpp` 里加：`OpenClipboard(nullptr)` → 先试 `CF_DIBV5`（若 `IsClipboardFormatAvailable(CF_DIBV5)`），退回 `CF_DIB` → `GetClipboardData` → `GlobalLock` → 把 `GlobalSize` 字节拷进 `out` → `GlobalUnlock` → `CloseClipboard`。注意 `CF_BITMAP`（HBITMAP）**不处理**：几乎所有复制位图的程序都会同时给 `CF_DIB`。

- [ ] **Step 5: 跑测试确认通过**

Run: `& $CMAKE --build build --config Release --target test_io; .\build\Release\test_io.exe`
Expected: `OK: test_io 全部通过`（四种 bpp/行序组合的“左上角是红的”断言全过）

- [ ] **Step 6: 提交**

```powershell
git add src/png.h src/png.cpp src/clipboard.h src/clipboard.cpp tests/test_io.cpp CMakeLists.txt
git commit -m "feat(todo): 剪贴板位图读取与 DIB→PNG 编码（WIC，覆盖 24/32bpp 与两种行序）"
```

---

### Task 3: 纯布局（不等行高的前缀和与命中）

**Files:**
- Create: `src/views/todo_layout.h`、`src/views/todo_layout.cpp`
- Test: `tests/test_layout.cpp`
- Modify: `CMakeLists.txt`（`test_layout` 与 `SG_SOURCES` 加 `src/views/todo_layout.cpp`）

**Interfaces:**
- Produces（`views/todo_layout.h`，纯逻辑、不碰 Renderer）:
  ```cpp
  namespace sg {
  constexpr float kTodoInputH = 34.f;    // 底部常驻输入框
  constexpr float kTodoRowTextH = 28.f;  // 文字/链接行
  constexpr float kTodoRowImageH = 96.f; // 图片行（缩略图 + 文件名）
  constexpr float kTodoThumbW = 160.f;
  constexpr float kTodoThumbH = 88.f;

  float todo_row_height(TodoKind kind);
  // offsets[i] = 第 i 行的上边（相对列表顶部），offsets[n] = 列表总高
  std::vector<float> todo_row_offsets(const std::vector<TodoKind>& kinds);
  // y（相对列表顶部）落在哪一行；越界返回 -1
  int todo_row_at(const std::vector<float>& offsets, float y);
  // 让 sel 可见；返回夹紧后的 scroll（像素）
  float todo_scroll_for(const std::vector<float>& offsets, float viewport_h, float scroll, int sel);
  }
  ```
- Consumes: `model/todo_kind.h` 的 `TodoKind`（`#include "model/todo_kind.h"`；这是 header-only 依赖，不引入 Windows 头）

- [ ] **Step 1: 写失败的测试**

`tests/test_layout.cpp` 加（并在 `main()` 里调用）：

```cpp
// 待办列表是不等高行（文字 28 / 图片 96）：前缀和必须精确
static void test_todo_row_offsets() {
    using sg::TodoKind;
    CHECK_EQ(sg::todo_row_height(TodoKind::Text), 28.f);
    CHECK_EQ(sg::todo_row_height(TodoKind::Link), 28.f);
    CHECK_EQ(sg::todo_row_height(TodoKind::Image), 96.f);

    const std::vector<TodoKind> kinds = { TodoKind::Text, TodoKind::Image, TodoKind::Text };
    const std::vector<float> off = sg::todo_row_offsets(kinds);
    CHECK_EQ(off.size(), size_t{4});
    CHECK_EQ(off[0], 0.f);
    CHECK_EQ(off[1], 28.f);
    CHECK_EQ(off[2], 124.f);   // 28 + 96
    CHECK_EQ(off[3], 152.f);   // + 28

    // 命中：行内任意 y 都落在该行；正好在边界上算下一行
    CHECK_EQ(sg::todo_row_at(off, 0.f), 0);
    CHECK_EQ(sg::todo_row_at(off, 27.9f), 0);
    CHECK_EQ(sg::todo_row_at(off, 28.f), 1);
    CHECK_EQ(sg::todo_row_at(off, 123.9f), 1);
    CHECK_EQ(sg::todo_row_at(off, 124.f), 2);
    CHECK_EQ(sg::todo_row_at(off, 151.9f), 2);
    CHECK_EQ(sg::todo_row_at(off, 152.f), -1);   // 列表下方空白
    CHECK_EQ(sg::todo_row_at(off, -1.f), -1);

    // 空列表
    CHECK_EQ(sg::todo_row_offsets({}).size(), size_t{1});
    CHECK_EQ(sg::todo_row_at(sg::todo_row_offsets({}), 5.f), -1);
}

static void test_todo_scroll_clamp_and_visibility() {
    using sg::TodoKind;
    std::vector<TodoKind> kinds(10, TodoKind::Text);   // 10 行 × 28 = 280
    const std::vector<float> off = sg::todo_row_offsets(kinds);
    const float viewport = 100.f;   // 只能看到约 3.5 行

    CHECK_EQ(sg::todo_scroll_for(off, viewport, 0.f, -1), 0.f);        // 没选中：不动
    CHECK_EQ(sg::todo_scroll_for(off, viewport, 0.f, 0), 0.f);         // 第 0 行本来就在视野里
    CHECK_EQ(sg::todo_scroll_for(off, viewport, 0.f, 5), 60.f);        // 让第 5 行（y=140）贴底
    CHECK_EQ(sg::todo_scroll_for(off, viewport, 200.f, 1), 0.f);       // 选中在上面 → 滚回去
    // 超出内容时不会留出空白（最大 scroll = 总高 - 视口）
    CHECK_EQ(sg::todo_scroll_for(off, viewport, 999.f, -1), 0.f);      // sel<0 时不夹紧
    CHECK_EQ(sg::todo_scroll_for(off, viewport, 999.f, 9), 180.f);     // 280 - 100
    // 视口比内容还高：scroll 只能是 0
    CHECK_EQ(sg::todo_scroll_for(off, 1000.f, 50.f, 9), 0.f);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `& $CMAKE --build build --config Release --target test_layout`
Expected: `error C1083: 无法打开包括文件: "views/todo_layout.h"`

- [ ] **Step 3: 实现 `views/todo_layout.cpp`**

要点：
- `todo_row_offsets`：累加 `todo_row_height(kind)` 进 `offsets`，最后 push 总高（所以 size = n+1）。
- `todo_row_at`：`y < 0` 返回 -1；`offsets` 少于 2 项返回 -1；用 `std::upper_bound(offsets.begin(), offsets.end(), y)` 找到第一个**大于** y 的位置 `it`，`idx = it - begin() - 1`；`idx < 0 || idx >= n` 返回 -1（`n = offsets.size() - 1`）。这样正好落在边界上会算下一行（`upper_bound` 语义）。
- `todo_scroll_for`：`sel < 0` 直接返回 `scroll`；算出 `top = offsets[sel]`、`bottom = offsets[sel + 1]`，`if (top < scroll) scroll = top; else if (bottom > scroll + viewport) scroll = bottom - viewport;` 最后把结果夹到 `[0, max(0, total - viewport)]`。

- [ ] **Step 4: 跑测试确认通过**

Run: `.\build\Release\test_layout.exe`
Expected: `OK: test_layout 全部通过`

- [ ] **Step 5: 提交**

```powershell
git add src/views/todo_layout.h src/views/todo_layout.cpp tests/test_layout.cpp CMakeLists.txt
git commit -m "feat(todo): 不等高行的前缀和布局与命中（纯逻辑，可测）"
```

---

### Task 4: Todo 视图骨架（渲染 + 导航 + 底部输入框）

**Files:**
- Create: `src/views/todo.h`、`src/views/todo.cpp`
- Modify: `src/viewapi.h`（新增 `TodoState` + `AppState::todo`）、`src/app.cpp`、`src/app.h`（不需要改消息）
- Modify: `CMakeLists.txt`（`SG_SOURCES` 加 `src/views/todo.cpp`）
- Test: 验收脚本（新建 `.superpowers/sdd/2026-09-23-stargazer-todo/t14-todo-view.ps1`）

**Interfaces:**
- Produces（`viewapi.h`）:
  ```cpp
  struct TodoState {
      int sel = -1;            // 列表选中行
      int hover = -1;
      float scroll = 0.f;      // 像素
      std::vector<TodoKind> kinds;    // 与 AppState::todos 同序（排序后）的行类型
      std::vector<float> offsets;     // 行偏移前缀和
      InlineEdit input;        // 底部常驻输入框（新增条目）
      InlineEdit edit;         // F2 改文字（临时叠在行上）
      long long pending_image_id = 0;  // 正在落盘的图片条目 id（0 = 无）；失败时回滚它
  };
  // AppState 新增：TodoState todo;
  ```
- Produces（`views/todo.h`）:
  ```cpp
  namespace sg {
  struct App;
  D2D1_RECT_F todo_input_rect(D2D1_SIZE_F client);
  D2D1_RECT_F todo_list_rect(D2D1_SIZE_F client);
  int todo_hittest(App& app, D2D1_POINT_2F pt);            // 行下标，未命中 -1
  bool todo_checkbox_hit(App& app, D2D1_POINT_2F pt);      // 是否点在左侧复选框热区
  void todo_rebuild_layout(AppState& s);                   // 重算 kinds/offsets
  void todo_render(App& app);
  bool todo_keydown(App& app, UINT vk);
  void todo_activate(App& app);   // 进入视图：重建布局 + 同步输入框
  void todo_leave(App& app);      // 离开视图：收起输入框
  void todo_sync_input(App& app); // 常驻输入框就位（含 ↑↓ 转发钩子）
  }
  ```
- Consumes: `views/todo_layout.h`、`model/store.h`（`TodoItem`/`sort_todos`）、`views/grid.h`（`kViewTabsH`/`kPad`/`view_tabs_render`）、`icons.h`（`icons_get` 画条目图标占位；Task 7 才换成缩略图）、`edit.h`

- [ ] **Step 1: 写失败的验收（先用命令确认它确实失败）**

新建 `t14-todo-view.ps1`（**纯 ASCII**，照抄 workspace 里 `t13-browse-ops.ps1` 的探针类）：写 fixture `todo.txt`（三条：文字、链接、图片引用指向一个真实存在的 png，用 Task 2 的编码器生成或直接放一个手写的小 PNG），`ui.txt` 里 `view=1`，启动后用 PrintWindow 数行：

```powershell
# 断言：三种 kind 各渲染出一行；文字行 28、图片行 96（物理像素乘 scale）
# 探针按行扫描：文字行有文字像素（bright>0），图片行有非背景色块（缩略图占位），
# 并打印每行的 y 边界以便比对 28/96 的间距
```

Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-view.ps1`
Expected: 失败（`todo.txt` 现在没人读，界面只有“待办：尚未实现”）

- [ ] **Step 2: 实现 `viewapi.h` 的 `TodoState`**

按 Interfaces 加结构体与 `AppState::todo`（`#include "views/todo_layout.h"` 不需要；`TodoKind` 已经通过 `model/store.h` 进来）。`kinds`/`offsets` 是为了渲染与命中共用同一份布局（**不要在两处各算一遍**）。

- [ ] **Step 3: 实现 `views/todo.cpp` 的布局与渲染**

- `todo_input_rect`：底部 `kTodoInputH` 高、左右 `kPad`。
- `todo_list_rect`：`kViewTabsH + kPad` 到输入框上方 `kPad`。
- `todo_rebuild_layout`：`kinds = 每个 todo 的 kind`（**依赖 `AppState::todos` 已经是排好序的**）；`offsets = todo_row_offsets(kinds)`；`sel` 夹到 `[-1, n-1]`；`scroll = todo_scroll_for(offsets, list_h, scroll, sel)`。
- `todo_hittest`：点在列表区 → `y = pt.y - list.top + scroll` → `todo_row_at(offsets, y)`。
- 复选框热区：`x` 在 `row.left .. row.left + 24` 之间，且是该行高度内的垂直中线附近（直接取整行也行，热区够大更好点）。
- `todo_render`：
  1. `IDWriteTextFormat*` 三档复用 `r.format()`（正文 13、链接 13、小字 11）。
  2. 遍历**可见行**（用 `offsets` + `scroll` 二分出首个可见行，画到超出列表底部为止）。
  3. 每行画：复选框方块（`stroke_round_rect`；`done` 时填强调色并画一个勾 —— 用两段 `fill_rect` 拼√，别引入字体符号）+ 内容：
     - `Text`：`text` 原样，超过 2 行截断（`D2D1_DRAW_TEXT_OPTIONS_CLIP` + 行高计算即可；多行文本的行高按 `kTodoRowTextH` 固定，超出部分裁掉）。
     - `Link`：`text` 用 `theme.accent` + 下划线（复用 `grid.cpp` 删除线那招：在文本基线附近 `fill_rect` 一条 1px 线）。
     - `Image`：先在缩略图框（`kTodoThumbW × kTodoThumbH`）画 `icons_get(r, attach, false)`（Task 7 换成缩略图）；下面是 `text`（空则 `file_name(attach)`）。
     - `done`：整行文字用 `theme.text_dim`。
     - `attach` 非空且 `missing`（Task 7 才有值）→ 删除线 + “图片已不存在”。
  4. 列表为空：居中一行 `theme.text_dim` 的“还没有记录：在下面输入，或 Ctrl+V 粘文字 / 链接 / 截图”。
- `todo_activate`：`todo_rebuild_layout` → `todo_sync_input` → `SetFocus(input)`。
- `todo_sync_input`：`input.open(app.panel, to_physical(todo_input_rect), L"", dpi, commit, nullptr)`；commit 回调先留空（Task 6 实现提交）；`input.on_key` 里把 `VK_UP/VK_DOWN/VK_PRIOR/VK_NEXT/VK_HOME/VK_END` 转发给面板（`PostMessage(panel, WM_KEYDOWN, vk, 0)`），`VK_DOWN` 时顺手 `sel = 0` 并把焦点交给面板（照抄旧启动板搜索框的写法）。
- `todo_keydown`：`↑↓/PgUp/PgDn/Home/End` 移动 `sel`（`todo_scroll_for` 保持可见）+ `InvalidateRect`；`VK_UP` 在首行时把焦点还给输入框（`focus()`）。其余键在 Task 5 实现。

- [ ] **Step 4: 在 `app.cpp` 接线**

- `app_load`：读 `todo.txt`（与 `boxes.txt` 同一套 `.bad` 备份逻辑，用独立字符串接内容）、`parse_todos`、`bad_lines += bad`、`sort_todos(s.todos)`。
- `app_save_if_dirty`：再写一份 `todo.txt`（分开写，任一份失败各自报告 —— 沿用浏览/收纳盒那次评审修好的写法）。
- `WM_PAINT`：`case View::Todo: todo_render(*app);`
- `WM_MOUSEMOVE`：Todo 视图下用 `todo_hittest` 更新 `hover`（只在变化时重绘）。
- `WM_MOUSEWHEEL`：Todo 视图下 `scroll -= step * 3*kTodoRowTextH` 后走 `todo_rebuild_layout` 的夹紧（或直接调 `todo_scroll_for`）。
- `WM_LBUTTONDOWN`：Todo 视图下 `sel = todo_hittest(...)`（Task 5 再加复选框点击与双击）。
- `WM_KEYDOWN`：`case View::Todo: todo_keydown(*app, wp);`
- `WM_CHAR`：Todo 视图下若面板有焦点且输入框已开 → `input.focus()` 并把字符转投给 EDIT（照抄旧启动板的 WM_CHAR 处理）。
- `app_set_view`/`app_show`：进 Todo 调 `todo_activate`；`app_hide` 与切走时 `todo_leave`（收起 `input`/`edit`）。

- [ ] **Step 5: 构建并跑测试**

Run: `& $CMAKE --build build --config Release --target test_model test_io test_layout stargazer; .\build\Release\test_model.exe; .\build\Release\test_io.exe; .\build\Release\test_layout.exe`
Expected: 构建 0 警告；三个测试 `OK`

- [ ] **Step 6: 跑验收确认三种 kind 都画出来**

Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-view.ps1`
Expected: PASS —— 文字行有文字像素、链接行有强调色像素、图片行高 96 且框内有非背景像素；
行间距与 28/96 一致；fixture 里那条**多行文本**（用 `|n` 转义写入）仍只占 28 一行（不撑破行高）

- [ ] **Step 7: 回归（别把已有视图弄坏）**

Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-22-stargazer-phase2-box\acceptance.ps1`
Expected: `pass=25 fail=0` + 浏览子脚本 `pass=8 fail=0`

- [ ] **Step 8: 提交**

```powershell
git add src/views/todo.h src/views/todo.cpp src/viewapi.h src/app.cpp CMakeLists.txt
git commit -m "feat(todo): 待办视图（不等高列表、三种 kind 渲染、方向键导航、底部常驻输入框）"
```

---

### Task 5: 交互（勾选 / 打开 / 删除 / 复制 / 改文字 / 右键菜单 / 清空已完成）

**Files:**
- Modify: `src/views/todo.h`、`src/views/todo.cpp`、`src/app.cpp`（鼠标双击 + 右键菜单分发）
- Test: 验收脚本 `t14-todo-ops.ps1`（新建）

**Interfaces:**
- Produces（`views/todo.h` 追加）:
  ```cpp
  void todo_toggle_done(App& app);       // 切换选中行完成态并落盘
  void todo_open_selected(App& app);     // link → 浏览器；image → 系统看图；text → 无动作
  void todo_delete_selected(App& app);   // 删条目 + 按规则清理 data\images 副本
  void todo_clear_done(App& app);        // 清空已完成（同样按规则清理副本）
  void todo_copy_selected(App& app);     // text/link → CF_UNICODETEXT；image → CF_HDROP
  void todo_rename_selected(App& app);   // F2：仅 text/link，输入框叠在行上
  void todo_context_menu(App& app, POINT screen_pt, POINT client_pt);
  void todo_reveal_selected(App& app);   // 仅 image：在资源管理器中显示
  ```
  入口绑定（写进 README）：`Space` 勾选、`Del` 删除、`F2` 改文字、`Ctrl+C` 复制、`Enter` 打开、`Ctrl+Shift+D` 清空已完成。
- Produces（`src/clipboard.h` 追加，供 Task 5/6 共用）:
  ```cpp
  // 把纯文本写进剪贴板（CF_UNICODETEXT）
  void clipboard_set_text(const std::wstring& text);
  ```
- Consumes: `model/todo_kind.h` 的 `todo_is_owned_copy` / `todo_copy_still_used`、`clipboard.h` 的 `clipboard_set_paths`、`model/paths.h` 的 `file_name`/`append_name`、`ShellExecuteW`

- [ ] **Step 1: 写失败的验收**

`t14-todo-ops.ps1`（纯 ASCII，照抄 `t13-browse-ops.ps1` 的骨架），fixture：`data\todo.txt` 四条（文字 / 链接 / 图片引用指向真实 png / 已完成），`data\images\` 下放一个我们自己的副本 png 并让第 3 条引用它。断言：

```
1  Space 勾掉第一条 → todo.txt 里该行 done=1，且排序后它沉底（读回文件按行序判断）
2  Del 删掉一条文字条目 → 该行从 todo.txt 消失，磁盘上没有任何文件被删
3  同一张副本图被两条引用、删其中一条 → data\images\ 里那份副本仍在（Review Focus 3）
4  「清空已完成」：按 Ctrl+Shift+D（这个键位是常驻绑定，写进 README）→ 已完成条目消失，且只删掉属于它的副本
5  Ctrl+C 文字条目 → 剪贴板 CF_UNICODETEXT 内容相符（复用探针类里的 HdropList/GetText）
6  Ctrl+C 图片条目 → 剪贴板有 CF_HDROP 且指向该图片路径
7  F2 改文字 → todo.txt 里该条 text 变了、kind 不变
```

按键映射（先定死，写进 README）：`Space` 勾选、`Del` 删除、`F2` 改文字、`Ctrl+C` 复制、`Ctrl+Shift+D` 清空已完成、`Enter` 打开。

Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-ops.ps1`
Expected: 全 FAIL（只有 Task 4 的导航能用）

- [ ] **Step 2: 实现 `todo_toggle_done` / `todo_delete_selected` / `todo_clear_done`**

公共小函数（`todo.cpp` 内部）：
```cpp
// 该 attach 是否是我们自己的副本，且删掉它之后没人再用
bool copy_is_orphan(const AppState& s, long long except_id, const std::wstring& attach);
```
实现：`todo_is_owned_copy(join_path(s.paths.data_dir, L"images"), attach)` 且把除 `except_id` 外所有条目的 `attach` 收集成 vector 传给 `todo_copy_still_used`，两者都为真才是孤儿。
- 删条目：`todos.erase` → 若 `copy_is_orphan` → `DeleteFileW(attach)`（失败只 `OutputDebugString`，不弹框：数据清理失败不值得打断用户）→ `sort_todos` → `todo_rebuild_layout` → `data_dirty = true` → `InvalidateRect`。
  - 注意：`attach` 可能指向一个**目录**（手改数据）→ 用 `GetFileAttributesW` 判断，是目录就不删（只删文件）。
- `todo_toggle_done`：`todos[sel].done = !done` → `sort_todos` → 重建布局 → `data_dirty = true` → **保持选中在同一 id 上**（排序后行号会变，按 id 找回）。
- `todo_clear_done`：`remove_if(done)`；对每条被删的条目先做 `copy_is_orphan` 判断再删副本（**必须先把要删的条目从数组里剔除后再判断**，否则“另一条已完成条目也引用同一张图”的情况会误删）。

- [ ] **Step 3: 实现打开 / 复制 / 改文字 / 资源管理器 / 右键菜单**

- `todo_open_selected`：`Text` 什么都不做（列表里按 Enter 不该有副作用）；`Link` → `ShellExecuteW(nullptr, L"open", text.c_str(), …)`；`Image` → 打开 `attach`；成功后 `app_hide(app)`（与收纳盒/浏览一致）。
- `todo_copy_selected`：`Text`/`Link` → `clipboard_set_paths` 不适用（那是路径），改用 `OpenClipboard`+`CF_UNICODETEXT`（把 `clipboard.cpp` 里的文本写入抽成 `clipboard_set_text(const std::wstring&)` 供两边复用）；`Image` → `clipboard_set_paths({ attach }, false)`。
- `todo_rename_selected`：`Link`/`Text` 才允许；`edit.open(...)` 叠在该行上，初始值 = `text`；提交时写回 + `data_dirty`；`Image` 直接 `tray_balloon` 提示“图片条目没有文字可改”。
- `todo_reveal_selected`：`attach` 非空才可用，`explorer /select,<attach>`。
- `todo_context_menu`：打开 / 复制 / 编辑 / 删除 / 在资源管理器中显示 / 清空已完成 / 粘贴（粘贴的实现在 Task 6，先留命令号与空分支并在 Task 6 接上）。
- `app.cpp`：`WM_LBUTTONDBLCLK` 加 Todo 分支（`todo_hittest` → `todo_open_selected`）；`WM_CONTEXTMENU` 加 Todo 分支；`WM_LBUTTONDOWN` 里若 `todo_checkbox_hit` → `todo_toggle_done`（单击复选框，而不是双击）。

- [ ] **Step 4: 构建 + 跑验收 + 回归**

Run:
```powershell
& $CMAKE --build build --config Release --target stargazer test_model test_io test_layout
.\build\Release\test_model.exe; .\build\Release\test_io.exe; .\build\Release\test_layout.exe
& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-ops.ps1
& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-22-stargazer-phase2-box\acceptance.ps1
```
Expected: 三个测试 `OK`；`t14-todo-ops` 全 PASS；阶段 2 验收 `pass=25 fail=0`

- [ ] **Step 5: 提交**

```powershell
git add src/views/todo.h src/views/todo.cpp src/app.cpp src/clipboard.h src/clipboard.cpp
git commit -m "feat(todo): 勾选/打开/删除/复制/改文字/右键菜单/清空已完成（含副本按引用计数清理）"
```

---

### Task 6: 输入流水线（回车提交、Ctrl+V 三类判定、拖入图片、图片落盘）

**Files:**
- Modify: `src/fs_work.h`、`src/fs_work.cpp`（新增“保存剪贴板图片”操作）、`src/views/todo.h`、`src/views/todo.cpp`、`src/app.cpp`
- Test: 验收脚本 `t14-todo-input.ps1`（新建）

**Interfaces:**
- Produces（`fs_work.h` 追加）:
  ```cpp
  // 把剪贴板位图存成 PNG（在工作线程做：4K 截图编码可达上百毫秒）
  void fs_save_image(const std::vector<uint8_t>& dib, const std::wstring& dest_path, uint64_t id);
  ```
  （复用现有的 `WM_APP_FS_OP_DONE` + `fs_take_op`；`fs_work.cpp` 内部加 `Kind::SaveImage`，执行体调 `png_encode_dib`）
- Produces（`views/todo.h` 追加）:
  ```cpp
  // 按 spec §3 的优先级把剪贴板内容变成条目；返回是否产生了条目
  bool todo_add_from_clipboard(App& app);
  bool todo_add_from_paths(App& app, const std::vector<std::wstring>& paths);  // 拖入
  void todo_add_text(App& app, const std::wstring& text);   // 回车提交走它
  void todo_on_image_saved(App& app, uint64_t id);          // WM_APP_FS_OP_DONE 里调
  ```
- Consumes: `clipboard_get_paths` / `clipboard_get_image_dib`、`todo_kind_from_text` / `is_image_path`、`png_encode_dib`（经 `fs_work`）、`model/paths.h`

- [ ] **Step 1: 写失败的验收**

`t14-todo-input.ps1`（纯 ASCII）：
```
1  在输入框里打字 + 回车 → todo.txt 多一行 kind=text，输入框清空且仍是焦点（读 EDIT 文本为空）
2  把 "https://example.com" 塞进剪贴板（Set-Clipboard）→ Ctrl+V → 多一行 kind=link
   且该行渲染成链接色（像素断言可选，先断言数据）
3  Review Focus 2：剪贴板同时有文本与位图时 → Ctrl+V 必须记成 text/link。
   探针做法：在 t14 的 C# 探针里加 SetTextPlusBitmap(byte[] dib)：
   OpenClipboard → EmptyClipboard → SetClipboardData(CF_DIB, hGlobal(dib)) → SetClipboardData(CF_UNICODETEXT, hGlobal(text)) → CloseClipboard
4  只放位图（探针造一张 8x4 红图）→ Ctrl+V → data\images\<id>.png 出现、todo.txt 多一行 kind=image
5  把一张真实 png 的路径用 SetFileClipboard 放进去 → Ctrl+V → 多一行 kind=image 且 attach 是该路径、
   data\images\ 下**没有**新文件（引用型不复制内容）
6  拖入：真实 IDropTarget 拖入无法自动化 → 列入人工项（Task 8 的 man 6）
   本脚本只覆盖 1..5（都走键盘 + 程序化剪贴板）
```
Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-input.ps1`
Expected: 全 FAIL

- [ ] **Step 2: 实现 `fs_save_image`（含 `png_encode_dib` 接线）**

`fs_work.cpp`：`enum class Kind` 加 `SaveImage`；`Request` 加 `std::vector<uint8_t> dib`；执行体：
```cpp
case Kind::SaveImage: {
    // 先确保目录存在（data\images\ 可能是用户手删过）
    const std::wstring dir = parent_path(req.path);
    ::CreateDirectoryW(dir.c_str(), nullptr);
    res.ok = png_encode_dib(req.path, req.dib, res.error);
    break;
}
```
（`parent_path` 用 `model/paths.h` 里 Task 3 之前就有的实现；`CreateDirectoryW` 在目录已存在时返回 ERROR_ALREADY_EXISTS，忽略即可。）

- [ ] **Step 3: 实现输入流水线（`todo_add_from_clipboard` / `todo_add_text` / `todo_add_from_paths`）**

判定顺序（**spec §3 的顺序被本计划修正**：文本先于位图，理由见下方 Ruling 提示）：
1. `clipboard_get_paths(...)` 非空 → 逐个：是图片的记一条 `Image`（`attach = 该路径`）；全不是图片 → 托盘气泡“待办只收文字、链接和图片”，返回 false。
2. `clipboard_get_text(...)`（新增：读 `CF_UNICODETEXT`）非空且**去掉首尾空白后非空** → `todo_kind_from_text` → 记一条 `Text`/`Link`。
3. `clipboard_get_image_dib(dib)` 成功 → `id = next_todo_id(todos)`；先插一条 `kind=Image, attach = join_path(images_dir, <id>.png)`；`++op_id; fs_save_image(dib, attach, op_id); data_dirty = true;`（**先插条目再编码**：编码失败时 `todo_on_image_saved` 会把这条删掉并气泡提示，避免“条目先出现又消失”的闪烁）。
   记 `pending_image_id = id`（放 `TodoState`）以便失败回滚。
4. 都没有 → 返回 false（不提示：Ctrl+V 粘空剪贴板是常见误操作，不值得弹气泡）。

- `todo_add_text`：`kind = todo_kind_from_text(text)`；`created = time(nullptr)`；`id = next_todo_id`；push 后 `sort_todos` + 重建布局 + `sel = 0`（新条目置顶）+ `data_dirty = true`。
- `todo_on_image_saved`：`fs_take_op(op_id, ok, error, note)` 取不到就直接返回；`ok` 时 `note/气泡` 都不发；失败时**删掉刚插的那条** + `tray_balloon(app, L"Stargazer", L"图片保存失败：" + error)`。
- `todo_add_from_paths`：与第 1 条同规则。

- [ ] **Step 4: 接上输入框与 Ctrl+V 钩子**

- `todo_sync_input` 的 commit 回调：`todo_add_text(app, t)` 后清空输入框并保持焦点（`input.set_text(L"")` + `input.focus()`）。
- `input.on_key`：加 `Ctrl+V` 判定 —— `if (ctrl && vk == 'V' && (剪贴板有文件 或 (有位图 且 无文本))) { todo_add_from_clipboard(app); return true; }` 否则返回 false 让 EDIT 自己粘文本。
- `todo_keydown`：加 `Ctrl+V`（面板有焦点时也走同一条）、`Ctrl+Shift+D`（清空已完成，Task 5 已实现的入口）、`Enter`（打开选中行）。
- `app.cpp`：拖入回调加 Todo 分支 —— `if (s.view == View::Todo) { todo_add_from_paths(*app, paths); return; }`（放在 Box 分支之后、`return` 之前，别改动 Box 的分支）。
- `WM_APP_FS_OP_DONE`：Todo 视图（或 `pending_image_id != 0`）时调 `todo_on_image_saved(*app, ...)`（与浏览的 `browse_on_op_done` 并列，各取各的 id，互不冲突）。

- [ ] **Step 5: 构建 + 跑验收 + 回归**

Run: 同 Task 5 的四条命令，另加 `t14-todo-input.ps1`
Expected: 三个测试 `OK`；`t14-todo-input` 全 PASS（第 3 条“文本与位图同时存在”必须记成 text/link）；阶段 2 验收仍 `pass=25 fail=0`

- [ ] **Step 6: 提交**

```powershell
git add src/fs_work.h src/fs_work.cpp src/views/todo.h src/views/todo.cpp src/app.cpp src/clipboard.h src/clipboard.cpp
git commit -m "feat(todo): 输入流水线（回车提交 / Ctrl+V 自动判定 / 拖入 / 位图落盘为 PNG）"
```

> **执行者注意（本步的 Ruling）**：spec §3 写的顺序是“HDROP → 位图 → 文本”，本计划改成“HDROP → 文本 → **位图**”。
> 原因是 Excel 复制单元格、Word 复制图文时剪贴板里**同时**有文本和位图，按 spec 的原顺序会把单元格变成一张图。
> 执行时按本计划实现，并把这条 Ruling 记进账本、回头把 spec §3 的顺序同步改掉。

---

### Task 7: 图片预览（系统缩略图）与存在性校验

**Files:**
- Create: `src/images.h`、`src/images.cpp`
- Modify: `src/app.h`（新增 `WM_APP_IMAGE_READY = WM_APP + 7`）、`src/main.cpp`（起停线程）、`src/app.cpp`（统一存在性回填 + 设备丢失时清缩略图位图）、`src/views/todo.cpp`（行内用 `images_get`）
- Modify: `CMakeLists.txt`（`SG_SOURCES` 加 `src/images.cpp`）
- Test: 验收脚本 `t14-todo-image.ps1`（新建）

**Interfaces:**
- Produces（`src/images.h`）:
  ```cpp
  namespace sg {
  struct Renderer;
  // 系统缩略图（IShellItemImageFactory），工作线程取像素、UI 线程建位图。
  // 未命中时投递请求并返回 nullptr（本帧画占位）；LRU 上限 32 张。
  ID2D1Bitmap* images_get(Renderer& r, const std::wstring& path);
  bool images_init(HWND notify_hwnd);   // 通知 WM_APP_IMAGE_READY
  void images_shutdown();
  void images_on_device_lost();         // 丢位图、留像素
  size_t images_count();                // 测试/诊断用
  }
  ```
- Consumes: `Renderer::make_bitmap`（`render.h` 已有）、`model/paths.h` 的 `normalize_key`

- [ ] **Step 1: 写失败的验收**

`t14-todo-image.ps1`（纯 ASCII）：
```
fixture：用 Task 2 的编码器生成一张 64x64 纯红 png 到 %TEMP%，data\todo.txt 一条 kind=image attach=该路径，
         ui.txt view=1
1  启动后该图片行里出现红色像素（>200,<60,<60）——证明缩略图真的取到了（不是占位色块）
2  把该 png 改名 → 重启 → 该行灰显 + 有删除线（像素：文字用 theme.text_dim，且出现一条横线），
   同时给出“图片已不存在”文案（用行内文字像素分布粗判即可）
3  改回名字 → 重启 → 红色像素回来（恢复）
4  data\images\ 被整体删除后程序不崩（把 fixture 指向 data\images\ 下不存在的文件即可覆盖）
```
Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-image.ps1`
Expected: 全 FAIL（现在只有扩展名占位色块，没有真缩略图）

- [ ] **Step 2: 实现 `src/images.cpp`（照抄 `icons.cpp` 的骨架）**

- 结构：`std::mutex` + `condition_variable` + `deque<Request{key,path}>` + `set` 去重 + `unordered_map<wstring, Entry{pixels,w,h,bitmap,last_used}>` + `g_quit` + `std::thread` + join。
- 工作线程：`CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`（**缩略图需要 STA；这是与 icons 线程不同的点** —— icons 用 MTA 是因为它只调 `SHGetFileInfo`）；取图：
  ```cpp
  IShellItemImageFactory* f = nullptr;
  if (SUCCEEDED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) && f) {
      SIZE sz{ 192, 192 };
      HBITMAP hbmp = nullptr;
      if (SUCCEEDED(f->GetImage(sz, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) && hbmp) {
          // HICON 那条路已证明可行：复用 icons.cpp 里“读 hbmColor 的 32bpp DIB + alpha”的写法
          // （把它抽成 bgra_from_hbitmap 放到 model/bgra 或 icons.cpp 顶部，两处共用）
      }
  }
  ```
  实现提示：`GetImage` 给的是 HBITMAP（32bpp DIB，通常已带 alpha）；如果 alpha 全 0 就当作不透明（缩略图服务对照片会给不透明图）。取不到就什么都不存，`images_get` 继续画占位。
- LRU 上限 32；`discard_bitmaps_locked()` / `evict_locked()` 照抄。`images_get` 命中未建位图时 `make_bitmap`。
- 请求去重用 `normalize_key(path)`。

- [ ] **Step 3: 起停线程与设备丢失**

- `main.cpp`：`icons_init(app.ctl)` / `fs_init(app.ctl)` 旁边加 `images_init(app.ctl)`；退出时 `images_shutdown()`。
- `app.h`：`constexpr UINT WM_APP_IMAGE_READY = WM_APP + 7;`；`ctl_wndproc` 里收到它就 `InvalidateRect(panel)`（与图标消息同一处理）。
- `app_hide`：`icons_on_device_lost()` 旁边加 `images_on_device_lost()`。
- `app.cpp` 的 `ensure_panel`：**保持 Box 的拖放注册不变**。

- [ ] **Step 4: 行内改用缩略图**

`todo_render` 的 Image 分支：`if (ID2D1Bitmap* bmp = images_get(r, todo.attach)) { 等比缩放进 kTodoThumbW×kTodoThumbH 居中 DrawBitmap } else { 画 ext_color 占位块 + 文件名 }`。
等比计算：`scale = min(thumbW / w, thumbH / h)`，居中偏移；`ID2D1Bitmap::GetSize` 拿原始尺寸。

- [ ] **Step 5: 统一存在性校验（收纳盒 + 待办共用一个回调）**

`app.cpp` 的 `dragdrop_set_hook` 之前，把回调设置从“盒子自己设置”改成**app 层设置一次**：
```cpp
// 呼出时投递：当前盒子的条目 + 当前待办列表里的引用型图片
// 结果按 path 一次回填两边（fs_work 的回调是单槽设计，别变成两个消费者）
fs_check_paths(paths, [&app](const std::wstring& p, bool exists) {
    for (auto& b : app.state.boxes) for (auto& it : b.items) if (it.path == p) it.missing = !exists;
    for (auto& t : app.state.todos) if (!t.attach.empty() && t.attach == p) t.missing = !exists;
});
```
做法（已定）：**给 `TodoItem` 加一个运行期字段** `bool missing = false;`（**不参与序列化**，与 `BoxItem::missing` 同一套约定），
回填与渲染都直接读写它，不另建同序数组。`TodoState` 不需要 missing 成员。
`box_request_check` 改为只**投递**（`fs_check_paths` 仍由它调，但回调已由 app 层统一设置）——即把 `box_request_check` 里的 lambda 删掉，改由 `app_show`/`app_set_view` 里那段统一投递负责（投递的路径集合 = 盒子 + 待办）。

- [ ] **Step 6: 构建 + 跑验收 + 回归**

Run:
```powershell
& $CMAKE --build build --config Release --target test_model test_io test_layout stargazer
.\build\Release\test_model.exe; .\build\Release\test_io.exe; .\build\Release\test_layout.exe
& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\t14-todo-image.ps1
& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-22-stargazer-phase2-box\acceptance.ps1
```
Expected: 三个测试 `OK`；`t14-todo-image` 全 PASS；阶段 2 验收**仍然** `pass=25 fail=0`（统一回填不能弄坏盒子的失效灰显）

- [ ] **Step 7: 提交**

```powershell
git add src/images.h src/images.cpp src/app.h src/app.cpp src/main.cpp src/views/todo.cpp src/viewapi.h src/model/store.h CMakeLists.txt
git commit -m "feat(todo): 系统缩略图预览（独立工作线程 + LRU）与收纳盒/待办统一的存在性校验"
```

---

### Task 8: 验收 + 文档

**Files:**
- Modify: `README.md`、`docs/superpowers/specs/2026-09-23-stargazer-todo-design.md`（把 Task 6 的 Ruling 同步进 §3；把“工作线程 B”改成“独立的缩略图线程”）
- Test: 阶段 3 验收脚本 `.superpowers/sdd/2026-09-23-stargazer-todo/acceptance.ps1`（把 t14-* 串起来并补全人工项）

- [ ] **Step 1: 补齐验收脚本**

把 `t14-todo-view.ps1` / `t14-todo-ops.ps1` / `t14-todo-input.ps1` / `t14-todo-image.ps1` 串进 `acceptance.ps1`（照阶段 2 的做法），并列出人工项：
```
man  1  Win+Shift+S 真截图 → Ctrl+V → 列表出现缩略图，重启后仍在
man  2  data\images\<id>.png 拷到别的目录整个搬走 exe+data → 预览仍在（便携）
man  3  右键菜单各项（打开/复制/编辑/删除/在资源管理器中显示/清空已完成/粘贴）
man  4  中文输入法输入 + 多行长文本粘贴 → 不崩、不撑破行高
man  5  点复选框（鼠标）→ 立刻沉底灰显
man  6  拖一个图片文件到窗口 → 记成引用型图片条目
perf 1  造 200 条 fixture（文字 150 / 链接 25 / 图片 25）→ Home/End 来回翻、滚轮到顶到底：
        不崩、不错位（End 后能看到最后一行、Home 后看到第一行）
perf 2  全完成 fixture：全部灰显、清空已完成后只剩空列表提示
perf 3  空闲：确认没有新增定时器 —— `Select-String src\*.cpp -Pattern 'SetTimer|PeekMessage'` 应为空
```

- [ ] **Step 2: 跑全量验收 + 内存**

Run: `& powershell -NoProfile -ExecutionPolicy Bypass -File .superpowers\sdd\2026-09-23-stargazer-todo\acceptance.ps1`
Expected: 全部 PASS + 人工项清单打印；另外用 `t8-memory.ps1`（阶段 2 的脚本，改 `-View 1`）测一次：仅托盘 ≤15MB、隐藏后 ≤10MB、呼出稳态 ≤30MB（Task 7 多了缩略图缓存 4.7MB 上限，超了就说明有泄漏）

- [ ] **Step 3: 更新 README**

加 `### 待办` 一节（键盘表、三种内容、图片两种来源、副本规则、清空已完成），数据表 `todo.txt` 改成 6 字段，已知限制补：无截止日期/优先级/提醒/子任务/标签/拖动排序/多选/搜索；缩略图缓存上限 32 张；引用型图片被外部删除会灰显。

- [ ] **Step 4: 提交**

```powershell
git add README.md docs/superpowers/specs/2026-09-23-stargazer-todo-design.md
git commit -m "docs(todo): 待办用法、6 字段格式、缩略图与禁用清单；spec 同步输入顺序与缩略图线程"
```

---

## 完成后的状态

- `Ctrl+1/2/3` 三个视图里，**待办**可以：打字回车记一条、`Ctrl+V` 粘文字/链接/截图、拖入图片文件、缩略图预览、点复选框沉底、一键清空已完成。
- `todo.txt` 六字段可手改；`data\images\` 只存我们自己生成的副本，删条目时按“还有别的引用吗”清理，**绝不碰引用型外部文件**。
- 新增两个可复用零件：`images`（Shell 缩略图 + LRU，独立线程）与 `png`（DIB→PNG）。
- 缩略图线程是第三条工作线程（spec §4 的“2 条固定线程”在此升级；升级路径已在 spec §16 记录）。
