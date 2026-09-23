#include "model/search.h"

namespace sg {

static wchar_t lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

static bool is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool contains_ci(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

int natural_compare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            const size_t si = i, sj = j;
            while (i < a.size() && is_digit(a[i])) ++i;
            while (j < b.size() && is_digit(b[j])) ++j;
            size_t zi = si, zj = sj;
            while (zi + 1 < i && a[zi] == L'0') ++zi;  // 去前导零，至少留一位
            while (zj + 1 < j && b[zj] == L'0') ++zj;
            const size_t li = i - zi, lj = j - zj;
            if (li != lj) return li < lj ? -1 : 1;
            const int c = a.compare(zi, li, b, zj, lj);
            if (c != 0) return c < 0 ? -1 : 1;
            continue;
        }
        const wchar_t ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace sg
