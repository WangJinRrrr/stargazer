#include "views/browse.h"

#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app.h"
#include "clipboard.h"
#include "icons.h"
#include "model/dirlist.h"
#include "model/paths.h"
#include "model/search.h"  // equals_ci（改名时防撞名）

namespace sg {

namespace {

// 列表项完整路径
std::wstring entry_path(const AppState& s, int index) {
    const BrowseState& b = s.browse;
    if (index < 0 || index >= static_cast<int>(b.entries.size())) return std::wstring();
    return append_name(b.path, b.entries[index].name);
}

// 当前列表里已占用的名字（新建/改名时防撞名）
// 筛选期间**被筛掉的名字也算占用**：只看 entries 就会造出重名
// （重名的那一项被筛选器盖着，用户看不见却真实存在）
std::vector<std::wstring> taken_names(const AppState& s) {
    std::vector<std::wstring> out;
    out.reserve(s.browse.entries.size() + s.browse.all.size());
    for (const auto& e : s.browse.entries) out.push_back(e.name);
    for (const auto& e : s.browse.all) out.push_back(e.name);
    return out;
}

// “正在读取 / 错误 / 提示”那一行的高度（没内容时为 0）。
// 筛选生效时也要让出这一行 —— 那行会显示“筛了多少条 / 怎么清掉”
float browse_note_h(const BrowseState& b) {
    return (b.error.empty() && b.note.empty() && !b.loading && b.filter.empty()) ? 0.f : 20.f;
}

// 按名字找回选中项：列表一变（筛选、自动刷新）保留下标就会指到另一个文件，
// 那意味着回车/删除动的是错的那一项
void restore_sel(BrowseState& b, const std::wstring& name) {
    b.sel = -1;
    if (name.empty()) return;
    for (size_t i = 0; i < b.entries.size(); ++i) {
        if (equals_ci(b.entries[i].name, name)) {
            b.sel = static_cast<int>(i);
            break;
        }
    }
}

// 常驻路径栏的内框：EDIT 子控件要缩进来，否则它那块方底会盖掉圆角与描边
D2D1_RECT_F browse_path_inner_rect(D2D1_SIZE_F client) {
    const D2D1_RECT_F bar = browse_path_rect(client);
    return D2D1::RectF(bar.left + 6.f, bar.top + 4.f, bar.right - 6.f, bar.bottom - 4.f);
}

D2D1_RECT_F browse_filter_inner_rect(D2D1_SIZE_F client) {
    const D2D1_RECT_F bar = browse_filter_rect(client);
    return D2D1::RectF(bar.left + 6.f, bar.top + 4.f, bar.right - 6.f, bar.bottom - 4.f);
}

void clamp_browse(AppState& s, D2D1_SIZE_F client) {
    BrowseState& b = s.browse;
    const int count = static_cast<int>(b.entries.size());
    if (b.sel >= count) b.sel = count > 0 ? count - 1 : -1;
    if (b.sel < -1) b.sel = -1;
    const int rows = browse_rows_visible(s, client);
    const int max_scroll = std::max(0, count - rows);
    b.scroll = std::clamp(b.scroll, 0, max_scroll);
    if (b.sel >= 0) {
        if (b.sel < b.scroll) b.scroll = b.sel;
        if (b.sel >= b.scroll + rows) b.scroll = b.sel - rows + 1;
    }
}

void set_note(App& app, const std::wstring& text) {
    app.state.browse.note = text;
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

}  // namespace

D2D1_RECT_F browse_filter_rect(D2D1_SIZE_F client) {
    // 窗口窄的时候按比例收窄，宁可筛选框小一点也别把路径栏挤没
    const float avail = client.width - kPad * 2.f;
    const float w = std::min(kBrowseFilterW, std::max(120.f, avail * 0.4f));
    return D2D1::RectF(client.width - kPad - w, kViewTabsH + kPad, client.width - kPad,
                       kViewTabsH + kPad + kBrowseBarH);
}

D2D1_RECT_F browse_path_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, kViewTabsH + kPad, browse_filter_rect(client).left - kGap,
                       kViewTabsH + kPad + kBrowseBarH);
}

D2D1_RECT_F browse_list_rect(const AppState& s, D2D1_SIZE_F client) {
    const float top = browse_path_rect(client).bottom + kPad + browse_note_h(s.browse);
    return D2D1::RectF(kPad, top, client.width - kPad, client.height - kPad);
}

// 列表行的名字区域（图标右侧）。渲染与 F2 改名框共用，两边不会错位。
D2D1_RECT_F browse_row_label_rect(const AppState& s, D2D1_SIZE_F client, int index) {
    const D2D1_RECT_F list = browse_list_rect(s, client);
    const float y = list.top + (index - s.browse.scroll) * kBrowseRowH;
    return D2D1::RectF(list.left + 6.f + kBrowseIcon + 8.f, y, list.right - 6.f,
                       y + kBrowseRowH - 2.f);
}

int browse_rows_visible(const AppState& s, D2D1_SIZE_F client) {
    const D2D1_RECT_F list = browse_list_rect(s, client);
    const int rows = static_cast<int>((list.bottom - list.top) / kBrowseRowH);
    return std::max(1, rows);
}

int browse_row_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt) {
    const D2D1_RECT_F list = browse_list_rect(s, client);
    if (pt.x < list.left || pt.x > list.right || pt.y < list.top || pt.y > list.bottom) return -1;
    const int row = static_cast<int>((pt.y - list.top) / kBrowseRowH);
    if (row < 0 || row >= browse_rows_visible(s, client)) return -1;
    const int idx = row + s.browse.scroll;
    if (idx < 0 || idx >= static_cast<int>(s.browse.entries.size())) return -1;
    return idx;
}

std::wstring browse_sel_path(const AppState& s) { return entry_path(s, s.browse.sel); }

std::wstring browse_root(const AppState& s, const std::wstring& exe_dir) {
    const std::wstring configured = config_get(s.config, L"browse_root", L"");
    if (!configured.empty()) return configured;
    return exe_dir;  // 没设置过就给个能用的起点，而不是空白
}

void browse_refresh(App& app) {
    BrowseState& b = app.state.browse;
    if (b.path.empty()) {
        b.entries.clear();
        b.error.clear();
        b.loading = false;
        return;
    }
    ++b.request_id;  // 旧结果回来时 id 不匹配，会被丢弃
    b.loading = true;
    fs_list_dir(b.path, b.request_id);
    fs_watch_dir(b.path);  // 目录被外部改动时自动重载；路径没变时它自己会跳过
}

void browse_go(App& app, const std::wstring& path) {
    BrowseState& b = app.state.browse;
    if (path.empty()) return;
    // 历史：丢掉当前位置之后的，再推入新路径
    if (b.hist_pos + 1 < static_cast<int>(b.history.size())) {
        b.history.resize(static_cast<size_t>(b.hist_pos) + 1);
    }
    if (b.history.empty() || b.history.back() != path) b.history.push_back(path);
    b.hist_pos = static_cast<int>(b.history.size()) - 1;
    b.path = path;
    b.sel = -1;
    b.scroll = 0;
    b.error.clear();
    b.note.clear();
    browse_refresh(app);
    browse_sync_path_edit(app);
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_up(App& app) {
    const std::wstring parent = parent_path(app.state.browse.path);
    if (parent.empty()) {
        set_note(app, L"已经在顶层目录（或这是 UNC 根）");
        return;
    }
    browse_go(app, parent);
}

void browse_back(App& app) {
    BrowseState& b = app.state.browse;
    if (b.hist_pos <= 0) return;
    --b.hist_pos;
    b.path = b.history[static_cast<size_t>(b.hist_pos)];
    b.sel = -1;
    b.scroll = 0;
    b.error.clear();
    browse_refresh(app);
    browse_sync_path_edit(app);
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_forward(App& app) {
    BrowseState& b = app.state.browse;
    if (b.hist_pos + 1 >= static_cast<int>(b.history.size())) return;
    ++b.hist_pos;
    b.path = b.history[static_cast<size_t>(b.hist_pos)];
    b.sel = -1;
    b.scroll = 0;
    b.error.clear();
    browse_refresh(app);
    browse_sync_path_edit(app);
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_edit_path(App& app) {
    AppState& s = app.state;
    BrowseState& b = s.browse;
    const RECT rc = app.render.to_physical(browse_path_inner_rect(app.render.client_logical()));
    b.path_edit.open(
        app.panel, rc, b.path, app.render.dpi,
        [&app](const std::wstring& t) {
            if (!t.empty()) browse_go(app, t);
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() { ::InvalidateRect(app.panel, nullptr, FALSE); });
    // ↑↓ 直接回到列表（不占着键盘）
    b.path_edit.on_key = [&app](UINT vk) {
        if (vk != VK_DOWN && vk != VK_UP) return false;
        HWND parent = app.state.browse.path_edit.parent;
        if (parent) {
            ::SetFocus(parent);
            ::InvalidateRect(parent, nullptr, FALSE);
        }
        return true;
    };
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_sync_path_edit(App& app) {
    BrowseState& b = app.state.browse;
    if (b.path_edit.is_open()) b.path_edit.set_text(b.path);
}

// 筛选：换一份**显示列表**，而不是插一层下标映射。
// 下标直通让 open/rename/delete 这些直接吃下标的地方不必改也不必担心漏改，
// 代价只是筛选期间多存一份全量（未筛选时 all 是空的，常见情况不占内存）。
void browse_filter_set(App& app, const std::wstring& text) {
    AppState& s = app.state;
    BrowseState& b = s.browse;
    if (b.filter == text) return;

    // 选中项必须在下边换列表之前按名字记下：b.sel 指的是**筛完后**的下标，
    // 列表一换它就指向别的文件了（那就等于回车/删除动错项）
    std::wstring keep;
    if (b.sel >= 0 && b.sel < static_cast<int>(b.entries.size())) {
        keep = b.entries[static_cast<size_t>(b.sel)].name;
    }

    const bool was = !b.filter.empty();
    const bool now = !text.empty();
    if (!was && now) {
        b.all = std::move(b.entries);  // 开始筛：全量搬进 all（move，不拷贝）
        b.entries.clear();
    } else if (was && !now) {
        b.entries = std::move(b.all);  // 清空筛选：全量搬回显示列表
        b.all = std::vector<FsEntry>();  // 真正还回内存（clear 不释放容量）
    }
    b.filter = text;

    // 只有筛选还在生效时才从全量重建显示列表。
    // 清空筛选那条分支里 all 已经空了，再无条件重建就会把列表抹成空的
    // （真正的 bug，挖出来时就是这个形状）
    if (!b.filter.empty()) {
        b.entries = filter_dir_entries(b.all, b.filter);
    }
    restore_sel(b, keep);
    clamp_browse(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

bool browse_clear_filter(App& app) {
    if (app.state.browse.filter.empty()) return false;
    browse_filter_set(app, L"");
    app.state.browse.filter_edit.close();  // 点钩上后把输入框也收掉
    return true;
}

void browse_edit_filter(App& app) {
    AppState& s = app.state;
    BrowseState& b = s.browse;
    const RECT rc = app.render.to_physical(browse_filter_inner_rect(app.render.client_logical()));
    b.filter_edit.open(
        app.panel, rc, b.filter, app.render.dpi,
        [&app](const std::wstring& t) {
            browse_filter_set(app, t);
            ::SetFocus(app.panel);  // 回车后键盘交给列表，直接 ↓ / Enter 就能用
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() {
            browse_filter_set(app, L"");  // Esc：清空筛选（面板藏不藏由 app 层决定）
            ::InvalidateRect(app.panel, nullptr, FALSE);
        });
    b.filter_edit.on_key = [&app](UINT vk) {  // ↓ 回列表，不占着键盘
        if (vk != VK_DOWN && vk != VK_UP) return false;
        ::SetFocus(app.panel);
        ::InvalidateRect(app.panel, nullptr, FALSE);
        return true;
    };
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_sync_edit_rects(App& app) {
    BrowseState& b = app.state.browse;
    const D2D1_SIZE_F client = app.render.client_logical();
    if (b.path_edit.is_open()) {
        b.path_edit.set_rect(app.render.to_physical(browse_path_inner_rect(client)));
    }
    if (b.filter_edit.is_open()) {
        b.filter_edit.set_rect(app.render.to_physical(browse_filter_inner_rect(client)));
    }
}

void browse_activate(App& app) {
    AppState& s = app.state;
    BrowseState& b = s.browse;
    if (b.path.empty()) {
        const std::wstring root = browse_root(s, app.paths.exe_dir);
        if (!root.empty()) {
            b.path = root;
            b.history.clear();
            b.history.push_back(root);
            b.hist_pos = 0;
        }
    } else {
        // 已经有路径（比如切走又切回来）：只重新枚举，不重置历史
        b.sel = -1;
    }
    ::SetFocus(app.panel);  // 键盘默认归列表，路径栏要点/Ctrl+L
    browse_refresh(app);
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_leave(App& app) {
    app.state.browse.path_edit.close();
    app.state.browse.filter_edit.close();  // 筛选词留着，输框收掉（回来还在）
    fs_watch_dir(L"");  // 离开视图就别再盯着目录了（网盘目录白耗 CPU）
}

void browse_on_dir_loaded(App& app) {
    AppState& s = app.state;
    BrowseState& b = s.browse;
    std::wstring dir;
    std::wstring error;
    std::vector<FsEntry> entries;
    if (!fs_take_dir(b.request_id, dir, entries, error)) return;  // 过期结果：丢弃
    if (dir != b.path) return;                                    // 双保险
    // 选中项按名字重新找回来：自动刷新时列表会变，保留下标会让回车打开错的那个
    std::wstring sel_name;
    if (b.sel >= 0 && b.sel < static_cast<int>(b.entries.size())) {
        sel_name = b.entries[static_cast<size_t>(b.sel)].name;
    }
    if (b.filter.empty()) {
        b.entries = std::move(entries);  // 不筛选：entries 就是全量，不额外存一份
    } else {
        b.all = std::move(entries);      // 筛选着：全量归 all，显示列表等下面重建
    }
    b.error = error;
    b.loading = false;
    if (b.filter.empty()) {
        restore_sel(b, sel_name);
    } else {
        b.entries = filter_dir_entries(b.all, b.filter);
        restore_sel(b, sel_name);
    }
    clamp_browse(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_on_dir_changed(App& app) {
    BrowseState& b = app.state.browse;
    if (b.path.empty()) return;
    ++b.request_id;  // 这次请求的 id；旧结果自动作废
    fs_list_dir(b.path, b.request_id);
}

void browse_on_op_done(App& app) {
    AppState& s = app.state;
    bool ok = true;
    std::wstring error;
    std::wstring note;
    if (!fs_take_op(s.browse.op_id, ok, error, note)) return;  // 过期结果：丢弃
    if (!ok) {
        set_note(app, L"操作失败：" + error);
    } else if (!note.empty()) {
        set_note(app, note);
    } else {
        s.browse.note.clear();
    }
    browse_refresh(app);  // 磁盘变了，重新读一次当前目录
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_open_selected(App& app) {
    AppState& s = app.state;
    const int sel = s.browse.sel;
    if (sel < 0 || sel >= static_cast<int>(s.browse.entries.size())) return;
    const std::wstring path = entry_path(s, sel);
    if (path.empty()) return;
    if (s.browse.entries[static_cast<size_t>(sel)].is_dir) {
        browse_go(app, path);
        return;
    }
    const HINSTANCE r =
        ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        set_note(app, L"无法打开：" + file_name(path));
        return;
    }
    app_hide(app);  // 与收纳盒一致：打开后收起面板，别让 TOPMOST 挡住刚开的窗口
}

void browse_reveal_selected(App& app) {
    const std::wstring path = browse_sel_path(app.state);
    if (path.empty()) return;
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void browse_copy_selected(App& app) {
    const std::wstring path = browse_sel_path(app.state);
    if (path.empty()) return;
    clipboard_set_paths({ path }, false);
    set_note(app, L"已复制路径到剪贴板（可在资源管理器里粘贴）");
}

void browse_paste(App& app) {
    AppState& s = app.state;
    if (s.browse.path.empty()) return;
    bool move = false;
    const std::vector<std::wstring> srcs = clipboard_get_paths(move);
    if (srcs.empty()) {
        set_note(app, L"剪贴板里没有文件（先在资源管理器里 Ctrl+C）");
        return;
    }
    s.browse.op_id = fs_next_op_id();
    fs_paste(srcs, s.browse.path, move, s.browse.op_id);
    set_note(app, move ? L"正在移动…" : L"正在复制…");
}

void browse_new_folder(App& app) {
    AppState& s = app.state;
    if (s.browse.path.empty()) return;
    const std::wstring name = new_folder_name(taken_names(s));
    s.browse.op_id = fs_next_op_id();
    fs_mkdir(append_name(s.browse.path, name), s.browse.op_id);
    set_note(app, L"正在新建：" + name);
}

void browse_delete_selected(App& app, bool recycle) {
    AppState& s = app.state;
    const std::wstring path = browse_sel_path(s);
    if (path.empty()) return;
    if (!recycle) {
        const std::wstring msg = L"永久删除（不进回收站，无法恢复）：\n\n" + path;
        if (::MessageBoxW(app.panel, msg.c_str(), L"Stargazer",
                          MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
    }
    s.browse.op_id = fs_next_op_id();
    fs_delete(path, recycle, s.browse.op_id);
    set_note(app, recycle ? L"已移到回收站" : L"正在永久删除…");
}

void browse_rename_selected(App& app) {
    AppState& s = app.state;
    Renderer& r = app.render;
    BrowseState& b = s.browse;
    const int sel = b.sel;
    if (sel < 0 || sel >= static_cast<int>(b.entries.size())) return;
    // 就地编辑：矩形只覆盖名字区域（图标保持可见），底色 = 选中行实际填充色，
    // 字色 = 名字最终的颜色 → 打字时看到的就是最终界面
    EditStyle style;
    style.pad_x = 0.f;
    style.paint.bg = blend(r.theme.sel_fill, to_solid(r.theme.bg));
    style.paint.text = blend(r.theme.text, style.paint.bg);
    const D2D1_RECT_F input =
        edit_box_rect(browse_row_label_rect(s, r.client_logical(), sel));
    const RECT rc = r.to_physical(input);
    const std::wstring current = b.entries[static_cast<size_t>(sel)].name;
    const std::wstring dir = b.path;
    b.path_edit.open(
        app.panel, rc, current, app.render.dpi,
        [&app, dir, current](const std::wstring& t) {
            AppState& st = app.state;
            if (t.empty() || t == current) {
                ::InvalidateRect(app.panel, nullptr, FALSE);
                return;
            }
            if (t.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
                set_note(app, L"名字里不能有 \\ / : * ? \" < > |");
                ::InvalidateRect(app.panel, nullptr, FALSE);
                return;
            }
            {
                std::vector<std::wstring> taken = taken_names(st);
                taken.erase(std::remove_if(taken.begin(), taken.end(),
                                           [&](const std::wstring& n) { return equals_ci(n, current); }),
                            taken.end());
                if (name_taken(taken, t)) {
                    set_note(app, L"已经有同名文件：" + t);
                    ::InvalidateRect(app.panel, nullptr, FALSE);
                    return;
                }
            }
            st.browse.op_id = fs_next_op_id();
            fs_rename(append_name(dir, current), append_name(dir, t), st.browse.op_id);
            st.browse.note = L"正在重命名…";
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() { ::InvalidateRect(app.panel, nullptr, FALSE); },
        style);
}

void browse_add_to_box(App& app) {
    AppState& s = app.state;
    const std::wstring path = browse_sel_path(s);
    if (path.empty()) return;
    const int added = static_cast<int>(box_add_paths(s.boxes, s.box_view.box, { path }));
    if (added > 0) {
        s.data_dirty = true;
        const int bi = std::clamp(s.box_view.box, 0, static_cast<int>(s.boxes.size()) - 1);
        set_note(app, L"已添加到收纳盒：" + s.boxes[static_cast<size_t>(bi)].name);
    }
}

bool browse_keydown(App& app, UINT vk) {
    AppState& s = app.state;
    BrowseState& b = s.browse;
    const int count = static_cast<int>(b.entries.size());
    const int rows = browse_rows_visible(s, app.render.client_logical());

    const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    // Ctrl+W/A/S/D：左手位浏览。W/S 上下移动选中；A = 进上一级目录，D = 进下一级目录。
    // 与 ↑↓ / Backspace / Alt+←→ 并存 —— 旧键位不动，手感都靠它们。
    // 注：Ctrl+A/D 是**目录层级**，不是浏览历史（历史仍是 Alt+←/→）；
    // “进下一级”只在选中项是目录时生效 —— 文件交回 Enter，避两者语义混在一起。
    if (ctrl) {
        if (vk == L'W') {
            vk = VK_UP;
        } else if (vk == L'S') {
            vk = VK_DOWN;
        } else if (vk == L'A') {
            browse_up(app);
            return true;
        } else if (vk == L'D') {
            const int sel = b.sel;
            if (sel >= 0 && sel < count && b.entries[static_cast<size_t>(sel)].is_dir) {
                browse_go(app, entry_path(s, sel));
            }
            return true;  // 不是目录就什么也不做（沉默，不弹提示）
        }
    }

    if (ctrl && vk == L'L') {
        browse_edit_path(app);
        return true;
    }
    if (ctrl && vk == L'F') {
        browse_edit_filter(app);
        return true;
    }
    if (ctrl && vk == L'C') {
        browse_copy_selected(app);
        return true;
    }
    if (ctrl && vk == L'V') {
        browse_paste(app);
        return true;
    }
    if (alt && vk == VK_LEFT) {
        browse_back(app);
        return true;
    }
    if (alt && vk == VK_RIGHT) {
        browse_forward(app);
        return true;
    }
    switch (vk) {
        case VK_DOWN:
            if (count > 0) b.sel = (b.sel < 0) ? 0 : std::min(b.sel + 1, count - 1);
            break;
        case VK_UP:
            if (count > 0) b.sel = (b.sel < 0) ? 0 : std::max(b.sel - 1, 0);
            break;
        case VK_PRIOR:
            b.sel = std::max(0, (b.sel < 0 ? 0 : b.sel) - rows);
            break;
        case VK_NEXT:
            b.sel = std::min(std::max(0, count - 1), (b.sel < 0 ? 0 : b.sel) + rows);
            break;
        case VK_HOME:
            if (count > 0) b.sel = 0;
            break;
        case VK_END:
            if (count > 0) b.sel = count - 1;
            break;
        case VK_RETURN:
            browse_open_selected(app);
            return true;
        case VK_BACK:
            browse_up(app);
            return true;
        case VK_F2:
            browse_rename_selected(app);
            return true;
        case VK_F5:
            b.note.clear();
            browse_refresh(app);
            ::InvalidateRect(app.panel, nullptr, FALSE);
            return true;
        case VK_F7:
            browse_new_folder(app);
            return true;
        case VK_DELETE:
            browse_delete_selected(app, !shift);
            return true;
        default:
            return false;
    }
    clamp_browse(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
    return true;
}

void browse_context_menu(App& app, POINT screen_pt, POINT client_pt) {
    AppState& s = app.state;
    const D2D1_SIZE_F client = app.render.client_logical();
    const D2D1_POINT_2F lpt = app.render.to_logical(client_pt);
    const int hit = browse_row_hittest(s, client, lpt);
    if (hit >= 0) s.browse.sel = hit;
    const bool has_sel = s.browse.sel >= 0;

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 1, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 2,
                  L"在资源管理器中显示(&E)");
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 3, L"复制路径(&C)");
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 4,
                  L"添加到收纳盒(&B)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 5, L"新建文件夹(&N)");
    ::AppendMenuW(menu, MF_STRING, 6, L"粘贴(&V)");
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 7, L"重命名(&R)");
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 8,
                  L"删除（回收站）(&D)");
    ::AppendMenuW(menu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), 9, L"永久删除(&X)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 10, L"刷新(&F5)");

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x,
                                      screen_pt.y, 0, app.panel, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            browse_open_selected(app);
            break;
        case 2:
            browse_reveal_selected(app);
            break;
        case 3:
            browse_copy_selected(app);
            break;
        case 4:
            browse_add_to_box(app);
            break;
        case 5:
            browse_new_folder(app);
            break;
        case 6:
            browse_paste(app);
            break;
        case 7:
            browse_rename_selected(app);
            break;
        case 8:
            browse_delete_selected(app, true);
            break;
        case 9:
            browse_delete_selected(app, false);
            break;
        case 10:
            browse_refresh(app);
            break;
        default:
            break;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void browse_render(App& app) {
    Renderer& r = app.render;
    AppState& s = app.state;
    BrowseState& b = s.browse;
    const D2D1_SIZE_F client = r.client_logical();

    // 路径栏：Win11 的地址栏（控件底 + 1px 描边 + 4 圆角）
    const D2D1_RECT_F bar = browse_path_rect(client);
    r.fill_round_rect(bar, kRadiusSm, r.theme.control);
    r.stroke_round_rect(bar, kRadiusSm, r.theme.border, 1.f);
    if (!b.path_edit.is_open()) {
        const std::wstring shown =
            b.path.empty() ? L"未设置浏览目录：托盘图标右键 → 设置浏览目录…" : b.path;
        const D2D1_RECT_F inner = browse_path_inner_rect(client);
        r.text(D2D1::RectF(inner.left + 10.f, bar.top, inner.right, bar.bottom), shown,
               r.format(14.f), b.path.empty() ? r.theme.text_faint : r.theme.text);
    }

    // 筛选框：与路径栏同一行（右侧），输入即筛；未输入时是占位文字
    const D2D1_RECT_F fbar = browse_filter_rect(client);
    r.fill_round_rect(fbar, kRadiusSm, r.theme.control);
    r.stroke_round_rect(fbar, kRadiusSm, r.theme.border, 1.f);
    if (!b.filter_edit.is_open()) {
        const D2D1_RECT_F fin = browse_filter_inner_rect(client);
        r.text(D2D1::RectF(fin.left + 10.f, fbar.top, fin.right, fbar.bottom),
               b.filter.empty() ? L"筛选 Ctrl+F" : b.filter, r.format(14.f),
               b.filter.empty() ? r.theme.text_faint : r.theme.text);
    }

    // 提示行 / 错误行（列表上沿由 browse_list_rect 让出这一行的高度）
    const D2D1_RECT_F list = browse_list_rect(s, client);
    // 筛选时把“筛了多少 / 怎么清掉”写在这一行，不然用户只能猜为什么面板里少东西
    const std::wstring filter_line =
        b.filter.empty() ? std::wstring()
                         : L"筛选 “" + b.filter + L"”：共 " +
                               std::to_wstring(b.entries.size()) + L"/" +
                               std::to_wstring(b.all.size()) + L" 项（Esc 清空）";
    if (!b.error.empty() || !b.note.empty() || b.loading || !b.filter.empty()) {
        const std::wstring line =
            !b.error.empty() ? (L"无法访问：" + b.error + L"（F5 重试）")
                             : (b.loading ? L"正在读取…"
                                          : (!b.note.empty() ? b.note : filter_line));
        const D2D1_COLOR_F color = b.error.empty() ? r.theme.text_dim : r.theme.danger;
        r.text(D2D1::RectF(kPad, bar.bottom + 2.f, client.width - kPad,
                           bar.bottom + 2.f + browse_note_h(b)),
               line, r.format(12.f), color);
    }

    // 列表
    if (b.entries.empty() && b.error.empty() && !b.loading) {
        r.text(D2D1::RectF(kPad, list.top, client.width - kPad, list.top + 24.f),
               b.filter.empty() ? L"（空目录）"
                                : (L"（没有名字含 “" + b.filter + L"” 的项）"),
               r.format(14.f), r.theme.text_faint);
        return;
    }

    IDWriteTextFormat* name_fmt = r.format(14.f);
    const int rows = browse_rows_visible(s, client);
    for (int i = b.scroll; i < std::min(static_cast<int>(b.entries.size()), b.scroll + rows);
         ++i) {
        const FsEntry& e = b.entries[static_cast<size_t>(i)];
        const float y = list.top + (i - b.scroll) * kBrowseRowH;
        const D2D1_RECT_F row =
            D2D1::RectF(kPad, y, client.width - kPad, y + kBrowseRowH - 2.f);
        const bool selected = i == b.sel;
        if (selected) {
            r.fill_round_rect(row, kRadiusSm, r.theme.sel_fill);
            r.stroke_round_rect(row, kRadiusSm, r.theme.sel_stroke, 1.f);
        } else if (i == b.hover) {
            r.fill_round_rect(row, kRadiusSm, r.theme.hover);
        }

        const float ix = row.left + 6.f;
        const float iy = y + (kBrowseRowH - 2.f - kBrowseIcon) / 2.f;
        const D2D1_RECT_F irect = D2D1::RectF(ix, iy, ix + kBrowseIcon, iy + kBrowseIcon);
        const std::wstring full = append_name(b.path, e.name);
        if (ID2D1Bitmap* bmp = icons_get(r, full, e.is_dir)) {
            r.rt->DrawBitmap(bmp, irect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            r.fill_round_rect(irect, 4.f, ext_color(e.name));  // 骨架先出，图标后到
        }

        const D2D1_RECT_F label = browse_row_label_rect(s, client, i);
        r.text(label, e.name, name_fmt, r.theme.text);
    }
}

}  // namespace sg
