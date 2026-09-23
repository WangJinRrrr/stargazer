#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>
#include <vector>

#include "viewapi.h"
#include "views/grid.h"
#include "views/todo_layout.h"

namespace sg {

struct App;

// 布局：底部常驻输入框 + 上面的单列列表
D2D1_RECT_F todo_input_rect(D2D1_SIZE_F client);
D2D1_RECT_F todo_list_rect(D2D1_SIZE_F client);
int todo_rows_visible(D2D1_SIZE_F client);

// 渲染与命中共用 offsets（前缀和）；client 用来夹紧 scroll
void todo_rebuild_layout(AppState& s, D2D1_SIZE_F client);
int todo_hittest(App& app, D2D1_POINT_2F pt);          // 行下标，未命中 -1
bool todo_checkbox_hit(App& app, D2D1_POINT_2F pt);    // 是否点在左侧复选框热区
bool todo_checkbox_hit_in_row(App& app, int row, D2D1_POINT_2F pt);

void todo_render(App& app);
bool todo_keydown(App& app, UINT vk);

// 进入/离开视图、常驻输入框就位
void todo_activate(App& app);
void todo_leave(App& app);
void todo_sync_input(App& app);

// --- 条目操作（Task 5 起）---
void todo_toggle_done(App& app);
void todo_open_selected(App& app);
void todo_delete_selected(App& app);
void todo_clear_done(App& app);
void todo_copy_selected(App& app);
void todo_rename_selected(App& app);
void todo_reveal_selected(App& app);
void todo_context_menu(App& app, POINT screen_pt, POINT client_pt);

// --- 输入流水线（Task 6 起）---
bool todo_add_from_clipboard(App& app);
bool todo_add_from_paths(App& app, const std::vector<std::wstring>& paths);
void todo_add_text(App& app, const std::wstring& text);
void todo_on_image_saved(App& app, uint64_t request_id);

}  // namespace sg
