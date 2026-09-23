#pragma once

#include <string>

namespace sg {

// 用于缓存键与去重：小写、'/'->'\\'、折叠重复分隔符、去尾分隔符（"c:\\" 保留）
std::wstring normalize_key(const std::wstring& path);

// 最后一段；以分隔符结尾时返回空
std::wstring file_name(const std::wstring& path);

// 小写扩展名，含点；无扩展名或以点开头返回空
std::wstring extension_of(const std::wstring& path);

// 上一级目录。带尾分隔符时先去掉；
// "C:\\a\\b" -> "C:\\a"，"C:\\a" -> "C:\\"，"C:\\" -> ""（已在顶层），
// "\\server\share" -> ""（UNC 根不再往上），相对路径 "a" -> ""。
std::wstring parent_path(const std::wstring& path);

// 把条目名拼成 "dir\\name"；dir 为空时返回名字本身
std::wstring append_name(const std::wstring& dir, const std::wstring& name);

std::wstring join_path(const std::wstring& dir, const std::wstring& name);

}  // namespace sg
