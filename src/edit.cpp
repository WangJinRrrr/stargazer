#include "edit.h"

#include <commctrl.h>
#include <uxtheme.h>  // SetWindowTheme：多行 EDIT 的滚动条也要深色

#include "render.h"  // ui_font_family()：输入框与自绘文字同脸

namespace sg {

namespace {

constexpr UINT_PTR kSubclassId = 1;

HBRUSH g_bg_brush = nullptr;

LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                               DWORD_PTR ref) {
    InlineEdit* e = reinterpret_cast<InlineEdit*>(ref);

    switch (msg) {
        case WM_KEYDOWN:
            // 全局热键（Ctrl+1..3 切视图、Ctrl+Tab 轮换）不该被输入框吃掉：
            // 待办的常驻输入框一进视图就抢焦点，不转发的话视图永远切不动。
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                ((wp >= '1' && wp <= '3') || wp == VK_TAB)) {
                if (e->parent) {
                    ::PostMessageW(e->parent, WM_KEYDOWN, wp, 0);
                    return 0;
                }
            }
            // 视图先过一遍：方向键等导航键不该被输入框吃掉
            if (e->on_key && e->on_key(static_cast<UINT>(wp))) return 0;
            if (wp == VK_RETURN) {
                // 回调里可能又开了一个新输入框（新建条目的两步输入），
                // 此时不能再 close()，否则关掉的是刚建好的那个（真 bug，已修）
                HWND before = e->hwnd;
                if (e->on_commit) e->on_commit(e->text());
                if (e->hwnd == before) {
                    if (e->keep_open_on_blur) {
                        // 常驻输入框：回车只提交，框留着（内容由视图自己清）
                        e->focus();
                    } else {
                        e->close();
                        if (e->parent) ::SetFocus(e->parent);
                    }
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                HWND parent = e->parent;
                if (e->text().empty() && parent) {
                    // 输入框已空：把 Esc 交给父窗口（搜索框 => 隐藏面板）。
                    // 否则呼出后第一次 Esc 只会被输入框吃掉，窗口不消失。
                    ::PostMessageW(parent, WM_KEYDOWN, VK_ESCAPE, 0);
                    return 0;
                }
                if (e->on_cancel) e->on_cancel();  // 有内容：先清空
                if (e->hwnd) ::SetWindowTextW(e->hwnd, L"");  // 输入框自己也要清
                if (e->keep_open_on_blur) return 0;  // 搜索框留着，等第二次 Esc
                e->close();
                if (parent) ::SetFocus(parent);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            // 点到别处视为提交（输入框的最后内容不会白打）
            if (e && e->hwnd && !e->committing) {
                e->committing = true;
                // 提交回调里可能重开一个输入框（常驻框：close + open）→
                // 那种情况下别把刚建好的新框关掉（否则框看着还在，实际已经死了）
                HWND before = e->hwnd;
                if (e->on_commit) e->on_commit(e->text());
                if (!e->keep_open_on_blur && e->hwnd == before) e->close();
                e->committing = false;
                return 0;
            }
            break;
        case WM_CHAR:
            // Enter 在本程序里一律是“提交”，不能落成换行符：
            // TranslateMessage 已经先于 WM_KEYDOWN 交了这条 WM_CHAR 到队列，
            // 多行 EDIT 会把它真的插进去 —— 上一句刚把内容清空，下一条就多一个空首行。
            if (wp == VK_TAB || wp == VK_RETURN || wp == L'\n') return 0;
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
        // 与“Theme::control 叠在 Theme::bg 上”的实色一致（0x202020 + 6% 白 ≈ #2E2E2E），
        // 否则编辑框在容器里会露出一块颜色不对的方角。
        g_bg_brush = ::CreateSolidBrush(RGB(46, 46, 46));
    }
    return g_bg_brush;
}

void InlineEdit::open(HWND parent_wnd, const RECT& rc, const std::wstring& initial, float dpi,
                      std::function<void(const std::wstring&)> commit,
                      std::function<void()> cancel, float pad_x, bool center) {
    close();
    parent = parent_wnd;
    last_rc = rc;
    on_commit = std::move(commit);
    on_cancel = std::move(cancel);
    // 角色相关标志必须在 open 时归零：否则同一个 InlineEdit 被搜索框/重命名框/新建框
    // 轮着用时，上一次身份的标志会继承下来（真 bug：重命名框会像搜索框一样失焦不关）
    on_key = nullptr;
    keep_open_on_blur = false;
    committing = false;

    if (font) ::DeleteObject(font);
    const int height = -::MulDiv(14, static_cast<int>(dpi > 0.f ? dpi : 96.f), 96);
    font = ::CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, ui_font_family());

    const DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (center ? ES_CENTER : 0) |
                        (multiline ? (ES_MULTILINE | ES_AUTOVSCROLL) : 0);
    hwnd = ::CreateWindowExW(0, L"EDIT", initial.c_str(), style, rc.left, rc.top,
                             rc.right - rc.left, rc.bottom - rc.top, parent, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        if (font) {
            ::DeleteObject(font);  // 创建失败时不留下泄漏的字体
            font = nullptr;
        }
        return;
    }

    ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);  // 深色滚动条/插入符
    // 内边距：默认 10 与容器里占位文字的 +10 对齐；
    // 改名框传 0 —— 它的矩形已经就是被改的那段文字的位置
    const int pad = ::MulDiv(static_cast<int>(pad_x), static_cast<int>(dpi > 0.f ? dpi : 96.f), 96);
    ::SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad, pad));
    if (multiline) {
        // 多行 EDIT 的文字区默认贴着上沿；给一点上边距让它在内框里看着居中
        const int top_pad = ::MulDiv(4, static_cast<int>(dpi > 0.f ? dpi : 96.f), 96);
        RECT fr{ 0, top_pad, rc.right - rc.left, rc.bottom - rc.top };
        ::SendMessageW(hwnd, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&fr));
    }
    ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // 全选，直接输入即替换
    ::SetWindowSubclass(hwnd, edit_subclass, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    focus();
}

void InlineEdit::set_rect(const RECT& rc) {
    if (hwnd) {
        last_rc = rc;
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

void InlineEdit::set_text(const std::wstring& text) {
    if (!hwnd) return;
    ::SetWindowTextW(hwnd, text.c_str());
}

void InlineEdit::focus() {
    if (hwnd) ::SetFocus(hwnd);
}

void edit_draw_focus_ring(Renderer& r, const InlineEdit& e) {
    if (!e.is_open()) return;
    const D2D1_RECT_F rc = r.to_logical_rect(e.last_rc);
    // 外扩 1px：画在 EDIT 的矩形之外才看得见（EDIT 是方底且盖在容器上面）。
    // 圆角与 views/grid.h 的 kRadiusSm(4) 一致。
    r.stroke_round_rect(
        D2D1::RectF(rc.left - 1.f, rc.top - 1.f, rc.right + 1.f, rc.bottom + 1.f), 4.f,
        r.theme.accent, 1.f);
}

D2D1_RECT_F edit_box_rect(const D2D1_RECT_F& area) {
    constexpr float kBox = 20.f;
    if (area.bottom - area.top > 28.f) {
        // 多行文字行（40）：文字从第一行开始排 → 框也贴顶
        return D2D1::RectF(area.left, area.top, area.right, area.top + kBox);
    }
    const float c = (area.top + area.bottom) / 2.f;
    return D2D1::RectF(area.left, c - kBox / 2.f, area.right, c + kBox / 2.f);
}

}  // namespace sg
