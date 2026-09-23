#include "text_io.h"

#include <windows.h>

namespace sg {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

bool read_file_utf8(const std::wstring& path, std::wstring& out) {
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        return false;
    }
    if (size.QuadPart > 32 * 1024 * 1024) {  // 数据文件不可能这么大，防手改坏
        ::CloseHandle(h);
        return false;
    }

    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = buf.empty()
        ? TRUE
        : ::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    buf.resize(read);

    if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf.erase(0, 3);  // 容忍用户用记事本加了 BOM
    }
    if (buf.empty()) {
        out.clear();
        return true;
    }
    // MB_ERR_INVALID_CHARS：非 UTF-8（例如记事本存成了 ANSI）必须报错。
    // 否则非法字节会被静默替换成 U+FFFD，用户改一次就被写成乱码，永久损坏。
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data(),
                                        static_cast<int>(buf.size()), nullptr, 0);
    if (n <= 0) return false;
    out.assign(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data(), static_cast<int>(buf.size()),
                          out.data(), n);
    return true;
}

bool write_file_utf8_atomic(const std::wstring& path, const std::wstring& text) {
    const std::wstring tmp = path + L".tmp";
    const std::string bytes = wide_to_utf8(text);

    const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = bytes.empty()
        ? TRUE
        : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flushed = ok && ::FlushFileBuffers(h);
    ::CloseHandle(h);

    if (!ok || !flushed || written != bytes.size()) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    const BOOL moved = ::MoveFileExW(tmp.c_str(), path.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!moved) ::DeleteFileW(tmp.c_str());
    return moved != FALSE;
}

}  // namespace sg
