#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>

namespace sg {
struct Renderer;

// 启动图标工作线程；notify_hwnd 会收到 WM_APP_ICON_READY
bool icons_init(HWND notify_hwnd);
void icons_shutdown();

// 命中则返回位图；未命中时投递后台请求并返回 nullptr（本帧画占位）
// key 用 sg::normalize_key(path)，目录额外加后缀 ":dir" 以区分同名文件与目录
ID2D1Bitmap* icons_get(Renderer& r, const std::wstring& path, bool is_dir);

// 设备丢失：丢弃位图保留像素，下一帧按需重建
void icons_on_device_lost();
void icons_clear();
size_t icons_count();

}  // namespace sg
