#include "icons.h"

#include <commctrl.h>      // commoncontrols.h 依赖这里的 IMAGELISTDRAWPARAMS / ILD_* 类型
#include <commoncontrols.h>  // IImageList、SHGetImageList
#include <shellapi.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app.h"  // WM_APP_ICON_READY
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

// HICON -> 预乘 BGRA。
// 不用 DrawIconEx：对掩码型图标它只写 RGB 不写 alpha，而且它写进哪个位图还依赖 DC 状态；
// 直接 GetDIBits 读 hbmColor（32bpp DDB 自带 alpha）更短也更可靠。
bool hicon_to_bgra(HICON icon, std::vector<uint8_t>& px, int& w, int& h) {
    ICONINFO ii{};
    if (!::GetIconInfo(icon, &ii)) return false;

    bool ok = false;
    BITMAP bm{};
    if (ii.hbmColor && ::GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
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
        px.assign(static_cast<size_t>(w) * h * 4, 0);
        ok = ::GetDIBits(screen, ii.hbmColor, 0, static_cast<UINT>(h), px.data(), &bi,
                         DIB_RGB_COLORS) != 0;

        if (ok) {
            bool has_alpha = false;
            for (size_t i = 3; i < px.size(); i += 4) {
                if (px[i] != 0) {
                    has_alpha = true;
                    break;
                }
            }
            if (!has_alpha) {
                // 掩码型图标：hbmMask 中位为 1 = 透明区
                std::vector<uint8_t> mask(static_cast<size_t>(w) * h * 4, 0);
                bool mask_ok = false;
                if (ii.hbmMask) {
                    BITMAPINFO mbi = bi;
                    mbi.bmiHeader.biWidth = w;
                    mbi.bmiHeader.biHeight = -h;
                    mask_ok = ::GetDIBits(screen, ii.hbmMask, 0, static_cast<UINT>(h), mask.data(),
                                          &mbi, DIB_RGB_COLORS) != 0;
                }
                for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                    // 掩码读不到时当作不透明，总比全透明强
                    px[i * 4 + 3] = (mask_ok && mask[i * 4] != 0) ? 0 : 255;
                }
            }
            premultiply_bgra(px.data(), px.size() / 4);
        }
        ::ReleaseDC(nullptr, screen);
    }

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

    if (!req.is_dir) {
        // 必须看返回值：失败时 sfi 是全零，而 iIcon==0 是合法索引，
        // 会把“按扩展名推断”这一级直接跳过并给出一个错误图标
        if (::SHGetFileInfoW(req.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) {
            if (HICON icon = icon_from_image_list(sfi.iIcon)) {
                const bool ok = hicon_to_bgra(icon, px, w, h);
                ::DestroyIcon(icon);
                if (ok) return true;
            }
        }
    }

    // 网盘/不存在的路径拿不到真图标时按扩展名推断（不触盘）
    ::SHGetFileInfoW(req.path.c_str(), attrs, &sfi, sizeof(sfi),
                     SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
    if (HICON icon = icon_from_image_list(sfi.iIcon)) {
        const bool ok = hicon_to_bgra(icon, px, w, h);
        ::DestroyIcon(icon);
        if (ok) return true;
    }

    if (HICON icon = ::LoadIconW(nullptr, IDI_APPLICATION)) {
        return hicon_to_bgra(icon, px, w, h);
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
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            notify = g_notify;
        }
        if (notify) ::PostMessageW(notify, WM_APP_ICON_READY, 0, 0);
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
    // 出锁再唤醒，避免工作线程醒来后立刻又阻塞在同一把锁上
    if (queued_now) g_cv.notify_one();
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
