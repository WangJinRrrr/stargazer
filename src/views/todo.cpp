#include "views/todo.h"

#include <shellapi.h>  // ShellExecuteW

#include <algorithm>
#include <ctime>
#include <cwctype>

#include "app.h"
#include "clipboard.h"
#include "icons.h"
#include "images.h"
#include "model/paths.h"

namespace sg {

namespace {

std::wstring trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::iswspace(static_cast<wint_t>(s[b])) != 0) ++b;
    while (e > b && std::iswspace(static_cast<wint_t>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

// attach 是不是我们自己的副本，且删掉 except_id 这条之后就没别人在用了
bool copy_is_orphan(const App& app, long long except_id, const std::wstring& attach) {
    const AppState& s = app.state;
    if (attach.empty()) return false;
    const std::wstring images = join_path(app.paths.data_dir, L"images");
    if (!todo_is_owned_copy(images, attach)) return false;
    std::vector<std::wstring> others;
    for (const auto& t : s.todos) {
        if (t.id == except_id) continue;
        if (!t.attach.empty()) others.push_back(t.attach);
    }
    return !todo_copy_still_used(others, attach);
}

// 删条目时的唯一磁盘副作用：删掉它的图片副本（仅限 data\images 下、且已无人引用）
void drop_owned_copy(const App& app, long long id, const std::wstring& attach) {
    if (!copy_is_orphan(app, id, attach)) return;
    const DWORD attr = ::GetFileAttributesW(attach.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return;  // 手改数据把目录塞进来时不动它
    ::DeleteFileW(attach.c_str());  // 失败就不管：清缓存失败不值得打断用户
}

// 数据变了之后统一收尾
void after_change(App& app) {
    AppState& s = app.state;
    sort_todos(s.todos);
    todo_rebuild_layout(s, app.render.client_logical());
    s.data_dirty = true;
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

// 排序后按 id 找回选中行（切换完成态会让行号变）
void reselect_by_id(AppState& s, long long id) {
    s.todo.sel = -1;
    for (size_t i = 0; i < s.todos.size(); ++i) {
        if (s.todos[i].id == id) {
            s.todo.sel = static_cast<int>(i);
            break;
        }
    }
}

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
        // 缩略图框：系统缩略图（工作线程取、UI 建位图）；未到位时先画占位色块
        const D2D1_RECT_F thumb = D2D1::RectF(content_x, row.top + 4.f, content_x + kTodoThumbW,
                                             row.top + 4.f + kTodoThumbH);
        if (ID2D1Bitmap* bmp = images_get(r, item.attach)) {
            const D2D1_SIZE_F sz = bmp->GetSize();
            if (sz.width > 0.f && sz.height > 0.f) {
                const float scale = std::min(kTodoThumbW / sz.width, kTodoThumbH / sz.height);
                const float w = sz.width * scale;
                const float h = sz.height * scale;
                const float x = thumb.left + (kTodoThumbW - w) / 2.f;
                const float y = thumb.top + (kTodoThumbH - h) / 2.f;
                r.rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + w, y + h), 1.f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else {
            r.fill_round_rect(thumb, 8.f, ext_color(item.attach));
        }
        // 名字画在缩略图**右侧**：图片行只有 96 高，88 高的缩略图加留白已没地方再放一行字
        const std::wstring label = item.text.empty() ? file_name(item.attach) : item.text;
        const D2D1_RECT_F name =
            D2D1::RectF(thumb.right + 10.f, row.top + 6.f, row.right - 6.f, row.top + 28.f);
        r.text(name, label, r.format(11.f), item.missing ? r.theme.text_dim : text_color);
        if (item.missing) {
            // 引用型图片被外部改名/删除：灰显 + 删除线 + 说明
            const float mid = (name.top + name.bottom) / 2.f;
            r.fill_rect(D2D1::RectF(name.left, mid - 0.5f, name.right, mid + 0.5f),
                        r.theme.text_dim);
            r.text(D2D1::RectF(thumb.right + 10.f, row.top + 30.f, row.right - 6.f, row.top + 52.f),
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

    const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && vk == L'C') {
        todo_copy_selected(app);
        return true;
    }
    if (ctrl && shift && vk == L'D') {
        todo_clear_done(app);
        return true;
    }
    if (ctrl && vk == L'V') {
        todo_add_from_clipboard(app);
        return true;
    }

    switch (vk) {
        case VK_DOWN:
            if (n > 0) t.sel = (t.sel < 0) ? 0 : std::min(t.sel + 1, n - 1);
            break;
        case VK_SPACE:
            todo_toggle_done(app);
            return true;
        case VK_RETURN:
            todo_open_selected(app);
            return true;
        case VK_DELETE:
            todo_delete_selected(app);
            return true;
        case VK_F2:
            todo_rename_selected(app);
            return true;
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
    // ↑↓ 等导航键要从输入框转给列表（输入框自己会吃掉方向键）；Ctrl+V 要看剪贴板里是什么
    t.input.on_key = [&app](UINT vk) {
        if (vk == L'V' && (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
            // 有文件或位图 → 自己接管（走待办流水线）；纯文本交回 EDIT（保留多行粘贴体验）
            bool move = false;
            const bool has_files = !clipboard_get_paths(move).empty();
            std::wstring text;
            const bool has_text = clipboard_get_text(text) && !trim(text).empty();
            std::vector<uint8_t> dib;
            const bool has_bitmap = !has_files && !has_text && clipboard_get_image_dib(dib);
            if (has_files || has_bitmap) {
                todo_add_from_clipboard(app);
                return true;
            }
            return false;
        }
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
    AppState& s = app.state;    if (text.empty()) return;
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

// Task 6：真正的输入流水线
bool todo_add_from_clipboard(App& app) {
    // 1) 文件（CF_HDROP）：逐个看，是图片的各记一条引用
    bool move = false;
    const std::vector<std::wstring> paths = clipboard_get_paths(move);
    if (!paths.empty()) {
        int added = 0;
        for (const auto& p : paths) {
            if (is_image_path(p)) {
                todo_add_image_ref(app, p);
                ++added;
            }
        }
        if (added == 0) {
            app_notify(app, L"待办只收文字、链接和图片");
            return false;
        }
        ::InvalidateRect(app.panel, nullptr, FALSE);
        return true;
    }
    // 2) 文本优先于位图：Excel/Word 复制时剪贴板里同时有文本和位图，
    //    按“位图优先”会把一个单元格变成一张图（Review Focus 2）
    std::wstring text;
    if (clipboard_get_text(text) && !trim(text).empty()) {
        todo_add_text(app, text);
        return true;
    }
    // 3) 位图（截图）：落盘成 data\images\<id>.png
    std::vector<uint8_t> dib;
    if (clipboard_get_image_dib(dib)) {
        todo_add_clipboard_image(app, dib);
        return true;
    }
    return false;  // 剪贴板什么都没有：不提示（Ctrl+V 粘空剪贴板是常见误操作）
}

bool todo_add_from_paths(App& app, const std::vector<std::wstring>& paths) {
    int added = 0;
    for (const auto& p : paths) {
        if (is_image_path(p)) {
            todo_add_image_ref(app, p);
            ++added;
        }
    }
    if (added == 0) {
        app_notify(app, L"待办只收文字、链接和图片");
        return false;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
    return true;
}

void todo_add_image_ref(App& app, const std::wstring& path) {
    AppState& s = app.state;
    TodoItem item;
    item.id = next_todo_id(s.todos);
    item.created = static_cast<long long>(::time(nullptr));
    item.kind = TodoKind::Image;
    item.attach = path;  // 引用：不复制内容
    s.todos.push_back(std::move(item));
    after_change(app);
    s.todo.sel = 0;
    s.todo.scroll = 0.f;
    todo_rebuild_layout(s, app.render.client_logical());
}

void todo_add_clipboard_image(App& app, const std::vector<uint8_t>& dib) {
    AppState& s = app.state;
    TodoItem item;
    item.id = next_todo_id(s.todos);
    item.created = static_cast<long long>(::time(nullptr));
    item.kind = TodoKind::Image;
    const std::wstring images = join_path(app.paths.data_dir, L"images");
    item.attach = join_path(images, std::to_wstring(item.id) + L".png");
    // 先插条目、再落盘：失败时 todo_on_image_saved 会把它撤掉并提示。
    // （顺序反过来会多一个“已落盘但还在编码中”的中间态，反而更难处理）
    const std::wstring attach = item.attach;
    const long long id = item.id;
    s.todos.push_back(std::move(item));
    after_change(app);
    s.todo.sel = 0;
    s.todo.scroll = 0.f;
    todo_rebuild_layout(s, app.render.client_logical());
    s.todo.pending_image_id = id;
    ++s.todo.op_id;
    fs_save_image(dib, attach, s.todo.op_id);
}

void todo_on_image_saved(App& app, uint64_t request_id) {
    AppState& s = app.state;
    if (s.todo.pending_image_id == 0) return;
    bool ok = true;
    std::wstring error;
    std::wstring note;
    if (!fs_take_op(request_id, ok, error, note)) return;  // 不是我们的结果
    const long long id = s.todo.pending_image_id;
    s.todo.pending_image_id = 0;
    if (!ok) {
        // 回滚刚插入的那条：宁可没记上，也不要留一个永远显示“图片已不存在”的条目
        s.todos.erase(std::remove_if(s.todos.begin(), s.todos.end(),
                                     [id](const TodoItem& t) { return t.id == id; }),
                      s.todos.end());
        s.todo.sel = -1;
        after_change(app);
        app_notify(app, L"图片保存失败：" + error);
        return;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void todo_toggle_done(App& app) {
    AppState& s = app.state;
    TodoState& t = s.todo;
    if (t.sel < 0 || t.sel >= static_cast<int>(s.todos.size())) return;
    const long long id = s.todos[static_cast<size_t>(t.sel)].id;
    s.todos[static_cast<size_t>(t.sel)].done = !s.todos[static_cast<size_t>(t.sel)].done;
    after_change(app);
    reselect_by_id(s, id);  // 沉底后选中跟着走，不会“跳”到别的条目
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void todo_delete_selected(App& app) {
    AppState& s = app.state;
    TodoState& t = s.todo;
    if (t.sel < 0 || t.sel >= static_cast<int>(s.todos.size())) return;
    const long long id = s.todos[static_cast<size_t>(t.sel)].id;
    const std::wstring attach = s.todos[static_cast<size_t>(t.sel)].attach;
    s.todos.erase(s.todos.begin() + t.sel);
    drop_owned_copy(app, id, attach);  // 已经从数组里剔除了；还有别的条目引用就不删
    after_change(app);
    if (t.sel >= static_cast<int>(s.todos.size())) {
        t.sel = s.todos.empty() ? -1 : static_cast<int>(s.todos.size()) - 1;
    }
    todo_rebuild_layout(s, app.render.client_logical());
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

void todo_clear_done(App& app) {
    AppState& s = app.state;
    std::vector<std::pair<long long, std::wstring>> gone;
    for (const auto& t : s.todos) {
        if (t.done) gone.emplace_back(t.id, t.attach);
    }
    if (gone.empty()) {
        app_notify(app, L"没有已完成的条目");
        return;
    }
    // 先整体剔除，再逐个判断副本是否还有人用（否则两条都指向同一张图时会误删）
    s.todos.erase(std::remove_if(s.todos.begin(), s.todos.end(),
                                 [](const TodoItem& t) { return t.done; }),
                  s.todos.end());
    for (const auto& g : gone) drop_owned_copy(app, g.first, g.second);
    s.todo.sel = -1;
    s.todo.scroll = 0.f;
    after_change(app);
}

void todo_open_selected(App& app) {
    AppState& s = app.state;
    TodoState& t = s.todo;
    if (t.sel < 0 || t.sel >= static_cast<int>(s.todos.size())) return;
    const TodoItem& item = s.todos[static_cast<size_t>(t.sel)];
    if (item.kind == TodoKind::Text) return;  // 文字条目回车不该有副作用
    const std::wstring target = (item.kind == TodoKind::Link) ? item.text : item.attach;
    if (target.empty()) return;
    if (item.missing) {
        app_notify(app, L"图片已不存在：" + target);
        return;
    }
    const HINSTANCE r =
        ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        app_notify(app, L"打不开：" + target);
        return;
    }
    app_hide(app);  // 与收纳盒/浏览一致：别让 TOPMOST 挡住刚打开的东西
}

void todo_reveal_selected(App& app) {
    const AppState& s = app.state;
    if (s.todo.sel < 0 || s.todo.sel >= static_cast<int>(s.todos.size())) return;
    const std::wstring attach = s.todos[static_cast<size_t>(s.todo.sel)].attach;
    if (attach.empty()) return;
    const std::wstring arg = L"/select," + attach;
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void todo_copy_selected(App& app) {
    AppState& s = app.state;
    TodoState& t = s.todo;
    if (t.sel < 0 || t.sel >= static_cast<int>(s.todos.size())) return;
    const TodoItem& item = s.todos[static_cast<size_t>(t.sel)];
    if (item.kind == TodoKind::Image) {
        if (item.attach.empty()) return;
        clipboard_set_paths({ item.attach }, false);  // 粘到资源管理器就是“粘贴文件”
        app_notify(app, L"已复制图片路径");
        return;
    }
    if (item.text.empty()) return;
    clipboard_set_text(item.text);
    app_notify(app, L"已复制文字");
}

void todo_rename_selected(App& app) {
    AppState& s = app.state;
    TodoState& t = s.todo;
    if (t.sel < 0 || t.sel >= static_cast<int>(s.todos.size())) return;
    const TodoItem& item = s.todos[static_cast<size_t>(t.sel)];
    if (item.kind == TodoKind::Image) {
        app_notify(app, L"图片条目没有文字可改（删了重记或改文件名）");
        return;
    }
    const D2D1_RECT_F list = todo_list_rect(app.render.client_logical());
    const float top = list.top + t.offsets[static_cast<size_t>(t.sel)] - t.scroll;
    const D2D1_RECT_F input =
        D2D1::RectF(list.left, top, list.right, top + todo_row_height(item.kind));
    const RECT rc = app.render.to_physical(input);
    const long long id = item.id;
    const std::wstring current = item.text;
    t.edit.open(
        app.panel, rc, current, app.render.dpi,
        [&app, id, current](const std::wstring& text) {
            AppState& st = app.state;
            if (!text.empty() && text != current) {
                for (auto& it : st.todos) {
                    if (it.id == id) {
                        it.text = text;
                        // 改文字不改类型：链接仍是链接，哪怕改成了别的串（用户自己知道）
                        break;
                    }
                }
                st.data_dirty = true;
            }
            todo_rebuild_layout(st, app.render.client_logical());
            ::InvalidateRect(app.panel, nullptr, FALSE);
        },
        [&app]() { ::InvalidateRect(app.panel, nullptr, FALSE); });
}

void todo_context_menu(App& app, POINT screen_pt, POINT client_pt) {
    AppState& s = app.state;
    const D2D1_POINT_2F lpt = app.render.to_logical(client_pt);
    const int hit = todo_hittest(app, lpt);
    if (hit >= 0) s.todo.sel = hit;
    const bool has = s.todo.sel >= 0 && s.todo.sel < static_cast<int>(s.todos.size());
    const bool is_image =
        has && s.todos[static_cast<size_t>(s.todo.sel)].kind == TodoKind::Image;

    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING | (has ? MF_ENABLED : MF_GRAYED), 1, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING | (has ? MF_ENABLED : MF_GRAYED), 2, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING | (has ? MF_ENABLED : MF_GRAYED), 3, L"编辑(&E)");
    ::AppendMenuW(menu, MF_STRING | (has ? MF_ENABLED : MF_GRAYED), 4,
                  L"切换完成(&T)");
    ::AppendMenuW(menu, MF_STRING | (has ? MF_ENABLED : MF_GRAYED), 5, L"删除(&D)");
    ::AppendMenuW(menu, MF_STRING | (is_image ? MF_ENABLED : MF_GRAYED), 6,
                  L"在资源管理器中显示(&R)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 7, L"粘贴(&V)");
    ::AppendMenuW(menu, MF_STRING, 8, L"清空已完成(&X)");

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x,
                                      screen_pt.y, 0, app.panel, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1:
            todo_open_selected(app);
            break;
        case 2:
            todo_copy_selected(app);
            break;
        case 3:
            todo_rename_selected(app);
            break;
        case 4:
            todo_toggle_done(app);
            break;
        case 5:
            todo_delete_selected(app);
            break;
        case 6:
            todo_reveal_selected(app);
            break;
        case 7:
            todo_add_from_clipboard(app);
            break;
        case 8:
            todo_clear_done(app);
            break;
        default:
            break;
    }
    ::InvalidateRect(app.panel, nullptr, FALSE);
}

}  // namespace sg
