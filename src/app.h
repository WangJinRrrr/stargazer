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
// 文件系统工作线程（线程 B）完成一批存在性校验
constexpr UINT WM_APP_FS_CHECKED = WM_APP + 4;
// 目录枚举完成（浏览视图）
constexpr UINT WM_APP_DIR_LOADED = WM_APP + 5;
// 文件操作（改名/新建/删除/粘贴）完成
constexpr UINT WM_APP_FS_OP_DONE = WM_APP + 6;
// 缩略图工作线程取到一张图（待办图片预览）
constexpr UINT WM_APP_IMAGE_READY = WM_APP + 7;

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
// 非致命提示（托盘气泡）。视图层用它报错，不弹模态框（spec §12）
void app_notify(App& app, const std::wstring& text);
// 投递一次存在性校验：当前盒子的条目 + 待办里的引用型图片。
// 结果按 path 一次回填 boxes 与 todos（fs_work 的回调是单槽设计，不能变成两个消费者）。
void app_request_fs_checks(App& app);

}  // namespace sg
