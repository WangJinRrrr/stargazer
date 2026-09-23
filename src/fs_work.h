#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "model/dirlist.h"  // DirEntry / sort_dir_entries

namespace sg {

// 目录里的一项：直接复用 model/dirlist 的 DirEntry（排序也是它提供的纯函数）
using FsEntry = DirEntry;

// 文件系统工作线程（线程 B）。与图标线程（线程 A）**分开**：
// 图标提取是毫秒级，存在性校验与目录枚举在断开的网盘上可能几秒一次。
// 放同一条队列里，一个断盘的路径会把它后面所有图标请求堵住。
//
// 所有请求都是异步的：完成时向 registry 的窗口 PostMessage。
//   - 存在性校验 / 目录枚举完成 -> WM_APP_FS_CHECKED / WM_APP_DIR_LOADED
//   - 文件操作完成           -> WM_APP_FS_OP_DONE
// UI 线程用 fs_take_* 取结果，并用 requestId 丢弃过期结果（用户可能已经切目录/切盒子）。
bool fs_init(HWND notify_hwnd);
void fs_shutdown();

// --- 存在性校验（收纳盒用）---
// 投递一批需要校验存在性的路径；同一路径只排一次。结果在 fs_drain() 里逐条回调。
void fs_check_paths(const std::vector<std::wstring>& paths,
                    std::function<void(const std::wstring& path, bool exists)> on_result);
// 在 UI 线程调用（WM_APP_FS_CHECKED 的处理里）：把已完成的存在性结果逐条回调。
void fs_drain();

// --- 目录枚举（浏览视图用）---
void fs_list_dir(const std::wstring& dir, uint64_t request_id);
// UI 线程：取回一次枚举结果。request_id 不匹配（过期）时返回 false，并丢弃该结果。
bool fs_take_dir(uint64_t request_id, std::wstring& dir, std::vector<FsEntry>& entries,
                 std::wstring& error);

// --- 文件操作（浏览视图用；都在工作线程执行）---
void fs_rename(const std::wstring& from, const std::wstring& to, uint64_t request_id);
void fs_mkdir(const std::wstring& path, uint64_t request_id);
// recycle=true 走回收站（可撤销）；false 永久删除
void fs_delete(const std::wstring& path, bool recycle, uint64_t request_id);
// 把 srcs 复制/移动到 dest_dir。同名冲突**跳过**（不覆盖），并在 note 里报告跳过了几个。
void fs_paste(const std::vector<std::wstring>& srcs, const std::wstring& dest_dir, bool move,
              uint64_t request_id);

// 全局单调的操作号：浏览视图与待办的图片落盘共用同一个池。
// 各自从 0 开始递增会撞号，而 fs_take_op 只认数字 → 会取到对方的结果。
uint64_t fs_next_op_id();

// 把剪贴板位图存成 PNG（在工作线程做：4K 截图编码可达上百毫秒，不能卡 UI）。
// dest_path 的目录不存在时会先建。
void fs_save_image(std::vector<uint8_t> dib, const std::wstring& dest_path, uint64_t request_id);

// UI 线程：取回一次文件操作结果。request_id 不匹配时返回 false，并丢弃该结果。
// note 用来带“跳过 N 个同名文件”这类非致命信息（可空）。
bool fs_take_op(uint64_t request_id, bool& ok, std::wstring& error, std::wstring& note);

}  // namespace sg
