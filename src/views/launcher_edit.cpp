#include "views/launcher.h"

#include <algorithm>
#include <memory>

#include "app.h"
#include "launch.h"

namespace sg {

void launcher_begin_rename(App& app, D2D1_SIZE_F client) {
    AppState& s = app.state;
    LauncherState& ls = s.launcher;
    if (ls.sel < 0 || ls.sel >= static_cast<int>(ls.filtered.size())) return;

    // 输入框叠在被改的那个格子上；坐标算法与渲染共用同一份
    const D2D1_RECT_F cr = launcher_cell_rect(s, client, ls.sel);
    const D2D1_RECT_F input = D2D1::RectF(cr.left, cr.top, cr.right, cr.top + 72.f);
    const RECT rc = app.render.to_physical(input);

    const int raw = ls.filtered[ls.sel];
    const int group = ls.group;
    const std::wstring current = s.groups[group].items[raw].name;
    ls.search.open(
        app.panel, rc, current, app.render.dpi,
        [&app, group, raw](const std::wstring& t) {
            AppState& s = app.state;
            if (!t.empty()) {
                s.groups[group].items[raw].name = t;
                s.data_dirty = true;
            }
            launcher_refilter(s);
            // 重命名框占用的就是搜索框那个 InlineEdit，用完得把搜索框还回来
            launcher_sync_search(s, app.panel, app.render.client_logical(), app.render);
        },
        [&app]() { launcher_sync_search(app.state, app.panel, app.render.client_logical(), app.render); });
}

void launcher_begin_new_item(App& app, D2D1_SIZE_F client) {
    AppState& s = app.state;
    LauncherState& ls = s.launcher;
    if (s.groups.empty()) s.groups.push_back(LaunchGroup{ L"常用", {} });

    const D2D1_RECT_F sr = launcher_search_rect(client);
    const RECT rc = app.render.to_physical(sr);

    // 两步输入：先名称，回车后再输目标。比自建对话框少写一百多行，且与搜索框行为一致
    auto pending = std::make_shared<std::wstring>();
    ls.search.open(
        app.panel, rc, L"", app.render.dpi,
        [&app, pending](const std::wstring& name) {
            if (name.empty()) return;
            *pending = name;
            AppState& st = app.state;
            const D2D1_RECT_F sr2 = launcher_search_rect(app.render.client_logical());
            const RECT rc2 = app.render.to_physical(sr2);
            st.launcher.search.open(
                app.panel, rc2, L"", app.render.dpi,
                [&app, pending](const std::wstring& target) {
                    AppState& s2 = app.state;
                    if (!target.empty()) {
                        const int gi =
                            std::clamp(s2.launcher.group, 0, static_cast<int>(s2.groups.size()) - 1);
                        s2.groups[gi].items.push_back(item_from_path_keep_name(target, *pending));
                        s2.data_dirty = true;
                        launcher_refilter(s2);
                    }
                    // 两步输入用完，把搜索框还回来
                    launcher_sync_search(s2, app.panel, app.render.client_logical(), app.render);
                    ::InvalidateRect(app.panel, nullptr, FALSE);
                },
                [&app]() {
                    launcher_sync_search(app.state, app.panel, app.render.client_logical(),
                                         app.render);
                });
        },
        nullptr);
}

}  // namespace sg
