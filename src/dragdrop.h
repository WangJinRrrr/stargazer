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

// 拖出：把 paths 以 CF_HDROP 交给系统做 OLE 拖放（会被资源管理器等接收方当成“粘贴文件”）。
// 内部跑嵌套消息循环，调用方需在此期间抑制悬停高亮（App::in_drag）。
// 返回 true 表示拖放被接收（Esc 取消或没有接收方时返回 false）。
bool dragdrop_begin_drag(HWND owner, const std::vector<std::wstring>& paths);

}  // namespace sg
