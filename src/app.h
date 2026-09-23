#pragma once

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <string>

#include "persist.h"
#include "render.h"
#include "viewapi.h"
#include "views/launcher.h"

namespace sg {

// 控制窗口（隐藏，托盘 + 热键 + 单实例目标）与面板窗口用两个不同的窗口类，
// 这样 WM_CREATE 时不必猜自己是谁，也不必为了省内存把两个窗口混在一起。
extern const wchar_t* kCtlClass;
extern const wchar_t* kPanelClass;

constexpr UINT WM_APP_SHOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_ICON_READY = WM_APP + 3;

struct App {
    HINSTANCE inst = nullptr;

    // ctl 从启动就存在（成本只有几十 KB），面板窗口与 D2D 首次呼出才建。
    // 实测：启动即建窗口+渲染目标时，仅驻留托盘也要 37MB 工作集。
    HWND ctl = nullptr;
    HWND panel = nullptr;

    Paths paths;
    Renderer render;
    UINT hotkey_id = 1;  // RegisterHotKey 的 id
    bool hotkey_ok = false;
    // 外部拖拽在窗口上悬停时为 true（抑制悬停高亮更新，不用于隐藏）
    bool in_drag = false;
    bool running = true;
    bool mouse_tracking = false;
    AppState state;
};

bool app_init(App& app, HINSTANCE inst);
// 读 launcher.txt / config.txt / ui.txt 到 AppState
void app_load(App& app);
// data_dirty 时落盘并清标志
void app_save_if_dirty(App& app);
void app_show(App& app);
void app_hide(App& app);
void app_toggle(App& app);
void app_shutdown(App& app);

}  // namespace sg
