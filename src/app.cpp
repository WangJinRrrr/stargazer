#include "app.h"

#include <shellapi.h>
#include <wchar.h>

#include <algorithm>
#include <string>

#include "icons.h"
#include "dragdrop.h"
#include "launch.h"
#include "model/paths.h"
#include "text_io.h"

namespace sg {

const wchar_t* kCtlClass = L"StargazerCtl";
const wchar_t* kPanelClass = L"StargazerWnd";

static void launch_selected(App& app);  // 定义在下方，先声明（双击与 Enter 都要用）

namespace {

const UINT kTrayId = 1;
const UINT kDefaultHotkeyMods = MOD_CONTROL | MOD_SHIFT;
const UINT kDefaultHotkeyKey = VK_SPACE;

// 默认窗口尺寸（96 DPI 逻辑像素）
const int kDefaultW = 960;
const int kDefaultH = 620;

void add_tray_icon(App& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.ctl;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Stargazer — Ctrl+Shift+Space 呼出");
    ::Shell_NotifyIconW(NIM_ADD, &nid);
}

void remove_tray_icon(App& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.ctl;
    nid.uID = kTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

void show_tray_menu(App& app) {
    ::SetForegroundWindow(app.ctl);  // 否则菜单不会因失焦而关闭

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"呼出 (&S)");
    ::AppendMenuW(menu, MF_STRING, 2, L"开机自启");
    if (autostart_enabled()) {
        ::CheckMenuItem(menu, 2, MF_BYCOMMAND | MF_CHECKED);
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 3, L"退出 (&X)");

    POINT pt{};
    ::GetCursorPos(&pt);
    // 自启状态每次从注册表读，config.txt 不存（注册表是唯一真相）
    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                      app.ctl, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            app_show(app);
            break;
        case 2:
            autostart_set(!autostart_enabled(), exe_path());
            break;
        case 3:
            ::PostMessageW(app.ctl, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

void on_tray(App& app, LPARAM lp) {
    switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            app_toggle(app);
            break;
        case WM_RBUTTONUP:
            show_tray_menu(app);
            break;
        default:
            break;
    }
}

// 面板窗口首次呼出时才创建；WM_CREATE 里初始化渲染器
bool ensure_panel(App& app) {
    if (app.panel) return true;

    const int dpi = static_cast<int>(::GetDpiForSystem());
    const int w = ::MulDiv(kDefaultW, dpi, 96);
    const int h = ::MulDiv(kDefaultH, dpi, 96);

    app.panel = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass, L"Stargazer",
                                  WS_POPUP, 0, 0, w, h, nullptr, nullptr, app.inst, &app);
    if (!app.panel) return false;

    app.render.init(app.panel);  // 内部会取 GetDpiForWindow，与上面 dpi 一致

    // 拖放注册在面板窗口上（用户是往面板上拖），面板是懒创建的，所以注册也在这里
    dragdrop_set_drag_flag(&app.in_drag);
    dragdrop_init(app.panel);
    dragdrop_set_hook([&app](const std::vector<std::wstring>& paths) {
        AppState& s = app.state;
        if (s.groups.empty()) s.groups.push_back(LaunchGroup{ L"常用", {} });
        const int gi = std::clamp(s.launcher.group, 0, static_cast<int>(s.groups.size()) - 1);
        for (const auto& p : paths) s.groups[gi].items.push_back(item_from_path(p));
        s.data_dirty = true;
        launcher_refilter(s);
        ::InvalidateRect(app.panel, nullptr, FALSE);
    });
    return true;
}

LRESULT CALLBACK ctl_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(
                                    reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            return TRUE;
        case WM_HOTKEY:
            if (app && wp == app->hotkey_id) app_toggle(*app);
            return 0;
        case WM_APP_TRAY:
            if (app) on_tray(*app, lp);
            return 0;
        case WM_APP_SHOW:
            if (app) app_show(*app);
            return 0;
        case WM_APP_ICON_READY:
            // 图标工作线程的通知窗口是 ctl，重绘目标是面板
            if (app && app->panel) ::InvalidateRect(app->panel, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK panel_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(
                                    reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            return TRUE;
        case WM_SIZE:
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 全部自绘，禁止系统擦背景以消除闪烁
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            if (app && app->render.begin()) {
                app->render.clear(app->render.theme.bg);
                launcher_render(app->render, app->state, app->render.client_logical());
                app->render.end();
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            ::SetTextColor(dc, RGB(217, 222, 230));
            ::SetBkColor(dc, RGB(37, 39, 45));
            return reinterpret_cast<LRESULT>(edit_bg_brush());
        }
        case WM_MOUSEMOVE: {
            if (!app || app->in_drag) return 0;
            const POINT phys{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const D2D1_POINT_2F lpt = app->render.to_logical(phys);
            const int hit = launcher_hittest(app->state, app->render.client_logical(), lpt);
            if (hit != app->state.launcher.hover) {
                app->state.launcher.hover = hit;
                if (!app->mouse_tracking) {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    ::TrackMouseEvent(&tme);
                    app->mouse_tracking = true;
                }
                // 只在悬停项变化时重绘，鼠标每动一下就重绘会让空闲 CPU 上去
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (app) {
                app->mouse_tracking = false;
                app->state.launcher.hover = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!app) return 0;
            const POINT phys{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const D2D1_POINT_2F lpt = app->render.to_logical(phys);
            const D2D1_SIZE_F cs = app->render.client_logical();

            const int tab = launcher_tab_hittest(app->state, cs, lpt);
            if (tab >= 0) {
                app->state.launcher.group = tab;
                app->state.launcher.sel = -1;
                app->state.launcher.scroll = 0;
                launcher_refilter(app->state);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int hit = launcher_hittest(app->state, cs, lpt);
            app->state.launcher.sel = hit;  // 点空白处 = 回到无选中态
            if (hit >= 0) ::SetFocus(hwnd);
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            // 双击启动（CS_DBLCLKS 已开启，系统保证只有快速双击才发这条消息）
            if (!app) return 0;
            const D2D1_POINT_2F lpt =
                app->render.to_logical(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            const int hit =
                launcher_hittest(app->state, app->render.client_logical(), lpt);
            if (hit >= 0) {
                app->state.launcher.sel = hit;
                launch_selected(*app);
            }
            return 0;
        }
        case WM_COMMAND:
            // 边打字边过滤：EDIT 每次内容变化都会给父窗口发 EN_CHANGE，
            // 只连 Enter/失焦的提交是不够的（那样只在提交时才过滤）。
            if (app && HIWORD(wp) == EN_CHANGE &&
                reinterpret_cast<HWND>(lp) == app->state.launcher.search.hwnd) {
                app->state.launcher.query = app->state.launcher.search.text();
                launcher_refilter(app->state);
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CHAR: {
            // 在网格里打字应当回到搜索框继续过滤，否则用户会以为搜索坏了
            if (!app) return 0;
            const wchar_t ch = static_cast<wchar_t>(wp);
            LauncherState& ls = app->state.launcher;
            if (ch >= 0x20 && ch != 0x7F && ls.search.is_open()) {
                ls.sel = -1;
                ls.search.focus();
                ::PostMessageW(ls.search.hwnd, WM_CHAR, wp, lp);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (!app) return 0;
            if (wp == VK_ESCAPE) {
                app_hide(*app);
                return 0;
            }
            if (wp == VK_RETURN) {
                if (app->state.launcher.sel >= 0) launch_selected(*app);
                return 0;
            }
            const D2D1_SIZE_F cs = app->render.client_logical();
            if (launcher_keydown(app->state, cs, static_cast<UINT>(wp))) {
                if (app->state.launcher.sel < 0) {
                    launcher_sync_search(app->state, hwnd, cs, app->render);
                    app->state.launcher.search.focus();
                } else {
                    ::SetFocus(hwnd);
                }
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CLOSE:
            app_hide(*app);  // 关面板不等于退出程序
            return 0;
        case WM_DESTROY:
            if (app) {
                app->state.launcher.search.close();
                app->render.shutdown();
            }
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void app_load(App& app) {
    AppState& s = app.state;
    std::wstring text;

    if (load_text(app.paths, L"launcher.txt", text)) {
        int bad = 0;
        s.groups = parse_launcher(text, bad);
        s.bad_lines += bad;
    }
    if (s.groups.empty()) {
        // 首次运行给一个能直接用的分组，而不是空白界面
        s.groups.push_back(LaunchGroup{ L"常用", {} });
    }

    if (load_text(app.paths, L"config.txt", text)) {
        int bad = 0;
        s.config = parse_config(text, bad);
        s.bad_lines += bad;
    }

    if (load_text(app.paths, L"ui.txt", text)) {
        int bad = 0;
        const Config ui = parse_config(text, bad);
        const std::wstring v = config_get(ui, L"group", L"");
        // 只接受存在于当前数据里的分组名，防止手改后越界
        for (size_t i = 0; i < s.groups.size(); ++i) {
            if (s.groups[i].name == v) {
                s.launcher.group = static_cast<int>(i);
                break;
            }
        }
        const std::wstring w = config_get(ui, L"w", L"");
        const std::wstring h = config_get(ui, L"h", L"");
        if (!w.empty() && !h.empty() && app.panel) {
            // ui.txt 里的是逻辑像素，SetWindowPos 要物理像素
            const float sc = app.render.scale();
            ::SetWindowPos(app.panel, nullptr, 0, 0,
                           static_cast<int>(_wtoi(w.c_str()) * sc),
                           static_cast<int>(_wtoi(h.c_str()) * sc), SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    launcher_refilter(s);
}

void app_save_if_dirty(App& app) {
    AppState& s = app.state;
    if (!s.data_dirty) return;
    s.data_dirty = false;
    if (!save_text(app.paths, L"launcher.txt", serialize_launcher(s.groups))) {
        ::MessageBoxW(app.ctl, L"保存失败：程序目录可能已变为不可写。", L"Stargazer",
                      MB_ICONWARNING);
    }
}

// 启动当前选中的条目，然后隐藏面板。失败用弹框告知（启动是用户主动发起的，静默失败更糟）
static void launch_selected(App& app) {
    LauncherState& ls = app.state.launcher;
    if (ls.sel < 0 || ls.sel >= static_cast<int>(ls.filtered.size())) return;
    if (app.state.groups.empty()) return;
    const LaunchItem& item = app.state.groups[ls.group].items[ls.filtered[ls.sel]];
    std::wstring err;
    if (!launch_item(item, &err)) {
        ::MessageBoxW(app.panel, err.c_str(), L"Stargazer", MB_ICONWARNING);
        return;
    }
    app_hide(app);
}

void app_show(App& app) {
    if (!ensure_panel(app)) return;

    POINT pt{};
    ::GetCursorPos(&pt);
    const HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);

    RECT rc{};
    ::GetWindowRect(app.panel, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    ::SetWindowPos(app.panel, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    ::SetForegroundWindow(app.panel);
    // 窗口现在才真正落在某块显示器上，此时取 DPI 才准（含跨显示器不同缩放）
    app.render.sync_dpi();

    // 每次呼出都从干净状态开始：清空搜索、选中态归位，焦点给搜索框
    AppState& s = app.state;
    s.launcher.query.clear();
    s.launcher.sel = -1;
    s.launcher.scroll = 0;
    launcher_refilter(s);
    launcher_sync_search(s, app.panel, app.render.client_logical(), app.render);
    s.launcher.search.focus();
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void app_hide(App& app) {
    if (!app.panel) return;
    app_save_if_dirty(app);  // 用户改完就切走很自然，落盘不能等退出
    app.state.launcher.search.close();  // 悬空的输入框比看不见的窗口更让人困惑
    ::ShowWindow(app.panel, SW_HIDE);
    // 隐藏时把绘制表面还给系统：150% 缩放下 1440x930 的表面本身就有 5MB+。
    // 复用设备丢失那条路径，下次 begin() 会自动重建。
    app.render.release_surfaces();
    icons_on_device_lost();

    // 再把常驻页赶回系统。实测：D2D/DWrite 首次绘制后常驻工作集 54MB，
    // 而裁剪后再次呼出只需要 17MB — 差的那些是字体/字形/命令缓冲缓存，
    // 不是一次绘制真的需要的。传 (-1,-1) 是“尽可能多地移除页”的惯用写法。
    // 代价：下次呼出要重新读回页（实测仍为 ~17MB，不卡）。
    // 注意：私有的提交量（~64MB，分配器高水位）不会因此下降，那要销毁工厂才能收回，
    // 但那样每次呼出都要重建设备（20-50ms），对热键呼出型工具不划算。
    ::SetProcessWorkingSetSize(::GetCurrentProcess(), static_cast<SIZE_T>(-1),
                               static_cast<SIZE_T>(-1));
}

void app_toggle(App& app) {
    if (app.panel && ::IsWindowVisible(app.panel)) {
        app_hide(app);
    } else {
        app_show(app);
    }
}

bool app_init(App& app, HINSTANCE inst) {
    app.inst = inst;

    // Review Focus 1：数据目录不可写时明确告知并退出，不静默丢数据
    if (!init_paths(app.paths)) {
        ::MessageBoxW(nullptr, L"无法定位程序所在目录。", L"Stargazer", MB_ICONERROR);
        return false;
    }
    if (!app.paths.writable) {
        std::wstring msg = L"Stargazer 是便携程序，需要在自己的目录下读写数据。\n\n无法写入：\n";
        msg += app.paths.data_dir;
        msg += L"\n\n请把 stargazer.exe 移到可写目录（例如 D:\\Tools\\stargazer）后重试。";
        ::MessageBoxW(nullptr, msg.c_str(), L"Stargazer", MB_ICONERROR);
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // 双击检测由系统完成，视图层不必自己计时
    wc.lpfnWndProc = ctl_wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kCtlClass;
    if (!::RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc = panel_wndproc;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 自绘
    wc.lpszClassName = kPanelClass;
    if (!::RegisterClassExW(&wc)) return false;

    // 控制窗口：不可见、零尺寸，只用来挂托盘与热键。成本几十 KB，
    // 换成 message-only 窗口就收不到 FindWindow 也收不到广播，得不偿失。
    app.ctl = ::CreateWindowExW(0, kCtlClass, L"Stargazer", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                inst, &app);
    if (!app.ctl) return false;

    app.hotkey_ok =
        ::RegisterHotKey(app.ctl, app.hotkey_id, kDefaultHotkeyMods, kDefaultHotkeyKey) != FALSE;
    if (!app.hotkey_ok) {
        ::MessageBoxW(nullptr,
                      L"全局热键 Ctrl+Shift+Space 注册失败（可能被其它程序占用）。\n"
                      L"可继续用托盘图标呼出。",
                      L"Stargazer", MB_ICONWARNING);
    }

    add_tray_icon(app);
    autostart_heal(exe_path());
    return true;
}

void app_shutdown(App& app) {
    if (app.panel) {
        app.render.shutdown();
        ::DestroyWindow(app.panel);
        app.panel = nullptr;
    }
    if (app.ctl) {
        ::UnregisterHotKey(app.ctl, app.hotkey_id);
        remove_tray_icon(app);
    }
}

}  // namespace sg
