#include "views/grid.h"

#include <algorithm>

#include "icons.h"
#include "model/paths.h"

namespace sg {

// 布局/命中/导航在 views/grid_layout.cpp（纯逻辑，可被 tests/test_layout.cpp 覆盖）

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

D2D1_RECT_F grid_label_rect(const D2D1_RECT_F& cell) {
    return D2D1::RectF(cell.left + 10.f, cell.top + 18.f + 44.f + 8.f, cell.right - 10.f,
                       cell.bottom - 12.f);
}

void grid_render(Renderer& r, const GridLayout& gl, const std::vector<GridItem>& items,
                 int scroll) {
    IDWriteTextFormat* name_fmt =
        r.format(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (size_t i = 0; i < items.size(); ++i) {
        const int row = static_cast<int>(i) / gl.cols;
        if (row < scroll || row >= scroll + gl.rows_visible) continue;  // 只画可见行

        const D2D1_RECT_F rc = grid_cell_rect(gl, static_cast<int>(i), scroll);
        const bool selected = items[i].selected;
        const float hover = items[i].hover < 0.f ? 0.f : (items[i].hover > 1.f ? 1.f : items[i].hover);

        // 软阴影：悬停时略沉一点，平时几乎贴着纸面
        const float drop = 3.f + hover * 3.f;
        const D2D1_RECT_F shadow =
            D2D1::RectF(rc.left, rc.top + drop, rc.right, rc.bottom + drop + 2.f);
        D2D1_COLOR_F shade = r.theme.shadow;
        shade.a *= 0.45f + hover * 0.55f;
        r.fill_round_rect(shadow, kRadiusMd, shade);

        r.fill_round_rect(rc, kRadiusMd, r.theme.card);
        if (selected) {
            r.fill_round_rect(rc, kRadiusMd, r.theme.sel_fill);
            r.stroke_round_rect(rc, kRadiusMd, r.theme.sel_stroke, 1.5f);
        } else if (hover > 0.01f) {
            D2D1_COLOR_F wash = r.theme.hover;
            wash.a = hover;
            r.fill_round_rect(rc, kRadiusMd, wash);
            D2D1_COLOR_F edge = r.theme.border_strong;
            edge.a = hover;
            r.stroke_round_rect(rc, kRadiusMd, edge, 1.f);
        } else {
            r.stroke_round_rect(rc, kRadiusMd, r.theme.border, 1.f);
        }

        constexpr float kIcon = 44.f;
        const float ix = rc.left + (kCell - kIcon) / 2.f;
        const float iy = rc.top + 18.f;
        const D2D1_RECT_F irect = D2D1::RectF(ix, iy, ix + kIcon, iy + kIcon);
        const float icon_a = items[i].missing ? 0.38f : 1.f;
        if (ID2D1Bitmap* bmp = icons_get(r, items[i].icon_src, items[i].is_dir)) {
            r.rt->DrawBitmap(bmp, irect, icon_a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            D2D1_COLOR_F tile = ext_color(items[i].icon_src);
            tile.a = icon_a;
            r.fill_round_rect(irect, 12.f, tile);
            const std::wstring initial =
                items[i].label.empty() ? std::wstring(L"?") : items[i].label.substr(0, 1);
            r.text(irect, initial,
                   r.format(18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                   D2D1::ColorF(1.f, 1.f, 1.f, icon_a));
        }

        const D2D1_RECT_F label = grid_label_rect(rc);
        const D2D1_COLOR_F name_color = items[i].missing ? r.theme.text_faint : r.theme.text;
        r.text(label, items[i].label, name_fmt, name_color);
        if (items[i].missing) {
            const D2D1_SIZE_F tw = r.measure_text(items[i].label, name_fmt);
            const float max_w = label.right - label.left;
            const float w = tw.width > 0.f ? std::min(tw.width, max_w) : max_w * 0.6f;
            const float cx = (label.left + label.right) / 2.f;
            const float mid = label.top + 9.f;
            r.fill_rect(D2D1::RectF(cx - w / 2.f, mid - 0.5f, cx + w / 2.f, mid + 0.5f),
                        r.theme.text_faint);
        }
    }
}

}  // namespace sg
