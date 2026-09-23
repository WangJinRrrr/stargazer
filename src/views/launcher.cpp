#include "views/launcher.h"

#include <algorithm>
#include <shellapi.h>  // ShellExecuteW（打开所在位置）

#include "app.h"  // 右键菜单需要完整的 App 定义
#include "model/search.h"

namespace sg {

// 启动板网格的纵向起点：视图标签行 + 分组标签行 + 搜索框 + 内边距（逻辑 DIP）
static float launcher_grid_top() {
    return kViewTabsH + kPad + kTabsH + kSearchH + kPad;
}

static GridLayout launcher_grid(D2D1_SIZE_F client) {
    return grid_measure(client.width, client.height, launcher_grid_top());
}

D2D1_RECT_F launcher_tabs_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, kViewTabsH + kPad, client.width - kPad, kViewTabsH + kPad + kTabsH);
}

D2D1_RECT_F launcher_search_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, kViewTabsH + kPad + kTabsH, client.width - kPad,
                       kViewTabsH + kPad + kTabsH + kSearchH);
}

static float tab_width(const std::wstring& name) {
    return 24.f + static_cast<float>(name.size()) * 13.f;
}

void launcher_layout(const AppState& s, D2D1_SIZE_F client, int& cols, int& rows_visible) {
    (void)s;
    const GridLayout gl = launcher_grid(client);
    cols = gl.cols;
    rows_visible = gl.rows_visible;
}

D2D1_RECT_F launcher_cell_rect(const AppState& s, D2D1_SIZE_F client, int filtered_index) {
    return grid_cell_rect(launcher_grid(client), filtered_index, s.launcher.scroll);
}

void launcher_refilter(AppState& s) {
    LauncherState& ls = s.launcher;
    ls.filtered.clear();
    if (s.groups.empty()) {
        ls.group = 0;
        ls.sel = -1;
        ls.scroll = 0;
        return;
    }
    ls.group = std::clamp(ls.group, 0, static_cast<int>(s.groups.size()) - 1);
    const auto& items = s.groups[ls.group].items;
    for (size_t i = 0; i < items.size(); ++i) {
        // 名字与目标都参与匹配：记不得名字时敲路径片段也能找到
        if (contains_ci(items[i].name, ls.query) || contains_ci(items[i].target, ls.query)) {
            ls.filtered.push_back(static_cast<int>(i));
        }
    }
    if (ls.sel >= static_cast<int>(ls.filtered.size())) {
        ls.sel = ls.filtered.empty() ? -1 : static_cast<int>(ls.filtered.size()) - 1;
    }
    if (ls.scroll < 0) ls.scroll = 0;
}

int launcher_tab_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt) {
    const D2D1_RECT_F tr = launcher_tabs_rect(client);
    if (pt.y < tr.top || pt.y > tr.bottom) return -1;
    float x = tr.left;
    for (size_t i = 0; i < s.groups.size(); ++i) {
        const float w = tab_width(s.groups[i].name);
        if (pt.x >= x && pt.x <= x + w) return static_cast<int>(i);
        x += w + 6.f;
    }
    return -1;
}

int launcher_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt) {
    // 搜索框区域归 EDIT 子控件：网格 top 之上不算命中（grid_hittest 内部同样拒收）
    return grid_hittest(launcher_grid(client), static_cast<int>(s.launcher.filtered.size()),
                        s.launcher.scroll, pt);
}

void launcher_render(Renderer& r, AppState& s, D2D1_SIZE_F client) {
    LauncherState& ls = s.launcher;

    // 顶部标签行
    const D2D1_RECT_F tr = launcher_tabs_rect(client);
    float x = tr.left;
    IDWriteTextFormat* tab_fmt = r.format(13.f);
    for (size_t i = 0; i < s.groups.size(); ++i) {
        const float w = tab_width(s.groups[i].name);
        const D2D1_RECT_F tab = D2D1::RectF(x, tr.top, x + w, tr.bottom);
        const bool active = static_cast<int>(i) == ls.group;
        const bool drop = static_cast<int>(i) == ls.drag_over_tab;
        if (active) {
            r.fill_round_rect(tab, 6.f, r.theme.accent);
            r.text(tab, s.groups[i].name, tab_fmt, D2D1::ColorF(1.f, 1.f, 1.f));
        } else {
            r.fill_round_rect(tab, 6.f, drop ? r.theme.accent : r.theme.card);
            r.text(tab, s.groups[i].name, tab_fmt, drop ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.text_dim);
        }
        x += w + 6.f;
    }

    // 搜索框背景（文字由 EDIT 子控件自己画，这里只画底与占位提示）
    const D2D1_RECT_F sr = launcher_search_rect(client);
    r.fill_round_rect(sr, 6.f, r.theme.card);
    if (ls.query.empty() && !ls.search.is_open()) {
        r.text(D2D1::RectF(sr.left + 10.f, sr.top, sr.right, sr.bottom), L"搜索名称或路径…",
               r.format(13.f), r.theme.text_dim);
    }

    const GridLayout gl = launcher_grid(client);
    ls.scroll = grid_clamp_scroll(gl, static_cast<int>(ls.filtered.size()), ls.scroll);

    std::vector<GridItem> cells;
    cells.reserve(ls.filtered.size());
    for (size_t i = 0; i < ls.filtered.size(); ++i) {
        const LaunchItem& item = s.groups[ls.group].items[ls.filtered[i]];
        GridItem gi;
        gi.label = item.name;
        gi.icon_src = item.icon.empty() ? item.target : item.icon;
        gi.is_dir = !gi.icon_src.empty() && gi.icon_src.back() == L'\\';
        gi.selected = static_cast<int>(i) == ls.sel;
        gi.hovered = static_cast<int>(i) == ls.hover;
        gi.missing = false;
        cells.push_back(std::move(gi));
    }
    grid_render(r, gl, cells, ls.scroll, r.theme.accent, r.theme.hover, r.theme.text,
                r.theme.text_dim);

    if (ls.filtered.empty() && !s.groups.empty()) {
        r.text(D2D1::RectF(kPad, 120.f, client.width - kPad, 180.f), L"没有匹配的条目",
               r.format(13.f), r.theme.text_dim);
    }
}

bool launcher_keydown(AppState& s, D2D1_SIZE_F client, UINT vk) {
    LauncherState& ls = s.launcher;
    const GridLayout gl = launcher_grid(client);
    return grid_keydown(gl, static_cast<int>(ls.filtered.size()), ls.sel, ls.scroll, vk);
}

void launcher_sync_search(AppState& s, HWND parent, D2D1_SIZE_F client, Renderer& r) {
    LauncherState& ls = s.launcher;
    const RECT rc = r.to_physical(launcher_search_rect(client));

    if (!ls.search.is_open()) {
        // 搜索框常驻：呼出时自动获得焦点，直接打字即搜索
        ls.search.open(
            parent, rc, ls.query, r.dpi,
            [&s](const std::wstring& t) {
                s.launcher.query = t;
                launcher_refilter(s);
            },
            [&s]() {
                s.launcher.query.clear();
                launcher_refilter(s);
            });
        // ↓ 从搜索框进网格：输入框自己会吃掉方向键，所以要在这里截住
        ls.search.on_key = [&s](UINT vk) {
            if (vk == VK_DOWN) {
                s.launcher.sel = s.launcher.filtered.empty() ? -1 : 0;
                HWND parent = s.launcher.search.parent;
                if (parent) {
                    ::SetFocus(parent);
                    ::InvalidateRect(parent, nullptr, FALSE);
                }
                return true;
            }
            // Ctrl+1..4 / Ctrl+Tab 是全局视图切换，但按键落在子 EDIT 上、到不了面板，
            // 所以在这里转投给父窗口（面板的 WM_KEYDOWN 里统一处理）。
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (vk == VK_TAB || (vk >= '1' && vk <= '4'))) {
                HWND parent = s.launcher.search.parent;
                if (parent) ::PostMessageW(parent, WM_KEYDOWN, vk, 0);
                return true;
            }
            return false;
        };
        // 搜索框在失焦时要留着：网格抢焦点不能让它和已输入的内容一起消失
        ls.search.keep_open_on_blur = true;
    } else {
        ls.search.set_rect(rc);
    }
}

void launcher_delete_selected(AppState& s) {
    LauncherState& ls = s.launcher;
    if (ls.sel < 0 || ls.sel >= static_cast<int>(ls.filtered.size())) return;
    if (s.groups.empty()) return;
    auto& items = s.groups[ls.group].items;
    items.erase(items.begin() + ls.filtered[ls.sel]);
    s.data_dirty = true;  // 只删引用，不碰磁盘上的文件
    launcher_refilter(s);
}

void launcher_add_group(AppState& s) {
    // 连续编号命名，避免为了一个新分组先弹输入框
    int n = static_cast<int>(s.groups.size()) + 1;
    std::wstring name = L"新分组 " + std::to_wstring(n);
    while (std::any_of(s.groups.begin(), s.groups.end(),
                       [&](const LaunchGroup& g) { return g.name == name; })) {
        name = L"新分组 " + std::to_wstring(++n);
    }
    s.groups.push_back(LaunchGroup{ name, {} });
    s.launcher.group = static_cast<int>(s.groups.size()) - 1;
    s.launcher.sel = -1;
    s.data_dirty = true;
    launcher_refilter(s);
}

void launcher_move_item_to_group(AppState& s, int filtered_index, int group_index) {
    LauncherState& ls = s.launcher;
    if (filtered_index < 0 || filtered_index >= static_cast<int>(ls.filtered.size())) return;
    if (group_index < 0 || group_index >= static_cast<int>(s.groups.size())) return;
    if (group_index == ls.group) return;

    auto& src = s.groups[ls.group].items;
    const int raw = ls.filtered[filtered_index];
    LaunchItem moved = src[raw];
    src.erase(src.begin() + raw);
    s.groups[group_index].items.push_back(std::move(moved));
    s.data_dirty = true;
    launcher_refilter(s);
}

void launcher_rename_group(App& app, D2D1_SIZE_F client) {
    AppState& s = app.state;
    LauncherState& ls = s.launcher;
    if (s.groups.empty()) return;
    const int gi = std::clamp(ls.group, 0, static_cast<int>(s.groups.size()) - 1);

    // 标签矩形：与渲染共用 tab_width 的累加
    const D2D1_RECT_F tr = launcher_tabs_rect(client);
    D2D1_RECT_F target = tr;
    float x = tr.left;
    for (int i = 0; i < static_cast<int>(s.groups.size()); ++i) {
        const float w = tab_width(s.groups[i].name);
        if (i == gi) {
            target = D2D1::RectF(x, tr.top, x + w, tr.bottom);
            break;
        }
        x += w + 6.f;
    }
    const RECT rc = app.render.to_physical(target);
    const std::wstring current = s.groups[gi].name;
    ls.search.open(
        app.panel, rc, current, app.render.dpi,
        [&app, gi](const std::wstring& t) {
            AppState& st = app.state;
            if (!t.empty() && gi < static_cast<int>(st.groups.size())) {
                // 重名分组在 parse 时会被合并（数据层语义），所以拒绝重名
                bool dup = false;
                for (size_t i = 0; i < st.groups.size(); ++i) {
                    if (static_cast<int>(i) != gi && st.groups[i].name == t) dup = true;
                }
                if (!dup) {
                    st.groups[gi].name = t;
                    st.data_dirty = true;
                }
            }
            launcher_refilter(st);
            // 改名框占用的是搜索框那个 InlineEdit，用完得把搜索框还回来
            launcher_sync_search(st, app.panel, app.render.client_logical(), app.render);
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() {
            launcher_sync_search(app.state, app.panel, app.render.client_logical(), app.render);
        });
}

void launcher_delete_group(App& app) {
    AppState& s = app.state;
    if (s.groups.empty()) return;
    const int gi = std::clamp(s.launcher.group, 0, static_cast<int>(s.groups.size()) - 1);

    const size_t n = s.groups[gi].items.size();
    if (n > 0) {
        const std::wstring msg = L"删除分组“" + s.groups[gi].name + L"”？\n\n" +
                                 std::to_wstring(n) +
                                 L" 个条目会被移除（只删快捷方式条目，磁盘上的文件不受影响）。";
        if (::MessageBoxW(app.panel, msg.c_str(), L"Stargazer",
                          MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }
    s.groups.erase(s.groups.begin() + gi);
    s.data_dirty = true;
    // 删到空了就补一个空分组：界面不能一个分组标签都没有
    if (s.groups.empty()) s.groups.push_back(LaunchGroup{ L"常用", {} });
    s.launcher.group = std::clamp(gi, 0, static_cast<int>(s.groups.size()) - 1);
    s.launcher.sel = -1;
    s.launcher.scroll = 0;
    launcher_refilter(s);
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void launcher_context_menu(App& app, POINT screen_pt, POINT client_pt) {
    AppState& s = app.state;
    const int hit = launcher_hittest(s, app.render.client_logical(),
                                     app.render.to_logical(client_pt));
    if (hit >= 0) s.launcher.sel = hit;

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"新建条目(&N)");
    ::AppendMenuW(menu, MF_STRING, 2, L"新建分组(&G)");
    ::AppendMenuW(menu, MF_STRING, 6, L"重命名分组(&P)");
    ::AppendMenuW(menu, MF_STRING, 7, L"删除分组(&T)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 3, L"重命名(&R)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 4, L"删除(&D)");
    ::AppendMenuW(menu, MF_STRING | (hit >= 0 ? MF_ENABLED : MF_GRAYED), 5,
                  L"打开所在位置(&F)");

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x,
                                      screen_pt.y, 0, app.panel, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            launcher_begin_new_item(app, app.render.client_logical());
            break;
        case 2:
            launcher_add_group(s);
            break;
        case 3:
            launcher_begin_rename(app, app.render.client_logical());
            break;
        case 4:
            launcher_delete_selected(s);
            break;
        case 5:
            if (hit >= 0) {
                const LaunchItem& it = s.groups[s.launcher.group].items[s.launcher.filtered[hit]];
                if (!it.target.empty()) {
                    // 选中该文件而不只是打开它所在目录
                    const std::wstring arg = L"/select," + it.target;
                    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                                    SW_SHOWNORMAL);
                }
            }
            break;
        case 6:
            launcher_rename_group(app, app.render.client_logical());
            break;
        case 7:
            launcher_delete_group(app);
            break;
        default:
            break;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

}  // namespace sg
