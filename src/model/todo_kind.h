#pragma once

#include <string>
#include <vector>

namespace sg {

// 待办条目的类型。用户不需要选它：由粘贴进来的内容自动判定（见 todo_kind_from_text）。
enum class TodoKind { Text, Link, Image };

std::wstring todo_kind_to_string(TodoKind k);          // L"text" / L"link" / L"image"
TodoKind todo_kind_from_string(const std::wstring& s);  // 未知值 → Text（宽松，不丢内容）

// 去掉首尾空白后以 http:// 或 https:// 开头（大小写不敏感）→ Link；否则 Text
TodoKind todo_kind_from_text(const std::wstring& text);

// 图片扩展名白名单：png jpg jpeg gif bmp webp ico tif tiff（大小写不敏感）
bool is_image_path(const std::wstring& path);

// path 是否位于 images_dir 下（我们要自己管的“图片副本”）。大小写不敏感，
// 且必须是 images_dir + 分隔符 开头（否则 ...\images2\ 会被误判）
bool todo_is_owned_copy(const std::wstring& images_dir, const std::wstring& path);

// 该副本是否还被别的条目引用（other_attaches = 除待删条目外其余条目的 attach）
bool todo_copy_still_used(const std::vector<std::wstring>& other_attaches,
                          const std::wstring& path);

}  // namespace sg
