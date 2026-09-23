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
