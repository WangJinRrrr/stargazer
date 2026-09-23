#include "views/box.h"

#include <algorithm>
#include <vector>

#include "app.h"
#include "fs_work.h"

namespace sg {

namespace {

// 与分组标签同款：按名字宽度排布（中文名字宽度不固定，等宽分段会挤）
float box_tab_width(const std::wstring& name) {
    return 24.f + static_cast<float>(name.size()) * 13.f;
}

}  // namespace

D2D1_RECT_F box_tabs_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, kViewTabsH + kPad, client.width - kPad, kViewTabsH + kPad + kTabsH);
}

GridLayout box_layout(D2D1_SIZE_F client) {
    return grid_measure(client.width, client.height, kViewTabsH + kPad + kTabsH + kPad);
}

D2D1_RECT_F box_cell_rect(const AppState& s, D2D1_SIZE_F client, int index) {
    return grid_cell_rect(box_layout(client), index, s.box_view.scroll);
}

int box_tab_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt) {
    const D2D1_RECT_F tr = box_tabs_rect(client);
    if (pt.y < tr.top || pt.y > tr.bottom) return -1;
    float x = tr.left;
    for (size_t i = 0; i < s.boxes.size(); ++i) {
        const float w = box_tab_width(s.boxes[i].name);
        if (pt.x >= x && pt.x <= x + w) return static_cast<int>(i);
        x += w + 6.f;
    }
    return -1;
}

void box_clamp(AppState& s, D2D1_SIZE_F client) {
    BoxState& bs = s.box_view;
    if (s.boxes.empty()) {
        bs.box = 0;
        bs.sel = -1;
        bs.hover = -1;
        bs.scroll = 0;
        return;
    }
    bs.box = std::clamp(bs.box, 0, static_cast<int>(s.boxes.size()) - 1);
    const int count = static_cast<int>(s.boxes[bs.box].items.size());
    if (bs.sel >= count) bs.sel = count > 0 ? count - 1 : -1;
    if (bs.hover >= count) bs.hover = -1;
    bs.scroll = grid_clamp_scroll(box_layout(client), count, bs.scroll);
}

int box_hittest(App& app, D2D1_POINT_2F pt) {
    const AppState& s = app.state;
    if (s.boxes.empty()) return -1;
    const int count = static_cast<int>(s.boxes[s.box_view.box].items.size());
    return grid_hittest(box_layout(app.render.client_logical()), count, s.box_view.scroll, pt);
}

void box_render(App& app) {
    Renderer& r = app.render;
    AppState& s = app.state;
    const D2D1_SIZE_F client = r.client_logical();
    const D2D1_RECT_F tr = box_tabs_rect(client);
    const D2D1_RECT_F hint = D2D1::RectF(kPad, tr.bottom + kPad, client.width - kPad,
                                         tr.bottom + kPad + 40.f);

    // 盒子标签行
    IDWriteTextFormat* tab_fmt = r.format(13.f);
    float x = tr.left;
    for (size_t i = 0; i < s.boxes.size(); ++i) {
        const float w = box_tab_width(s.boxes[i].name);
        const D2D1_RECT_F tab = D2D1::RectF(x, tr.top, x + w, tr.bottom);
        const bool active = static_cast<int>(i) == s.box_view.box;
        const bool drop = static_cast<int>(i) == s.box_view.drag_over_tab;
        if (active) {
            r.fill_round_rect(tab, 6.f, r.theme.accent);
            r.text(tab, s.boxes[i].name, tab_fmt, D2D1::ColorF(1.f, 1.f, 1.f));
        } else {
            r.fill_round_rect(tab, 6.f, drop ? r.theme.accent : r.theme.card);
            r.text(tab, s.boxes[i].name, tab_fmt,
                   drop ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.text_dim);
        }
        x += w + 6.f;
    }

    if (s.boxes.empty()) {
        r.text(hint, L"还没有收纳盒：右键菜单里新建，或直接把文件拖进来", r.format(13.f),
               r.theme.text_dim);
        return;
    }

    const auto& items = s.boxes[s.box_view.box].items;
    std::vector<GridItem> cells;
    cells.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        GridItem gi;
        gi.label = items[i].name;
        gi.icon_src = items[i].path;
        gi.is_dir = !gi.icon_src.empty() && gi.icon_src.back() == L'\\';
        gi.selected = static_cast<int>(i) == s.box_view.sel;
        gi.hovered = static_cast<int>(i) == s.box_view.hover;
        gi.missing = items[i].missing;  // Task 4 才会填
        cells.push_back(std::move(gi));
    }
    grid_render(r, box_layout(client), cells, s.box_view.scroll, r.theme.accent, r.theme.hover,
                r.theme.text, r.theme.text_dim);

    if (items.empty()) {
        r.text(hint, L"这个盒子还是空的：把文件拖到窗口里就会添加引用", r.format(13.f),
               r.theme.text_dim);
    }
}

bool box_keydown(App& app, UINT vk) {
    AppState& s = app.state;
    const D2D1_SIZE_F client = app.render.client_logical();
    if (s.boxes.empty()) return false;

    // Ctrl+Shift+Delete：清理本盒失效项（只删引用）。
    // 修饰键用 GetAsyncKeyState：连击很快时 GetKeyState 的队列同步态可能还没更新
    if (vk == VK_DELETE && (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        box_clear_missing(app);
        return true;
    }

    const int count = static_cast<int>(s.boxes[s.box_view.box].items.size());
    if (!grid_keydown(box_layout(client), count, s.box_view.sel, s.box_view.scroll, vk)) {
        return false;
    }
    box_clamp(s, client);
    ::InvalidateRect(app.panel, nullptr, FALSE);
    return true;
}

void box_request_check(App& app) {
    AppState& s = app.state;
    if (s.view != View::Box || s.boxes.empty()) return;
    std::vector<std::wstring> paths;
    paths.reserve(s.boxes[s.box_view.box].items.size());
    for (const auto& it : s.boxes[s.box_view.box].items) paths.push_back(it.path);
    if (paths.empty()) return;
    // 结果回来时用户可能已经换盒或删了条目，所以**只按 path 回填，不按索引**。
    // 在所有盒子里找同一个 path：同一文件可以同时存在于多个盒子（合法用法）。
    fs_check_paths(paths, [&app](const std::wstring& path, bool exists) {
        for (auto& b : app.state.boxes) {
            for (auto& it : b.items) {
                if (it.path == path) it.missing = !exists;
            }
        }
    });
}

void box_clear_missing(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    auto& items = s.boxes[s.box_view.box].items;
    const size_t before = items.size();
    // 只从数据里删引用：磁盘上的文件一个也不动
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const BoxItem& it) { return it.missing; }),
                items.end());
    if (items.size() != before) s.data_dirty = true;
    s.box_view.sel = -1;
    box_clamp(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

}  // namespace sg
