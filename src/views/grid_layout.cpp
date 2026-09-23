#include "views/grid.h"

#include <algorithm>

// 纯布局/命中/导航：不碰 Renderer，也不碰图标与 Shell。
// 单独成文件的原因：这样它可以在没有窗口与 D2D 设备的情况下被控制台测试覆盖
// （tests/test_layout.cpp）。布局与命中必须共用同一份算法，否则会出现
// “点得到的格子画不出来 / 画出来的格子点不到”。

namespace sg {

GridLayout grid_measure(float client_w, float client_h, float top) {
    GridLayout gl;
    gl.top = top;
    const float avail_w = client_w - kPad * 2.f;
    const float avail_h = client_h - top;
    gl.cols = static_cast<int>((avail_w + kGap) / (kCell + kGap));
    if (gl.cols < 1) gl.cols = 1;
    gl.rows_visible = static_cast<int>((avail_h + kGap) / (kCell + kGap));
    if (gl.rows_visible < 1) gl.rows_visible = 1;
    gl.bottom = top + gl.rows_visible * (kCell + kGap);
    return gl;
}

D2D1_RECT_F grid_cell_rect(const GridLayout& gl, int index, int scroll) {
    const int col = index % gl.cols;
    const int row = index / gl.cols - scroll;
    const float x = kPad + col * (kCell + kGap);
    const float y = gl.top + row * (kCell + kGap);
    return D2D1::RectF(x, y, x + kCell, y + kCell);
}

int grid_hittest(const GridLayout& gl, int count, int scroll, D2D1_POINT_2F pt) {
    if (pt.y < gl.top) return -1;
    const float gx = pt.x - kPad;
    const float gy = pt.y - gl.top;
    if (gx < 0 || gy < 0) return -1;
    const int col = static_cast<int>(gx / (kCell + kGap));
    const int row = static_cast<int>(gy / (kCell + kGap));
    // 落在格子间隙里也算没命中，避免“点空白就启动了程序”
    if (gx - col * (kCell + kGap) > kCell) return -1;
    if (gy - row * (kCell + kGap) > kCell) return -1;
    if (col >= gl.cols) return -1;
    // 网格区下方那条不足一行的空隙不属于任何格子：不拦住它就会出现
    // “点到看不见的条目并把它启动”（下标算得出来，但那一行根本没画）
    if (row >= gl.rows_visible) return -1;
    const int idx = (row + scroll) * gl.cols + col;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

int grid_clamp_scroll(const GridLayout& gl, int count, int scroll) {
    const int total_rows = (count + gl.cols - 1) / gl.cols;
    return std::clamp(scroll, 0, std::max(0, total_rows - gl.rows_visible));
}

bool grid_keydown(const GridLayout& gl, int count, int& sel, int& scroll, UINT vk) {
    const int cols = gl.cols;
    const int rows = gl.rows_visible;

    switch (vk) {
        case VK_DOWN:
            sel = (sel < 0) ? (count > 0 ? 0 : -1) : std::min(sel + cols, count - 1);
            break;
        case VK_UP:
            if (sel >= 0 && sel < cols) {
                sel = -1;  // 回到搜索框/网格外
            } else {
                sel = std::max(0, sel - cols);
            }
            break;
        case VK_LEFT:
            sel = std::max(0, sel - 1);
            break;
        case VK_RIGHT:
            sel = std::min(count - 1, sel + 1);
            break;
        case VK_PRIOR:
            scroll = std::max(0, scroll - rows);
            break;
        case VK_NEXT:
            scroll += rows;
            break;
        default:
            return false;
    }

    // 选中项必须在可见范围内
    if (sel >= 0) {
        scroll = std::clamp(scroll, std::max(0, sel / cols - rows + 1), sel / cols);
    }
    return true;
}

}  // namespace sg
