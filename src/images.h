#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>

namespace sg {

struct Renderer;

// 系统缩略图（IShellItemImageFactory）。工作线程取像素、UI 线程建位图；
// 未命中时投递后台请求并返回 nullptr（本帧画占位），完成后 PostMessage(notify, WM_APP_IMAGE_READY)。
// LRU 上限 32 张（约 4.7MB）。
ID2D1Bitmap* images_get(Renderer& r, const std::wstring& path);

bool images_init(HWND notify_hwnd);
void images_shutdown();

// 设备丢失：丢弃位图、保留像素，下一帧按需重建
void images_on_device_lost();

// 清掉“取图失败”的记忆（呼出时调）：这样把文件改回来再呼出就能恢复预览，
// 同时避免在断网盘上每次重绘都重新去问 Shell。
void images_forget_failures();

size_t images_count();

}  // namespace sg
