#include "fs_work.h"

#include <shellapi.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "app.h"  // WM_APP_FS_CHECKED / WM_APP_DIR_LOADED / WM_APP_FS_OP_DONE
#include "model/dirlist.h"
#include "model/paths.h"

namespace sg {
namespace {

enum class Kind { Exists, ListDir, Rename, Mkdir, Delete, Paste };

struct Request {
    Kind kind = Kind::Exists;
    uint64_t id = 0;
    std::wstring path;           // Exists / ListDir / Mkdir / Delete；Rename 的 from
    std::wstring path2;          // Rename 的 to；Paste 的 dest_dir
    std::vector<std::wstring> many;  // Paste 的源
    bool flag = false;           // Delete: 回收站?；Paste: move?
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
        Request r;
        r.kind = Kind::ListDir;
        r.path = dir;
        r.id = request_id;
        g_queue.push_back(std::move(r));
        queued = true;
    }
    if (queued) g_cv.notify_one();
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
