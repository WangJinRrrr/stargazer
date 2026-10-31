#pragma once

#include <windows.h>

#include <climits>
#include <string>
#include <vector>

#include "edit.h"
#include "model/store.h"
#include "render.h"

namespace sg {

// 文件盒子的界面状态。程序现在只有这一个视图。
struct BoxState {
    int box = 0;             // 当前盒子下标
    int sel = -1;            // 选中条目在盒子里的真实下标；-1 = 无
    int hover = -1;          // 悬停条目（真实下标）
    int hover_from = -1;     // 上一个悬停，淡出用
    float hover_t = 1.f;     // 0→1：从 hover_from 过渡到 hover
    unsigned long long hover_ms = 0;
    int side_hover = -1;     // 侧栏里悬停的盒子
    int chrome_hover = 0;    // ShellControl：按钮/搜索框的悬停
    bool split_hot = false;  // 鼠标在侧栏分隔条上
    float side_w = 0.f;      // 侧栏宽（逻辑像素）。0 = 用默认宽度
    int side_scroll = 0;
    int scroll = 0;          // 网格起始行
    int drag_over_box = -1;  // 内部拖拽时高亮的目标盒子
    int renaming_item = -1;  // 正在改显示名的条目；-1 = 没有
    int renaming_box = -1;   // 正在改名的盒子
    std::wstring filter;     // 筛选词（空 = 不筛）。只影响看得见的格子
    InlineEdit edit;         // 改名
    InlineEdit search;       // 筛选框（常驻，失焦不关）
};

struct AppState {
    std::vector<Box> boxes;
    Config config;
    int bad_lines = 0;
    BoxState box_view;
    bool data_dirty = false;
    // ui.txt 里想要的窗口尺寸（逻辑像素）与位置（**物理像素**，INT_MIN = 没记过）。
    // 尺寸存起来等面板真正创建时再应用；位置在呼出时用，_MIN 时回退到“光标所在显示器居中”。
    // 位置只能存物理像素：DIP 没有绝对原点，跨不同缩放的显示器时只有物理坐标能用。
    int ui_w = 0;
    int ui_h = 0;
    int ui_x = INT_MIN;
    int ui_y = INT_MIN;
};

}  // namespace sg
