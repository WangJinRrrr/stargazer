#pragma once

#include <windows.h>

#include "viewapi.h"

namespace sg {

// 启动板布局常量（均为 96 DPI 逻辑像素）
constexpr float kCell = 96.f;
constexpr float kGap = 8.f;
constexpr float kPad = 16.f;
constexpr float kTabsH = 36.f;
constexpr float kSearchH = 34.f;

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

// 按扩展名给占位块配色，同一扩展名总是同一颜色
D2D1_COLOR_F ext_color(const std::wstring& path);

}  // namespace sg
