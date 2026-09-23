#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app.h"
#include "dragdrop.h"  // TEMP(Task 8 删除)：dragdrop_test_invoke
#include "fs_work.h"
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

// TEMP(Task 8 删除)：--drop-test <path> 启动后直接调一次拖入回调，
// 用来验证“拖入按当前视图分发”这段粘连代码（OLE 拖放无法在脚本里模拟）。
std::wstring drop_test_path() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return std::wstring();
    std::wstring found;
    for (int i = 1; i + 1 < argc; ++i) {
        if (::_wcsicmp(argv[i], L"--drop-test") == 0) {
            found = argv[i + 1];
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

    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    sg::App app;
    if (!sg::app_init(app, inst)) {
        ::CoUninitialize();
        if (once) ::CloseHandle(once);
        return 1;
    }

    sg::icons_init(app.ctl);
    sg::fs_init(app.ctl);

    // 先读数据：app_show 要用已加载的分组与 ui.txt 里的窗口尺寸
    sg::app_load(app);
    // 开机自启时只驻留托盘，不弹窗、不抢焦点
    if (!has_autostart_flag()) sg::app_show(app);

    // TEMP(Task 8 删除)
    const std::wstring drop_test = drop_test_path();
    if (!drop_test.empty()) sg::dragdrop_test_invoke({ drop_test });

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    sg::icons_shutdown();
    sg::fs_shutdown();
    sg::app_save_if_dirty(app);
    sg::app_save_ui(app);
    sg::app_shutdown(app);
    ::CoUninitialize();
    if (once) ::CloseHandle(once);
    return 0;
}
