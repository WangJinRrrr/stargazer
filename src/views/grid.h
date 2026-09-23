#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>
#include <vector>

#include "render.h"

namespace sg {

// 共用网格常量（96 DPI 逻辑像素）。启动板与收纳盒的网格必须用同一份布局算法，
// 否则会出现“点得到但画不出”。
constexpr float kCell = 96.f;
constexpr float kGap = 8.f;
constexpr float kPad = 16.f;
constexpr float kTabsH = 36.f;    // 分组/盒子标签行
constexpr float kSearchH = 34.f;  // 搜索框行

// 网格只认识这些中性字段，不认识 LaunchItem 也不认识 BoxItem：
// 视图负责把各自的数据翻译成 GridItem。
struct GridItem {
    std::wstring label;      // 显示名
    std::wstring icon_src;   // 取图标的路径（空则用 label 画占位）
    bool is_dir = false;
    bool selected = false;
    bool hovered = false;
    bool missing = false;    // 失效项：灰显 + 删除线
};

// 网格区域在窗口里的纵向范围（top 由视图给出：视图可以用标签行/搜索框/两者）
struct GridLayout {
    float top = 0.f;      // 网格区上沿（逻辑 DIP）
    float bottom = 0.f;   // 网格区下沿
    int cols = 1;
    int rows_visible = 1;
};

GridLayout grid_measure(float client_w, float client_h, float top);
D2D1_RECT_F grid_cell_rect(const GridLayout& gl, int index, int scroll);
// 返回 index，未命中返回 -1
int grid_hittest(const GridLayout& gl, int count, int scroll, D2D1_POINT_2F pt);
// 滚动夹紧；返回夹紧后的 scroll
int grid_clamp_scroll(const GridLayout& gl, int count, int scroll);
// 键盘导航；返回 true 表示按键被消费。移动 sel/scroll（sel<0 = 焦点不在网格）
bool grid_keydown(const GridLayout& gl, int count, int& sel, int& scroll, UINT vk);

// 把一屏可见的格子画出来（虚拟化：只画可见行）
void grid_render(Renderer& r, const GridLayout& gl, const std::vector<GridItem>& items,
                 int scroll, D2D1_COLOR_F accent, D2D1_COLOR_F hover_color,
                 D2D1_COLOR_F text_color, D2D1_COLOR_F missing_color);

// 按扩展名给占位块配色（同一扩展名总是同一颜色且重启不变）
D2D1_COLOR_F ext_color(const std::wstring& path);

// --- 顶层视图标签行（启动板 / 收纳盒 / 待办 / 浏览）---
constexpr float kViewTabsH = 28.f;
constexpr int kViewCount = 4;

D2D1_RECT_F view_tabs_rect(D2D1_SIZE_F client);
// 返回视图下标（0..3），未命中 -1
int view_tab_hittest(D2D1_SIZE_F client, D2D1_POINT_2F pt);
void view_tabs_render(Renderer& r, D2D1_SIZE_F client, int active);
const wchar_t* view_name(int v);

}  // namespace sg
