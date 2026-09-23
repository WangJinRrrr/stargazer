#pragma once

#include <windows.h>

#include "persist.h"
#include "render.h"

namespace sg {

extern const wchar_t* kWindowClass;

constexpr UINT WM_APP_SHOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_ICON_READY = WM_APP + 3;

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    Paths paths;
    Renderer render;
    UINT hotkey_id = 1;  // RegisterHotKey 的 id
    bool hotkey_ok = false;
    // 外部拖拽在窗口上悬停时为 true（抑制悬停高亮更新，不用于隐藏）
    bool in_drag = false;
    bool running = true;
};

bool app_init(App& app, HINSTANCE inst);
void app_show(App& app);
void app_hide(App& app);
void app_toggle(App& app);
void app_shutdown(App& app);

LRESULT CALLBACK app_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace sg
