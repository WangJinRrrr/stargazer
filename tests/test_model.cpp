#include <cstdio>
#include <string>
#include <vector>

#include "model/bgra.h"
#include "model/paths.h"
#include "model/rowformat.h"
#include "model/search.h"
#include "model/store.h"

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

// 含制表符、换行、反斜杠、emoji、中文的文件名必须逐字节还原
static void test_field_roundtrip() {
    const std::wstring cases[] = {
        L"",
        L"普通中文路径",
        L"C:\\Users\\wjr\\文档\\a b.txt",
        L"含\t制表符",
        L"含\n换行",
        L"含|竖线",
        L"|t 字面量（竖线加字母 t）",
        L"emoji \U0001F600 混合 CJK",
        L"末尾单个竖线|",
    };
    for (const auto& c : cases) {
        CHECK_EQ(sg::unescape_field(sg::escape_field(c)), c);
    }
}

// 回归：Windows 路径里的 \n / \t 不能被当成转义序列。
// 这是用反斜杠做转义符时的真 bug：C:\\new -> "C:" + 换行 + "ew"。
static void test_windows_paths_need_no_escaping() {
    const std::wstring paths[] = {
        L"C:\\Windows\\notepad.exe",   // 含 \n（from \notepad）
        L"C:\\temp\\a.txt",            // 含 \t
        L"D:\\new folder",              // 含 \n
        L"D:\\tools\\bin",              // 含 \t
        L"\\\\server\\share\\x",         // UNC
    };
    for (const auto& p : paths) {
        CHECK_EQ(sg::escape_field(p), p);  // 路径不需要任何转义
        CHECK_EQ(sg::unescape_field(p), p);
    }

    // 整行层面也要保证：写进去再读回来，路径一字不差
    std::vector<std::wstring> row = { L"常用", L"记事本", L"C:\\Windows\\notepad.exe", L"", L"", L"" };
    std::vector<std::wstring> back;
    CHECK(sg::split_row(sg::join_row(row), 6, back));
    CHECK_EQ(back[2], std::wstring(L"C:\\Windows\\notepad.exe"));
}

static void test_row_roundtrip() {
    std::vector<std::wstring> row = { L"常用", L"名字\t带制表符", L"D:\\a\\b.lnk",
                                      L"--arg \"x y\"", L"", L"C:\\ico\\i.ico" };
    std::vector<std::wstring> back;
    CHECK(sg::split_row(sg::join_row(row), 6, back));
    CHECK(back == row);
}

// 字段数不对的行跳过并计数，不使整份文件失败
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

// 混入坏行后其余记录照常加载 —— 在 test_boxes_ghost_lines 里覆盖。
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

// 回归：空盒子必须能落盘并读回来（否则新建的盒子重启后就消失）
static void test_empty_box_roundtrip() {
    std::vector<sg::Box> boxes(2);
    boxes[0].name = L"新盒子";  // 完全没有条目
    boxes[1].name = L"归档";
    boxes[1].items.push_back({ L"文档", L"C:\\temp" });

    int bad = 0;
    auto back = sg::parse_boxes(sg::serialize_boxes(boxes), bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(back.size(), size_t{2});
    if (back.size() == 2) {  // 尺寸不对时不再索引，让 RED 打印失败而不是崩掉
        CHECK_EQ(back[0].name, std::wstring(L"新盒子"));
        CHECK_EQ(back[0].items.size(), size_t{0});  // 空盒子保住了，但没多出幽灵条目
        CHECK_EQ(back[1].items.size(), size_t{1});
        CHECK_EQ(back[1].items[0].path, std::wstring(L"C:\\temp"));
    }
}

// 手改 boxes.txt：空盒子名 / 空路径的行不产生幽灵条目，且被计入坏行
static void test_boxes_ghost_lines() {
    // 第 1 行正常；第 2 行空路径；第 3 行空盒子名；第 4 行是空盒子的占位行
    const std::wstring text =
        L"工作\t有路径\tC:\\temp\n"
        L"工作\t空路径\t\n"
        L"\t无名盒\tC:\\a\n"
        L"空盒\t\t\n";
    int bad = 0;
    auto back = sg::parse_boxes(text, bad);
    CHECK_EQ(bad, 2);  // 空路径与空盒子名那两行算坏行
    CHECK_EQ(back.size(), size_t{2});
    if (back.size() == 2) {
        CHECK_EQ(back[0].name, std::wstring(L"工作"));
        CHECK_EQ(back[0].items.size(), size_t{1});  // 只有带路径的那行产生条目
        if (back[0].items.size() == 1) {
            CHECK_EQ(back[0].items[0].path, std::wstring(L"C:\\temp"));
        }
        CHECK_EQ(back[1].name, std::wstring(L"空盒"));
        CHECK_EQ(back[1].items.size(), size_t{0});  // 占位行只建盒子不建条目
    }
}

// 拖入分发：同一路径允许重复添加，不自动去重；
// 重复添加后每条都能单独删除（按条删，不按路径删），不会产生删不掉的项。
static void test_box_add_paths() {
    // ① boxes 为空时先建盒子（拖到空界面也应能用）
    std::vector<sg::Box> boxes;
    const size_t n1 = sg::box_add_paths(boxes, 0, { L"C:\\Windows\\notepad.exe" });
    CHECK_EQ(n1, size_t{1});
    CHECK_EQ(boxes.size(), size_t{1});
    CHECK_EQ(boxes[0].items.size(), size_t{1});
    CHECK_EQ(boxes[0].items[0].name, std::wstring(L"notepad.exe"));  // 显示名 = 文件名
    CHECK_EQ(boxes[0].items[0].path, std::wstring(L"C:\\Windows\\notepad.exe"));

    // ② 同一盒子内重复添加：两条独立条目，各自可删
    const size_t n2 = sg::box_add_paths(boxes, 0, { L"C:\\Windows\\notepad.exe" });
    CHECK_EQ(n2, size_t{1});
    CHECK_EQ(boxes[0].items.size(), size_t{2});
    boxes[0].items.erase(boxes[0].items.begin());
    CHECK_EQ(boxes[0].items.size(), size_t{1});  // 删掉一条，另一条还在

    // ③ 同一路径进两个盒子：互不影响
    boxes.push_back(sg::Box{ L"网盘", {} });
    const size_t n3 = sg::box_add_paths(boxes, 1, { L"D:\\\u7f51\u76d8\\Movies", L"" });
    CHECK_EQ(n3, size_t{1});  // 空路径被跳过
    CHECK_EQ(boxes[1].items.size(), size_t{1});
    CHECK_EQ(boxes[0].items.size(), size_t{1});  // 另一个盒子不动

    // ④ 越界下标夹紧到最后一个盒子，而不是崩或丢
    const size_t n4 = sg::box_add_paths(boxes, 99, { L"C:\\a.txt" });
    CHECK_EQ(n4, size_t{1});
    CHECK_EQ(boxes[1].items.size(), size_t{2});
}

// 拖到盒子标签换盒：条目换盒后两边都能正常显示，越界/同盒不产生副作用
static void test_box_move_item() {
    std::vector<sg::Box> boxes(2);
    boxes[0].name = L"A";
    boxes[1].name = L"B";
    boxes[0].items.push_back({ L"one", L"C:\\1.txt" });
    boxes[0].items.push_back({ L"two", L"C:\\2.txt" });

    CHECK(sg::box_move_item(boxes, 0, 1, 1));  // 移第二条：用非 0 下标才能抓到“删错下标”
    CHECK_EQ(boxes[0].items.size(), size_t{1});
    CHECK_EQ(boxes[0].items[0].name, std::wstring(L"one"));  // 留下的必须是第一条
    CHECK_EQ(boxes[1].items.size(), size_t{1});
    CHECK_EQ(boxes[1].items[0].path, std::wstring(L"C:\\2.txt"));
    CHECK_EQ(boxes[1].items[0].name, std::wstring(L"two"));  // 名字随条目一起走

    // 同盒 / 越界：不动
    CHECK(!sg::box_move_item(boxes, 0, 0, 0));
    CHECK(!sg::box_move_item(boxes, 0, 5, 1));
    CHECK(!sg::box_move_item(boxes, 0, 0, 9));
    CHECK(!sg::box_move_item(boxes, -1, 0, 1));
    CHECK_EQ(boxes[0].items.size(), size_t{1});
    CHECK_EQ(boxes[1].items.size(), size_t{1});
}

// 盒内拖拽换位置：落点即最终下标，占着落点的那条与中间的一起朝反方向挪
static void test_box_move_within() {
    sg::Box box;
    box.name = L"A";
    for (int i = 1; i <= 4; ++i)
        box.items.push_back({ L"n" + std::to_wstring(i), L"C:\\" + std::to_wstring(i) + L".txt" });

    CHECK(sg::box_move_within(box, 0, 2));       // 往后拖：1 落到 2 号格
    CHECK_EQ(box.items[2].name, std::wstring(L"n1"));  // 最终下标 == 落点
    CHECK_EQ(box.items[0].name, std::wstring(L"n2"));  // 中间的朝前挪一格
    CHECK_EQ(box.items[1].name, std::wstring(L"n3"));
    CHECK_EQ(box.items[3].name, std::wstring(L"n4"));  // 落点之后的没被碰

    CHECK(sg::box_move_within(box, 3, 0));       // 往前拖：n4 落到 0 号格
    CHECK_EQ(box.items[0].name, std::wstring(L"n4"));
    CHECK_EQ(box.items[1].name, std::wstring(L"n2"));
    CHECK_EQ(box.items[2].name, std::wstring(L"n3"));
    CHECK_EQ(box.items[3].name, std::wstring(L"n1"));
    CHECK_EQ(box.items.size(), size_t{4});       // 只挪位置，不丢不重

    // 原地 / 越界：不动。空白格（下标 >= size）不算位置，否则会把条目甩到尾部
    CHECK(!sg::box_move_within(box, 0, 0));
    CHECK(!sg::box_move_within(box, -1, 2));
    CHECK(!sg::box_move_within(box, 2, 4));
    CHECK_EQ(box.items[0].name, std::wstring(L"n4"));
}

// 改名不能造出重名盒子 —— parse_boxes 对同名盒子是“合并”，
// 允许重名会在下次启动时静默把两个盒子并成一个（组织关系丢了）。

// 筛选返回的是真实下标，顺序不变。打开/删除用这个下标，不会指到被藏起来的条目。
static void test_box_visible() {
    std::vector<sg::BoxItem> items = {
        { L"季度报告", L"D:\\docs\\Report.PDF" },
        { L"备忘", L"D:\\docs\\notes.txt" },
        { L"封面", L"D:\\img\\cover.png" },
    };
    CHECK_EQ(sg::box_visible(items, L"").size(), size_t{3});
    const auto rep = sg::box_visible(items, L"rep");  // 文件名，大小写不敏感
    CHECK_EQ(rep.size(), size_t{1});
    CHECK_EQ(rep[0], 0);
    const auto by_name = sg::box_visible(items, L"备忘");
    CHECK_EQ(by_name.size(), size_t{1});
    CHECK_EQ(by_name[0], 1);
    CHECK_EQ(sg::box_visible(items, L"zzz").size(), size_t{0});
    // 显示名（季度报告）和文件名（cover.png）都能命中，顺序保持原样
    const auto by_display = sg::box_visible(items, L"报告");
    CHECK_EQ(by_display.size(), size_t{1});
    CHECK_EQ(by_display[0], 0);
    const auto by_file = sg::box_visible(items, L"cov");
    CHECK_EQ(by_file.size(), size_t{1});
    CHECK_EQ(by_file[0], 2);
}

static void test_box_name_taken() {
    std::vector<sg::Box> boxes(2);
    boxes[0].name = L"A";
    boxes[1].name = L"B";
    CHECK(sg::box_name_taken(boxes, L"A", 1));   // 别的盒子已经叫 A
    CHECK(!sg::box_name_taken(boxes, L"A", 0));  // 改的就是自己：不算冲突
    CHECK(!sg::box_name_taken(boxes, L"C", 0));
    CHECK(!sg::box_name_taken(boxes, L"", 0));   // 空名不算占用（调用方本来就拒绝空名）
}

static void test_parent_path() {
    CHECK_EQ(sg::parent_path(L"C:\\a\\b"), std::wstring(L"C:\\a"));
    CHECK_EQ(sg::parent_path(L"C:\\a\\b\\"), std::wstring(L"C:\\a"));
    CHECK_EQ(sg::parent_path(L"C:\\a"), std::wstring(L"C:\\"));
    CHECK_EQ(sg::parent_path(L"C:\\"), std::wstring(L""));            // 已在根
    CHECK_EQ(sg::parent_path(L"D:/x/y/"), std::wstring(L"D:/x"));      // 正斜杠同样处理
    CHECK_EQ(sg::parent_path(L"a"), std::wstring(L""));                 // 相对路径没有上一级
    CHECK_EQ(sg::parent_path(L"\\\\server\\share"), std::wstring(L""));  // UNC 根不再往上
    CHECK_EQ(sg::parent_path(L"\\\\server\\share\\dir"), std::wstring(L"\\\\server\\share"));
    CHECK_EQ(sg::append_name(L"C:\\a", L"b"), std::wstring(L"C:\\a\\b"));
    CHECK_EQ(sg::append_name(L"C:\\a\\", L"b"), std::wstring(L"C:\\a\\b"));
    CHECK_EQ(sg::append_name(L"", L"b"), std::wstring(L"b"));
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

int main() {
    test_field_roundtrip();
    test_windows_paths_need_no_escaping();
    test_row_roundtrip();
    test_bad_row_skipped();
    test_empty_fields_and_crlf();
    test_build_text();
    test_normalize_key();
    test_file_name_and_ext();
    test_join_path();
    test_contains_ci();
    test_natural_compare();
    test_boxes_roundtrip();
    test_empty_box_roundtrip();
    test_boxes_ghost_lines();
    test_box_add_paths();
    test_box_move_item();
    test_box_move_within();
    test_box_visible();
    test_box_name_taken();
    test_parent_path();
    test_config();
    test_premultiply_bgra();

    if (g_failed == 0) {
        std::printf("OK: test_model 全部通过\n");
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    return 1;
}
