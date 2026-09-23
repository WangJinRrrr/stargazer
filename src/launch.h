#pragma once

#include <string>

#include "model/store.h"

namespace sg {

// 启动失败时 err 被填入用户可读的原因（用于托盘气泡，不弹 MessageBox）
bool launch_item(const LaunchItem& item, std::wstring* err);

// 解析 .lnk 的目标/参数/工作目录/图标；失败返回 false
bool resolve_lnk(const std::wstring& lnk_path, LaunchItem& out);

LaunchItem item_from_path(const std::wstring& path);

// 拖入/新建时用户已经给了名字，不能被文件名的自动推导覆盖
LaunchItem item_from_path_keep_name(const std::wstring& path, const std::wstring& name);

}  // namespace sg
