#pragma once

#include <string>

namespace sg {

// 用于缓存键与去重：小写、'/'->'\\'、折叠重复分隔符、去尾分隔符（"c:\\" 保留）
std::wstring normalize_key(const std::wstring& path);

// 最后一段；以分隔符结尾时返回空
std::wstring file_name(const std::wstring& path);

// 小写扩展名，含点；无扩展名或以点开头返回空
std::wstring extension_of(const std::wstring& path);

std::wstring join_path(const std::wstring& dir, const std::wstring& name);

}  // namespace sg
