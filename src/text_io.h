#pragma once

#include <string>

namespace sg {

// 文件内容按 UTF-8（无 BOM）读写；失败返回 false。
// 读取不存在的文件返回 false 且 out 不变。
bool read_file_utf8(const std::wstring& path, std::wstring& out);

// 原子写：先写 <path>.tmp，再 MoveFileExW 覆盖；失败时清理 .tmp
bool write_file_utf8_atomic(const std::wstring& path, const std::wstring& text);

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

}  // namespace sg
