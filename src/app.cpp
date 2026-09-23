#include "app.h"

#include <shellapi.h>
#include <wchar.h>

#include <string>

#include "icons.h"
#include "text_io.h"

namespace sg {

const wchar_t* kCtlClass = L"StargazerCtl";
const wchar_t* kPanelClass = L"StargazerWnd";

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

    RECT rc{};
    ::GetWindowRect(app.panel, &rc);
    app.render.init(app.panel);  // 内部会取 GetDpiForWindow，与上面 dpi 一致
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
                // 必须用逻辑尺寸：直接用 GetClientRect 的物理像素会让卡片在高 DPI 下超出窗口
                const D2D1_SIZE_F cs = app->render.client_logical();
                const D2D1_RECT_F card = D2D1::RectF(16.f, 56.f, cs.width - 16.f, cs.height - 16.f);
                app->render.fill_round_rect(card, 8.f, app->render.theme.panel);
                app->render.text(D2D1::RectF(24.f, 16.f, 400.f, 44.f), L"Stargazer 渲染层就绪",
                                 app->render.format(16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                                 app->render.theme.text);
                // TEMP(Task 9 移除)：图标验证钩子
                if (!app->debug_icon.empty()) {
                    const bool is_dir = app->debug_icon.back() == L'\\';
                    ID2D1Bitmap* dbg_bmp = icons_get(app->render, app->debug_icon, is_dir);
                    // TEMP 观测：把绘制分支写在标题上（Task 9 随钩子一起删）
                    ::SetWindowTextW(hwnd, dbg_bmp ? L"paint:icon" : L"paint:placeholder");
                    if (dbg_bmp) {
                        app->render.rt->DrawBitmap(
                            dbg_bmp,
                            D2D1::RectF(card.left + 16.f, card.top + 16.f, card.left + 64.f,
                                        card.top + 64.f),
                            1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    } else {
                        // 未就绪先画占位，证明“骨架先出、图标后到”
                        app->render.fill_round_rect(
                            D2D1::RectF(card.left + 16.f, card.top + 16.f, card.left + 64.f,
                                        card.top + 64.f),
                            8.f, D2D1::ColorF(0.35f, 0.45f, 0.62f));
                    }
                }
                app->render.end();
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            // app 为空的路径理论到不了这里，但不必为此崩一次
            if (!app) return 0;
            // TEMP(Task 9 移除)：按 1/2 验证图标三级提取与异步回投
            if (wp == L'1') {
                app->debug_icon = L"C:\\Windows\\notepad.exe";
                ::SetWindowTextW(hwnd, (L"dbg1:" + app->debug_icon).c_str());  // TEMP 观测
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == L'2') {
                app->debug_icon = L"D:\\不存在的网盘目录\\a.psd";
                ::SetWindowTextW(hwnd, (L"dbg2:" + app->debug_icon).c_str());  // TEMP 观测
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_ESCAPE) app_hide(*app);
            return 0;
        case WM_CLOSE:
            app_hide(*app);  // 关面板不等于退出程序
            return 0;
        case WM_DESTROY:
            if (app) app->render.shutdown();
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

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
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void app_hide(App& app) {
    if (!app.panel) return;
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
