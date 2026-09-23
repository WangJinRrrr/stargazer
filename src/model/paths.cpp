#include "model/paths.h"

namespace sg {

static wchar_t lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

static bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring normalize_key(const std::wstring& path) {
    std::wstring out;
    out.reserve(path.size());
    // UNC 前导 "\\\\" 会被折叠逻辑吃掉一个，先记下再补回
    const bool unc = path.size() >= 2 && is_sep(path[0]) && is_sep(path[1]);

    for (size_t i = 0; i < path.size(); ++i) {
        const wchar_t c = path[i];
        if (is_sep(c)) {
            if (unc && out.empty()) {
                out += L'\\';
                out += L'\\';
                continue;
            }
            if (!out.empty() && is_sep(out.back())) continue;  // 折叠
            out += L'\\';
        } else {
            out += lower(c);
        }
    }
    while (out.size() > 3 && is_sep(out.back())) out.pop_back();
    return out;
}

std::wstring file_name(const std::wstring& path) {
    if (path.empty() || is_sep(path.back())) return std::wstring();
    size_t i = path.size();
    while (i > 0 && !is_sep(path[i - 1])) --i;
    return path.substr(i);
}

std::wstring extension_of(const std::wstring& path) {
    const std::wstring name = file_name(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 == name.size()) return std::wstring();
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& c : ext) c = lower(c);
    return L"." + ext;
}

std::wstring join_path(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (!is_sep(dir.back())) return dir + L"\\" + name;
    return dir + name;
}

}  // namespace sg
