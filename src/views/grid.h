#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>
#include <vector>

#include "render.h"

namespace sg {

// 共用网格常量（96 DPI 逻辑像素）。网格布局算法只有一份，否则会出现“点得到但画不出”。
// 格子和间距故意留得开：卡片之间要能透出底色，才有呼吸感。
constexpr float kCell = 128.f;
constexpr float kGap = 18.f;
constexpr float kPad = 28.f;
constexpr float kRadiusSm = 10.f;
constexpr float kRadiusMd = 16.f;
constexpr float kRadiusLg = 22.f;

// 网格只认识这些中性字段，不认识 BoxItem：
// 视图负责把各自的数据翻译成 GridItem。
struct GridItem {
    std::wstring label;      // 显示名
    std::wstring icon_src;   // 取图标的路径（空则用 label 画占位）
    bool is_dir = false;
    bool selected = false;
    float hover = 0.f;       // 0..1，悬停淡入（由视图插值）
    bool missing = false;    // 失效项：灰显 + 删除线
};

// 网格区域在窗口里的范围。left 让内容区可以让出侧栏，不必从窗口左缘起算。
struct GridLayout {
    float top = 0.f;
    float bottom = 0.f;
    float left = kPad;
    int cols = 1;
    int rows_visible = 1;
};

// right_pad 是网格右缘到窗口右缘的留白。底部同样留出 kPad，给提示文字。
GridLayout grid_measure(float client_w, float client_h, float top, float left = kPad,
                        float right_pad = kPad);
D2D1_RECT_F grid_cell_rect(const GridLayout& gl, int index, int scroll);
// 格子里的名字区域（图标下方）。渲染与 F2 改名框共用它，两边不会错位。
D2D1_RECT_F grid_label_rect(const D2D1_RECT_F& cell);
// 返回 index，未命中返回 -1
int grid_hittest(const GridLayout& gl, int count, int scroll, D2D1_POINT_2F pt);
// 滚动夹紧；返回夹紧后的 scroll
int grid_clamp_scroll(const GridLayout& gl, int count, int scroll);
// 键盘导航；返回 true 表示按键被消费。移动 sel/scroll（sel<0 = 焦点不在网格）
bool grid_keydown(const GridLayout& gl, int count, int& sel, int& scroll, UINT vk);

// 把一屏可见的格子画出来（虚拟化：只画可见行）
void grid_render(Renderer& r, const GridLayout& gl, const std::vector<GridItem>& items,
                 int scroll);

// 按扩展名给占位块配色（同一扩展名总是同一颜色且重启不变）
D2D1_COLOR_F ext_color(const std::wstring& path);

}  // namespace sg
