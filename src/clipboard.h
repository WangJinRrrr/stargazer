#pragma once

#include <string>
#include <vector>

namespace sg {

// 把一批路径写进剪贴板：CF_HDROP（资源管理器能“粘贴文件”）+ CF_UNICODETEXT（纯文本路径）
// + “Preferred DropEffect”（告诉资源管理器这是复制还是剪切），三者同时给。
void clipboard_set_paths(const std::vector<std::wstring>& paths, bool move);

// 读剪贴板里的文件列表；没有 CF_HDROP 时返回空。
// move 从 “Preferred DropEffect” 读出（缺省按复制处理）。
std::vector<std::wstring> clipboard_get_paths(bool& move);

}  // namespace sg
