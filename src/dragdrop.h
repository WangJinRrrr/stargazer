#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sg {

// 构造 CF_HDROP（全局内存），返回 HDROP；失败返回 nullptr。
// 调用方在 DoDragDrop / SetClipboardData 之后不要再碰它（所有权已转移）。
// 路径必须是绝对路径：这是与资源管理器互通的格式。
HGLOBAL make_hdrop(const std::vector<std::wstring>& paths);

bool dragdrop_init(HWND hwnd);
void dragdrop_shutdown(HWND hwnd);

// 拖入完成时回调，参数为绝对路径列表
void dragdrop_set_hook(std::function<void(const std::vector<std::wstring>&)> hook);

// 把 App::in_drag 的地址交给拖放层；外部拖拽悬停期间为 true（抑制悬停高亮更新）
void dragdrop_set_drag_flag(bool* flag);

// TEMP(Task 8 删除)：直接调用已注册的拖入回调，用来在无法模拟 OLE 拖放的环境里
// 验证“拖入按当前视图分发”这段粘连代码。
void dragdrop_test_invoke(const std::vector<std::wstring>& paths);

}  // namespace sg
