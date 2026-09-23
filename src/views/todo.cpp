#include "views/todo.h"

#include <algorithm>
#include <ctime>

#include "app.h"
#include "icons.h"
#include "model/paths.h"

namespace sg {

namespace {

// 文本宽度（给链接下划线用）。取不到就返回 0，不影响其它绘制。
float text_width(Renderer& r, const std::wstring& s, IDWriteTextFormat* fmt, float max_w) {
    if (!r.dwrite || !fmt || s.empty()) return 0.f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(r.dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, max_w,
                                          1000.f, &layout)) ||
        !layout) {
        return 0.f;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.width;
}

// 一条列表项；row 是它的整行矩形（文字 28、图片 96）
void draw_row(App& app, int index, const D2D1_RECT_F& row) {
    Renderer& r = app.render;
    const TodoItem& item = app.state.todos[static_cast<size_t>(index)];
    const bool selected = index == app.state.todo.sel;

    if (selected) {
        r.fill_round_rect(row, 4.f, r.theme.accent);
    } else if (index == app.state.todo.hover) {
        r.fill_round_rect(row, 4.f, r.theme.hover);
    }

    const D2D1_COLOR_F normal = selected ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.text;
    const D2D1_COLOR_F text_color = item.done ? r.theme.text_dim : normal;

    // 左侧复选框（16×16）。图片行的复选框对齐缩略图顶部，其余垂直居中。
    const float cb = 16.f;
    const float cy = (item.kind == TodoKind::Image)
                         ? row.top + 8.f
                         : row.top + (todo_row_height(item.kind) - cb) / 2.f;
    const D2D1_RECT_F box =
        D2D1::RectF(row.left + 6.f, cy, row.left + 6.f + cb, cy + cb);
    if (item.done) {
        r.fill_round_rect(box, 4.f, r.theme.text_dim);
        // 勾：两段短线拼出来，不引入字体符号
        r.fill_rect(D2D1::RectF(box.left + 3.f, box.top + 8.f, box.left + 7.f, box.top + 12.f),
                    r.theme.bg);
        r.fill_rect(D2D1::RectF(box.left + 7.f, box.top + 4.f, box.left + 13.f, box.top + 8.f),
                    r.theme.bg);
    } else {
        r.stroke_round_rect(box, 4.f, selected ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.border,
                            1.5f);
    }

    const float content_x = box.right + 10.f;
    const float content_w = std::max(0.f, row.right - 6.f - content_x);
    const D2D1_RECT_F lab = D2D1::RectF(content_x, row.top, row.right - 6.f, row.bottom);

    if (item.kind == TodoKind::Image) {
        // 缩略图框：Task 7 用系统缩略图，之前先用图标/色块占位
        const D2D1_RECT_F thumb = D2D1::RectF(content_x, row.top + 4.f, content_x + kTodoThumbW,
                                             row.top + 4.f + kTodoThumbH);
        if (ID2D1Bitmap* bmp = icons_get(r, item.attach, false)) {
            r.rt->DrawBitmap(bmp, thumb, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            r.fill_round_rect(thumb, 8.f, ext_color(item.attach));
        }
        const std::wstring label = item.text.empty() ? file_name(item.attach) : item.text;
        const D2D1_RECT_F caption =
            D2D1::RectF(content_x, thumb.bottom + 2.f, row.right - 6.f, row.bottom);
        r.text(caption, label, r.format(11.f), item.missing ? r.theme.text_dim : text_color);
        if (item.missing) {
            // 引用型图片被外部改名/删除：灰显 + 删除线 + 说明
            const float mid = (caption.top + caption.bottom) / 2.f;
            r.fill_rect(D2D1::RectF(caption.left, mid - 0.5f, caption.right, mid + 0.5f),
                        r.theme.text_dim);
            r.text(D2D1::RectF(thumb.right + 8.f, row.top + 4.f, row.right - 6.f, row.top + 24.f),
                   L"图片已不存在", r.format(11.f), r.theme.text_dim);
        }
    } else if (item.kind == TodoKind::Link) {
        // 链接：强调色 + 下划线（选中时用白字，否则强调色在强调色底上看不见）
        const D2D1_COLOR_F link_color =
            item.done ? r.theme.text_dim : (selected ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.accent);
        r.text(lab, item.text, r.format(13.f), link_color);
        const float w = std::min(text_width(r, item.text, r.format(13.f), content_w), content_w);
        if (w > 4.f) {
            const float uy = row.top + todo_row_height(TodoKind::Text) - 8.f;
            r.fill_rect(D2D1::RectF(content_x, uy, content_x + w, uy + 1.f), link_color);
        }
    } else {
        r.text(lab, item.text, r.format(13.f), text_color);
    }
}

}  // namespace

D2D1_RECT_F todo_input_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, client.height - kPad - kTodoInputH, client.width - kPad,
                       client.height - kPad);
}

D2D1_RECT_F todo_list_rect(D2D1_SIZE_F client) {
    const float top = kViewTabsH + kPad;
    return D2D1::RectF(kPad, top, client.width - kPad,
                       todo_input_rect(client).top - kPad);
}

int todo_rows_visible(D2D1_SIZE_F client) {
    const D2D1_RECT_F list = todo_list_rect(client);
    const float h = list.bottom - list.top;
    return std::max(1, static_cast<int>(h / kTodoRowTextH));  // 文字行数（图片行更高）
}

void todo_rebuild_layout(AppState& s, D2D1_SIZE_F client) {
    TodoState& t = s.todo;
    t.kinds.clear();
    t.kinds.reserve(s.todos.size());
    for (const auto& item : s.todos) t.kinds.push_back(item.kind);
    t.offsets = todo_row_offsets(t.kinds);

    const int n = static_cast<int>(s.todos.size());
    if (t.sel >= n) t.sel = n > 0 ? n - 1 : -1;
    if (t.sel < -1) t.sel = -1;

    const D2D1_RECT_F list = todo_list_rect(client);
    t.scroll = todo_scroll_for(t.offsets, list.bottom - list.top, t.scroll, t.sel);
}

int todo_hittest(App& app, D2D1_POINT_2F pt) {
    const AppState& s = app.state;
    if (s.todo.offsets.size() < 2) return -1;
    const D2D1_RECT_F list = todo_list_rect(app.render.client_logical());
    if (pt.x < list.left || pt.x > list.right || pt.y < list.top || pt.y > list.bottom) return -1;
    const float y = pt.y - list.top + s.todo.scroll;
    return todo_row_at(s.todo.offsets, y);
}

bool todo_checkbox_hit_in_row(App& app, int row, D2D1_POINT_2F pt) {
    const AppState& s = app.state;
    if (row < 0 || row >= static_cast<int>(s.todos.size())) return false;
    const D2D1_RECT_F list = todo_list_rect(app.render.client_logical());
    const float top = list.top + s.todo.offsets[static_cast<size_t>(row)] - s.todo.scroll;
    const float h = todo_row_height(s.todos[static_cast<size_t>(row)].kind);
    if (pt.y < top || pt.y > top + h) return false;
    // 复选框热区放宽一点（点到行左端就算），比精确的 16px 方块好点得多
    return pt.x >= list.left && pt.x <= list.left + 30.f;
}

bool todo_checkbox_hit(App& app, D2D1_POINT_2F pt) {
    return todo_checkbox_hit_in_row(app, todo_hittest(app, pt), pt);
}

void todo_render(App& app) {
    Renderer& r = app.render;
    AppState& s = app.state;
    TodoState& t = s.todo;
    const D2D1_SIZE_F client = r.client_logical();
    const D2D1_RECT_F list = todo_list_rect(client);
    const int n = static_cast<int>(s.todos.size());

    if (n == 0) {
        r.text(D2D1::RectF(list.left, list.top, list.right, list.top + 24.f),
               L"还没有记录：在下面输入，或 Ctrl+V 粘文字 / 链接 / 截图", r.format(13.f),
               r.theme.text_dim);
    } else {
        // 虚拟化：从 scroll 位置对应的第一行画到超出列表底部为止
        int first = todo_row_at(t.offsets, t.scroll);
        if (first < 0) first = 0;
        for (int i = first; i < n; ++i) {
            const float y = list.top + t.offsets[static_cast<size_t>(i)] - t.scroll;
            if (y > list.bottom) break;
            const float h = todo_row_height(s.todos[static_cast<size_t>(i)].kind);
            draw_row(app, i, D2D1::RectF(list.left, y, list.right, y + h));
        }
    }

    // 底部输入框：文字由 EDIT 子控件自己画，这里只画底与占位提示
    const D2D1_RECT_F in = todo_input_rect(client);
    r.fill_round_rect(in, 6.f, r.theme.card);
    if (!t.input.is_open()) {
        r.text(D2D1::RectF(in.left + 10.f, in.top, in.right - 10.f, in.bottom), L"记一条…",
               r.format(13.f), r.theme.text_dim);
    }
}

bool todo_keydown(App& app, UINT vk) {
    AppState& s = app.state;
    TodoState& t = s.todo;
    const int n = static_cast<int>(s.todos.size());
    const D2D1_RECT_F list = todo_list_rect(app.render.client_logical());
    const float viewport = list.bottom - list.top;
    const int rows = todo_rows_visible(app.render.client_logical());

    switch (vk) {
        case VK_DOWN:
            if (n > 0) t.sel = (t.sel < 0) ? 0 : std::min(t.sel + 1, n - 1);
            break;
        case VK_UP:
            if (n == 0) break;
            if (t.sel <= 0) {
                t.sel = -1;
                todo_sync_input(app);
                t.input.focus();  // 首行再往上 = 回到输入框
                ::InvalidateRect(app.panel, nullptr, FALSE);
                return true;
            }
            --t.sel;
            break;
        case VK_PRIOR:
            if (n > 0) t.sel = std::max(0, (t.sel < 0 ? 0 : t.sel) - rows);
            break;
        case VK_NEXT:
            if (n > 0) t.sel = std::min(n - 1, (t.sel < 0 ? 0 : t.sel) + rows);
            break;
        case VK_HOME:
            if (n > 0) t.sel = 0;
            break;
        case VK_END:
            if (n > 0) t.sel = n - 1;
            break;
        default:
            return false;
    }
    t.scroll = todo_scroll_for(t.offsets, viewport, t.scroll, t.sel);
    ::InvalidateRect(app.panel, nullptr, FALSE);
    return true;
}

void todo_sync_input(App& app) {
    TodoState& t = app.state.todo;
    const RECT rc = app.render.to_physical(todo_input_rect(app.render.client_logical()));
    if (t.input.is_open()) {
        t.input.set_rect(rc);
        return;
    }
    t.input.open(
        app.panel, rc, L"", app.render.dpi,
        [&app](const std::wstring& text) {
            // 常驻输入框：回车后必须立刻回来。先自己 close 再重开，
            // 这样 InlineEdit 里“hwnd 变了就不再关一次”的既有判断会保住新开的框。
            TodoState& st = app.state.todo;
            st.input.close();
            todo_sync_input(app);
            todo_add_text(app, text);
            st.input.set_text(L"");
            st.input.focus();
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        nullptr);
    // ↑↓ 等导航键要从输入框转给列表（输入框自己会吃掉方向键）
    t.input.on_key = [&app](UINT vk) {
        switch (vk) {
            case VK_UP:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_HOME:
            case VK_END: {
                HWND parent = app.state.todo.input.parent;
                if (parent) {
                    if (vk == VK_DOWN && app.state.todo.sel < 0 && !app.state.todos.empty()) {
                        app.state.todo.sel = 0;
                        app.state.todo.scroll = todo_scroll_for(
                            app.state.todo.offsets,
                            todo_list_rect(app.render.client_logical()).bottom -
                                todo_list_rect(app.render.client_logical()).top,
                            app.state.todo.scroll, 0);
                    }
                    ::PostMessageW(parent, WM_KEYDOWN, vk, 0);
                    ::SetFocus(parent);
                    ::InvalidateRect(parent, nullptr, FALSE);
                }
                return true;
            }
            default:
                return false;
        }
    };
}

void todo_activate(App& app) {
    todo_rebuild_layout(app.state, app.render.client_logical());
    todo_sync_input(app);
    app.state.todo.input.focus();
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void todo_leave(App& app) {
    app.state.todo.input.close();
    app.state.todo.edit.close();
}

void todo_add_text(App& app, const std::wstring& text) {
    // Task 4 就需要它：底部输入框的回车要能真的记一条（剪贴板/拖入/图片在 Task 6）
    AppState& s = app.state;
    if (text.empty()) return;
    TodoItem item;
    item.id = next_todo_id(s.todos);
    item.created = static_cast<long long>(::time(nullptr));
    item.kind = todo_kind_from_text(text);
    item.text = text;
    s.todos.push_back(std::move(item));
    sort_todos(s.todos);
    s.todo.sel = 0;      // 新条目置顶，选中它
    s.todo.scroll = 0.f;
    todo_rebuild_layout(s, app.render.client_logical());
    s.data_dirty = true;
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

// Task 6 实现；先给空实现，让 Task 4 能链接
bool todo_add_from_clipboard(App& app) {
    (void)app;
    return false;
}

bool todo_add_from_paths(App& app, const std::vector<std::wstring>& paths) {
    (void)app;
    (void)paths;
    return false;
}

void todo_on_image_saved(App& app, uint64_t request_id) {
    (void)app;
    (void)request_id;
}

}  // namespace sg
