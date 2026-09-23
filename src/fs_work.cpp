#include "fs_work.h"

#include <shellapi.h>

#include <algorithm>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "app.h"  // WM_APP_FS_CHECKED / WM_APP_DIR_LOADED / WM_APP_FS_OP_DONE
#include "model/dirlist.h"
#include "model/paths.h"
#include "png.h"

namespace sg {
namespace {

enum class Kind { Exists, ListDir, Rename, Mkdir, Delete, Paste, SaveImage };

struct Request {
    Kind kind = Kind::Exists;
    uint64_t id = 0;
    std::wstring path;  // Exists/ListDir/Mkdir/Delete；Rename 的 from；SaveImage 的目标
    std::wstring path2;  // Rename 的 to；Paste 的 dest_dir
    std::vector<std::wstring> many;  // Paste 的源
    std::vector<uint8_t> dib;        // SaveImage 的 DIB 字节
    bool flag = false;               // Delete: 回收站?；Paste: move?
};

struct ExistsResult {
    std::wstring path;
    bool exists = true;
};

struct DirResult {
    uint64_t id = 0;
    std::wstring dir;
    std::vector<FsEntry> entries;
    std::wstring error;  // 非空 = 枚举失败
};

struct OpResult {
    uint64_t id = 0;
    bool ok = true;
    std::wstring error;
    std::wstring note;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Request> g_queue;
std::set<std::wstring> g_queued;  // 存在性校验去重：重绘/多次呼出不会重复投递同一路径
std::deque<ExistsResult> g_done_exists;
std::deque<DirResult> g_done_dir;
std::deque<OpResult> g_done_op;
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

std::wstring win32_error_text(DWORD err) {
    wchar_t buf[256] = {};
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, err, 0, buf, 256, nullptr);
    std::wstring s(buf, n > 0 ? n : 0);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    if (s.empty()) s = L"错误码 " + std::to_wstring(err);
    return s;
}

// 双 NUL 结尾的路径列表（SHFileOperationW 要求）
std::vector<wchar_t> path_list(const std::vector<std::wstring>& paths) {
    std::vector<wchar_t> buf;
    for (const auto& p : paths) {
        buf.insert(buf.end(), p.begin(), p.end());
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');
    return buf;
}

bool run_shell_op(FILEOP_FLAGS flags, UINT func, const std::vector<std::wstring>& src,
                  const std::wstring& dst_dir, std::wstring& error) {
    SHFILEOPSTRUCTW op{};
    op.wFunc = func;
    std::vector<wchar_t> from = path_list(src);
    op.pFrom = from.data();
    std::vector<wchar_t> to;
    if (!dst_dir.empty()) {
        to.assign(dst_dir.begin(), dst_dir.end());
        if (to.back() != L'\\' && to.back() != L'/') to.push_back(L'\\');  // pTo 必须以分隔符结尾
        to.push_back(L'\0');
        to.push_back(L'\0');
        op.pTo = to.data();
    }
    op.fFlags = flags;
    ::SetLastError(0);
    const int rc = ::SHFileOperationW(&op);
    if (rc == 0 && !op.fAnyOperationsAborted) return true;
    error = rc == 0 ? L"操作被取消" : win32_error_text(static_cast<DWORD>(rc));
    return false;
}

void do_list_dir(const Request& req, DirResult& res) {
    res.id = req.id;
    res.dir = req.path;
    const std::wstring pattern = append_name(req.path, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        res.error = win32_error_text(::GetLastError());
        return;
    }
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        FsEntry e;
        e.name = name;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        res.entries.push_back(std::move(e));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    sort_dir_entries(res.entries);  // 目录在前 + 自然序（纯函数，已被 test_model 覆盖）
}

void do_op(const Request& req, OpResult& res) {
    res.id = req.id;
    switch (req.kind) {
        case Kind::Rename: {
            if (!::MoveFileExW(req.path.c_str(), req.path2.c_str(), 0)) {
                res.ok = false;
                res.error = win32_error_text(::GetLastError());
            }
            break;
        }
        case Kind::Mkdir: {
            if (!::CreateDirectoryW(req.path.c_str(), nullptr)) {
                res.ok = false;
                res.error = win32_error_text(::GetLastError());
            }
            break;
        }
        case Kind::Delete: {
            const FILEOP_FLAGS flags =
                (req.flag ? FOF_ALLOWUNDO : 0) | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            res.ok = run_shell_op(flags, FO_DELETE, { req.path }, std::wstring(), res.error);
            break;
        }
        case Kind::Paste: {
            // 先剔除同名冲突：SHFileOperation 在 NOCONFIRMATION 下会直接覆盖，
            // 那等于静默毁掉目标目录里的同名文件。跳过并报告，不覆盖。
            std::vector<std::wstring> todo;
            int skipped = 0;
            const std::wstring dest = req.path2;
            for (const auto& src : req.many) {
                const std::wstring name = file_name(src);
                if (name.empty()) continue;
                const std::wstring target = append_name(dest, name);
                if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ++skipped;
                    continue;
                }
                todo.push_back(src);
            }
            if (!todo.empty()) {
                const FILEOP_FLAGS flags =
                    FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_ALLOWUNDO;
                res.ok = run_shell_op(flags, req.flag ? FO_MOVE : FO_COPY, todo, dest, res.error);
            }
            if (skipped > 0) res.note = L"跳过 " + std::to_wstring(skipped) + L" 个同名文件";
            break;
        }
        case Kind::SaveImage: {
            // 目录可能被用户手删过：先建出来再写
            ::CreateDirectoryW(parent_path(req.path).c_str(), nullptr);
            res.ok = png_encode_dib(req.path, req.dib, res.error);
            break;
        }
        default:
            break;
    }
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

        UINT msg = WM_APP_FS_CHECKED;
        ExistsResult er;
        DirResult dr;
        OpResult orr;
        switch (req.kind) {
            case Kind::Exists:
                er.path = req.path;
                er.exists = path_exists(req.path);
                msg = WM_APP_FS_CHECKED;
                break;
            case Kind::ListDir:
                do_list_dir(req, dr);
                msg = WM_APP_DIR_LOADED;
                break;
            default:
                do_op(req, orr);
                msg = WM_APP_FS_OP_DONE;
                break;
        }

        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (req.kind == Kind::Exists) {
                g_queued.erase(req.path);
                g_done_exists.push_back(std::move(er));
            } else if (req.kind == Kind::ListDir) {
                g_done_dir.push_back(std::move(dr));
            } else {
                g_done_op.push_back(std::move(orr));
            }
            notify = g_notify;
        }
        // 只通知“有结果了”，不带索引：UI 线程按 requestId 取，过期结果不会填错槽位
        if (notify) ::PostMessageW(notify, msg, 0, 0);
    }
}

// ── 目录监视（浏览视图的自动刷新）──
// 一条长驻线程阻塞在 ReadDirectoryChangesW 上，不能塞进上面那条工作线程：
// 它一阻塞就是无限期，存在性校验与文件操作会全部堵在后面。
std::mutex g_watch_mu;
std::wstring g_watch_dir;  // 想监视的目录（空 = 不监视）
HANDLE g_watch_wake = nullptr;  // 重定向信号（manual reset）
HANDLE g_watch_quit = nullptr;  // 退出信号（manual reset）
std::thread g_watch_thread;

void watch_main() {
    // 一次复制/解压会连着发成百条通知，按寂静期合并：只通知 UI 重载一次
    constexpr ULONGLONG kCoalesceMs = 300;
    std::vector<BYTE> buf(8 * 1024);
    std::wstring current;
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE ov_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov_ev) return;  // 连事件都建不出来：不监视就是了（没它会在 Wait 上转圈烧 CPU）
    OVERLAPPED ov{};
    ov.hEvent = ov_ev;
    ULONGLONG last_notify = 0;

    for (;;) {
        if (::WaitForSingleObject(g_watch_quit, 0) == WAIT_OBJECT_0) break;

        std::wstring want;
        {
            std::lock_guard<std::mutex> lk(g_watch_mu);
            want = g_watch_dir;
        }
        if (want != current || dir == INVALID_HANDLE_VALUE) {
            if (dir != INVALID_HANDLE_VALUE) {
                ::CancelIoEx(dir, nullptr);  // 挂着的读作废（结果丢弃）
                ::CloseHandle(dir);
                dir = INVALID_HANDLE_VALUE;
            }
            current = want;
        }
        if (dir == INVALID_HANDLE_VALUE && !current.empty()) {
            dir = ::CreateFileW(current.c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                nullptr);
        }
        if (dir == INVALID_HANDLE_VALUE) {
            // 没东西可监视（视图切走了）/ 打不开（网盘没连、权限不足）：
            // 等重定向信号；打不开时每 5 秒重试一次，网络盘挂上来能自己接上
            const DWORD waitMs = current.empty() ? INFINITE : 5000;
            HANDLE idle[2] = { g_watch_wake, g_watch_quit };
            const DWORD r = ::WaitForMultipleObjects(2, idle, FALSE, waitMs);
            if (r == WAIT_OBJECT_0 + 1) break;  // 退出
            if (r == WAIT_OBJECT_0) ::ResetEvent(g_watch_wake);
            continue;
        }

        ::ResetEvent(ov_ev);
        DWORD got = 0;
        if (!::ReadDirectoryChangesW(dir, buf.data(), static_cast<DWORD>(buf.size()), FALSE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                     &got, &ov, nullptr)) {
            ::CloseHandle(dir);
            dir = INVALID_HANDLE_VALUE;
            continue;
        }

        HANDLE waits[2] = { ov_ev, g_watch_wake };
        if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            ::CancelIoEx(dir, &ov);  // 换目录 / 退出：取消这次读，回上层重开
            ::ResetEvent(g_watch_wake);
            continue;
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now - last_notify < kCoalesceMs) continue;
        last_notify = now;
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            notify = g_notify;
        }
        // 只通知“目录变了”，不自己去枚举：UI 线程用 browse_on_dir_changed 重载，
        // 顺带走它那套 requestId 与防抖，监视线程不与工作线程抢磁盘
        if (notify) ::PostMessageW(notify, WM_APP_DIR_CHANGED, 0, 0);
    }

    if (dir != INVALID_HANDLE_VALUE) {
        ::CancelIoEx(dir, nullptr);
        ::CloseHandle(dir);
    }
    ::CloseHandle(ov_ev);
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

uint64_t fs_next_op_id() {
    static std::atomic<uint64_t> next{0};
    return ++next;  // 从 1 开始：0 永远不会匹配到真结果
}

void fs_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_quit = true;
        g_notify = nullptr;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();

    // 监视线程要先信号退出再 join：它可能正阻塞在 ReadDirectoryChangesW 上。
    // 必须在释放 g_mu 之后做 —— watch_main 自己会取 g_mu 读通知窗口，抱着锁 join 会死锁。
    if (g_watch_quit) ::SetEvent(g_watch_quit);
    if (g_watch_wake) ::SetEvent(g_watch_wake);
    if (g_watch_thread.joinable()) g_watch_thread.join();
    if (g_watch_wake) {
        ::CloseHandle(g_watch_wake);
        g_watch_wake = nullptr;
    }
    if (g_watch_quit) {
        ::CloseHandle(g_watch_quit);
        g_watch_quit = nullptr;
    }

    std::lock_guard<std::mutex> lk(g_mu);
    {
        std::lock_guard<std::mutex> wl(g_watch_mu);
        g_watch_dir.clear();
    }
    g_queue.clear();
    g_queued.clear();
    g_done_exists.clear();
    g_done_dir.clear();
    g_done_op.clear();
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
            Request r;
            r.kind = Kind::Exists;
            r.path = p;
            g_queue.push_back(std::move(r));
            queued_now = true;
        }
    }
    if (queued_now) g_cv.notify_one();
}

void fs_drain() {
    std::deque<ExistsResult> done;
    std::function<void(const std::wstring&, bool)> cb;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        done.swap(g_done_exists);
        cb = g_cb;
    }
    if (!cb) return;
    for (const auto& r : done) cb(r.path, r.exists);
}

void fs_list_dir(const std::wstring& dir, uint64_t request_id) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;
        // 只保留最新一次枚举：目录自动刷新会在一个大复制/解压期间反复投递，
        // 过期的那些结果 UI 本来就会按 requestId 丢掉，留在队列里只会把后面的
        // 文件操作堵在一串白跑的目录扫描后面（同一时刻只有一个浏览视图）
        g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(),
                                     [](const Request& r) { return r.kind == Kind::ListDir; }),
                      g_queue.end());
        Request r;
        r.kind = Kind::ListDir;
        r.path = dir;
        r.id = request_id;
        g_queue.push_back(std::move(r));
        queued = true;
    }
    if (queued) g_cv.notify_one();
}

void fs_watch_dir(const std::wstring& dir) {
    // 路径没变就不打扰监视线程：browse_refresh 每次刷新都会调到这里，
    // 否则每次重载都要重开一次目录句柄
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(g_watch_mu);
        if (g_watch_dir == dir) return;
        g_watch_dir = dir;
        start = !g_watch_thread.joinable();
    }
    if (start) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_worker.joinable()) return;  // 已经 fs_shutdown 过了
        }
        g_watch_wake = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_watch_quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_watch_thread = std::thread(watch_main);
    }
    if (g_watch_wake) ::SetEvent(g_watch_wake);
}

bool fs_take_dir(uint64_t request_id, std::wstring& dir, std::vector<FsEntry>& entries,
                 std::wstring& error) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_done_dir.begin(); it != g_done_dir.end(); ++it) {
        if (it->id != request_id) continue;
        dir = it->dir;
        entries = std::move(it->entries);
        error = it->error;
        g_done_dir.erase(it);
        return true;
    }
    return false;  // 过期结果就留在队列里，等真正的主人（或下次清空）
}

void fs_rename(const std::wstring& from, const std::wstring& to, uint64_t request_id) {
    Request r;
    r.kind = Kind::Rename;
    r.path = from;
    r.path2 = to;
    r.id = request_id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;
        g_queue.push_back(std::move(r));
    }
    g_cv.notify_one();
}

void fs_mkdir(const std::wstring& path, uint64_t request_id) {
    Request r;
    r.kind = Kind::Mkdir;
    r.path = path;
    r.id = request_id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;
        g_queue.push_back(std::move(r));
    }
    g_cv.notify_one();
}

void fs_delete(const std::wstring& path, bool recycle, uint64_t request_id) {
    Request r;
    r.kind = Kind::Delete;
    r.path = path;
    r.flag = recycle;
    r.id = request_id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;
        g_queue.push_back(std::move(r));
    }
    g_cv.notify_one();
}

void fs_paste(const std::vector<std::wstring>& srcs, const std::wstring& dest_dir, bool move,
              uint64_t request_id) {
    Request r;
    r.kind = Kind::Paste;
    r.many = srcs;
    r.path2 = dest_dir;
    r.flag = move;
    r.id = request_id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;
        g_queue.push_back(std::move(r));
    }
    g_cv.notify_one();
}

void fs_save_image(std::vector<uint8_t> dib, const std::wstring& dest_path, uint64_t request_id) {
    Request r;
    r.kind = Kind::SaveImage;
    r.dib = std::move(dib);  // 4K 截图十几 MB，不再拷一份
    r.path = dest_path;
    r.id = request_id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_worker.joinable()) return;
        g_queue.push_back(std::move(r));
    }
    g_cv.notify_one();
}

bool fs_take_op(uint64_t request_id, bool& ok, std::wstring& error, std::wstring& note) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_done_op.begin(); it != g_done_op.end(); ++it) {
        if (it->id != request_id) continue;
        ok = it->ok;
        error = it->error;
        note = it->note;
        g_done_op.erase(it);
        return true;
    }
    return false;
}

}  // namespace sg
