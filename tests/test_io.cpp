#include <windows.h>

#include <cstdio>
#include <string>

#include "model/paths.h"
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
