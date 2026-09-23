#include "model/rowformat.h"

namespace sg {

std::wstring escape_field(const std::wstring& v) {
    std::wstring out;
    out.reserve(v.size() + 8);
    for (wchar_t c : v) {
        switch (c) {
            case L'\\': out += L"\\\\"; break;
            case L'\t': out += L"\\t"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': break;  // 换行统一为 \n
            default: out += c; break;
        }
    }
    return out;
}

std::wstring unescape_field(const std::wstring& v) {
    std::wstring out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            const wchar_t n = v[i + 1];
            if (n == L'\\') { out += L'\\'; ++i; continue; }
            if (n == L't') { out += L'\t'; ++i; continue; }
            if (n == L'n') { out += L'\n'; ++i; continue; }
            // 其它 "\x" 原样保留（保证手改文件不会被吞字符）
        }
        out += v[i];
    }
    return out;
}

std::wstring join_row(const std::vector<std::wstring>& fields) {
    std::wstring out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) out += L'\t';
        out += escape_field(fields[i]);
    }
    return out;
}

// escape_field never emits a raw TAB, so splitting on raw TAB is exact.
bool split_row(const std::wstring& line, size_t expect, std::vector<std::wstring>& out) {
    out.clear();
    std::wstring cur;
    for (wchar_t c : line) {
        if (c == L'\t') {
            out.push_back(unescape_field(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(unescape_field(cur));
    return out.size() == expect;
}

std::vector<std::vector<std::wstring>> parse_rows(const std::wstring& text, size_t expect, int& bad) {
    bad = 0;
    std::vector<std::vector<std::wstring>> rows;
    std::wstring line;
    line.reserve(256);

    for (size_t i = 0; i <= text.size(); ++i) {
        const bool at_end = (i == text.size());
        if (at_end || text[i] == L'\n') {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!line.empty()) {
                std::vector<std::wstring> f;
                if (split_row(line, expect, f)) {
                    rows.push_back(std::move(f));
                } else {
                    ++bad;
                }
            }
            line.clear();
        } else {
            line += text[i];
        }
    }
    return rows;
}

std::wstring build_text(const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring out;
    for (const auto& r : rows) {
        out += join_row(r);
        out += L'\n';
    }
    return out;
}

}  // namespace sg
