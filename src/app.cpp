#include "app.h"

#include <shellapi.h>
#include <wchar.h>

#include <string>

#include "text_io.h"

namespace sg {

const wchar_t* kWindowClass = L"StargazerWnd";

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
    nid.hWnd = app.hwnd;
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
    nid.hWnd = app.hwnd;
    nid.uID = kTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

void show_tray_menu(App& app) {
    ::SetForegroundWindow(app.hwnd);  // 否则菜单不会因失焦而关闭

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
                                      app.hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            app_show(app);
            break;
        case 2:
            autostart_set(!autostart_enabled(), exe_path());
            break;
        case 3:
            ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
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

}  // namespace

void app_show(App& app) {
    POINT pt{};
    ::GetCursorPos(&pt);
    const HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);

    RECT rc{};
    ::GetWindowRect(app.hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    ::SetWindowPos(app.hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    ::SetForegroundWindow(app.hwnd);
    ::InvalidateRect(app.hwnd, nullptr, FALSE);
}

void app_hide(App& app) { ::ShowWindow(app.hwnd, SW_HIDE); }

void app_toggle(App& app) {
    if (::IsWindowVisible(app.hwnd)) {
        app_hide(app);
    } else {
        app_show(app);
    }
}

LRESULT CALLBACK app_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(
                                    reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            return TRUE;
        case WM_APP_SHOW:
            if (app) app_show(*app);
            return 0;
        case WM_APP_TRAY:
            if (app) on_tray(*app, lp);
            return 0;
        case WM_HOTKEY:
            if (app && wp == app->hotkey_id) app_toggle(*app);
            return 0;
        case WM_KEYDOWN:
            // app 为空的路径理论到不了这里，但不必为此崩一次
            if (app && wp == VK_ESCAPE) app_hide(*app);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 全部自绘，禁止系统擦背景以消除闪烁
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(ps.hdc, &rc, static_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH)));
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
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
    wc.lpfnWndProc = app_wndproc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 自绘
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc)) return false;

    const UINT sys_dpi = ::GetDpiForSystem();
    const int w = ::MulDiv(kDefaultW, static_cast<int>(sys_dpi), 96);
    const int h = ::MulDiv(kDefaultH, static_cast<int>(sys_dpi), 96);

    // TOOLWINDOW：不出现在任务栏与 Alt+Tab；TOPMOST：呼出后始终在顶层
    app.hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"Stargazer",
                                 WS_POPUP, 0, 0, w, h, nullptr, nullptr, inst, &app);
    if (!app.hwnd) return false;

    app.hotkey_ok =
        ::RegisterHotKey(app.hwnd, app.hotkey_id, kDefaultHotkeyMods, kDefaultHotkeyKey) != FALSE;
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
    if (app.hwnd) {
        ::UnregisterHotKey(app.hwnd, app.hotkey_id);
        remove_tray_icon(app);
    }
}

}  // namespace sg
