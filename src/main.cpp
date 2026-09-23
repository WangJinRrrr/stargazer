#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app.h"
#include "icons.h"

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
        // 找控制窗口而不是面板：面板可能还没建（懒加载）
        if (HWND existing = ::FindWindowW(sg::kCtlClass, nullptr)) {
            ::PostMessageW(existing, sg::WM_APP_SHOW, 0, 0);
        }
        ::CloseHandle(once);
        return 0;
    }

    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    sg::App app;
    if (!sg::app_init(app, inst)) {
        ::CoUninitialize();
        if (once) ::CloseHandle(once);
        return 1;
    }

    sg::icons_init(app.ctl);

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
    sg::app_save_if_dirty(app);
    sg::app_shutdown(app);
    ::CoUninitialize();
    if (once) ::CloseHandle(once);
    return 0;
}
