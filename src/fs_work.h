#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sg {

// 文件系统工作线程（线程 B）。与图标线程（线程 A）**分开**：
// 图标提取是毫秒级，存在性校验在断开的网盘上可能几秒一次。
// 放同一条队列里，一个断盘的路径会把它后面所有图标请求堵住。
bool fs_init(HWND notify_hwnd);
void fs_shutdown();

// 投递一批需要校验存在性的路径；同一路径只排一次。
// 结果通过 on_result 拿到 —— 但不是在调用时，而是在 UI 线程处理
// WM_APP_FS_CHECKED（调 fs_drain）时逐条回调。
void fs_check_paths(const std::vector<std::wstring>& paths,
                    std::function<void(const std::wstring& path, bool exists)> on_result);

// 在 UI 线程调用（WM_APP_FS_CHECKED 的处理里）：把已完成的结果逐条回调。
void fs_drain();

}  // namespace sg
