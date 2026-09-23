#include "persist.h"

#include <windows.h>

#include "model/paths.h"
#include "text_io.h"

namespace sg {

namespace {
const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kRunValue = L"stargazer";

// 读回 Run 值里引号包裹的 exe 路径；失败返回空
std::wstring autostart_target() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buf[1024] = {};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG r = ::RegQueryValueExW(key, kRunValue, nullptr, &type,
                                      reinterpret_cast<BYTE*>(buf), &bytes);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ) return std::wstring();

    const std::wstring v = buf;
    const size_t a = v.find(L'"');
    if (a == std::wstring::npos) return std::wstring();
    const size_t b = v.find(L'"', a + 1);
    if (b == std::wstring::npos) return std::wstring();
    return v.substr(a + 1, b - a - 1);
}
}  // namespace

bool dir_writable(const std::wstring& dir) {
    const DWORD attr = ::GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const std::wstring probe = join_path(dir, L".writetest");
    const HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

bool init_paths(Paths& out) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    out.exe_dir = buf;
    const size_t cut = out.exe_dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return false;
    out.exe_dir.resize(cut);

    out.data_dir = join_path(out.exe_dir, L"data");
    ::CreateDirectoryW(out.data_dir.c_str(), nullptr);

    out.writable = dir_writable(out.data_dir);
    return true;
}

std::wstring data_file(const Paths& p, const wchar_t* name) {
    return join_path(p.data_dir, name);
}

std::wstring exe_path() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return buf;
}

bool save_text(const Paths& p, const wchar_t* name, const std::wstring& text) {
    return write_file_utf8_atomic(data_file(p, name), text);
}

bool load_text(const Paths& p, const wchar_t* name, std::wstring& out) {
    return read_file_utf8(data_file(p, name), out);
}

bool data_file_exists(const Paths& p, const wchar_t* name) {
    const DWORD a = ::GetFileAttributesW(data_file(p, name).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool backup_bad(const Paths& p, const wchar_t* name) {
    const std::wstring src = data_file(p, name);
    const std::wstring dst = src + L".bad";
    ::DeleteFileW(dst.c_str());  // 只保留最近一次坏文件
    return ::MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool autostart_enabled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const LONG r = ::RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr);
    ::RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool autostart_set(bool enabled, const std::wstring& exe_path) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                          nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    if (enabled) {
        const std::wstring cmd = L"\"" + exe_path + L"\" --autostart";
        const DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        ok = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(cmd.c_str()), bytes) == ERROR_SUCCESS;
    } else {
        const LONG r = ::RegDeleteValueW(key, kRunValue);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    ::RegCloseKey(key);
    return ok;
}

bool autostart_heal(const std::wstring& exe_path) {
    if (!autostart_enabled()) return true;
    const std::wstring recorded = autostart_target();
    if (recorded.empty()) return true;
    if (normalize_key(recorded) == normalize_key(exe_path)) return true;
    return autostart_set(true, exe_path);  // 程序被挪动过，改写为新路径
}

}  // namespace sg
