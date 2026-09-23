#pragma once

#include <string>
#include <vector>

namespace sg {

// 字段内转义：'\\' -> "\\\\"，'\t' -> "\\t"，'\n' -> "\\n"，'\r' 丢弃
std::wstring escape_field(const std::wstring& v);
std::wstring unescape_field(const std::wstring& v);

// 字段 -> 一行（Tab 连接）
std::wstring join_row(const std::vector<std::wstring>& fields);

// 解析一行；字段数不等于 expect 时返回 false（out 内容不定）
bool split_row(const std::wstring& line, size_t expect, std::vector<std::wstring>& out);

// 解析整份文本：空行跳过，字段数不符的行跳过并计入 bad
std::vector<std::vector<std::wstring>> parse_rows(const std::wstring& text, size_t expect, int& bad);

// 多行 -> 文本；每行以 '\n' 结尾，不含 BOM
std::wstring build_text(const std::vector<std::vector<std::wstring>>& rows);

}  // namespace sg
