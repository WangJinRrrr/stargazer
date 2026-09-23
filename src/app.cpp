#include "app.h"

#include <shellapi.h>
#include <wchar.h>

#include <algorithm>
#include <string>

#include "icons.h"
#include "clipboard.h"
#include "dragdrop.h"
#include "fs_work.h"
#include "model/paths.h"
#include "text_io.h"
#include "views/box.h"
#include "views/browse.h"
#include "views/todo.h"

#include <shobjidl.h>  // IFileDialog（托盘菜单选浏览目录）

namespace sg {

const wchar_t* kCtlClass = L"StargazerCtl";
const wchar_t* kPanelClass = L"StargazerWnd";

namespace {

const UINT kTrayId = 1;

// 托盘菜单要用（定义在下方）
static void app_set_view(App& app, View v);

// 崩溃日志路径。写不进去就算了，崩溃路径上不能再抛异常
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
    // 写日志后直接结束进程：恢复后的进程状态不可信，继续跑只会损坏数据文件
    return EXCEPTION_EXECUTE_HANDLER;
}

// 托盘气泡。坏行、保存失败等一律走它，不弹模态框打断操作流
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
const UINT kDefaultHotkeyMods = MOD_CONTROL | MOD_SHIFT;
const UINT kDefaultHotkeyKey = VK_SPACE;

// 默认窗口尺寸（96 DPI 逻辑像素）
const int kDefaultW = 960;
const int kDefaultH = 620;

// 托盘菜单用：系统文件夹选择框（托盘是“呼出型”工具的设置入口，不开设置窗口）
bool pick_folder(HWND owner, std::wstring& out) {
    IFileDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    bool ok = false;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"选择浏览目录（可以是网盘挂载的目录）");
        if (SUCCEEDED(dlg->Show(owner))) {
            if (IShellItem* item = nullptr; SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out = path;
                    ok = true;
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
    }
    dlg->Release();
    return ok;
}

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
    ::AppendMenuW(menu, MF_STRING, 4, L"设置浏览目录…(&D)");
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
        case 4: {
            // 选浏览目录：系统文件夹选择框 → 写 config.txt（next 启动仍在）→ 直接跳过去
            if (std::wstring dir; pick_folder(app.ctl, dir)) {
                config_set(app.state.config, L"browse_root", dir);
                save_text(app.paths, L"config.txt", serialize_config(app.state.config));
                app.state.browse.history.clear();
                app.state.browse.hist_pos = -1;
                app.state.browse.path.clear();
                app_show(app);
                app_set_view(app, View::Browse);
            }
            break;
        }
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
    const int def_w = ::MulDiv(kDefaultW, dpi, 96);
    const int def_h = ::MulDiv(kDefaultH, dpi, 96);
    // 上次的尺寸（ui.txt 存的是逻辑像素）在这里生效：app_load 跑在面板创建之前
    int w = def_w;
    int h = def_h;
    if (app.state.ui_w > 0 && app.state.ui_h > 0) {
        w = ::MulDiv(app.state.ui_w, dpi, 96);
        h = ::MulDiv(app.state.ui_h, dpi, 96);
    }

    // WS_THICKFRAME 才让鼠标能拉边框缩放；配 WM_NCCALCSIZE 返回 0 去掉系统边框，保持无边框外观
    app.panel = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass, L"Stargazer",
                                  WS_POPUP | WS_THICKFRAME, 0, 0, w, h, nullptr, nullptr,
                                  app.inst, &app);
    if (!app.panel) return false;

    app.render.init(app.panel);  // 内部会取 GetDpiForWindow，与上面 dpi 一致

    // 拖放注册在面板窗口上（用户是往面板上拖），面板是懒创建的，所以注册也在这里
    dragdrop_set_drag_flag(&app.in_drag);
    if (!dragdrop_init(app.panel)) {
        // 静默失败会表现为“拖动时光标全程禁止”而完全看不出原因（真的踩过），
        // 所以至少要让用户知道有东西坏了（spec §12：用气泡不用弹框）
        tray_balloon(app, L"Stargazer", L"拖放初始化失败：往窗口里拖文件将不会被接受。");
    }
    dragdrop_set_hook([&app](const std::vector<std::wstring>& paths) {
        AppState& s = app.state;
        if (s.view == View::Box) {
            // 进当前盒子。不去重：同一文件在多个盒子里、或同盒重复都存在合法用法
            if (box_add_paths(s.boxes, s.box_view.box, paths) > 0) s.data_dirty = true;
            box_clamp(s, app.render.client_logical());
            ::InvalidateRect(app.panel, nullptr, FALSE);
            return;
        }
        if (s.view == View::Todo) {
            // 图片文件记成引用型条目；非图片会托着气泡提示，不静默吞掉
            todo_add_from_paths(app, paths);
            return;
        }
        // 浏览视图：拖入暂不支持（粘贴请用 Ctrl+V）
    });
    return true;
}

// 切换顶层视图。盒子重命名框是所有视图共用的同一个 InlineEdit，切走前先关掉。
static void app_set_view(App& app, View v) {
    if (app.panel == nullptr || v == app.state.view) return;
    AppState& s = app.state;
    s.box_view.edit.close();
    s.browse.path_edit.close();
    s.view = v;
    if (v == View::Box) {
        s.box_view.sel = -1;
        s.box_view.hover = -1;
        s.box_view.scroll = 0;
        box_clamp(s, app.render.client_logical());
        ::SetFocus(app.panel);  // 网格视图自己收键盘，不需要子控件
        box_request_check(app);  // 切进来也要校验，否则失效标记是上一次的
    }
    if (v == View::Todo) {
        todo_rebuild_layout(s, app.render.client_logical());
        todo_sync_input(app);
    }
    if (v == View::Browse) {
        browse_activate(app);
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

// Ctrl+1..3 / Ctrl+Tab。返回 true = 已被消费。
static bool app_view_hotkey(App& app, UINT vk) {
    // 用 GetAsyncKeyState 而不是 GetKeyState：后者是“队列同步态”，
    // 快速按下 Ctrl+数字（连击很快）时可能还没反映出来，热键会时灵时不灵。
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) return false;
    if (vk == VK_TAB) {
        app_set_view(app, static_cast<View>((static_cast<int>(app.state.view) + 1) % kViewCount));
        return true;
    }
    if (vk >= '1' && vk <= '3') {
        app_set_view(app, static_cast<View>(vk - '1'));
        return true;
    }
    return false;
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
        case WM_APP_FS_CHECKED:
            // 存在性校验结果：在 UI 线程按 path 回填 missing 再重绘（灰显 + 删除线）
            if (app && app->panel) {
                fs_drain();
                ::InvalidateRect(app->panel, nullptr, FALSE);
            }
            return 0;
        case WM_APP_DIR_LOADED:
            if (app && app->panel) browse_on_dir_loaded(*app);
            return 0;
        case WM_APP_FS_OP_DONE:
            if (app && app->panel) {
                browse_on_op_done(*app);
                todo_on_image_saved(*app, app->state.todo.op_id);
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
            return 0;  // 客户区 = 整个窗口：WS_THICKFRAME 只用来提供缩放热区
        case WM_NCHITTEST: {
            // 无边框窗口自己给出命中区：顶部标题条拖动移动，四边/四角缩放
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc{};
            ::GetWindowRect(hwnd, &rc);
            const int m = ::MulDiv(6, static_cast<int>(::GetDpiForWindow(hwnd)), 96);  // 缩放热区
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
            // 顶部标题条（逻辑高 52）= 拖动区；子 EDIT 不会被走到这里
            const int title_h = ::MulDiv(52, static_cast<int>(::GetDpiForWindow(hwnd)), 96);
            if (pt.y < rc.top + title_h) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_MOUSEWHEEL: {
            if (!app) return 0;
            const int step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            if (app->state.view == View::Box) {
                app->state.box_view.scroll = std::max(0, app->state.box_view.scroll + step);
                box_clamp(app->state, app->render.client_logical());
            } else if (app->state.view == View::Todo) {
                TodoState& t = app->state.todo;
                const D2D1_RECT_F tl = todo_list_rect(app->render.client_logical());
                t.scroll -= static_cast<float>(step) * 3.f * kTodoRowTextH;
                t.scroll = todo_scroll_for(t.offsets, tl.bottom - tl.top, t.scroll, t.sel);
            } else if (app->state.view == View::Browse) {
                BrowseState& b = app->state.browse;
                b.scroll = std::max(0, b.scroll + step);
            } else {
                return 0;
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app) app_save_ui(*app);  // 用户拉完窗口就把尺寸记住
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 全部自绘，禁止系统擦背景以消除闪烁
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            if (app && app->render.begin()) {
                app->render.clear(app->render.theme.bg);
                const D2D1_SIZE_F cs = app->render.client_logical();
                view_tabs_render(app->render, cs, static_cast<int>(app->state.view));
                switch (app->state.view) {
                    case View::Box:
                        box_render(*app);
                        break;
                    case View::Todo:
                        todo_render(*app);
                        break;
                    case View::Browse:
                        browse_render(*app);
                        break;
                    default:
                        // 待办还没实现；这一行只用来证明切换真的生效
                        app->render.text(
                            D2D1::RectF(kPad, kViewTabsH + kPad * 2.f, cs.width - kPad,
                                        kViewTabsH + kPad * 2.f + 40.f),
                            std::wstring(view_name(static_cast<int>(app->state.view))) +
                                L"：尚未实现",
                            app->render.format(13.f), app->render.theme.text_dim);
                        break;
                }
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
            if (!app) return 0;
            const POINT phys{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const D2D1_POINT_2F lpt = app->render.to_logical(phys);
            const D2D1_SIZE_F cs = app->render.client_logical();

            if (app->in_drag) return 0;  // 外部 OLE 拖拽悬停中，不高亮悬停项

            // 盒内拖拽中：高亮目标盒子标签；鼠标离开窗口就转成 OLE 拖出
            if (app->state.view == View::Box && app->internal_drag) {
                bool changed = false;
                const int over = box_tab_hittest(app->state, cs, lpt);
                if (over != app->state.box_view.drag_over_tab) {
                    app->state.box_view.drag_over_tab = over;
                    changed = true;
                }
                const bool outside = lpt.x < 0.f || lpt.y < 0.f || lpt.x >= cs.width ||
                                     lpt.y >= cs.height;
                if (outside) {
                    // 拖出到外部（资源管理器等）：走 OLE，提供 CF_HDROP。
                    // 拖出只给路径，**不删本地的引用**（目标自己决定复制还是移动）。
                    const BoxState& bs = app->state.box_view;
                    std::wstring path;
                    if (!app->state.boxes.empty() && bs.box >= 0 &&
                        bs.box < static_cast<int>(app->state.boxes.size())) {
                        const auto& items = app->state.boxes[bs.box].items;
                        if (app->drag_from >= 0 && app->drag_from < static_cast<int>(items.size())) {
                            path = items[app->drag_from].path;
                        }
                    }
                    app->internal_drag = false;
                    app->drag_from = -1;
                    app->state.box_view.drag_over_tab = -1;
                    ::ReleaseCapture();
                    if (!path.empty()) {
                        app->in_drag = true;  // 抑制拖放期间的悬停更新
                        dragdrop_begin_drag(hwnd, { path });
                        app->in_drag = false;
                    }
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (changed) ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (app->state.view != View::Box) {
                // 待办：悬停高亮
                if (app->state.view == View::Todo) {
                    TodoState& t = app->state.todo;
                    const int th = todo_hittest(*app, lpt);
                    if (th != t.hover) {
                        t.hover = th;
                        if (!app->mouse_tracking) {
                            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                            ::TrackMouseEvent(&tme);
                            app->mouse_tracking = true;
                        }
                        ::InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                // 浏览：悬停高亮（键盘归列表，不需子控件）
                if (app->state.view == View::Browse) {
                    const int bh = browse_row_hittest(app->state, cs, lpt);
                    if (bh != app->state.browse.hover) {
                        app->state.browse.hover = bh;
                        if (!app->mouse_tracking) {
                            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                            ::TrackMouseEvent(&tme);
                            app->mouse_tracking = true;
                        }
                        ::InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }

            const int hit = box_hittest(*app, lpt);
            int& hover = app->state.box_view.hover;

            if (hit != hover) {
                hover = hit;
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
                app->state.box_view.hover = -1;
                app->state.browse.hover = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!app) return 0;
            const POINT phys{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const D2D1_POINT_2F lpt = app->render.to_logical(phys);
            const D2D1_SIZE_F cs = app->render.client_logical();

            // 视图标签行在最顶部，比盒子标签更先命中
            const int vt = view_tab_hittest(cs, lpt);
            if (vt >= 0) {
                app_set_view(*app, static_cast<View>(vt));
                return 0;
            }
            if (app->state.view == View::Todo) {
                const int row = todo_hittest(*app, lpt);
                if (row >= 0 && todo_checkbox_hit_in_row(*app, row, lpt)) {
                    app->state.todo.sel = row;
                    todo_toggle_done(*app);   // 点复选框 = 切换完成态
                    return 0;
                }
                app->state.todo.sel = row;  // 点空白处 = 取消选中
                if (row >= 0) ::SetFocus(hwnd);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (app->state.view == View::Browse) {
                // 路径栏点击 → 开输入框；列表点击 → 选中行
                const D2D1_RECT_F bar = browse_path_rect(cs);
                if (lpt.x >= bar.left && lpt.x <= bar.right && lpt.y >= bar.top &&
                    lpt.y <= bar.bottom) {
                    browse_edit_path(*app);
                    return 0;
                }
                const int row = browse_row_hittest(app->state, cs, lpt);
                app->state.browse.sel = row;
                if (row >= 0) ::SetFocus(hwnd);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (app->state.view != View::Box) return 0;

            const int tab = box_tab_hittest(app->state, cs, lpt);
            if (tab >= 0) {
                app->state.box_view.box = tab;
                app->state.box_view.sel = -1;
                app->state.box_view.scroll = 0;
                box_clamp(app->state, cs);
                // 换盒子必须重校验：否则上一个盒子的失效标记会留着，
                // 文件已经恢复的条目仍是灰色，一键清理会误删这条活引用（代码评审 Important 3）
                box_request_check(*app);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            {
                const int hit = box_hittest(*app, lpt);
                app->state.box_view.sel = hit;  // 点空白处 = 回到无选中态
                if (hit >= 0) {
                    ::SetFocus(hwnd);
                    // 内部拖拽状态：拖到别的盒子标签上换盒，拖出窗口则转成 OLE 拖出
                    app->internal_drag = true;
                    app->drag_from = hit;
                    ::SetCapture(hwnd);
                }
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            // 捕获被别的窗口抢走（alt-tab 等）：把内部拖拽状态收干净，
            // 否则 internal_drag 一直是 true，悬停高亮永远不恢复
            if (app && app->internal_drag) {
                app->internal_drag = false;
                app->drag_from = -1;
                app->state.box_view.drag_over_tab = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (app && app->internal_drag) {
                app->internal_drag = false;
                ::ReleaseCapture();
                if (app->state.view == View::Box) {
                    const int dst = app->state.box_view.drag_over_tab;
                    if (dst >= 0 && app->drag_from >= 0 &&
                        box_move_item(app->state.boxes, app->state.box_view.box, app->drag_from,
                                      dst)) {
                        app->state.data_dirty = true;
                        app->state.box_view.sel = -1;
                        box_clamp(app->state, app->render.client_logical());
                    }
                }
                app->state.box_view.drag_over_tab = -1;
                app->drag_from = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDBLCLK: {
            // 双击打开（CS_DBLCLKS 已开启，系统保证只有快速双击才发这条消息）
            if (!app) return 0;
            const D2D1_POINT_2F lpt =
                app->render.to_logical(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (app->state.view == View::Todo) {
                const int hit = todo_hittest(*app, lpt);
                if (hit >= 0) {
                    app->state.todo.sel = hit;
                    todo_open_selected(*app);  // 链接进浏览器，图片用系统看图
                }
                return 0;
            }
            if (app->state.view == View::Browse) {
                const int row = browse_row_hittest(app->state, app->render.client_logical(), lpt);
                if (row >= 0) {
                    app->state.browse.sel = row;
                    browse_open_selected(*app);  // 目录=进入；文件=打开
                }
                return 0;
            }
            if (app->state.view != View::Box) return 0;
            const int bhit = box_hittest(*app, lpt);
            if (bhit >= 0) {
                app->state.box_view.sel = bhit;
                box_open_selected(*app);
            }
            return 0;
        }
        case WM_COMMAND:
            // 启动板删掉后不再有“边打字边过滤”，WM_COMMAND 无需处理
            return 0;
        case WM_CONTEXTMENU: {
            if (!app) return 0;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) {  // 键盘唤出菜单
                RECT rc{};
                ::GetWindowRect(hwnd, &rc);
                pt.x = rc.left + 40;
                pt.y = rc.top + 40;
            }
            POINT client = pt;
            ::ScreenToClient(hwnd, &client);
            // 菜单按当前视图分发（与启动板菜单同一入口）
            // 菜单按当前视图分发
            if (app->state.view == View::Box) {
                box_context_menu(*app, pt, client);
            } else if (app->state.view == View::Browse) {
                browse_context_menu(*app, pt, client);
            } else if (app->state.view == View::Todo) {
                todo_context_menu(*app, pt, client);
            }
            return 0;
        }
        case WM_CHAR: {
            // 在列表里打字就回到输入框继续记（列表本身不收文本）
            if (!app) return 0;
            if (app->state.view != View::Todo) return 0;
            const wchar_t ch = static_cast<wchar_t>(wp);
            TodoState& t = app->state.todo;
            if (ch >= 0x20 && ch != 0x7F) {
                if (!t.input.is_open()) todo_sync_input(*app);
                t.sel = -1;
                t.input.focus();
                ::PostMessageW(t.input.hwnd, WM_CHAR, wp, lp);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (!app) return 0;
            if (wp == VK_ESCAPE) {
                app_hide(*app);
                return 0;
            }
            if (app_view_hotkey(*app, static_cast<UINT>(wp))) return 0;
            if (app->state.view == View::Box) {
                box_keydown(*app, static_cast<UINT>(wp));
            } else if (app->state.view == View::Todo) {
                todo_keydown(*app, static_cast<UINT>(wp));
            } else if (app->state.view == View::Browse) {
                browse_keydown(*app, static_cast<UINT>(wp));
            }
            return 0;
        }
        case WM_CLOSE:
            app_hide(*app);  // 关面板不等于退出程序
            return 0;
        case WM_DESTROY:
            if (app) {
                app->state.box_view.edit.close();
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

    // 收纳盒：读不出来但文件存在 => 先备份 .bad（否则用户改一次就把乱码写回去）。
    // 用单独的字符串接内容：复用 text 会把上一份文件的内容当成盒子解析（真陷阱）。
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

    // 待办：同一套读法（存在但读不出来 → 先备份 .bad），同样用独立字符串接内容
    std::wstring todos_text;
    if (data_file_exists(app.paths, L"todo.txt") &&
        !load_text(app.paths, L"todo.txt", todos_text)) {
        backup_bad(app.paths, L"todo.txt");
        ++s.bad_lines;
    } else if (!todos_text.empty()) {
        int bad = 0;
        s.todos = parse_todos(todos_text, bad);
        s.bad_lines += bad;
        sort_todos(s.todos);
    }

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
        // 上次的视图（0 基，与 View 枚举一致）。手改越界时夹紧
        const std::wstring vw = config_get(ui, L"view", L"");
        if (!vw.empty()) {
            const int vi = std::clamp(_wtoi(vw.c_str()), 0, kViewCount - 1);
            s.view = static_cast<View>(vi);
        }
        // 上次的盒子：按名字匹配（盒子可被改名），匹配不到就用第一个。
        // 这里只夹紧下标：scroll 的夹紧要等面板存在后才有客户区尺寸。
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

    // 坏行/坏文件只提示一次，用托盘气泡而不是模态框
    if (s.bad_lines > 0) {
        tray_balloon(app, L"Stargazer",
                     L"数据文件里有 " + std::to_wstring(s.bad_lines) +
                         L" 处无法解析的内容，已跳过（原文件已备份为 .bad）。");
    }
}

void app_save_if_dirty(App& app) {
    AppState& s = app.state;
    if (!s.data_dirty) return;
    s.data_dirty = false;
    // 分开写：一个文件写失败不该让另一个文件连尝试都没有（评审 minor）
    const bool box_ok = save_text(app.paths, L"boxes.txt", serialize_boxes(s.boxes));
    const bool todo_ok = save_text(app.paths, L"todo.txt", serialize_todos(s.todos));
    if (!box_ok || !todo_ok) {
        ::MessageBoxW(app.ctl, L"保存失败：程序目录可能已变为不可写。", L"Stargazer",
                      MB_ICONWARNING);
    }
}

void app_notify(App& app, const std::wstring& text) {
    tray_balloon(app, L"Stargazer", text);
}

void app_save_ui(App& app) {
    if (!app.panel) return;
    // 尺寸存逻辑像素：这样换到不同缩放的显示器上尺寸语义不变
    const float sc = app.render.scale();
    RECT rc{};
    ::GetWindowRect(app.panel, &rc);
    Config ui;
    config_set(ui, L"w", std::to_wstring(static_cast<int>((rc.right - rc.left) / sc)));
    config_set(ui, L"h", std::to_wstring(static_cast<int>((rc.bottom - rc.top) / sc)));
    config_set(ui, L"view", std::to_wstring(static_cast<int>(app.state.view)));
    if (!app.state.boxes.empty()) {
        const int bi =
            std::clamp(app.state.box_view.box, 0, static_cast<int>(app.state.boxes.size()) - 1);
        config_set(ui, L"box", app.state.boxes[bi].name);
    }
    save_text(app.paths, L"ui.txt", serialize_config(ui));
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

    // 每次呼出都从干净状态开始：选中态归位（输入框在下面按视图处理）
    AppState& s = app.state;
    if (s.view == View::Box) {
        s.box_view.sel = -1;
        s.box_view.hover = -1;
        s.box_view.scroll = 0;
        box_clamp(s, app.render.client_logical());
        ::SetFocus(app.panel);
        // 呼出时校验当前盒子（只校验当前盒子：大盒子全量校验会拖慢呼出）
        box_request_check(app);
    } else if (s.view == View::Browse) {
        ::SetFocus(app.panel);
        browse_activate(app);  // 首路径从 config 的 browse_root 取（空则用 exe 目录）并枚举
    } else if (s.view == View::Todo) {
        todo_rebuild_layout(s, app.render.client_logical());
        todo_sync_input(app);
        s.todo.input.focus();
    } else {
        ::SetFocus(app.panel);  // 其他视图自己收键盘
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void app_hide(App& app) {
    if (!app.panel) return;
    app_save_if_dirty(app);  // 用户改完就切走很自然，落盘不能等退出
    app.state.box_view.edit.close();  // 悬空的输入框比看不见的窗口更让人困惑
    app.state.browse.path_edit.close();
    app.state.todo.input.close();
    app.state.todo.edit.close();
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

    g_crash_log = join_path(app.paths.exe_dir, L"crash.log");
    ::SetUnhandledExceptionFilter(crash_filter);

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
