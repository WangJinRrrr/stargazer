#include "fs_work.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "app.h"  // WM_APP_FS_CHECKED

namespace sg {
namespace {


struct Request {
    std::wstring path;
};

struct Result {
    std::wstring path;
    bool exists = true;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Request> g_queue;
std::set<std::wstring> g_queued;  // 去重：重绘/多次呼出不会重复投递同一路径
std::deque<Result> g_done;
HWND g_notify = nullptr;
std::thread g_worker;
bool g_quit = false;
// ponytail: 单个消费者回调（后设的覆盖先设的）。当前只有收纳盒一个消费者，
// 且它按 path 查表、不认识就忽略，所以覆盖无害。出现第二个消费者时改成每条请求自带回调。
std::function<void(const std::wstring&, bool)> g_cb;

// 只有“明确的不存在”才算失效。断盘、权限不足等其它错误一律当作存在：
// 宁可显示正常，也不要因为一次读盘失败就让用户以为文件没了。
bool path_exists(const std::wstring& p) {
    if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD err = ::GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return false;
    return true;
}

void worker_main() {
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_quit || !g_queue.empty(); });
            if (g_quit && g_queue.empty()) break;
            req = std::move(g_queue.front());
            g_queue.pop_front();
        }

        const bool exists = path_exists(req.path);

        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_queued.erase(req.path);
            g_done.push_back(Result{ req.path, exists });
            notify = g_notify;
        }
        // 只通知“有结果了”，不带索引：UI 线程按 path 回填，过期结果不会填错槽位
        if (notify) ::PostMessageW(notify, WM_APP_FS_CHECKED, 0, 0);
    }
}

}  // namespace

bool fs_init(HWND notify_hwnd) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_worker.joinable()) return true;
    g_notify = notify_hwnd;
    g_quit = false;
    g_worker = std::thread(worker_main);
    return true;
}

void fs_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_quit = true;
        g_notify = nullptr;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();

    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.clear();
    g_queued.clear();
    g_done.clear();
    g_cb = nullptr;
}

void fs_check_paths(const std::vector<std::wstring>& paths,
                    std::function<void(const std::wstring&, bool)> on_result) {
    bool queued_now = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;  // 线程没起来（初始化失败）就当没有这个功能
        g_cb = std::move(on_result);
        for (const auto& p : paths) {
            if (p.empty()) continue;
            if (!g_queued.insert(p).second) continue;
            g_queue.push_back(Request{ p });
            queued_now = true;
        }
    }
    if (queued_now) g_cv.notify_one();
}

void fs_drain() {
    std::deque<Result> done;
    std::function<void(const std::wstring&, bool)> cb;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        done.swap(g_done);
        cb = g_cb;
    }
    if (!cb) return;
    for (const auto& r : done) cb(r.path, r.exists);
}

}  // namespace sg
