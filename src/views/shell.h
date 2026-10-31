#pragma once

#include <d2d1.h>

#include "views/grid.h"

namespace sg {

// 窗口骨架（96 DPI 逻辑像素）。侧栏放盒子，右侧是当前盒子的内容。
// 命中与绘制共用这些矩形，避免“点得到的按钮画在别处”。
constexpr float kTitleH = 56.f;
constexpr float kSidebarW = 236.f;
constexpr float kSidePad = 14.f;
constexpr float kSideRowH = 44.f;
constexpr float kSideRowGap = 6.f;
constexpr float kSearchH = 44.f;
constexpr float kBtnH = 36.f;
constexpr float kBannerH = 44.f;

enum class ShellControl {
    None = 0,
    Dismiss,    // 标题栏「收起」：隐藏面板，不退出
    NewBox,     // 侧栏底部
    AddFiles,   // 内容区主按钮
    Clean,      // 失效提示上的「清理」
    Search,     // 筛选框
    Heading,    // 当前盒子的名字（点击改名）
};

struct ShellLayout {
    D2D1_RECT_F title{};
    D2D1_RECT_F dismiss{};
    D2D1_RECT_F sidebar{};
    D2D1_RECT_F side_label{};
    D2D1_RECT_F rows{};      // 盒子列表的可视区域
    D2D1_RECT_F new_box{};
    D2D1_RECT_F heading{};
    D2D1_RECT_F add{};
    D2D1_RECT_F search{};
    D2D1_RECT_F banner{};    // show_banner 为 false 时是空矩形
    D2D1_RECT_F clean{};
    D2D1_RECT_F grid_area{};  // 卡片区（含底部留白之前的内容）
    float row_h = kSideRowH;
    float row_gap = kSideRowGap;
    int rows_visible = 1;
    bool show_banner = false;
};

// show_banner：当前盒子有失效项时，搜索框下面多一行提示。
ShellLayout shell_measure(float client_w, float client_h, bool show_banner);

D2D1_RECT_F shell_row_rect(const ShellLayout& sh, int index, int scroll);
int shell_row_hittest(const ShellLayout& sh, int count, int scroll, D2D1_POINT_2F pt);
int shell_clamp_scroll(const ShellLayout& sh, int count, int scroll);

ShellControl shell_control_at(const ShellLayout& sh, D2D1_POINT_2F pt);
bool shell_contains(const D2D1_RECT_F& r, D2D1_POINT_2F pt);

// 内容区网格：左缘让出侧栏，底部留白给一行提示。
GridLayout shell_grid(const ShellLayout& sh, float client_w, float client_h);

}  // namespace sg
