#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app.h"
#include "fs_work.h"
#include "icons.h"
#include "images.h"

namespace {

bool has_autostart_flag() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (::_wcsicmp(argv[i], L"--autostart") == 0) {
            found = true;
            break;
        }
    }
    ::LocalFree(argv);
    return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // per-monitor-v2：多显示器不同缩放时不糊
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Review Focus 5：已有实例时唤出它并退出自己，绝不启动第二个进程
    HANDLE once = ::CreateMutexW(nullptr, TRUE, L"Local\\stargazer-singleton");
    if (once && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 找控制窗口而不是面板：面板可能还没建（懒加载）。
        // 若自己是 --autostart 启动的，就不要去弹已有实例的面板：
        // 开机时用户可能已经手动开着它，弹出来就是挠人。
        if (!has_autostart_flag()) {
            if (HWND existing = ::FindWindowW(sg::kCtlClass, nullptr)) {
                ::PostMessageW(existing, sg::WM_APP_SHOW, 0, 0);
            }
        }
        ::CloseHandle(once);
        return 0;
    }

    // 拖放要求 OLE 初始化：只调 CoInitializeEx 时 RegisterDragDrop 会返回 0x8007000E (E_OUTOFMEMORY)，
    // 面板根本不会成为拖放目标 —— 症状是拖动时光标全程显示禁止（实测日志确认）。
    // OleInitialize 内部既做 STA 初始化也做 OLE 初始化（返回值 S_FALSE = 已经初始化过）。
    const HRESULT hr = ::OleInitialize(nullptr);
    if (FAILED(hr)) return 1;

    sg::App app;
    if (!sg::app_init(app, inst)) {
        ::OleUninitialize();
        if (once) ::CloseHandle(once);
        return 1;
    }

    sg::icons_init(app.ctl);
    sg::fs_init(app.ctl);
    sg::images_init(app.ctl);

    // 先读数据：app_show 要用已加载的分组与 ui.txt 里的窗口尺寸
    sg::app_load(app);
    // 开机自启时只驻留托盘，不弹窗、不抢焦点
    if (!has_autostart_flag()) sg::app_show(app);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    sg::icons_shutdown();
    sg::fs_shutdown();
    sg::images_shutdown();
    sg::app_save_if_dirty(app);
    sg::app_save_ui(app);
    sg::app_shutdown(app);
    ::OleUninitialize();
    if (once) ::CloseHandle(once);
    return 0;
}
