#include "views/box.h"

#include <algorithm>
#include <string>
#include <vector>

#include <shellapi.h>  // ShellExecuteW（打开）

#include "app.h"
#include "clipboard.h"
#include "dragdrop.h"  // box_move_item 不需要，但 make_hdrop 由 clipboard 使用
#include "fs_work.h"

namespace sg {

namespace {

// 与分组标签同款：按名字宽度排布（中文名字宽度不固定，等宽分段会挤）
float box_tab_width(const std::wstring& name) {
    return 24.f + static_cast<float>(name.size()) * 13.f;
}

// 第 index 个盒子标签的矩形（改名输入框要把自己叠在这上面）
D2D1_RECT_F box_tab_rect(const AppState& s, D2D1_SIZE_F client, int index) {
    const D2D1_RECT_F tr = box_tabs_rect(client);
    float x = tr.left;
    for (int i = 0; i < static_cast<int>(s.boxes.size()); ++i) {
        const float w = box_tab_width(s.boxes[i].name);
        if (i == index) return D2D1::RectF(x, tr.top, x + w, tr.bottom);
        x += w + 6.f;
    }
    return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
}

// CF_UNICODETEXT 的全局内存块。成功后所有权归剪贴板，调用方不得再释放

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

    // 条目动作（Ctrl+Shift+Delete 已在上面拦走，不会落到这里的删除）
    if (vk == VK_DELETE) {
        box_delete_selected(app);  // 只删引用
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
    if (vk == 'C' && (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        box_copy_selected(app);  // Ctrl+C：路径文本 + CF_HDROP
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

void box_delete_selected(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    auto& items = s.boxes[s.box_view.box].items;
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(items.size())) return;
    items.erase(items.begin() + sel);  // 只删引用，磁盘上的文件不动
    s.data_dirty = true;
    s.box_view.sel = items.empty() ? -1 : std::min(sel, static_cast<int>(items.size()) - 1);
    box_clamp(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void box_rename_selected(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    const int sel = s.box_view.sel;
    auto& items = s.boxes[s.box_view.box].items;
    if (sel < 0 || sel >= static_cast<int>(items.size())) return;

    // 输入框叠在被改的那个格子上；坐标算法与渲染共用同一份
    const D2D1_RECT_F cr = box_cell_rect(s, app.render.client_logical(), sel);
    const D2D1_RECT_F input = D2D1::RectF(cr.left, cr.top, cr.right, cr.top + 72.f);
    const RECT rc = app.render.to_physical(input);
    const int box = s.box_view.box;
    const std::wstring current = items[sel].name;
    s.box_view.edit.open(
        app.panel, rc, current, app.render.dpi,
        [&app, box, sel](const std::wstring& t) {
            AppState& st = app.state;
            if (!t.empty() && box < static_cast<int>(st.boxes.size())) {
                auto& its = st.boxes[box].items;
                if (sel < static_cast<int>(its.size())) {
                    its[sel].name = t;
                    st.data_dirty = true;
                }
            }
            box_clamp(st, app.render.client_logical());
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() { ::InvalidateRect(app.panel, nullptr, FALSE); });
}

void box_add_box(App& app) {
    AppState& s = app.state;
    // 连续编号命名，不为了一个新盒子先弹输入框
    int n = static_cast<int>(s.boxes.size()) + 1;
    std::wstring name = L"新盒子 " + std::to_wstring(n);
    while (std::any_of(s.boxes.begin(), s.boxes.end(),
                       [&](const Box& b) { return b.name == name; })) {
        name = L"新盒子 " + std::to_wstring(++n);
    }
    s.boxes.push_back(Box{ name, {} });
    s.box_view.box = static_cast<int>(s.boxes.size()) - 1;
    s.box_view.sel = -1;
    s.data_dirty = true;
    box_clamp(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void box_rename_box(App& app, int index) {
    AppState& s = app.state;
    if (index < 0 || index >= static_cast<int>(s.boxes.size())) return;
    const D2D1_RECT_F tr = box_tab_rect(s, app.render.client_logical(), index);
    const RECT rc = app.render.to_physical(tr);
    const std::wstring current = s.boxes[index].name;
    s.box_view.edit.open(
        app.panel, rc, current, app.render.dpi,
        [&app, index](const std::wstring& t) {
            AppState& st = app.state;
            // 重名盒子在 parse 时会被合并（数据层语义），所以改名要拒绝重名
            if (!t.empty() && !box_name_taken(st.boxes, t, index) &&
                index < static_cast<int>(st.boxes.size())) {
                st.boxes[index].name = t;
                st.data_dirty = true;
            }
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() { ::InvalidateRect(app.panel, nullptr, FALSE); });
}

void box_delete_box(App& app, int index) {
    AppState& s = app.state;
    if (index < 0 || index >= static_cast<int>(s.boxes.size())) return;

    // 非空盒子才确认，且文案要明确“不动磁盘”
    const size_t n = s.boxes[index].items.size();
    if (n > 0) {
        const std::wstring msg = L"删除盒子“" + s.boxes[index].name + L"”？\n\n" +
                                 std::to_wstring(n) +
                                 L" 个引用会被移除（只删引用，磁盘上的文件不受影响）。";
        if (::MessageBoxW(app.panel, msg.c_str(), L"Stargazer",
                          MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }
    s.boxes.erase(s.boxes.begin() + index);
    s.data_dirty = true;
    s.box_view.sel = -1;
    box_clamp(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void box_copy_selected(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    auto& items = s.boxes[s.box_view.box].items;
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(items.size())) return;
    const std::wstring path = items[sel].path;
    if (path.empty()) return;

    // 两种格式一起给：粘到文本框是路径文本，粘到资源管理器就是“粘贴文件”。
    // 与浏览视图共用同一份实现（clipboard）
    clipboard_set_paths({ path }, false);
}

void box_open_selected(App& app) {
    AppState& s = app.state;
    if (s.boxes.empty()) return;
    auto& items = s.boxes[s.box_view.box].items;
    const int sel = s.box_view.sel;
    if (sel < 0 || sel >= static_cast<int>(items.size())) return;
    const std::wstring path = items[sel].path;
    if (path.empty()) return;

    if (items[sel].missing) {
        ::MessageBoxW(app.panel, (L"原文件已不存在：\n\n" + path).c_str(), L"Stargazer",
                      MB_ICONINFORMATION);
        return;
    }
    const HINSTANCE r =
        ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        ::MessageBoxW(app.panel, (L"打开失败：\n\n" + path).c_str(), L"Stargazer", MB_ICONWARNING);
        return;
    }
    app_hide(app);  // 与启动板一致：打开后收起面板，不让 TOPMOST 挡住刚开的窗口
}

void box_context_menu(App& app, POINT screen_pt, POINT client_pt) {
    AppState& s = app.state;
    const D2D1_SIZE_F client = app.render.client_logical();
    const D2D1_POINT_2F lpt = app.render.to_logical(client_pt);
    const int hit = box_hittest(app, lpt);
    if (hit >= 0) s.box_view.sel = hit;

    const bool has_box = !s.boxes.empty();
    const int tab = has_box ? box_tab_hittest(s, client, lpt) : -1;
    if (tab >= 0) {
        s.box_view.box = tab;
        box_request_check(app);  // 与点击标签同一规则：换盒子就重校验
    }

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"新建盒子(&B)");
    ::AppendMenuW(menu, MF_STRING | (has_box ? MF_ENABLED : MF_GRAYED), 2, L"重命名盒子(&R)");
    ::AppendMenuW(menu, MF_STRING | (has_box ? MF_ENABLED : MF_GRAYED), 3, L"删除盒子(&D)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 4, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 5, L"复制路径(&C)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 6, L"重命名条目(&N)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 7, L"删除条目(&X)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (has_box ? MF_ENABLED : MF_GRAYED), 8, L"清理失效项(&M)");

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x,
                                      screen_pt.y, 0, app.panel, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            box_add_box(app);
            break;
        case 2:
            box_rename_box(app, has_box ? s.box_view.box : -1);
            break;
        case 3:
            box_delete_box(app, has_box ? s.box_view.box : -1);
            break;
        case 4:
            box_open_selected(app);
            break;
        case 5:
            box_copy_selected(app);
            break;
        case 6:
            box_rename_selected(app);
            break;
        case 7:
            box_delete_selected(app);
            break;
        case 8:
            box_clear_missing(app);
            break;
        default:
            break;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
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
