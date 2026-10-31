#include "app.h"

#include <shellapi.h>
#include <wchar.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <string>

#include "clipboard.h"
#include "dragdrop.h"
#include "fs_work.h"
#include "icons.h"
#include "model/paths.h"
#include "resource.h"
#include "text_io.h"
#include "views/box.h"

namespace sg {

const wchar_t* kCtlClass = L"StargazerCtl";
const wchar_t* kPanelClass = L"StargazerWnd";

namespace {

const UINT kTrayId = 1;
constexpr UINT_PTR kHoverTimer = 1;
constexpr UINT_PTR kRevealTimer = 2;
constexpr unsigned long long kRevealMs = 220;
const int kDefaultW = 1080;
const int kDefaultH = 720;

// 系统菜单和对话框跟着浅色走。uxtheme 的 SetPreferredAppMode 只导出了序数 135。
// 3 = ForceLight。失败就算了，自绘的窗口不受影响。
static void apply_light_app_mode() {
    static bool done = false;
    if (done) return;
    done = true;
    if (HMODULE ux = ::LoadLibraryW(L"uxtheme.dll")) {
        using SetPreferredAppModeFn = int(WINAPI*)(int);
        auto set_mode = reinterpret_cast<SetPreferredAppModeFn>(
            reinterpret_cast<void*>(::GetProcAddress(ux, MAKEINTRESOURCEA(135))));
        if (set_mode) set_mode(3);
        ::FreeLibrary(ux);
    }
}

// 圆角 + 系统投影。浅色框：不再要沉浸式深色标题。
static void apply_win11_chrome(HWND hwnd) {
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
    const BOOL dark = FALSE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWORD corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const MARGINS margins{ 1, 1, 1, 1 };
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);
}

static void show_hotkey_dialog(App& app);

std::wstring g_crash_log;

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    if (!g_crash_log.empty()) {
        HANDLE h = ::CreateFileW(g_crash_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t line[256] = {};
            ::swprintf_s(line, L"[%04d-%02d-%02d %02d:%02d:%02d] 异常码 0x%08X 地址 %p\r\n", st.wYear,
                         st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         info ? info->ExceptionRecord->ExceptionCode : 0u,
                         info ? info->ExceptionRecord->ExceptionAddress : nullptr);
            DWORD written = 0;
            ::WriteFile(h, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written,
                        nullptr);
            ::CloseHandle(h);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void tray_balloon(App& app, const wchar_t* title, const std::wstring& text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.ctl;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static HICON load_app_icon(HINSTANCE inst, int cx, int cy) {
    return reinterpret_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                cx, cy, LR_DEFAULTCOLOR));
}

void add_tray_icon(App& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.ctl;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = load_app_icon(app.inst, ::GetSystemMetrics(SM_CXSMICON),
                              ::GetSystemMetrics(SM_CYSMICON));
    app.tray_icon = nid.hIcon;
    if (!nid.hIcon) nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
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

static RECT work_area_for(int x, int y) {
    const HMONITOR mon = ::MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) {
        return RECT{ 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    }
    return mi.rcWork;
}

bool ensure_panel(App& app);

void show_tray_menu(App& app) {
    ::SetForegroundWindow(app.ctl);

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"呼出 (&S)");
    ::AppendMenuW(menu, MF_STRING, 5, L"设置呼出热键…(&K)");
    ::AppendMenuW(menu, MF_STRING, 2, L"开机自启");
    if (autostart_enabled()) ::CheckMenuItem(menu, 2, MF_BYCOMMAND | MF_CHECKED);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 3, L"退出 (&X)");

    POINT pt{};
    ::GetCursorPos(&pt);
    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                      app.ctl, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            app_show(app);
            break;
        case 5:
            show_hotkey_dialog(app);
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

bool ensure_panel(App& app) {
    if (app.panel) return true;

    const int dpi = static_cast<int>(::GetDpiForSystem());
    int w = ::MulDiv(kDefaultW, dpi, 96);
    int h = ::MulDiv(kDefaultH, dpi, 96);
    if (app.state.ui_w > 0 && app.state.ui_h > 0) {
        w = ::MulDiv(app.state.ui_w, dpi, 96);
        h = ::MulDiv(app.state.ui_h, dpi, 96);
    }

    app.panel = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass, L"Stargazer",
                                  WS_POPUP | WS_THICKFRAME, 0, 0, w, h, nullptr, nullptr,
                                  app.inst, &app);
    if (!app.panel) return false;

    app.render.init(app.panel);
    apply_win11_chrome(app.panel);

    dragdrop_set_drag_flag(&app.in_drag);
    if (!dragdrop_init(app.panel)) {
        tray_balloon(app, L"Stargazer", L"拖放初始化失败：往窗口里拖文件将不会被接受。");
    }
    dragdrop_set_hook([&app](const std::vector<std::wstring>& paths) { box_ingest(app, paths); });
    return true;
}

// ---------------------------------------------------------------- 呼出热键设置
const wchar_t* kHotkeyDlgClass = L"StargazerHotkeyDlg";
constexpr int kHkCtlId = 103;
constexpr int kHkOkId = IDOK;
constexpr int kHkCancelId = IDCANCEL;

struct HotkeyDlg {
    App* app = nullptr;
    HWND hotkey = nullptr;
    HFONT font = nullptr;
};

HBRUSH dlg_brush() {
    static HBRUSH b = ::CreateSolidBrush(RGB(247, 246, 243));
    return b;
}

BYTE hotkeyf_of(UINT mods) {
    BYTE f = 0;
    if ((mods & MOD_SHIFT) != 0) f |= HOTKEYF_SHIFT;
    if ((mods & MOD_CONTROL) != 0) f |= HOTKEYF_CONTROL;
    if ((mods & MOD_ALT) != 0) f |= HOTKEYF_ALT;
    return f;
}

HWND dlg_child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
               int w, int h, int id, HFONT font, HINSTANCE inst, UINT dpi) {
    const auto S = [dpi](int v) { return ::MulDiv(v, static_cast<int>(dpi), 96); };
    HWND c = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                               parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst,
                               nullptr);
    if (c && font) ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return c;
}

LRESULT CALLBACK hotkey_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HotkeyDlg* d = reinterpret_cast<HotkeyDlg*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(
                                    reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            return TRUE;
        case WM_CREATE: {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            const HINSTANCE inst = cs->hInstance;
            const UINT dpi = ::GetDpiForWindow(hwnd);
            d->font = ::CreateFontW(-::MulDiv(14, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL,
                                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, ui_font_family());
            dlg_child(hwnd, L"STATIC", L"按下新的组合键（至少带一个 Ctrl / Alt / Shift）：",
                      SS_LEFT, 16, 16, 328, 20, -1, d->font, inst, dpi);
            d->hotkey = dlg_child(hwnd, L"msctls_hotkey32", L"", WS_BORDER | WS_TABSTOP, 16, 42,
                                  180, 24, kHkCtlId, d->font, inst, dpi);
            if (d->hotkey) {
                ::SendMessageW(d->hotkey, HKM_SETRULES,
                               HKCOMB_NONE | HKCOMB_S | HKCOMB_C | HKCOMB_A | HKCOMB_SC |
                                   HKCOMB_SA | HKCOMB_CA,
                               MAKELPARAM(HOTKEYF_CONTROL, 0));
                ::SendMessageW(d->hotkey, HKM_SETHOTKEY,
                               MAKEWORD(d->app->hotkey_key, hotkeyf_of(d->app->hotkey_mods)), 0);
            }
            dlg_child(hwnd, L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, 180, 96, 78, 28,
                      kHkOkId, d->font, inst, dpi);
            dlg_child(hwnd, L"BUTTON", L"取消", WS_TABSTOP, 266, 96, 78, 28, kHkCancelId, d->font,
                      inst, dpi);
            return 0;
        }
        case WM_COMMAND:
            if (!d) break;
            if (LOWORD(wp) == kHkOkId) {
                const WORD v = static_cast<WORD>(::SendMessageW(d->hotkey, HKM_GETHOTKEY, 0, 0));
                UINT mods = 0;
                if ((v & HOTKEYF_SHIFT) != 0) mods |= MOD_SHIFT;
                if ((v & HOTKEYF_CONTROL) != 0) mods |= MOD_CONTROL;
                if ((v & HOTKEYF_ALT) != 0) mods |= MOD_ALT;
                const UINT key = v & 0xFF;
                if (key == 0 || mods == 0) {
                    tray_balloon(*d->app, L"Stargazer", L"请按一个带 Ctrl / Alt / Shift 的组合键");
                    return 0;
                }
                if (!app_set_hotkey(*d->app, mods, key)) {
                    tray_balloon(*d->app, L"Stargazer",
                                 L"这个组合被别的程序占用了：" + hotkey_text(mods, key));
                    return 0;
                }
                config_set(d->app->state.config, kHotkeyModsKey, std::to_wstring(mods));
                config_set(d->app->state.config, kHotkeyKeyKey, std::to_wstring(key));
                if (!save_text(d->app->paths, L"config.txt",
                               serialize_config(d->app->state.config))) {
                    tray_balloon(*d->app, L"Stargazer", L"热键已生效，但写 config.txt 失败");
                }
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == kHkCancelId) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            ::SetTextColor(dc, RGB(28, 25, 23));
            ::SetBkColor(dc, RGB(247, 246, 243));
            return reinterpret_cast<LRESULT>(dlg_brush());
        }
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (d && d->font) {
                ::DeleteObject(d->font);
                d->font = nullptr;
            }
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void show_hotkey_dialog(App& app) {
    if (app.modal) return;
    if (!ensure_panel(app)) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = hotkey_dlg_proc;
        wc.hInstance = app.inst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = ::LoadIconW(app.inst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.hIconSm = load_app_icon(app.inst, ::GetSystemMetrics(SM_CXSMICON),
                                   ::GetSystemMetrics(SM_CYSMICON));
        wc.hbrBackground = dlg_brush();
        wc.lpszClassName = kHotkeyDlgClass;
        if (!::RegisterClassExW(&wc)) return;
        registered = true;
    }

    POINT pt{};
    ::GetCursorPos(&pt);
    const RECT wa = work_area_for(pt.x, pt.y);
    const UINT dpi = static_cast<UINT>(app.render.dpi > 0.f ? app.render.dpi : 96.f);
    const auto S = [dpi](int v) { return ::MulDiv(v, static_cast<int>(dpi), 96); };

    HotkeyDlg d;
    d.app = &app;
    HWND dlg = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kHotkeyDlgClass,
                                 L"设置呼出热键", WS_POPUP | WS_CAPTION | WS_SYSMENU, wa.left,
                                 wa.top, S(360) + 32, S(132) + 64, app.panel, nullptr, app.inst,
                                 &d);
    if (!dlg) return;

    RECT wr{}, cr{};
    ::GetWindowRect(dlg, &wr);
    ::GetClientRect(dlg, &cr);
    const int outer_w = S(360) + ((wr.right - wr.left) - cr.right);
    const int outer_h = S(132) + ((wr.bottom - wr.top) - cr.bottom);
    const int x = wa.left + ((wa.right - wa.left) - outer_w) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - outer_h) / 2;
    ::SetWindowPos(dlg, nullptr, x, y, outer_w, outer_h, SWP_NOZORDER | SWP_NOACTIVATE);

    app.modal = true;
    ::EnableWindow(app.panel, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    if (d.hotkey) ::SetFocus(d.hotkey);

    MSG msg{};
    const auto modifier_down = [] {
        return (::GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
               (::GetKeyState(VK_SHIFT) & 0x8000) != 0 || (::GetKeyState(VK_MENU) & 0x8000) != 0;
    };
    while (::IsWindow(dlg)) {
        if (::GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            ::PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            ::DestroyWindow(dlg);
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && !modifier_down()) {
            ::SendMessageW(dlg, WM_COMMAND, kHkOkId, 0);
            continue;
        }
        if (!::IsDialogMessageW(dlg, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    ::EnableWindow(app.panel, TRUE);
    app.modal = false;
    if (::IsWindow(app.panel)) ::SetForegroundWindow(app.panel);
}

float ease_out(float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

void start_reveal(App& app) {
    app.reveal_ms = ::GetTickCount64();
    if (app.panel && ::SetTimer(app.panel, kRevealTimer, 16, nullptr)) {
        app.reveal = 0.f;
    } else {
        app.reveal = 1.f;
    }
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
            if (app && !app->modal && wp == app->hotkey_id) app_toggle(*app);
            return 0;
        case WM_APP_TRAY:
            if (app) on_tray(*app, lp);
            return 0;
        case WM_APP_SHOW:
            if (app) app_show(*app);
            return 0;
        case WM_APP_ICON_READY:
            if (app && app->panel) ::InvalidateRect(app->panel, nullptr, FALSE);
            return 0;
        case WM_APP_FS_CHECKED:
            if (app && app->panel) {
                fs_drain();
                ::InvalidateRect(app->panel, nullptr, FALSE);
            }
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
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc{};
            ::GetWindowRect(hwnd, &rc);
            const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
            const int m = ::MulDiv(6, dpi, 96);
            const bool left = pt.x < rc.left + m;
            const bool right = pt.x >= rc.right - m;
            const bool top = pt.y < rc.top + m;
            const bool bottom = pt.y >= rc.bottom - m;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (app && app->render.dwrite) {
                const POINT client{ pt.x - rc.left, pt.y - rc.top };
                const D2D1_POINT_2F lpt = app->render.to_logical(client);
                if (box_in_caption(app->render.client_logical(), lpt)) return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_NCLBUTTONDBLCLK:
            if (wp == HTCAPTION) return 0;  // 双击标题不最大化，这是一块浮着的面板
            break;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
            mmi->ptMinTrackSize.x = ::MulDiv(880, dpi, 96);
            mmi->ptMinTrackSize.y = ::MulDiv(560, dpi, 96);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!app) return 0;
            const int step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            box_mouse_wheel(*app, step, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        }
        case WM_TIMER:
            if (!app) return 0;
            if (wp == kHoverTimer) {
                if (!box_anim_tick(*app)) ::KillTimer(hwnd, kHoverTimer);
                ::InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == kRevealTimer) {
                const float t = std::min(
                    1.f, static_cast<float>(::GetTickCount64() - app->reveal_ms) /
                             static_cast<float>(kRevealMs));
                app->reveal = ease_out(t);
                if (app->reveal >= 1.f) ::KillTimer(hwnd, kRevealTimer);
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SIZE:
            if (app) box_size(*app);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app) app_save_ui(*app);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            if (app && app->render.begin()) {
                box_render(*app);
                if (app->reveal < 1.f) {
                    D2D1_COLOR_F veil = app->render.theme.bg;
                    veil.a = 1.f - app->reveal;
                    const D2D1_SIZE_F cs = app->render.client_logical();
                    app->render.fill_rect(D2D1::RectF(0.f, 0.f, cs.width, cs.height), veil);
                }
                app->render.end();
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            EditPaint p;
            if (app && edit_paint_for(reinterpret_cast<HWND>(lp), p) && p.bg != CLR_INVALID) {
                ::SetTextColor(dc, p.text);
                ::SetBkColor(dc, p.bg);
                return reinterpret_cast<LRESULT>(p.brush ? p.brush : edit_bg_brush());
            }
            const Theme& th = app ? app->render.theme : Theme{};
            const COLORREF bg = blend(th.control, to_solid(th.bg));
            ::SetTextColor(dc, blend(th.text, bg));
            ::SetBkColor(dc, bg);
            return reinterpret_cast<LRESULT>(edit_bg_brush());
        }
        case WM_MOUSEMOVE:
            if (!app) return 0;
            box_mouse_move(*app, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case WM_MOUSELEAVE:
            if (app) box_mouse_leave(*app);
            return 0;
        case WM_LBUTTONDOWN:
            if (!app) return 0;
            box_lbutton_down(*app, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case WM_LBUTTONUP:
            if (!app) return 0;
            box_lbutton_up(*app, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case WM_LBUTTONDBLCLK:
            if (!app) return 0;
            box_lbutton_dblclk(*app, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case WM_CAPTURECHANGED:
            if (app && (app->internal_drag || app->drag_pending || app->press != 0)) {
                app->internal_drag = false;
                app->drag_pending = false;
                app->drag_from = -1;
                app->press = 0;
                app->state.box_view.drag_over_box = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (app && HIWORD(wp) == EN_CHANGE &&
                reinterpret_cast<HWND>(lp) == app->state.box_view.search.hwnd) {
                box_search_changed(*app);
            }
            return 0;
        case WM_CONTEXTMENU: {
            if (!app) return 0;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                ::GetWindowRect(hwnd, &rc);
                pt.x = rc.left + 80;
                pt.y = rc.top + 80;
            }
            POINT client = pt;
            ::ScreenToClient(hwnd, &client);
            box_context_menu(*app, pt, client);
            return 0;
        }
        case WM_CHAR:
            if (app) box_char(*app, static_cast<wchar_t>(wp));
            return 0;
        case WM_KEYDOWN:
            if (!app) return 0;
            if (wp == VK_ESCAPE) {
                if (!box_keydown(*app, VK_ESCAPE)) app_hide(*app);
                return 0;
            }
            box_keydown(*app, static_cast<UINT>(wp));
            return 0;
        case WM_CLOSE:
            if (app) app_hide(*app);
            return 0;
        case WM_DESTROY:
            if (app) {
                app->state.box_view.edit.close();
                app->state.box_view.search.close();
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

    std::wstring boxes_text;
    if (data_file_exists(app.paths, L"boxes.txt") &&
        !load_text(app.paths, L"boxes.txt", boxes_text)) {
        backup_bad(app.paths, L"boxes.txt");
        ++s.bad_lines;
    } else if (!boxes_text.empty()) {
        int bad = 0;
        s.boxes = parse_boxes(boxes_text, bad);
        s.bad_lines += bad;
    }

    std::wstring text;
    if (load_text(app.paths, L"config.txt", text)) {
        int bad = 0;
        s.config = parse_config(text, bad);
        s.bad_lines += bad;
    }

    if (load_text(app.paths, L"ui.txt", text)) {
        int bad = 0;
        const Config ui = parse_config(text, bad);
        const std::wstring w = config_get(ui, L"w", L"");
        const std::wstring h = config_get(ui, L"h", L"");
        if (!w.empty() && !h.empty()) {
            s.ui_w = _wtoi(w.c_str());
            s.ui_h = _wtoi(h.c_str());
        }
        const std::wstring px = config_get(ui, L"x", L"");
        const std::wstring py = config_get(ui, L"y", L"");
        if (!px.empty() && !py.empty()) {
            s.ui_x = _wtoi(px.c_str());
            s.ui_y = _wtoi(py.c_str());
        }
        // 上次的盒子按名字找回。旧的 ui.txt 里可能还有 view= 字段，这里直接忽略。
        const std::wstring bn = config_get(ui, L"box", L"");
        if (!bn.empty()) {
            for (size_t i = 0; i < s.boxes.size(); ++i) {
                if (s.boxes[i].name == bn) {
                    s.box_view.box = static_cast<int>(i);
                    break;
                }
            }
        }
        if (s.box_view.box >= static_cast<int>(s.boxes.size())) s.box_view.box = 0;
    }

    if (s.bad_lines > 0) {
        tray_balloon(app, L"Stargazer",
                     L"数据文件里有 " + std::to_wstring(s.bad_lines) +
                         L" 处无法解析的内容，已跳过（读不出来的文件已备份为 .bad）。");
    }
}

void app_save_if_dirty(App& app) {
    AppState& s = app.state;
    if (!s.data_dirty) return;
    s.data_dirty = false;
    if (!save_text(app.paths, L"boxes.txt", serialize_boxes(s.boxes))) {
        ::MessageBoxW(app.ctl, L"保存失败：程序目录可能已变为不可写。", L"Stargazer",
                      MB_ICONWARNING);
    }
}

void app_notify(App& app, const std::wstring& text) { tray_balloon(app, L"Stargazer", text); }

std::wstring hotkey_text(UINT mods, UINT key) {
    std::wstring s;
    if ((mods & MOD_CONTROL) != 0) s += L"Ctrl+";
    if ((mods & MOD_ALT) != 0) s += L"Alt+";
    if ((mods & MOD_SHIFT) != 0) s += L"Shift+";
    switch (key) {
        case VK_SPACE:
            s += L"Space";
            break;
        case VK_TAB:
            s += L"Tab";
            break;
        case VK_RETURN:
            s += L"Enter";
            break;
        case VK_ESCAPE:
            s += L"Esc";
            break;
        default:
            if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
                s += static_cast<wchar_t>(key);
            } else if (key >= VK_F1 && key <= VK_F24) {
                s += L"F" + std::to_wstring(key - VK_F1 + 1);
            } else {
                wchar_t buf[16] = {};
                ::swprintf_s(buf, L"VK 0x%02X", key);
                s += buf;
            }
            break;
    }
    return s;
}

bool app_set_hotkey(App& app, UINT mods, UINT key) {
    if (mods == 0 || key == 0) {
        const std::wstring ms = config_get(app.state.config, kHotkeyModsKey, L"");
        const std::wstring ks = config_get(app.state.config, kHotkeyKeyKey, L"");
        const UINT m = ms.empty() ? kDefaultHotkeyMods : static_cast<UINT>(_wtoi(ms.c_str()));
        const UINT k = ks.empty() ? kDefaultHotkeyKey : static_cast<UINT>(_wtoi(ks.c_str()));
        const UINT valid = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        if (k == 0 || (m & valid) == 0 || m == MOD_SHIFT) {
            mods = kDefaultHotkeyMods;
            key = kDefaultHotkeyKey;
        } else {
            mods = m;
            key = k;
        }
    }
    if (app.hotkey_ok) {
        ::UnregisterHotKey(app.ctl, app.hotkey_id);
        app.hotkey_ok = false;
    }
    if (::RegisterHotKey(app.ctl, app.hotkey_id, mods, key) != FALSE) {
        app.hotkey_mods = mods;
        app.hotkey_key = key;
        app.hotkey_ok = true;
        return true;
    }
    if (::RegisterHotKey(app.ctl, app.hotkey_id, app.hotkey_mods, app.hotkey_key) != FALSE) {
        app.hotkey_ok = true;
    }
    return false;
}

void app_request_fs_checks(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    const int bi = std::clamp(s.box_view.box, 0, static_cast<int>(s.boxes.size()) - 1);
    std::vector<std::wstring> paths;
    for (const auto& it : s.boxes[static_cast<size_t>(bi)].items) {
        if (!it.path.empty()) paths.push_back(it.path);
    }
    if (paths.empty()) return;
    fs_check_paths(paths, [&app](const std::wstring& path, bool exists) {
        for (auto& b : app.state.boxes) {
            for (auto& it : b.items) {
                if (it.path == path) it.missing = !exists;
            }
        }
    });
}

void app_save_ui(App& app) {
    if (!app.panel) return;
    const float sc = app.render.scale();
    RECT rc{};
    ::GetWindowRect(app.panel, &rc);
    Config ui;
    config_set(ui, L"w", std::to_wstring(static_cast<int>((rc.right - rc.left) / sc)));
    config_set(ui, L"h", std::to_wstring(static_cast<int>((rc.bottom - rc.top) / sc)));
    config_set(ui, L"x", std::to_wstring(rc.left));
    config_set(ui, L"y", std::to_wstring(rc.top));
    app.state.ui_x = rc.left;
    app.state.ui_y = rc.top;
    if (!app.state.boxes.empty()) {
        const int bi =
            std::clamp(app.state.box_view.box, 0, static_cast<int>(app.state.boxes.size()) - 1);
        config_set(ui, L"box", app.state.boxes[static_cast<size_t>(bi)].name);
    }
    save_text(app.paths, L"ui.txt", serialize_config(ui));
}

void app_show(App& app) {
    if (!ensure_panel(app)) return;

    RECT rc{};
    ::GetWindowRect(app.panel, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    AppState& s = app.state;
    int x = 0;
    int y = 0;
    if (s.ui_x != INT_MIN && s.ui_y != INT_MIN) {
        const RECT wa = work_area_for(s.ui_x, s.ui_y);
        x = std::clamp(s.ui_x, static_cast<int>(wa.left),
                       std::max(static_cast<int>(wa.left), static_cast<int>(wa.right) - w));
        y = std::clamp(s.ui_y, static_cast<int>(wa.top),
                       std::max(static_cast<int>(wa.top), static_cast<int>(wa.bottom) - h));
    } else {
        POINT pt{};
        ::GetCursorPos(&pt);
        const RECT wa = work_area_for(pt.x, pt.y);
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    }
    s.ui_x = x;
    s.ui_y = y;

    ::SetWindowPos(app.panel, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    ::SetForegroundWindow(app.panel);
    app.render.sync_dpi();
    start_reveal(app);
    box_on_show(app);
}

void app_hide(App& app) {
    if (!app.panel) return;
    box_on_hide(app);
    app_save_if_dirty(app);
    ::ShowWindow(app.panel, SW_HIDE);
    if (app.panel) ::KillTimer(app.panel, kRevealTimer);
    app.reveal = 1.f;
    app.render.release_surfaces();
    icons_on_device_lost();
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
    apply_light_app_mode();

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

    g_crash_log = join_path(app.paths.exe_dir, L"crash.log");
    ::SetUnhandledExceptionFilter(crash_filter);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ctl_wndproc;
    wc.hInstance = inst;
    wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = load_app_icon(inst, ::GetSystemMetrics(SM_CXSMICON),
                               ::GetSystemMetrics(SM_CYSMICON));
    wc.lpszClassName = kCtlClass;
    if (!::RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc = panel_wndproc;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;
    if (!::RegisterClassExW(&wc)) return false;

    app.ctl = ::CreateWindowExW(0, kCtlClass, L"Stargazer", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                inst, &app);
    if (!app.ctl) return false;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&icc);

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
        if (app.tray_icon) {
            ::DestroyIcon(app.tray_icon);
            app.tray_icon = nullptr;
        }
    }
}

}  // namespace sg
