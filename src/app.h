#pragma once

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <string>

#include "persist.h"
#include "render.h"
#include "viewapi.h"

namespace sg {

// 控制窗口（隐藏，托盘 + 热键 + 单实例目标）与面板窗口用两个不同的窗口类，
// 这样 WM_CREATE 时不必猜自己是谁，也不必为了省内存把两个窗口混在一起。
extern const wchar_t* kCtlClass;
extern const wchar_t* kPanelClass;

constexpr UINT WM_APP_SHOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_ICON_READY = WM_APP + 3;
// 文件系统工作线程（线程 B）完成一批存在性校验
constexpr UINT WM_APP_FS_CHECKED = WM_APP + 4;
// 目录枚举完成（浏览视图）
constexpr UINT WM_APP_DIR_LOADED = WM_APP + 5;
// 文件操作（改名/新建/删除/粘贴）完成
constexpr UINT WM_APP_FS_OP_DONE = WM_APP + 6;

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
    // 内部拖拽（把条目拖到分组标签上换组）。不走 OLE：自写状态机更短也更可控。
    bool internal_drag = false;
    int drag_from = -1;  // filtered 下标
    AppState state;
};

bool app_init(App& app, HINSTANCE inst);
// 读 boxes.txt / config.txt / ui.txt 到 AppState
void app_load(App& app);
// data_dirty 时落盘并清标志
void app_save_if_dirty(App& app);
// 窗口尺寸与当前分组写到 ui.txt（呼出时从它还原）
void app_save_ui(App& app);
void app_show(App& app);
void app_hide(App& app);
void app_toggle(App& app);
void app_shutdown(App& app);

}  // namespace sg
