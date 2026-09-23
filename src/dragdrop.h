#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sg {

bool dragdrop_init(HWND hwnd);
void dragdrop_shutdown(HWND hwnd);

// 拖入完成时回调，参数为绝对路径列表
void dragdrop_set_hook(std::function<void(const std::vector<std::wstring>&)> hook);

// 把 App::in_drag 的地址交给拖放层；外部拖拽悬停期间为 true（抑制悬停高亮更新）
void dragdrop_set_drag_flag(bool* flag);

}  // namespace sg
