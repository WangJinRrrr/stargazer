#include "views/launcher.h"

#include <algorithm>
#include <shellapi.h>  // ShellExecuteW（打开所在位置）

#include "app.h"  // 右键菜单需要完整的 App 定义
#include "icons.h"
#include "model/paths.h"
#include "model/search.h"

namespace sg {

D2D1_COLOR_F ext_color(const std::wstring& path) {
    // 固定 8 色调色板：扩展名哈希取模，同一类型总是同色且重启不变
    static const D2D1_COLOR_F palette[8] = {
        D2D1::ColorF(0.35f, 0.45f, 0.62f), D2D1::ColorF(0.32f, 0.53f, 0.48f),
        D2D1::ColorF(0.58f, 0.44f, 0.35f), D2D1::ColorF(0.50f, 0.38f, 0.55f),
        D2D1::ColorF(0.40f, 0.48f, 0.36f), D2D1::ColorF(0.58f, 0.38f, 0.42f),
        D2D1::ColorF(0.34f, 0.42f, 0.56f), D2D1::ColorF(0.45f, 0.45f, 0.45f),
    };
    const std::wstring ext = extension_of(path);
    if (ext.empty()) return palette[7];
    unsigned h = 2166136261u;  // FNV-1a
    for (wchar_t c : ext) {
        h ^= static_cast<unsigned>(c);
        h *= 16777619u;
    }
    return palette[h % 8];
}

D2D1_RECT_F launcher_tabs_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, kPad, client.width - kPad, kPad + kTabsH);
}

D2D1_RECT_F launcher_search_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, kPad + kTabsH, client.width - kPad, kPad + kTabsH + kSearchH);
}

static float tab_width(const std::wstring& name) {
    return 24.f + static_cast<float>(name.size()) * 13.f;
}

void launcher_layout(const AppState& s, D2D1_SIZE_F client, int& cols, int& rows_visible) {
    (void)s;
    const float avail_w = client.width - kPad * 2.f;
    const float avail_h = client.height - kTabsH - kSearchH - kPad * 2.f;
    cols = static_cast<int>((avail_w + kGap) / (kCell + kGap));
    if (cols < 1) cols = 1;
    rows_visible = static_cast<int>((avail_h + kGap) / (kCell + kGap));
    if (rows_visible < 1) rows_visible = 1;
}

D2D1_RECT_F launcher_cell_rect(const AppState& s, D2D1_SIZE_F client, int filtered_index) {
    int cols = 1, rows = 1;
    launcher_layout(s, client, cols, rows);
    const int col = filtered_index % cols;
    const int row = filtered_index / cols - s.launcher.scroll;
    const float x = kPad + col * (kCell + kGap);
    const float y = kPad + kTabsH + kSearchH + kPad + row * (kCell + kGap);
    return D2D1::RectF(x, y, x + kCell, y + kCell);
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
    const float search_bottom = launcher_search_rect(client).bottom;
    if (pt.y < search_bottom) return -1;  // 搜索框区域归 EDIT 子控件

    int cols = 1, rows = 1;
    launcher_layout(s, client, cols, rows);
    const float gx = pt.x - kPad;
    const float gy = pt.y - (kPad + kTabsH + kSearchH + kPad);
    if (gx < 0 || gy < 0) return -1;
    const int col = static_cast<int>(gx / (kCell + kGap));
    const int row = static_cast<int>(gy / (kCell + kGap));
    // 落在格子间隙里也算没命中，避免“点空白就启动了程序”
    if (gx - col * (kCell + kGap) > kCell) return -1;
    if (gy - row * (kCell + kGap) > kCell) return -1;
    if (col >= cols) return -1;
    const int idx = (row + s.launcher.scroll) * cols + col;
    if (idx < 0 || idx >= static_cast<int>(s.launcher.filtered.size())) return -1;
    return idx;
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

    int cols = 1, rows = 1;
    launcher_layout(s, client, cols, rows);
    const int total_rows = (static_cast<int>(ls.filtered.size()) + cols - 1) / cols;
    ls.scroll = std::clamp(ls.scroll, 0, std::max(0, total_rows - rows));

    IDWriteTextFormat* name_fmt =
        r.format(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (size_t i = 0; i < ls.filtered.size(); ++i) {
        const int row = static_cast<int>(i) / cols;
        if (row < ls.scroll || row >= ls.scroll + rows) continue;  // 只画可见行（虚拟化）

        const D2D1_RECT_F rc = launcher_cell_rect(s, client, static_cast<int>(i));
        const bool selected = static_cast<int>(i) == ls.sel;

        if (selected) {
            r.fill_round_rect(rc, 8.f, r.theme.accent);
        } else if (static_cast<int>(i) == ls.hover) {
            r.fill_round_rect(rc, 8.f, r.theme.hover);
        }

        const LaunchItem& item = s.groups[ls.group].items[ls.filtered[i]];
        const std::wstring icon_src = item.icon.empty() ? item.target : item.icon;
        const bool is_dir = !icon_src.empty() && icon_src.back() == L'\\';

        const float ix = rc.left + (kCell - 48.f) / 2.f;
        const float iy = rc.top + 8.f;
        const D2D1_RECT_F irect = D2D1::RectF(ix, iy, ix + 48.f, iy + 48.f);
        if (ID2D1Bitmap* bmp = icons_get(r, icon_src, is_dir)) {
            r.rt->DrawBitmap(bmp, irect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // 图标未就绪：扩展名色块 + 首字母（骨架先出，图标后到）
            r.fill_round_rect(irect, 8.f, ext_color(icon_src));
            const std::wstring initial =
                item.name.empty() ? std::wstring(L"?") : item.name.substr(0, 1);
            r.text(irect, initial,
                   r.format(20.f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                   D2D1::ColorF(1.f, 1.f, 1.f));
        }

        const D2D1_RECT_F label =
            D2D1::RectF(rc.left + 4.f, iy + 48.f + 4.f, rc.right - 4.f, rc.bottom - 4.f);
        const D2D1_COLOR_F label_color =
            selected ? D2D1::ColorF(1.f, 1.f, 1.f) : r.theme.text;
        r.text(label, item.name, name_fmt, label_color);
    }

    if (ls.filtered.empty() && !s.groups.empty()) {
        r.text(D2D1::RectF(kPad, 120.f, client.width - kPad, 180.f), L"没有匹配的条目",
               r.format(13.f), r.theme.text_dim);
    }
}

bool launcher_keydown(AppState& s, D2D1_SIZE_F client, UINT vk) {
    LauncherState& ls = s.launcher;
    int cols = 1, rows = 1;
    launcher_layout(s, client, cols, rows);
    const int count = static_cast<int>(ls.filtered.size());

    switch (vk) {
        case VK_DOWN:
            // 从搜索框进入网格
            ls.sel = (ls.sel < 0) ? (count > 0 ? 0 : -1) : std::min(ls.sel + cols, count - 1);
            break;
        case VK_UP:
            if (ls.sel >= 0 && ls.sel < cols) {
                ls.sel = -1;  // 回到搜索框
            } else {
                ls.sel = std::max(0, ls.sel - cols);
            }
            break;
        case VK_LEFT:
            ls.sel = std::max(0, ls.sel - 1);
            break;
        case VK_RIGHT:
            ls.sel = std::min(count - 1, ls.sel + 1);
            break;
        case VK_PRIOR:
            ls.scroll = std::max(0, ls.scroll - rows);
            break;
        case VK_NEXT:
            ls.scroll += rows;
            break;
        default:
            return false;
    }

    // 选中项必须在可见范围内
    if (ls.sel >= 0) {
        ls.scroll = std::clamp(ls.scroll, std::max(0, ls.sel / cols - rows + 1), ls.sel / cols);
    }
    return true;
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
            if (vk != VK_DOWN) return false;
            s.launcher.sel = s.launcher.filtered.empty() ? -1 : 0;
            HWND parent = s.launcher.search.parent;
            if (parent) {
                ::SetFocus(parent);
                ::InvalidateRect(parent, nullptr, FALSE);
            }
            return true;
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

void launcher_context_menu(App& app, POINT screen_pt, POINT client_pt) {
    AppState& s = app.state;
    const int hit = launcher_hittest(s, app.render.client_logical(),
                                     app.render.to_logical(client_pt));
    if (hit >= 0) s.launcher.sel = hit;

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"新建条目(&N)");
    ::AppendMenuW(menu, MF_STRING, 2, L"新建分组(&G)");
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
        default:
            break;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

}  // namespace sg
