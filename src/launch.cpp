#include "launch.h"

#include <windows.h>  // 必须先于 shellapi.h / shlobj.h
#include <shellapi.h>
#include <shlobj.h>

#include "model/paths.h"

namespace sg {

bool launch_item(const LaunchItem& item, std::wstring* err) {
    if (item.target.empty()) {
        if (err) *err = L"条目没有目标路径";
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;  // 自己报错，不让系统弹框
    sei.lpVerb = L"open";
    sei.lpFile = item.target.c_str();
    sei.lpParameters = item.args.empty() ? nullptr : item.args.c_str();
    sei.lpDirectory = item.workdir.empty() ? nullptr : item.workdir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&sei)) {
        if (err) {
            const DWORD e = ::GetLastError();
            wchar_t buf[256] = {};
            ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                             0, buf, 256, nullptr);
            *err = L"启动失败：";
            *err += item.target;
            if (buf[0]) {
                *err += L"\n";
                *err += buf;
            }
        }
        return false;
    }
    return true;
}

bool resolve_lnk(const std::wstring& lnk_path, LaunchItem& out) {
    IShellLinkW* link = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link)))) {
        return false;
    }

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        if (SUCCEEDED(file->Load(lnk_path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH] = {};
            wchar_t args[1024] = {};
            wchar_t workdir[MAX_PATH] = {};
            wchar_t icon[MAX_PATH] = {};
            int icon_index = 0;
            if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) &&
                target[0]) {
                link->GetArguments(args, 1024);
                link->GetWorkingDirectory(workdir, MAX_PATH);
                // 显式设置了图标时才用它，否则交给 target 自己
                if (SUCCEEDED(link->GetIconLocation(icon, MAX_PATH, &icon_index)) && icon[0]) {
                    out.icon = icon;
                }
                out.target = target;
                out.args = args;
                out.workdir = workdir;
                ok = true;
            }
        }
        file->Release();
    }
    link->Release();
    return ok;
}

LaunchItem item_from_path(const std::wstring& path) {
    LaunchItem item;
    item.name = file_name(path);
    const std::wstring ext = extension_of(path);

    if (ext == L".lnk") {
        // 解析失败也要保留条目：target 落到 .lnk 本身，双击仍能启动
        LaunchItem resolved;
        if (resolve_lnk(path, resolved)) {
            if (!item.name.empty()) resolved.name = item.name;
            return resolved;
        }
    }

    item.target = path;
    return item;
}

LaunchItem item_from_path_keep_name(const std::wstring& path, const std::wstring& name) {
    LaunchItem item = item_from_path(path);
    if (!name.empty()) item.name = name;
    return item;
}

}  // namespace sg
