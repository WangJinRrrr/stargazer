#pragma once

#include <windows.h>
#include <d2d1.h>

#include "viewapi.h"
#include "views/grid.h"

namespace sg {

struct App;

// 盒子标签行（位于顶层视图标签行之下的逻辑 DIP 矩形）
D2D1_RECT_F box_tabs_rect(D2D1_SIZE_F client);
// 返回盒子下标，未命中 -1
int box_tab_hittest(const AppState& s, D2D1_SIZE_F client, D2D1_POINT_2F pt);

// 网格区域：Box 没有搜索框，网格紧贴盒子标签行
GridLayout box_layout(D2D1_SIZE_F client);

// 夹紧 box/sel/scroll（Box 无过滤，只需要夹紧）
void box_clamp(AppState& s, D2D1_SIZE_F client);
// 当前盒子里第 index 个格子的矩形
D2D1_RECT_F box_cell_rect(const AppState& s, D2D1_SIZE_F client, int index);

// 返回当前盒子的格子下标，未命中 -1
int box_hittest(App& app, D2D1_POINT_2F pt);

void box_render(App& app);
// 返回 true 表示按键已被消费
bool box_keydown(App& app, UINT vk);

// 呼出时投递当前盒子的存在性校验（工作线程 B）；结果在 UI 线程按 path 回填
void box_request_check(App& app);
// 清理当前盒子里的失效条目（只删引用，**绝不碰磁盘**）
void box_clear_missing(App& app);

}  // namespace sg
