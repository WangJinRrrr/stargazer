#include <windows.h>
#include <shellapi.h>  // DragQueryFileW
#include <shlobj.h>
#include <wincodec.h>  // 把编出的 PNG 解回来验证

#include <cstdio>
#include <string>
#include <vector>

#include "model/paths.h"
#include "clipboard.h"
#include "dragdrop.h"
#include "persist.h"
#include "png.h"
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

// 中文与特殊字符经 UTF-8 落盘再读回必须一致
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

// 不可写目录必须被探测出来
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

// 造一小段 DIB（纯红），bpp = 24/32，top_down 控制 biHeight 的正负
static std::vector<uint8_t> make_dib(int w, int h, int bpp, bool top_down) {
    const int stride = ((w * bpp + 31) / 32) * 4;
    std::vector<uint8_t> dib(sizeof(BITMAPINFOHEADER) + size_t(stride) * h, 0);
    auto* bi = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = w;
    bi->biHeight = top_down ? -h : h;  // 负 = 自上而下
    bi->biPlanes = 1;
    bi->biBitCount = static_cast<WORD>(bpp);
    bi->biCompression = BI_RGB;
    uint8_t* px = dib.data() + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = px + size_t(y) * stride + size_t(x) * (bpp / 8);
            p[0] = 0; p[1] = 0; p[2] = 255;  // BGR = 红
            if (bpp == 32) p[3] = 255;
        }
    }
    return dib;
}

// 四种位图变体都要能编出“能看”的 PNG（不黑块、不上下颠倒、不负片）
static void test_png_encode_dib() {
    const std::wstring dir = temp_dir();
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
        if (f) {
            CHECK(::fread(head.data(), 1, 8, f) == 8);
            ::fclose(f);
        }
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
                CHECK(SUCCEEDED(frame->CopyPixels(nullptr, 8 * 4,
                                                  static_cast<UINT>(rgba.size()), rgba.data())));
                CHECK(rgba[2] > 200);  // R
                CHECK(rgba[1] < 60);   // G
                CHECK(rgba[0] < 60);   // B
                CHECK_EQ(rgba[3], 255u);  // A：能看见（32bpp 里 alpha 写的就是 255）
            }
            if (frame) frame->Release();
            if (dec) dec->Release();
            fac->Release();
        }
        ::DeleteFileW(path.c_str());
    }

    // alpha 全 0 的 32bpp DIB（有些程序就是这么拷的，意思是“我不管 alpha”）：
    // 照抄会得到一张全透明 PNG（贴出来是白块）→ 必须当不透明处理
    {
        const std::wstring path = sg::join_path(dir, L"t32_zeroalpha.png");
        ::DeleteFileW(path.c_str());
        std::vector<uint8_t> dib = make_dib(8, 4, 32, false);
        const int stride = ((8 * 32 + 31) / 32) * 4;
        for (int y = 0; y < 4; ++y) {
            uint8_t* row = dib.data() + sizeof(BITMAPINFOHEADER) + static_cast<size_t>(y) * stride;
            for (int x = 0; x < 8; ++x) row[x * 4 + 3] = 0;
        }
        std::wstring err;
        CHECK(sg::png_encode_dib(path, dib, err));
        IWICImagingFactory* fac = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&fac)))) {
            IWICBitmapDecoder* dec = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &dec))) {
                dec->GetFrame(0, &frame);
            }
            if (frame) {
                std::vector<uint8_t> rgba(8u * 4u * 4u);
                CHECK(SUCCEEDED(frame->CopyPixels(nullptr, 8 * 4,
                                                  static_cast<UINT>(rgba.size()), rgba.data())));
                CHECK(rgba[2] > 200);
                CHECK_EQ(rgba[3], 255u);  // 不能是 0
            } else {
                CHECK(false);
            }
            if (frame) frame->Release();
            if (dec) dec->Release();
            fac->Release();
        }
        ::DeleteFileW(path.c_str());
    }

    // 坏输入不能崩：头都不够长
    std::wstring err;
    CHECK(!sg::png_encode_dib(sg::join_path(dir, L"bad.png"), std::vector<uint8_t>{ 1, 2, 3 },
                              err));
    CHECK(!err.empty());
}

// 剪贴板里没有位图时返回 false（同机运行，不依赖外部状态）
static void test_clipboard_image_read() {
    std::vector<uint8_t> dib;
    if (!sg::clipboard_get_image_dib(dib)) CHECK(dib.empty());
    std::wstring text;
    if (!sg::clipboard_get_text(text)) CHECK(text.empty());
}

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_roundtrip_utf8();
    test_read_missing_file();
    test_dir_writable_probe();
    test_atomic_write_leaves_no_tmp();
    test_make_hdrop();
    test_png_encode_dib();
    test_clipboard_image_read();

    if (g_failed == 0) {
        std::printf("OK: test_io 全部通过\n");
        ::CoUninitialize();
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    ::CoUninitialize();
    return 1;
}
