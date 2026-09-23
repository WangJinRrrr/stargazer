#pragma once

#include <windows.h>

#include "viewapi.h"
#include "views/grid.h"  // kCell/kGap/kPad/kTabsH/kSearchH 与网格算法都在这里

namespace sg {

// 启动板自己的布局：视图标签行/分组标签行/搜索框都不属于共用网格，
// 这里只保留纵向拼装（常量来自 grid.h，不另写一份）。

// 坐标约定：client 一律是逻辑 DIP（Renderer::client_logical()），
// 鼠标点先经 Renderer::to_logical()，交给 InlineEdit 的矩形先经 Renderer::to_physical()。
// D2D 用 SetDpi 后是逻辑坐标系，而 GetClientRect / 鼠标 lParam / 子 HWND 都是物理像素。

void launcher_layout(const AppState& s, D2D1_SIZE_F client, int& cols, int& rows_visible);

// 根据 query 重建 filtered；filtered 变化后夹紧 sel 与 scroll
void launcher_refilter(AppState& s);

void launcher_render(Renderer& r, AppState& s, D2D1_SIZE_F client);

// 返回 filtered 下标，未命中返回 -1
int launcher_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt);
// 返回分组下标，未命中返回 -1
int launcher_tab_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt);

// 返回 true 表示按键已被消费
bool launcher_keydown(AppState& s, D2D1_SIZE_F client, UINT vk);

D2D1_RECT_F launcher_search_rect(D2D1_SIZE_F client);
D2D1_RECT_F launcher_cell_rect(const AppState& s, D2D1_SIZE_F client, int filtered_index);

// 把搜索框 EDIT 对齐到布局位置；未打开则打开（呼出时调用）
void launcher_sync_search(AppState& s, HWND parent, D2D1_SIZE_F client, Renderer& r);

// --- 条目与分组管理 ---

// 删掉当前选中条目（只删引用，不碰磁盘上的文件）
void launcher_delete_selected(AppState& s);
// 新建分组（用连续编号命名，不为了一个新分组先弹输入框）
void launcher_add_group(AppState& s);
// 把条目从当前分组移到另一个分组
void launcher_move_item_to_group(AppState& s, int filtered_index, int group_index);

// 右键菜单：命中用客户区坐标，弹菜单用屏幕坐标（两个坐标不能混）
struct App;
void launcher_context_menu(App& app, POINT screen_pt, POINT client_pt);

// 重命名当前条目；输入框叠在该格子上
void launcher_begin_rename(App& app, D2D1_SIZE_F client);
// 新建条目：两步输入（先名称，回车后再输目标）
void launcher_begin_new_item(App& app, D2D1_SIZE_F client);

}  // namespace sg
