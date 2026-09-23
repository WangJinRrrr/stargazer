#pragma once

#include <string>

namespace sg {

// 大小写不敏感的子串匹配；needle 为空时返回 true
// 注意：仅 ASCII 大小写折叠，中文按码点比较
bool contains_ci(const std::wstring& hay, const std::wstring& needle);

// 大小写不敏感的全等（用于“这个名字是否已被占用”——不能用 natural_compare：
// 那会把 "1" 与 "01" 判为相等，而 Windows 认为它们是两个不同的名字）
bool equals_ci(const std::wstring& a, const std::wstring& b);

// 自然序：数字段按数值比较，其余按 ASCII 大小写折叠后比较码点
// 返回 <0 / 0 / >0
int natural_compare(const std::wstring& a, const std::wstring& b);

}  // namespace sg
