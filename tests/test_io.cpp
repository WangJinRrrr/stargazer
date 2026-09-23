#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

#include "model/paths.h"
#include "launch.h"
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

// .lnk 解析：真正造一个快捷方式再解析回来（拖入 .lnk 是主要添加方式，不能只靠手工验）
static void test_resolve_lnk() {
    IShellLinkW* link = nullptr;
    CHECK(SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&link))));
    if (!link) return;
    link->SetPath(L"C:\\Windows\\notepad.exe");
    link->SetArguments(L"--flag value");

    const std::wstring lnk = sg::join_path(temp_dir(), L"t.lnk");
    IPersistFile* file = nullptr;
    CHECK(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))));
    if (file) {
        CHECK(SUCCEEDED(file->Save(lnk.c_str(), TRUE)));
        file->Release();
    }
    link->Release();

    const sg::LaunchItem item = sg::item_from_path(lnk);
    CHECK_EQ(item.target, std::wstring(L"C:\\Windows\\notepad.exe"));
    CHECK_EQ(item.args, std::wstring(L"--flag value"));
    CHECK_EQ(item.name, std::wstring(L"t.lnk"));  // 名字暂用快捷方式文件名，用户可改名

    ::DeleteFileW(lnk.c_str());
}

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_roundtrip_utf8();
    test_read_missing_file();
    test_dir_writable_probe();
    test_atomic_write_leaves_no_tmp();
    test_resolve_lnk();

    if (g_failed == 0) {
        std::printf("OK: test_io 全部通过\n");
        ::CoUninitialize();
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    ::CoUninitialize();
    return 1;
}
