#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sg {

// 把一批路径写进剪贴板：CF_HDROP（资源管理器能“粘贴文件”）+ CF_UNICODETEXT（纯文本路径）
// + “Preferred DropEffect”（告诉资源管理器这是复制还是剪切），三者同时给。
void clipboard_set_paths(const std::vector<std::wstring>& paths, bool move);

// 读剪贴板里的文件列表；没有 CF_HDROP 时返回空。
// move 从 “Preferred DropEffect” 读出（缺省按复制处理）。
std::vector<std::wstring> clipboard_get_paths(bool& move);

// 把纯文本写进剪贴板（CF_UNICODETEXT）
void clipboard_set_text(const std::wstring& text);

// 读剪贴板里的纯文本（CF_UNICODETEXT）；没有文本返回 false
bool clipboard_get_text(std::wstring& out);

// 读剪贴板里的位图（CF_DIBV5 优先，退回 CF_DIB），拿到 DIB 原始字节（BITMAPINFOHEADER 起）；
// 剪贴板里没有位图返回 false
bool clipboard_get_image_dib(std::vector<uint8_t>& out);

}  // namespace sg
