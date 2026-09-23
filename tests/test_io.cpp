#include <windows.h>
#include <shellapi.h>  // DragQueryFileW
#include <shlobj.h>

#include <cstdio>
#include <string>

#include "model/paths.h"
#include "dragdrop.h"
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

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto _a = (a);                                                     \
        auto _b = (b);                                                     \
        if (!(_a == _b)) {                                                 \
            std::printf("FAIL %s:%d  %s != %s\n", __FILE__, __LINE__, #a, #b); \
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

// (.lnk 解析的测试随启动板一起删除：收纳盒只把 .lnk 当作普通文件引用，不做解析。)

// CF_HDROP 构造：与资源管理器互通的关键格式，最容易错的是结尾的 NUL 数量、
// fWide 与 pFiles 偏移 —— 所以这里逐项断言，而不是“能粘进去就算过”。
static void test_make_hdrop() {
    const std::vector<std::wstring> paths = {
        L"C:\\Windows\\notepad.exe",
        L"D:\\\u7f51\u76d8\\\u6587\u6863\\",  // 中文 + 尾反斜杠（目录）
        L"C:\\Users\\wjr\\My Docs\\a b.txt",
    };
    HGLOBAL h = sg::make_hdrop(paths);
    CHECK(h != nullptr);
    if (!h) return;

    auto* df = static_cast<DROPFILES*>(::GlobalLock(h));
    CHECK(df != nullptr);
    if (!df) {
        ::GlobalFree(h);
        return;
    }
    CHECK_EQ(df->pFiles, static_cast<DWORD>(sizeof(DROPFILES)));  // 偏移必须是头大小
    CHECK(df->fWide != FALSE);                                    // 必须是宽字符

    // 用系统 API 读回（比手算偏移可靠）
    const HDROP drop = static_cast<HDROP>(h);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    CHECK_EQ(static_cast<size_t>(count), paths.size());
    for (UINT i = 0; i < count && i < paths.size(); ++i) {
        const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
        std::wstring got(len + 1, L'\0');
        ::DragQueryFileW(drop, i, got.data(), len + 1);
        got.resize(len);
        CHECK_EQ(got, paths[i]);
    }

    // 结尾必须是双 NUL：最后一条路径的终止 NUL + 额外的收尾 NUL
    const auto* p =
        reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(df) + df->pFiles);
    size_t content = 0;
    for (const auto& s : paths) content += s.size() + 1;  // 每条路径各带一个 NUL
    CHECK_EQ(p[content - 1], L'\0');  // 最后一条路径的终止符
    CHECK_EQ(p[content], L'\0');      // 额外的收尾 NUL（双 NUL）
    // 第一个双 NUL 必须正好出现在这里，不能提前（否则中间会出现空路径条目）
    size_t n = 0;
    while (p[n] != L'\0' || p[n + 1] != L'\0') ++n;
    CHECK_EQ(n, content - 1);

    ::GlobalUnlock(h);
    ::GlobalFree(h);
}

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_roundtrip_utf8();
    test_read_missing_file();
    test_dir_writable_probe();
    test_atomic_write_leaves_no_tmp();
    test_make_hdrop();

    if (g_failed == 0) {
        std::printf("OK: test_io 全部通过\n");
        ::CoUninitialize();
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    ::CoUninitialize();
    return 1;
}
