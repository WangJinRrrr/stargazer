#include "model/todo_kind.h"

#include <cwctype>

#include "model/paths.h"

namespace sg {

namespace {

wchar_t lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

// 大小写不敏感的前缀判断
bool starts_with_ci(const std::wstring& s, const wchar_t* prefix) {
    size_t i = 0;
    for (; prefix[i] != L'\0'; ++i) {
        if (i >= s.size() || lower(s[i]) != lower(prefix[i])) return false;
    }
    return true;
}

bool is_space(wchar_t c) { return std::iswspace(static_cast<wint_t>(c)) != 0; }

std::wstring trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::wstring todo_kind_to_string(TodoKind k) {
    switch (k) {
        case TodoKind::Link:
            return L"link";
        case TodoKind::Image:
            return L"image";
        case TodoKind::Text:
        default:
            return L"text";
    }
}

TodoKind todo_kind_from_string(const std::wstring& s) {
    if (s == L"link") return TodoKind::Link;
    if (s == L"image") return TodoKind::Image;
    // 未知值（手改数据）按文字处理：宁可当普通文字，也不要让用户的记录消失
    return TodoKind::Text;
}

TodoKind todo_kind_from_text(const std::wstring& text) {
    const std::wstring t = trim(text);
    if (starts_with_ci(t, L"http://") || starts_with_ci(t, L"https://")) return TodoKind::Link;
    return TodoKind::Text;
}

bool is_image_path(const std::wstring& path) {
    std::wstring ext = extension_of(path);  // 含点；无扩展名返回空
    if (ext.empty()) return false;
    for (auto& c : ext) c = lower(c);
    static const wchar_t* kOk[] = { L".png", L".jpg", L".jpeg", L".gif", L".bmp",
                                    L".webp", L".ico", L".tif", L".tiff" };
    for (const wchar_t* e : kOk) {
        if (ext == e) return true;
    }
    return false;
}

bool todo_is_owned_copy(const std::wstring& images_dir, const std::wstring& path) {
    if (path.empty() || images_dir.empty()) return false;
    const std::wstring dir = normalize_key(images_dir);
    const std::wstring p = normalize_key(path);
    if (p.size() <= dir.size()) return false;  // 目录本身不算
    if (p.compare(0, dir.size(), dir) != 0) return false;
    // 必须是目录下的直接/间接成员：下一个字符得是分隔符（否则 images2 会被误判）
    return p[dir.size()] == L'\\';
}

bool todo_copy_still_used(const std::vector<std::wstring>& other_attaches,
                          const std::wstring& path) {
    if (path.empty()) return false;
    const std::wstring key = normalize_key(path);
    for (const auto& a : other_attaches) {
        if (!a.empty() && normalize_key(a) == key) return true;
    }
    return false;
}

}  // namespace sg
