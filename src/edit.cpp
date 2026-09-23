#include "edit.h"

#include <commctrl.h>

namespace sg {

namespace {

constexpr UINT_PTR kSubclassId = 1;

HBRUSH g_bg_brush = nullptr;

LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                               DWORD_PTR ref) {
    InlineEdit* e = reinterpret_cast<InlineEdit*>(ref);

    switch (msg) {
        case WM_KEYDOWN:
            // 视图先过一遍：方向键等导航键不该被输入框吃掉
            if (e->on_key && e->on_key(static_cast<UINT>(wp))) return 0;
            if (wp == VK_RETURN) {
                if (e->on_commit) e->on_commit(e->text());
                e->close();
                if (e->parent) ::SetFocus(e->parent);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (e->on_cancel) e->on_cancel();
                e->close();
                if (e->parent) ::SetFocus(e->parent);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            // 点到别处视为提交（输入框的最后内容不会白打）
            if (e && e->hwnd && !e->committing) {
                e->committing = true;
                if (e->on_commit) e->on_commit(e->text());
                if (e->keep_open_on_blur) {
                    // 搜索框：内容已提交，输入框留着（网格抢焦点时不能让它消失）
                } else {
                    e->close();
                }
                e->committing = false;
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == VK_TAB) return 0;  // Tab 不插字符，留给视图切控件
            break;
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, edit_subclass, id);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

HBRUSH edit_bg_brush() {
    if (!g_bg_brush) {
        // 与 Theme::card 一致的实色（37,39,45）
        g_bg_brush = ::CreateSolidBrush(RGB(37, 39, 45));
    }
    return g_bg_brush;
}

void InlineEdit::open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
                      std::function<void(const std::wstring&)> commit,
                      std::function<void()> cancel) {
    close();
    parent = parent_wnd;
    on_commit = std::move(commit);
    on_cancel = std::move(cancel);
    committing = false;

    if (font) ::DeleteObject(font);
    const int height = -::MulDiv(12, static_cast<int>(dpi > 0.f ? dpi : 96.f), 96);
    font = ::CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    hwnd = ::CreateWindowExW(0, L"EDIT", initial.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent,
                             nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ::SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // 全选，直接输入即替换
    ::SetWindowSubclass(hwnd, edit_subclass, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    focus();
}

void InlineEdit::set_rect(const RECT& rc) {
    if (hwnd) {
        ::SetWindowPos(hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void InlineEdit::close() {
    if (!hwnd) return;
    HWND tmp = hwnd;
    hwnd = nullptr;  // 先清空，避免 DestroyWindow 触发的失焦消息重入
    ::DestroyWindow(tmp);
    if (font) {
        ::DeleteObject(font);
        font = nullptr;
    }
    on_commit = nullptr;
    on_cancel = nullptr;
    on_key = nullptr;
}

std::wstring InlineEdit::text() const {
    if (!hwnd) return std::wstring();
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len) + 1, L'\0');
    const int got = ::GetWindowTextW(hwnd, out.data(), len + 1);
    out.resize(static_cast<size_t>(got > 0 ? got : 0));
    return out;
}

void InlineEdit::focus() {
    if (hwnd) ::SetFocus(hwnd);
}

}  // namespace sg
