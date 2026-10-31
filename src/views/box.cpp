#include "views/box.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app.h"
#include "clipboard.h"
#include "dragdrop.h"
#include "views/shell.h"

namespace sg {

void box_open_selected(App& app);
void box_rename_box(App& app, int index);
void box_add_box(App& app);
void box_clear_missing(App& app);
void box_pick_files(App& app);
void box_paste(App& app);
void box_switch(App& app, int index);

namespace {

constexpr UINT_PTR kHoverTimer = 1;
constexpr unsigned long long kHoverMs = 160;
constexpr int kDragSlop = 6;

bool ctrl_down() { return (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }
bool shift_down() { return (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

int missing_count(const Box& b) {
    int n = 0;
    for (const auto& it : b.items)
        if (it.missing) ++n;
    return n;
}

bool current_has_missing(const AppState& s) {
    if (s.boxes.empty()) return false;
    const int i = std::clamp(s.box_view.box, 0, static_cast<int>(s.boxes.size()) - 1);
    return missing_count(s.boxes[static_cast<size_t>(i)]) > 0;
}

ShellLayout box_shell(const App& app) {
    const D2D1_SIZE_F cs = app.render.client_logical();
    return shell_measure(cs.width, cs.height, current_has_missing(app.state));
}

const Box* current_box(const AppState& s) {
    if (s.boxes.empty()) return nullptr;
    const int i = std::clamp(s.box_view.box, 0, static_cast<int>(s.boxes.size()) - 1);
    return &s.boxes[static_cast<size_t>(i)];
}

std::vector<int> visible_of(const AppState& s) {
    const Box* b = current_box(s);
    if (!b) return {};
    return box_visible(b->items, s.box_view.filter);
}

int visible_pos(const std::vector<int>& vis, int real) {
    for (int i = 0; i < static_cast<int>(vis.size()); ++i) {
        if (vis[static_cast<size_t>(i)] == real) return i;
    }
    return -1;
}

void invalidate(App& app) {
    if (app.panel) ::InvalidateRect(app.panel, nullptr, FALSE);
}

void track_mouse(App& app) {
    if (app.mouse_tracking || !app.panel) return;
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, app.panel, 0 };
    ::TrackMouseEvent(&tme);
    app.mouse_tracking = true;
}

float ease_out(float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

float hover_amount(const BoxState& b, int real) {
    if (real < 0) return 0.f;
    if (real == b.hover) return b.hover_from < 0 && b.hover_t >= 1.f ? 1.f : b.hover_t;
    if (real == b.hover_from) return 1.f - b.hover_t;
    return 0.f;
}

void set_item_hover(App& app, int real) {
    BoxState& b = app.state.box_view;
    if (b.hover == real) return;
    b.hover_from = b.hover;
    b.hover = real;
    b.hover_t = 0.f;
    b.hover_ms = ::GetTickCount64();
    if (!app.panel || !::SetTimer(app.panel, kHoverTimer, 16, nullptr)) {
        b.hover_t = 1.f;
        b.hover_from = -1;
    }
    invalidate(app);
}

D2D1_RECT_F inset(D2D1_RECT_F r, float d) {
    return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d);
}

void reveal_item(AppState& s, const GridLayout& gl) {
    const auto vis = visible_of(s);
    const int v = visible_pos(vis, s.box_view.sel);
    if (v < 0) return;
    const int row = v / gl.cols;
    if (row < s.box_view.scroll) s.box_view.scroll = row;
    if (row >= s.box_view.scroll + gl.rows_visible) {
        s.box_view.scroll = row - gl.rows_visible + 1;
    }
}

int box_hittest(App& app, D2D1_POINT_2F pt) {
    const AppState& s = app.state;
    if (s.boxes.empty()) return -1;
    const auto vis = visible_of(s);
    const D2D1_SIZE_F cs = app.render.client_logical();
    const int v = grid_hittest(shell_grid(box_shell(app), cs.width, cs.height),
                               static_cast<int>(vis.size()), s.box_view.scroll, pt);
    if (v < 0 || v >= static_cast<int>(vis.size())) return -1;
    return vis[static_cast<size_t>(v)];
}

void request_check(App& app) { app_request_fs_checks(app); }

void clear_filter(App& app) {
    AppState& s = app.state;
    s.box_view.filter.clear();
    if (s.box_view.search.is_open() && !s.box_view.search.text().empty()) {
        s.box_view.search.set_text(L"");
    }
}

void place_edits(App& app) {
    AppState& s = app.state;
    Renderer& r = app.render;
    const ShellLayout sh = box_shell(app);
    if (s.box_view.search.is_open()) {
        s.box_view.search.set_rect(r.to_physical(inset(sh.search, 1.f)));
    }
    if (s.box_view.renaming_box >= 0) {
        const D2D1_RECT_F row =
            shell_row_rect(sh, s.box_view.renaming_box, s.box_view.side_scroll);
        s.box_view.edit.set_rect(r.to_physical(inset(row, 4.f)));
    } else if (s.box_view.renaming_item >= 0) {
        const auto vis = visible_of(s);
        const int v = visible_pos(vis, s.box_view.renaming_item);
        if (v >= 0) {
            const D2D1_SIZE_F cs = r.client_logical();
            const D2D1_RECT_F cell =
                grid_cell_rect(shell_grid(sh, cs.width, cs.height), v, s.box_view.scroll);
            s.box_view.edit.set_rect(r.to_physical(edit_box_rect(grid_label_rect(cell))));
        }
    }
}

EditStyle light_edit(float pad, bool center, COLORREF bg) {
    EditStyle style;
    style.pad_x = pad;
    style.center = center;
    style.paint.bg = bg;
    style.paint.text = RGB(28, 25, 23);
    return style;
}

void open_search(App& app, const std::wstring& seed) {
    AppState& s = app.state;
    BoxState& b = s.box_view;
    if (b.search.is_open()) {
        if (!seed.empty()) {
            const std::wstring t = b.search.text() + seed;
            b.search.set_text(t);
            const int n = static_cast<int>(b.search.text().size());
            ::SendMessageW(b.search.hwnd, EM_SETSEL, n, n);
        }
        b.search.focus();
        return;
    }
    const ShellLayout sh = box_shell(app);
    const std::wstring initial = seed.empty() ? b.filter : seed;
    b.search.open(app.panel, app.render.to_physical(inset(sh.search, 1.f)), initial,
                  app.render.dpi,
                  [&app](const std::wstring& t) {
                      app.state.box_view.filter = t;
                      box_clamp(app.state, app.render.client_logical());
                      invalidate(app);
                  },
                  [&app]() {
                      app.state.box_view.filter.clear();
                      box_clamp(app.state, app.render.client_logical());
                      invalidate(app);
                  },
                  light_edit(14.f, false, RGB(255, 255, 255)));
    b.search.keep_open_on_blur = true;
    b.search.on_key = [&app](UINT vk) {
        AppState& st = app.state;
        if (vk == VK_DOWN) {
            const auto vis = visible_of(st);
            if (st.box_view.sel < 0 && !vis.empty()) st.box_view.sel = vis[0];
            if (app.panel) ::SetFocus(app.panel);
            invalidate(app);
            return true;
        }
        if (vk == VK_RETURN) {
            const auto vis = visible_of(st);
            if (st.box_view.sel < 0 && !vis.empty()) st.box_view.sel = vis[0];
            box_open_selected(app);
            return true;
        }
        return false;
    };
    if (!seed.empty() && b.search.hwnd) {
        const int n = static_cast<int>(b.search.text().size());
        ::SendMessageW(b.search.hwnd, EM_SETSEL, n, n);
    }
}

void set_filter_keep_sel(App& app, const std::wstring& text) {
    AppState& s = app.state;
    if (s.box_view.filter == text) return;
    s.box_view.filter = text;
    const auto vis = visible_of(s);
    if (visible_pos(vis, s.box_view.sel) < 0) {
        s.box_view.sel = vis.empty() ? -1 : vis[0];
    }
    box_clamp(s, app.render.client_logical());
    invalidate(app);
}

bool looks_like_path(const std::wstring& s) {
    if (s.size() >= 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) return true;
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') return true;
    return false;
}

std::vector<std::wstring> paths_from_text(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring line;
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') {
            if (looks_like_path(line)) out.push_back(line);
            line.clear();
            continue;
        }
        line.push_back(c);
    }
    if (looks_like_path(line)) out.push_back(line);
    return out;
}

void paint_button(Renderer& r, const D2D1_RECT_F& rc, const std::wstring& label, bool primary,
                  bool hot, bool down) {
    IDWriteTextFormat* fmt =
        r.format(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (primary) {
        r.fill_round_rect(rc, kRadiusSm, down ? r.theme.accent_press : r.theme.accent);
        r.text(rc, label, fmt, r.theme.on_accent);
        return;
    }
    const D2D1_COLOR_F fill = down ? r.theme.pressed : (hot ? r.theme.hover : r.theme.card);
    r.fill_round_rect(rc, kRadiusSm, fill);
    r.stroke_round_rect(rc, kRadiusSm, hot || down ? r.theme.border_strong : r.theme.border, 1.f);
    r.text(rc, label, fmt, r.theme.text);
}

bool hot_ctrl(const App& app, ShellControl c) {
    return app.state.box_view.chrome_hover == static_cast<int>(c);
}
bool down_ctrl(const App& app, ShellControl c) {
    return app.press == static_cast<int>(c);
}

void draw_empty(Renderer& r, const ShellLayout& sh, bool no_box) {
    D2D1_RECT_F well = sh.grid_area;
    if (well.bottom - well.top < 180.f) well.bottom = well.top + 180.f;
    r.fill_round_rect(well, kRadiusLg, r.theme.card);
    r.stroke_round_rect(well, kRadiusLg, r.theme.border, 1.f);
    const float mid_y = (well.top + well.bottom) / 2.f;
    const D2D1_RECT_F title =
        D2D1::RectF(well.left + 32.f, mid_y - 48.f, well.right - 32.f, mid_y - 12.f);
    const D2D1_RECT_F body =
        D2D1::RectF(well.left + 48.f, mid_y - 4.f, well.right - 48.f, mid_y + 64.f);
    if (no_box) {
        r.text(title, L"从一个盒子开始",
               r.format(22.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
               r.theme.text);
        r.text(D2D1::RectF(body.left, body.top, body.right, body.top + 24.f),
               L"盒子只记住文件在哪里，不会移动或删除原文件。",
               r.format(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
               r.theme.text_dim);
        r.text(D2D1::RectF(body.left, body.top + 26.f, body.right, body.top + 50.f),
               L"点左下角「新盒子」，或直接把文件拖进来。",
               r.format(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
               r.theme.text_dim);
    } else {
        r.text(title, L"把文件拖到这里",
               r.format(22.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
               r.theme.text);
        r.text(D2D1::RectF(body.left, body.top, body.right, body.top + 24.f),
               L"也可以点「添加文件」，或按 Ctrl+V 粘贴路径。",
               r.format(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
               r.theme.text_dim);
        r.text(D2D1::RectF(body.left, body.top + 26.f, body.right, body.top + 50.f),
               L"这里保存的是引用，磁盘上的文件留在原地。",
               r.format(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
               r.theme.text_dim);
    }
}

std::wstring path_of_sel(const AppState& s) {
    const Box* b = current_box(s);
    if (!b) return {};
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(b->items.size())) return {};
    return b->items[static_cast<size_t>(sel)].path;
}

void begin_external_drag(App& app) {
    if (!app.internal_drag) return;
    std::wstring path;
    const Box* b = current_box(app.state);
    const int from = app.drag_from;
    if (b && from >= 0 && from < static_cast<int>(b->items.size())) {
        path = b->items[static_cast<size_t>(from)].path;
    }
    app.internal_drag = false;
    app.drag_pending = false;
    app.drag_from = -1;
    app.state.box_view.drag_over_box = -1;
    if (app.panel) ::ReleaseCapture();
    if (!path.empty()) {
        app.in_drag = true;
        dragdrop_begin_drag(app.panel, { path });
        app.in_drag = false;
    }
    invalidate(app);
}

}  // namespace

void box_clamp(AppState& s, D2D1_SIZE_F client) {
    BoxState& b = s.box_view;
    const ShellLayout sh = shell_measure(client.width, client.height, current_has_missing(s));
    if (s.boxes.empty()) {
        b.box = 0;
        b.sel = -1;
        b.hover = -1;
        b.scroll = 0;
        b.side_scroll = 0;
        return;
    }
    b.box = std::clamp(b.box, 0, static_cast<int>(s.boxes.size()) - 1);
    const int count = static_cast<int>(s.boxes[static_cast<size_t>(b.box)].items.size());
    if (b.sel >= count) b.sel = -1;
    if (b.hover >= count) b.hover = -1;
    const auto vis = box_visible(s.boxes[static_cast<size_t>(b.box)].items, b.filter);
    b.scroll = grid_clamp_scroll(shell_grid(sh, client.width, client.height),
                                 static_cast<int>(vis.size()), b.scroll);
    b.side_scroll = shell_clamp_scroll(sh, static_cast<int>(s.boxes.size()), b.side_scroll);
    if (b.drag_over_box >= static_cast<int>(s.boxes.size())) b.drag_over_box = -1;
}

void box_ingest(App& app, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    AppState& s = app.state;
    const int before_boxes = static_cast<int>(s.boxes.size());
    int index = s.boxes.empty() ? 0 : s.box_view.box;
    const size_t before = s.boxes.empty()
                              ? 0
                              : s.boxes[static_cast<size_t>(std::clamp(
                                    index, 0, static_cast<int>(s.boxes.size()) - 1))]
                                    .items.size();
    if (box_add_paths(s.boxes, index, paths) == 0) return;
    s.data_dirty = true;
    if (before_boxes == 0) s.box_view.box = 0;
    s.box_view.box = std::clamp(s.box_view.box, 0, static_cast<int>(s.boxes.size()) - 1);
    clear_filter(app);
    auto& items = s.boxes[static_cast<size_t>(s.box_view.box)].items;
    s.box_view.sel = before < items.size() ? static_cast<int>(before) : -1;
    box_clamp(s, app.render.client_logical());
    reveal_item(s, shell_grid(box_shell(app), app.render.client_logical().width,
                              app.render.client_logical().height));
    request_check(app);
    invalidate(app);
}

void box_switch(App& app, int index) {
    AppState& s = app.state;
    if (index < 0 || index >= static_cast<int>(s.boxes.size())) return;
    if (index == s.box_view.box) return;
    s.box_view.edit.close();
    s.box_view.renaming_box = -1;
    s.box_view.renaming_item = -1;
    clear_filter(app);
    s.box_view.box = index;
    s.box_view.sel = -1;
    s.box_view.hover = -1;
    s.box_view.hover_from = -1;
    s.box_view.scroll = 0;
    const ShellLayout sh = box_shell(app);
    if (index < s.box_view.side_scroll) s.box_view.side_scroll = index;
    if (index >= s.box_view.side_scroll + sh.rows_visible) {
        s.box_view.side_scroll = index - sh.rows_visible + 1;
    }
    box_clamp(s, app.render.client_logical());
    request_check(app);
    invalidate(app);
}

void box_add_box(App& app) {
    AppState& s = app.state;
    int n = static_cast<int>(s.boxes.size()) + 1;
    std::wstring name = L"新盒子 " + std::to_wstring(n);
    while (box_name_taken(s.boxes, name, -1)) {
        name = L"新盒子 " + std::to_wstring(++n);
    }
    s.boxes.push_back(Box{ name, {} });
    s.box_view.box = static_cast<int>(s.boxes.size()) - 1;
    s.box_view.sel = -1;
    s.box_view.scroll = 0;
    clear_filter(app);
    s.data_dirty = true;
    box_clamp(s, app.render.client_logical());
    invalidate(app);
    box_rename_box(app, s.box_view.box);
}

void box_rename_box(App& app, int index) {
    AppState& s = app.state;
    if (index < 0 || index >= static_cast<int>(s.boxes.size())) return;
    s.box_view.edit.close();
    s.box_view.renaming_item = -1;
    if (index != s.box_view.box) box_switch(app, index);
    const ShellLayout sh = box_shell(app);
    if (index < s.box_view.side_scroll || index >= s.box_view.side_scroll + sh.rows_visible) {
        s.box_view.side_scroll = index;
        box_clamp(s, app.render.client_logical());
    }
    const D2D1_RECT_F row = shell_row_rect(sh, index, s.box_view.side_scroll);
    const std::wstring current = s.boxes[static_cast<size_t>(index)].name;
    s.box_view.renaming_box = index;
    s.box_view.edit.open(
        app.panel, app.render.to_physical(inset(row, 4.f)), current, app.render.dpi,
        [&app, index](const std::wstring& t) {
            AppState& st = app.state;
            st.box_view.renaming_box = -1;
            if (!t.empty() && !box_name_taken(st.boxes, t, index) &&
                index < static_cast<int>(st.boxes.size())) {
                st.boxes[static_cast<size_t>(index)].name = t;
                st.data_dirty = true;
            }
            invalidate(app);
        },
        [&app]() {
            app.state.box_view.renaming_box = -1;
            invalidate(app);
        },
        light_edit(8.f, false, RGB(255, 255, 255)));
}

void box_delete_box(App& app, int index) {
    AppState& s = app.state;
    if (index < 0 || index >= static_cast<int>(s.boxes.size())) return;
    const size_t n = s.boxes[static_cast<size_t>(index)].items.size();
    if (n > 0) {
        const std::wstring msg = L"删除盒子「" + s.boxes[static_cast<size_t>(index)].name +
                                 L"」？\n\n" + std::to_wstring(n) +
                                 L" 条引用会被去掉。磁盘上的文件不会被移动或删除。";
        if (::MessageBoxW(app.panel, msg.c_str(), L"Stargazer", MB_YESNO | MB_ICONQUESTION) !=
            IDYES) {
            return;
        }
    }
    s.box_view.edit.close();
    s.box_view.renaming_box = -1;
    s.box_view.renaming_item = -1;
    s.boxes.erase(s.boxes.begin() + index);
    s.data_dirty = true;
    s.box_view.sel = -1;
    if (s.box_view.box >= static_cast<int>(s.boxes.size())) {
        s.box_view.box = static_cast<int>(s.boxes.size()) - 1;
    }
    if (s.box_view.box < 0) s.box_view.box = 0;
    box_clamp(s, app.render.client_logical());
    invalidate(app);
}

void box_delete_selected(App& app) {
    AppState& s = app.state;
    Box* b = s.boxes.empty() ? nullptr
                             : &s.boxes[static_cast<size_t>(std::clamp(
                                   s.box_view.box, 0, static_cast<int>(s.boxes.size()) - 1))];
    if (!b) return;
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(b->items.size())) return;
    b->items.erase(b->items.begin() + sel);
    s.data_dirty = true;
    if (s.box_view.renaming_item == sel) {
        s.box_view.edit.close();
        s.box_view.renaming_item = -1;
    }
    // 删掉之后停在原来的位置附近：可见列表里不小于原下标的第一项，否则最后一项
    const auto vis = box_visible(b->items, s.box_view.filter);
    s.box_view.sel = -1;
    for (int real : vis) {
        if (real >= sel) {
            s.box_view.sel = real;
            break;
        }
    }
    if (s.box_view.sel < 0 && !vis.empty()) s.box_view.sel = vis.back();
    box_clamp(s, app.render.client_logical());
    invalidate(app);
}

void box_rename_selected(App& app) {
    AppState& s = app.state;
    const Box* b = current_box(s);
    if (!b) return;
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(b->items.size())) {
        box_rename_box(app, s.box_view.box);
        return;
    }
    const auto vis = visible_of(s);
    const int v = visible_pos(vis, sel);
    if (v < 0) return;
    s.box_view.edit.close();
    s.box_view.renaming_box = -1;
    const D2D1_SIZE_F cs = app.render.client_logical();
    const D2D1_RECT_F cell =
        grid_cell_rect(shell_grid(box_shell(app), cs.width, cs.height), v, s.box_view.scroll);
    const int box = s.box_view.box;
    const std::wstring current = b->items[static_cast<size_t>(sel)].name;
    const COLORREF bg = blend(app.render.theme.sel_fill, RGB(255, 255, 255));
    s.box_view.renaming_item = sel;
    s.box_view.edit.open(
        app.panel, app.render.to_physical(edit_box_rect(grid_label_rect(cell))), current,
        app.render.dpi,
        [&app, box, sel](const std::wstring& t) {
            AppState& st = app.state;
            st.box_view.renaming_item = -1;
            if (!t.empty() && box < static_cast<int>(st.boxes.size())) {
                auto& its = st.boxes[static_cast<size_t>(box)].items;
                if (sel < static_cast<int>(its.size())) {
                    its[static_cast<size_t>(sel)].name = t;
                    st.data_dirty = true;
                }
            }
            invalidate(app);
        },
        [&app]() {
            app.state.box_view.renaming_item = -1;
            invalidate(app);
        },
        light_edit(0.f, true, bg));
}

void box_copy_selected(App& app) {
    const std::wstring path = path_of_sel(app.state);
    if (path.empty()) return;
    clipboard_set_paths({ path }, false);
}

void box_show_in_explorer(App& app) {
    const std::wstring path = path_of_sel(app.state);
    if (path.empty()) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void box_open_selected(App& app) {
    AppState& s = app.state;
    const Box* b = current_box(s);
    if (!b) return;
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(b->items.size())) return;
    const std::wstring path = b->items[static_cast<size_t>(sel)].path;
    if (path.empty()) return;
    if (b->items[static_cast<size_t>(sel)].missing) {
        ::MessageBoxW(app.panel, (L"原文件已经不在这个位置：\n\n" + path).c_str(), L"Stargazer",
                      MB_ICONINFORMATION);
        return;
    }
    const HINSTANCE rc =
        ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        ::MessageBoxW(app.panel, (L"打不开：\n\n" + path).c_str(), L"Stargazer", MB_ICONWARNING);
        return;
    }
    app_hide(app);  // 面板是置顶的，打开后收起，免得挡住刚打开的窗口
}

void box_clear_missing(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    auto& items = s.boxes[static_cast<size_t>(s.box_view.box)].items;
    const size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const BoxItem& it) { return it.missing; }),
                items.end());
    if (items.size() != before) s.data_dirty = true;
    s.box_view.sel = -1;
    box_clamp(s, app.render.client_logical());
    invalidate(app);
}

void box_pick_files(App& app) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg)))) {
        return;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                        FOS_PATHMUSTEXIST);
    }
    dlg->SetTitle(L"添加到当前盒子（只保存路径，不移动文件）");
    std::vector<std::wstring> paths;
    if (SUCCEEDED(dlg->Show(app.panel))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* it = nullptr;
                if (FAILED(items->GetItemAt(i, &it)) || !it) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    paths.emplace_back(path);
                    ::CoTaskMemFree(path);
                }
                it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    box_ingest(app, paths);
}

void box_paste(App& app) {
    bool move = false;
    std::vector<std::wstring> paths = clipboard_get_paths(move);
    if (paths.empty()) {
        std::wstring text;
        if (clipboard_get_text(text)) paths = paths_from_text(text);
    }
    if (paths.empty()) {
        app_notify(app, L"剪贴板里没有文件路径");
        return;
    }
    box_ingest(app, paths);
}

void box_render(App& app) {
    Renderer& r = app.render;
    AppState& s = app.state;
    const D2D1_SIZE_F cs = r.client_logical();
    const ShellLayout sh = box_shell(app);
    const BoxState& b = s.box_view;

    r.fill_rect(D2D1::RectF(0.f, 0.f, cs.width, cs.height), r.theme.bg);
    r.fill_rect(sh.sidebar, r.theme.panel);
    r.fill_rect(D2D1::RectF(kSidebarW, kTitleH, kSidebarW + 1.f, cs.height), r.theme.divider);

    r.text(D2D1::RectF(kSidePad, 0.f, 160.f, kTitleH), L"Stargazer",
           r.format(16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING),
           r.theme.text);
    r.text(D2D1::RectF(118.f, 0.f, kSidebarW - 8.f, kTitleH), L"文件盒子",
           r.format(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING),
           r.theme.text_faint);
    paint_button(r, sh.dismiss, L"收起", false, hot_ctrl(app, ShellControl::Dismiss),
                 down_ctrl(app, ShellControl::Dismiss));

    r.text(sh.side_label, L"盒子",
           r.format(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING),
           r.theme.text_faint);

    IDWriteTextFormat* row_fmt = r.format(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextFormat* count_fmt =
        r.format(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    for (int i = 0; i < static_cast<int>(s.boxes.size()); ++i) {
        const int row = i - b.side_scroll;
        if (row < 0 || row >= sh.rows_visible) continue;
        const D2D1_RECT_F rc = shell_row_rect(sh, i, b.side_scroll);
        const bool active = i == b.box;
        const bool over = i == b.side_hover;
        const bool drop = i == b.drag_over_box;
        if (active || over || drop) {
            const D2D1_RECT_F shadow =
                D2D1::RectF(rc.left, rc.top + 2.f, rc.right, rc.bottom + 3.f);
            r.fill_round_rect(shadow, kRadiusSm, r.theme.shadow_soft);
        }
        if (drop) {
            r.fill_round_rect(rc, kRadiusSm, r.theme.sel_fill);
            r.stroke_round_rect(rc, kRadiusSm, r.theme.accent, 1.5f);
        } else if (active) {
            r.fill_round_rect(rc, kRadiusSm, r.theme.card);
            r.stroke_round_rect(rc, kRadiusSm, r.theme.border, 1.f);
            r.fill_round_rect(D2D1::RectF(rc.left + 5.f, rc.top + 12.f, rc.left + 8.f, rc.bottom - 12.f),
                              1.5f, r.theme.accent);
        } else if (over) {
            r.fill_round_rect(rc, kRadiusSm, r.theme.hover);
        }
        const D2D1_RECT_F name =
            D2D1::RectF(rc.left + (active ? 16.f : 12.f), rc.top, rc.right - 44.f, rc.bottom);
        r.text(name, s.boxes[static_cast<size_t>(i)].name, row_fmt,
               active ? r.theme.text : r.theme.text_dim);
        const D2D1_RECT_F count =
            D2D1::RectF(rc.right - 40.f, rc.top, rc.right - 10.f, rc.bottom);
        r.text(count, std::to_wstring(s.boxes[static_cast<size_t>(i)].items.size()), count_fmt,
               r.theme.text_faint);
    }

    paint_button(r, sh.new_box, L"＋  新盒子", false, hot_ctrl(app, ShellControl::NewBox),
                 down_ctrl(app, ShellControl::NewBox));

    const Box* cur = current_box(s);
    if (!cur) {
        r.text(D2D1::RectF(sh.heading.left, sh.heading.top, sh.heading.right, sh.heading.top + 32.f),
               L"还没有盒子",
               r.format(22.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING),
               r.theme.text);
    } else {
        const D2D1_RECT_F name_rc =
            D2D1::RectF(sh.heading.left, sh.heading.top, sh.heading.right, sh.heading.top + 30.f);
        const D2D1_RECT_F sub_rc =
            D2D1::RectF(sh.heading.left, sh.heading.top + 30.f, sh.heading.right, sh.heading.bottom);
        r.text(name_rc, cur->name,
               r.format(22.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING),
               r.theme.text);
        const auto vis = visible_of(s);
        std::wstring sub = b.filter.empty()
                               ? std::to_wstring(cur->items.size()) + L" 个文件"
                               : std::to_wstring(vis.size()) + L" / " +
                                     std::to_wstring(cur->items.size());
        sub += L"    只保存引用";
        r.text(sub_rc, sub, r.format(12.f), r.theme.text_faint);
    }

    paint_button(r, sh.add, L"添加文件", true, hot_ctrl(app, ShellControl::AddFiles),
                 down_ctrl(app, ShellControl::AddFiles));

    const bool search_hot = hot_ctrl(app, ShellControl::Search);
    r.fill_round_rect(sh.search, kRadiusMd, r.theme.card);
    r.stroke_round_rect(sh.search, kRadiusMd,
                        b.search.is_open() || search_hot ? r.theme.accent : r.theme.border,
                        b.search.is_open() ? 1.5f : 1.f);
    if (!b.search.is_open()) {
        const D2D1_RECT_F inner = inset(sh.search, 0.f);
        if (b.filter.empty()) {
            r.text(D2D1::RectF(inner.left + 16.f, inner.top, inner.right - 12.f, inner.bottom),
                   L"搜索这个盒子", r.format(14.f), r.theme.text_faint);
        } else {
            r.text(D2D1::RectF(inner.left + 16.f, inner.top, inner.right - 12.f, inner.bottom),
                   b.filter, r.format(14.f), r.theme.text);
        }
    }

    if (sh.show_banner && cur) {
        D2D1_COLOR_F wash = r.theme.danger;
        wash.a = 0.08f;
        r.fill_round_rect(sh.banner, kRadiusMd, wash);
        D2D1_COLOR_F edge = r.theme.danger;
        edge.a = 0.35f;
        r.stroke_round_rect(sh.banner, kRadiusMd, edge, 1.f);
        const D2D1_RECT_F msg =
            D2D1::RectF(sh.banner.left + 16.f, sh.banner.top, sh.clean.left - 8.f, sh.banner.bottom);
        r.text(msg,
               std::to_wstring(missing_count(*cur)) + L" 个文件已不在原处。清理只去掉引用。",
               r.format(13.f), r.theme.danger);
        paint_button(r, sh.clean, L"清理", false, hot_ctrl(app, ShellControl::Clean),
                     down_ctrl(app, ShellControl::Clean));
    }

    if (!cur || cur->items.empty() || visible_of(s).empty()) {
        const bool filtered = cur && !cur->items.empty() && !b.filter.empty();
        if (filtered) {
            r.text(sh.grid_area, L"没有符合的文件",
                   r.format(16.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                   r.theme.text_faint);
        } else {
            draw_empty(r, sh, cur == nullptr);
        }
    } else {
        const auto vis = visible_of(s);
        std::vector<GridItem> cells;
        cells.reserve(vis.size());
        for (int real : vis) {
            const BoxItem& it = cur->items[static_cast<size_t>(real)];
            GridItem gi;
            gi.label = it.name;
            gi.icon_src = it.path;
            gi.is_dir = !gi.icon_src.empty() &&
                        (gi.icon_src.back() == L'\\' || gi.icon_src.back() == L'/');
            gi.selected = real == b.sel;
            gi.hover = hover_amount(b, real);
            gi.missing = it.missing;
            cells.push_back(std::move(gi));
        }
        const GridLayout gl = shell_grid(sh, cs.width, cs.height);
        if (r.rt) {
            const D2D1_RECT_F clip = D2D1::RectF(sh.grid_area.left - 16.f, sh.grid_area.top - 8.f,
                                                 cs.width, cs.height - 26.f);
            r.rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            grid_render(r, gl, cells, b.scroll);
            r.rt->PopAxisAlignedClip();
        }
        r.text(D2D1::RectF(sh.grid_area.left, cs.height - 26.f, cs.width - kPad, cs.height - 6.f),
               L"双击打开   ·   拖到左侧另一个盒子即可移动   ·   Del 只移除引用",
               r.format(12.f), r.theme.text_faint);
    }
}

bool box_keydown(App& app, UINT vk) {
    AppState& s = app.state;
    const D2D1_SIZE_F cs = app.render.client_logical();

    if (vk == 'N' && ctrl_down() && !shift_down()) {
        box_add_box(app);
        return true;
    }
    if (vk == 'O' && ctrl_down() && !shift_down()) {
        box_pick_files(app);
        return true;
    }
    if (vk == 'F' && ctrl_down() && !shift_down()) {
        open_search(app, L"");
        return true;
    }
    if (vk == 'V' && ctrl_down() && !shift_down()) {
        box_paste(app);
        return true;
    }
    if (vk == VK_ESCAPE) {
        if (!s.box_view.filter.empty() ||
            (s.box_view.search.is_open() && !s.box_view.search.text().empty())) {
            clear_filter(app);
            box_clamp(s, cs);
            invalidate(app);
            return true;
        }
        return false;
    }
    if (s.boxes.empty()) return false;

    if (vk >= '1' && vk <= '9' && ctrl_down()) {
        const int want = vk - '1';
        if (want < static_cast<int>(s.boxes.size())) box_switch(app, want);
        return true;
    }
    if (vk == VK_TAB && ctrl_down()) {
        const int n = static_cast<int>(s.boxes.size());
        const int next = shift_down() ? (s.box_view.box + n - 1) % n : (s.box_view.box + 1) % n;
        box_switch(app, next);
        return true;
    }
    if (vk == 'D' && ctrl_down() && shift_down()) {
        box_clear_missing(app);
        return true;
    }
    if (vk == VK_DELETE) {
        box_delete_selected(app);
        return true;
    }
    if (vk == VK_F2) {
        box_rename_selected(app);
        return true;
    }
    if (vk == VK_RETURN) {
        box_open_selected(app);
        return true;
    }
    if (vk == 'C' && ctrl_down() && !shift_down()) {
        box_copy_selected(app);
        return true;
    }

    const auto vis = visible_of(s);
    int vis_sel = visible_pos(vis, s.box_view.sel);
    const GridLayout gl = shell_grid(box_shell(app), cs.width, cs.height);
    if (!grid_keydown(gl, static_cast<int>(vis.size()), vis_sel, s.box_view.scroll, vk)) {
        return false;
    }
    if (vis_sel < 0 || vis_sel >= static_cast<int>(vis.size())) {
        s.box_view.sel = -1;
    } else {
        s.box_view.sel = vis[static_cast<size_t>(vis_sel)];
    }
    if (vis_sel < 0 && (vk == VK_UP)) open_search(app, L"");
    box_clamp(s, cs);
    invalidate(app);
    return true;
}

void box_char(App& app, wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return;
    if (app.state.box_view.edit.is_open()) return;
    open_search(app, std::wstring(1, ch));
}

void box_search_changed(App& app) {
    if (!app.state.box_view.search.is_open()) return;
    set_filter_keep_sel(app, app.state.box_view.search.text());
}

bool box_anim_tick(App& app) {
    BoxState& b = app.state.box_view;
    const float t =
        std::min(1.f, static_cast<float>(::GetTickCount64() - b.hover_ms) / static_cast<float>(kHoverMs));
    b.hover_t = ease_out(t);
    if (b.hover_t >= 1.f) {
        b.hover_from = -1;
        return false;
    }
    return true;
}

void box_mouse_move(App& app, POINT phys) {
    if (app.in_drag) return;
    const D2D1_POINT_2F lpt = app.render.to_logical(phys);
    const D2D1_SIZE_F cs = app.render.client_logical();
    AppState& s = app.state;
    const ShellLayout sh = box_shell(app);
    track_mouse(app);

    if (app.drag_pending && !app.internal_drag) {
        const int dx = phys.x - app.drag_anchor.x;
        const int dy = phys.y - app.drag_anchor.y;
        const int slop = std::max(kDragSlop, ::MulDiv(kDragSlop, static_cast<int>(app.render.dpi), 96));
        if (dx * dx + dy * dy >= slop * slop) app.internal_drag = true;
    }

    if (app.internal_drag) {
        const bool outside =
            lpt.x < 0.f || lpt.y < 0.f || lpt.x >= cs.width || lpt.y >= cs.height;
        if (outside) {
            begin_external_drag(app);
            return;
        }
        const int row = shell_row_hittest(sh, static_cast<int>(s.boxes.size()), s.box_view.side_scroll,
                                          lpt);
        const int over = (row >= 0 && row != s.box_view.box) ? row : -1;
        bool changed = over != s.box_view.drag_over_box;
        s.box_view.drag_over_box = over;
        if (over < 0) {
            const int cell = box_hittest(app, lpt);
            if (cell != s.box_view.hover) {
                set_item_hover(app, cell);
                changed = false;  // set_item_hover already invalidates
            }
        }
        if (changed) invalidate(app);
        return;
    }

    const int chrome = static_cast<int>(shell_control_at(sh, lpt));
    const int row =
        shell_row_hittest(sh, static_cast<int>(s.boxes.size()), s.box_view.side_scroll, lpt);
    const int cell = (row < 0 && chrome == 0) ? box_hittest(app, lpt) : -1;
    bool changed = false;
    if (chrome != s.box_view.chrome_hover) {
        s.box_view.chrome_hover = chrome;
        changed = true;
    }
    if (row != s.box_view.side_hover) {
        s.box_view.side_hover = row;
        changed = true;
    }
    if (cell != s.box_view.hover || (cell < 0 && s.box_view.hover >= 0 && row < 0 && chrome != 0)) {
        set_item_hover(app, cell);
        changed = false;
    }
    if (changed) invalidate(app);
}

void box_mouse_leave(App& app) {
    app.mouse_tracking = false;
    if (app.internal_drag) {
        begin_external_drag(app);
        return;
    }
    app.drag_pending = false;
    app.state.box_view.chrome_hover = 0;
    app.state.box_view.side_hover = -1;
    set_item_hover(app, -1);
    invalidate(app);
}

void box_lbutton_down(App& app, POINT phys) {
    const D2D1_POINT_2F lpt = app.render.to_logical(phys);
    AppState& s = app.state;
    const ShellLayout sh = box_shell(app);
    const ShellControl ctrl = shell_control_at(sh, lpt);

    if (ctrl == ShellControl::Search) {
        open_search(app, L"");
        return;
    }
    if (ctrl == ShellControl::Heading) {
        if (!s.boxes.empty()) box_rename_box(app, s.box_view.box);
        return;
    }
    if (ctrl == ShellControl::Dismiss || ctrl == ShellControl::AddFiles ||
        ctrl == ShellControl::Clean || ctrl == ShellControl::NewBox) {
        app.press = static_cast<int>(ctrl);
        if (app.panel) ::SetCapture(app.panel);
        invalidate(app);
        return;
    }

    const int row =
        shell_row_hittest(sh, static_cast<int>(s.boxes.size()), s.box_view.side_scroll, lpt);
    if (row >= 0) {
        box_switch(app, row);
        if (app.panel) ::SetFocus(app.panel);
        return;
    }

    const int hit = box_hittest(app, lpt);
    s.box_view.sel = hit;
    if (hit >= 0) {
        app.drag_pending = true;
        app.drag_from = hit;
        app.drag_anchor = phys;
        if (app.panel) {
            ::SetFocus(app.panel);
            ::SetCapture(app.panel);
        }
    } else if (app.panel) {
        ::SetFocus(app.panel);
    }
    invalidate(app);
}

void box_lbutton_up(App& app, POINT phys) {
    const D2D1_POINT_2F lpt = app.render.to_logical(phys);
    if (app.press != 0) {
        const int fired = app.press;
        app.press = 0;
        if (app.panel) ::ReleaseCapture();
        const ShellControl ctrl = shell_control_at(box_shell(app), lpt);
        if (static_cast<int>(ctrl) == fired) {
            switch (ctrl) {
                case ShellControl::Dismiss:
                    app_hide(app);
                    break;
                case ShellControl::NewBox:
                    box_add_box(app);
                    break;
                case ShellControl::AddFiles:
                    box_pick_files(app);
                    break;
                case ShellControl::Clean:
                    box_clear_missing(app);
                    break;
                default:
                    break;
            }
        }
        invalidate(app);
        return;
    }

    if (!app.internal_drag && !app.drag_pending) return;
    const bool dragging = app.internal_drag;
    const int from = app.drag_from;
    const int dst = app.state.box_view.drag_over_box;
    app.internal_drag = false;
    app.drag_pending = false;
    app.drag_from = -1;
    app.state.box_view.drag_over_box = -1;
    if (app.panel) ::ReleaseCapture();

    if (dragging && from >= 0) {
        AppState& s = app.state;
        if (dst >= 0 && dst != s.box_view.box && box_move_item(s.boxes, s.box_view.box, from, dst)) {
            s.data_dirty = true;
            s.box_view.sel = -1;
        } else if (s.box_view.box >= 0 && s.box_view.box < static_cast<int>(s.boxes.size())) {
            const int cell = box_hittest(app, lpt);
            if (cell >= 0 && box_move_within(s.boxes[static_cast<size_t>(s.box_view.box)], from, cell)) {
                s.data_dirty = true;
                s.box_view.sel = cell;
            }
        }
        box_clamp(s, app.render.client_logical());
    }
    invalidate(app);
}

void box_lbutton_dblclk(App& app, POINT phys) {
    const D2D1_POINT_2F lpt = app.render.to_logical(phys);
    const ShellLayout sh = box_shell(app);
    const int row = shell_row_hittest(sh, static_cast<int>(app.state.boxes.size()),
                                      app.state.box_view.side_scroll, lpt);
    if (row >= 0) {
        box_rename_box(app, row);
        return;
    }
    const int hit = box_hittest(app, lpt);
    if (hit >= 0) {
        app.state.box_view.sel = hit;
        box_open_selected(app);
    }
}

void box_mouse_wheel(App& app, int step, POINT screen) {
    POINT client = screen;
    if (app.panel) ::ScreenToClient(app.panel, &client);
    const D2D1_POINT_2F lpt = app.render.to_logical(client);
    AppState& s = app.state;
    if (lpt.x < kSidebarW) {
        s.box_view.side_scroll += step;
    } else {
        s.box_view.scroll += step;
    }
    box_clamp(s, app.render.client_logical());
    invalidate(app);
}

void box_size(App& app) {
    box_clamp(app.state, app.render.client_logical());
    place_edits(app);
    invalidate(app);
}

void box_context_menu(App& app, POINT screen_pt, POINT client_pt) {
    AppState& s = app.state;
    const D2D1_POINT_2F lpt = app.render.to_logical(client_pt);
    const ShellLayout sh = box_shell(app);
    const int row =
        shell_row_hittest(sh, static_cast<int>(s.boxes.size()), s.box_view.side_scroll, lpt);
    if (row >= 0) box_switch(app, row);
    const int hit = box_hittest(app, lpt);
    if (hit >= 0) s.box_view.sel = hit;

    const bool has_box = !s.boxes.empty();
    const bool has_item =
        has_box && s.box_view.sel >= 0 &&
        s.box_view.sel < static_cast<int>(s.boxes[static_cast<size_t>(s.box_view.box)].items.size());

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"添加文件(&A)…");
    ::AppendMenuW(menu, MF_STRING, 2, L"新建盒子(&B)");
    ::AppendMenuW(menu, MF_STRING | (has_box ? MF_ENABLED : MF_GRAYED), 3, L"重命名盒子(&R)");
    ::AppendMenuW(menu, MF_STRING | (has_box ? MF_ENABLED : MF_GRAYED), 4, L"删除盒子(&D)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (has_item ? MF_ENABLED : MF_GRAYED), 5, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING | (has_item ? MF_ENABLED : MF_GRAYED), 6, L"复制路径(&C)");
    ::AppendMenuW(menu, MF_STRING | (has_item ? MF_ENABLED : MF_GRAYED), 7, L"在资源管理器中显示(&E)");
    ::AppendMenuW(menu, MF_STRING | (has_item ? MF_ENABLED : MF_GRAYED), 8, L"重命名(&N)");
    ::AppendMenuW(menu, MF_STRING | (has_item ? MF_ENABLED : MF_GRAYED), 9, L"移除引用(&X)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (has_box ? MF_ENABLED : MF_GRAYED), 10, L"清理失效项(&M)");

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x,
                                      screen_pt.y, 0, app.panel, nullptr);
    ::DestroyMenu(menu);
    switch (cmd) {
        case 1:
            box_pick_files(app);
            break;
        case 2:
            box_add_box(app);
            break;
        case 3:
            box_rename_box(app, s.box_view.box);
            break;
        case 4:
            box_delete_box(app, s.box_view.box);
            break;
        case 5:
            box_open_selected(app);
            break;
        case 6:
            box_copy_selected(app);
            break;
        case 7:
            box_show_in_explorer(app);
            break;
        case 8:
            box_rename_selected(app);
            break;
        case 9:
            box_delete_selected(app);
            break;
        case 10:
            box_clear_missing(app);
            break;
        default:
            break;
    }
}

void box_on_show(App& app) {
    AppState& s = app.state;
    s.box_view.sel = -1;
    s.box_view.hover = -1;
    s.box_view.hover_from = -1;
    s.box_view.chrome_hover = 0;
    box_clamp(s, app.render.client_logical());
    if (!s.box_view.filter.empty()) {
        open_search(app, L"");  // 把还在的筛选词放回输入框，焦点留在那里
    } else if (app.panel) {
        ::SetFocus(app.panel);
    }
    request_check(app);
    invalidate(app);
}

void box_on_hide(App& app) {
    BoxState& b = app.state.box_view;
    b.edit.close();
    b.search.close();
    b.renaming_box = -1;
    b.renaming_item = -1;
    b.drag_over_box = -1;
    app.internal_drag = false;
    app.drag_pending = false;
    app.drag_from = -1;
    app.press = 0;
    if (app.panel) ::ReleaseCapture();
}

bool box_in_caption(D2D1_SIZE_F client, D2D1_POINT_2F pt) {
    if (pt.y < 0.f || pt.y >= kTitleH) return false;
    const ShellLayout sh = shell_measure(client.width, client.height, false);
    return !shell_contains(sh.dismiss, pt);
}

}  // namespace sg
