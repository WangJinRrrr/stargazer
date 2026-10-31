#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sg {

// 文件系统工作线程。只做存在性校验：盒子里的条目是路径引用，
// 断开的网盘上 GetFileAttributes 可能要几秒，不能放在 UI 线程。
// 完成时向 notify 窗口 PostMessage(WM_APP_FS_CHECKED)，UI 线程再 fs_drain()。
bool fs_init(HWND notify_hwnd);
void fs_shutdown();

// 投递一批路径；同一路径只排一次。结果在 fs_drain() 里逐条回调。
void fs_check_paths(const std::vector<std::wstring>& paths,
                    std::function<void(const std::wstring& path, bool exists)> on_result);
// 在 UI 线程调用：把已完成的存在性结果逐条回调。
void fs_drain();

}  // namespace sg
