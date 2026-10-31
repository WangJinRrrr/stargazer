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

// 全局呼出热键的默认值，以及它在 config.txt 里的键名。
// 注册时优先用 config 里的值，失败就退回默认值（见 app_set_hotkey）。
constexpr UINT kDefaultHotkeyMods = MOD_CONTROL | MOD_SHIFT;
constexpr UINT kDefaultHotkeyKey = VK_SPACE;
constexpr const wchar_t* kHotkeyModsKey = L"hotkey_mods";
constexpr const wchar_t* kHotkeyKeyKey = L"hotkey_key";

constexpr UINT WM_APP_SHOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_ICON_READY = WM_APP + 3;
// 文件系统工作线程完成一批存在性校验
constexpr UINT WM_APP_FS_CHECKED = WM_APP + 4;

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
    // 当前生效的呼出热键（供托盘菜单的气泡与设置对话框显示）
    UINT hotkey_mods = kDefaultHotkeyMods;
    UINT hotkey_key = kDefaultHotkeyKey;
    // 模态小窗（设置热键）开着时为 true：期间忽略全局热键，也不开第二个模态窗
    bool modal = false;
    // 自己加载的托盘小图标（退出时 DestroyIcon）；用系统图标时保持 nullptr
    HICON tray_icon = nullptr;
    // 外部拖拽在窗口上悬停时为 true（抑制悬停高亮更新，不用于隐藏）
    bool in_drag = false;
    bool running = true;
    bool mouse_tracking = false;
    // 内部拖拽（拖到侧栏换盒子，或拖到另一张卡片换位置）。
    // 超过一点距离才算拖，普通点击不会把条目挪走。
    bool internal_drag = false;
    bool drag_pending = false;
    POINT drag_anchor{};
    int drag_from = -1;  // 盒子里的真实下标
    int press = 0;       // ShellControl：按钮按下、尚未松开
    // 拖侧栏右缘改宽度。锚点是按下时的逻辑坐标和当时的侧栏宽。
    bool side_resizing = false;
    float side_anchor_x = 0.f;
    float side_anchor_w = 0.f;
    // 呼出时内容从纸色淡入。1 = 淡入结束。
    float reveal = 1.f;
    unsigned long long reveal_ms = 0;
    AppState state;
};

bool app_init(App& app, HINSTANCE inst);
// 注册全局呼出热键。mods/key 任一为 0 时从 config.txt 取（取不到用默认值）。
// 先解掉旧注册再试新的（RegisterHotKey 同 id 重复注册会失败）；返回是否成功。
bool app_set_hotkey(App& app, UINT mods, UINT key);
// "Ctrl+Shift+Space" 这种可读名字（气泡与设置对话框用）
std::wstring hotkey_text(UINT mods, UINT key);
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
// 非致命提示（托盘气泡）。视图层用它报错，不弹模态框
void app_notify(App& app, const std::wstring& text);
// 投递一次存在性校验：只校验当前盒子的条目。结果按 path 回填 missing。
void app_request_fs_checks(App& app);

}  // namespace sg
