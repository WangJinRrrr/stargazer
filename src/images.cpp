#include "images.h"
#include <shellapi.h>       // SHCreateItemFromParsingName / IShellItemImageFactory
#include <shobjidl.h>       // IShellItemImageFactory / SIGDN / SIIGBF

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app.h"  // WM_APP_IMAGE_READY
#include "model/bgra.h"
#include "model/paths.h"
#include "render.h"

#include <cstdarg>

namespace sg {
namespace {


// 一张缩略图 192×192×4B ≈ 147KB，32 张 ≈ 4.7MB。
// ponytail: 缓存的键是规范化路径、不带 mtime —— 图片极少变，改图后重启即刷新；
// 若出现“改了图预览不变”的抱怨，再把 mtime 拼进键。
constexpr size_t kMaxEntries = 32;
constexpr LONG kThumbPx = 192;

struct Entry {
    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;
    ID2D1Bitmap* bitmap = nullptr;
    uint64_t last_used = 0;
};

struct Request {
    std::wstring key;
    std::wstring path;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::unordered_map<std::wstring, Entry> g_cache;
std::deque<Request> g_queue;
std::set<std::wstring> g_queued;
// 取图失败的 key：避免每次重绘都向 Shell 重试（断网盘上每次尝试可能要等好几秒）。
// 呼出时清一次，所以“把文件改回来再呼出”就能恢复预览。
std::set<std::wstring> g_failed;
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

// HBITMAP（缩略图服务给的 32bpp DIB）→ 预乘 BGRA
bool hbitmap_to_bgra(HBITMAP hbmp, std::vector<uint8_t>& px, int& w, int& h) {
    BITMAP bm{};
    if (!hbmp || !::GetObjectW(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return false;
    }
    w = bm.bmWidth;
    h = bm.bmHeight;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 负 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = ::GetDC(nullptr);
    px.assign(static_cast<size_t>(w) * h * 4, 0);
    const bool ok =
        ::GetDIBits(dc, hbmp, 0, static_cast<UINT>(h), px.data(), &bi, DIB_RGB_COLORS) != 0;
    ::ReleaseDC(nullptr, dc);
    if (!ok) return false;

    // 缩略图服务通常给不透明图（alpha 全 0）→ 当作不透明，否则会画成全透明
    bool has_alpha = false;
    for (size_t i = 3; i < px.size(); i += 4) {
        if (px[i] != 0) {
            has_alpha = true;
            break;
        }
    }
    if (!has_alpha) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    }
    premultiply_bgra(px.data(), px.size() / 4);
    return true;
}

// 取一张缩略图。失败返回 false（视图继续画占位色块，不弹任何框）
bool fetch_thumbnail(const std::wstring& path, std::vector<uint8_t>& px, int& w, int& h) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
        !factory) {
        return false;
    }
    SIZE want{ kThumbPx, kThumbPx };
    HBITMAP hbmp = nullptr;
    // 先要真缩略图；拿不到就退而求其次要一张缩放图（例如缩略图服务没开）
    HRESULT hr = factory->GetImage(want, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
    if (FAILED(hr) || !hbmp) {
        hr = factory->GetImage(want, SIIGBF_BIGGERSIZEOK, &hbmp);
    }
    factory->Release();
    if (FAILED(hr) || !hbmp) return false;

    const bool ok = hbitmap_to_bgra(hbmp, px, w, h);
    ::DeleteObject(hbmp);
    return ok;
}

void worker_main() {
    // 缩略图走 Shell，需要 STA（与 icons 线程不同：那边只调 SHGetFileInfo，用 MTA 即可）
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
        const bool ok = fetch_thumbnail(req.path, px, w, h);

        HWND notify = nullptr;
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
                g_failed.erase(req.key);
                evict_locked();
            } else {
                g_failed.insert(req.key);
            }
            notify = g_notify;
        }
        // 只通知“有结果了”，不带索引：视图按 key 查表，过期结果不会填错槽位
        if (notify) ::PostMessageW(notify, WM_APP_IMAGE_READY, 0, 0);
    }
    ::CoUninitialize();
}

}  // namespace

bool images_init(HWND notify_hwnd) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_worker.joinable()) return true;
    g_notify = notify_hwnd;
    g_quit = false;
    g_worker = std::thread(worker_main);
    return true;
}

void images_shutdown() {
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
    g_failed.clear();
}

ID2D1Bitmap* images_get(Renderer& r, const std::wstring& path) {
    if (path.empty()) return nullptr;
    const std::wstring key = normalize_key(path);
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
        if (queued_now) {
            if (g_failed.count(key) != 0) {
                // 已知取不到（且还没重新呼出过）：不再打扰 Shell
                g_queued.erase(key);
                return nullptr;
            }
            g_queue.push_back(Request{ key, path });
        }
    }
    if (queued_now) g_cv.notify_one();
    return nullptr;
}

void images_on_device_lost() {
    std::lock_guard<std::mutex> lk(g_mu);
    discard_bitmaps_locked();
}

void images_forget_failures() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_failed.clear();
}

size_t images_count() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cache.size();
}

}  // namespace sg
