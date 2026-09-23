#include "views/grid.h"

#include <algorithm>

#include "icons.h"
#include "model/paths.h"

namespace sg {

D2D1_COLOR_F ext_color(const std::wstring& path) {
    // 固定 8 色调色板：扩展名哈希取模，同一类型总是同色且重启不变
    static const D2D1_COLOR_F palette[8] = {
        D2D1::ColorF(0.35f, 0.45f, 0.62f), D2D1::ColorF(0.32f, 0.53f, 0.48f),
        D2D1::ColorF(0.58f, 0.44f, 0.35f), D2D1::ColorF(0.50f, 0.38f, 0.55f),
        D2D1::ColorF(0.40f, 0.48f, 0.36f), D2D1::ColorF(0.58f, 0.38f, 0.42f),
        D2D1::ColorF(0.34f, 0.42f, 0.56f), D2D1::ColorF(0.45f, 0.45f, 0.45f),
    };
    const std::wstring ext = extension_of(path);
    if (ext.empty()) return palette[7];
    unsigned h = 2166136261u;  // FNV-1a
    for (wchar_t c : ext) {
        h ^= static_cast<unsigned>(c);
        h *= 16777619u;
    }
    return palette[h % 8];
}

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

void grid_render(Renderer& r, const GridLayout& gl, const std::vector<GridItem>& items,
                 int scroll, D2D1_COLOR_F accent, D2D1_COLOR_F hover_color,
                 D2D1_COLOR_F text_color, D2D1_COLOR_F missing_color) {
    IDWriteTextFormat* name_fmt =
        r.format(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (size_t i = 0; i < items.size(); ++i) {
        const int row = static_cast<int>(i) / gl.cols;
        if (row < scroll || row >= scroll + gl.rows_visible) continue;  // 只画可见行

        const D2D1_RECT_F rc = grid_cell_rect(gl, static_cast<int>(i), scroll);
        const bool selected = items[i].selected;

        if (selected) {
            r.fill_round_rect(rc, 8.f, accent);
        } else if (items[i].hovered) {
            r.fill_round_rect(rc, 8.f, hover_color);
        }

        const float ix = rc.left + (kCell - 48.f) / 2.f;
        const float iy = rc.top + 8.f;
        const D2D1_RECT_F irect = D2D1::RectF(ix, iy, ix + 48.f, iy + 48.f);
        if (ID2D1Bitmap* bmp = icons_get(r, items[i].icon_src, items[i].is_dir)) {
            r.rt->DrawBitmap(bmp, irect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // 图标未就绪：扩展名色块 + 首字母（骨架先出，图标后到）
            r.fill_round_rect(irect, 8.f, ext_color(items[i].icon_src));
            const std::wstring initial =
                items[i].label.empty() ? std::wstring(L"?") : items[i].label.substr(0, 1);
            r.text(irect, initial,
                   r.format(20.f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                   D2D1::ColorF(1.f, 1.f, 1.f));
        }

        const D2D1_RECT_F label =
            D2D1::RectF(rc.left + 4.f, iy + 48.f + 4.f, rc.right - 4.f, rc.bottom - 4.f);
        if (items[i].missing) {
            // 失效项：名字灰显 + 中段一条删除线（先画正常底与图标，只动名字）
            r.text(label, items[i].label, name_fmt, missing_color);
            const float mid = (label.top + label.bottom) / 2.f;
            r.fill_rect(D2D1::RectF(label.left, mid - 0.5f, label.right, mid + 0.5f),
                        missing_color);
        } else {
            const D2D1_COLOR_F label_color =
                selected ? D2D1::ColorF(1.f, 1.f, 1.f) : text_color;
            r.text(label, items[i].label, name_fmt, label_color);
        }
    }
}

}  // namespace sg
