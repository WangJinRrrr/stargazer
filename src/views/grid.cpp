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

void grid_render(Renderer& r, const GridLayout& gl, const std::vector<GridItem>& items,
                 int scroll) {
    IDWriteTextFormat* name_fmt =
        r.format(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (size_t i = 0; i < items.size(); ++i) {
        const int row = static_cast<int>(i) / gl.cols;
        if (row < scroll || row >= scroll + gl.rows_visible) continue;  // 只画可见行

        const D2D1_RECT_F rc = grid_cell_rect(gl, static_cast<int>(i), scroll);
        const bool selected = items[i].selected;

        // Win11 文件格：悬停 = 极淡填充；选中 = 强调色低透明底 + 强调色描边（文字仍用 primary）
        if (selected) {
            r.fill_round_rect(rc, kRadiusMd, r.theme.sel_fill);
            r.stroke_round_rect(rc, kRadiusMd, r.theme.sel_stroke, 1.f);
        } else if (items[i].hovered) {
            r.fill_round_rect(rc, kRadiusMd, r.theme.hover);
        }

        const float ix = rc.left + (kCell - 48.f) / 2.f;
        const float iy = rc.top + 8.f;
        const D2D1_RECT_F irect = D2D1::RectF(ix, iy, ix + 48.f, iy + 48.f);
        if (ID2D1Bitmap* bmp = icons_get(r, items[i].icon_src, items[i].is_dir)) {
            r.rt->DrawBitmap(bmp, irect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // 图标未就绪：扩展名色块 + 首字母（骨架先出，图标后到）
            r.fill_round_rect(irect, 6.f, ext_color(items[i].icon_src));
            const std::wstring initial =
                items[i].label.empty() ? std::wstring(L"?") : items[i].label.substr(0, 1);
            r.text(irect, initial,
                   r.format(20.f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                   D2D1::ColorF(1.f, 1.f, 1.f, 0.9f));
        }

        const D2D1_RECT_F label =
            D2D1::RectF(rc.left + 4.f, iy + 48.f + 3.f, rc.right - 4.f, rc.bottom - 4.f);
        if (items[i].missing) {
            // 失效项：名字灰显 + 中段一条删除线（先画正常底与图标，只动名字）
            r.text(label, items[i].label, name_fmt, r.theme.text_faint);
            const float mid = (label.top + label.bottom) / 2.f;
            r.fill_rect(D2D1::RectF(label.left, mid - 0.5f, label.right, mid + 0.5f),
                        r.theme.text_faint);
        } else {
            r.text(label, items[i].label, name_fmt,
                   selected ? r.theme.text : r.theme.text_dim);
        }
    }
}

D2D1_RECT_F view_tabs_rect(D2D1_SIZE_F client) {
    return D2D1::RectF(kPad, 0.f, client.width - kPad, kViewTabsH);
}

// 标签宽度 = 文字宽 + 两侧内边距 + 标签间距。只和 DPI/字体有关，
// 而 WM_NCHITTEST 每动一下鼠标都会问一次，所以量一次就缓存住。
D2D1_RECT_F view_tab_rect(Renderer& r, int i) {
    static float cached_dpi = 0.f;
    static D2D1_RECT_F cached[kViewCount] = {};
    if (cached_dpi != r.dpi) {
        IDWriteTextFormat* fmt = r.format(14.f);
        float x = kPad;
        for (int k = 0; k < kViewCount; ++k) {
            const float tw = r.measure_text(view_name(k), fmt).width;
            const float w = std::max(56.f, tw + 28.f);
            cached[k] = D2D1::RectF(x, 0.f, x + w, kViewTabsH);
            x += w + 4.f;
        }
        cached_dpi = r.dpi;
    }
    if (i < 0 || i >= kViewCount) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
    return cached[i];
}

int view_tab_hittest(Renderer& r, D2D1_POINT_2F pt) {
    if (pt.y < 0.f || pt.y > kViewTabsH) return -1;
    for (int i = 0; i < kViewCount; ++i) {
        const D2D1_RECT_F tab = view_tab_rect(r, i);
        if (pt.x >= tab.left && pt.x <= tab.right) return i;
    }
    return -1;
}

const wchar_t* view_name(int v) {
    switch (v) {
        case 0:
            return L"收纳盒";
        case 1:
            return L"待办";
        case 2:
            return L"浏览";
        default:
            return L"";
    }
}

void view_tabs_render(Renderer& r, D2D1_SIZE_F client, int active, int hover) {
    IDWriteTextFormat* fmt =
        r.format(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < kViewCount; ++i) {
        const D2D1_RECT_F tab = view_tab_rect(r, i);
        if (tab.right <= tab.left) continue;
        const std::wstring name = view_name(i);
        if (i == active) {
            // 选中：primary 文字 + 底部 3px 圆头强调条（Win11 NavigationView 的做法）
            r.text(tab, name, fmt, r.theme.text);
            const float tw = r.measure_text(name, fmt).width;
            const float cx = (tab.left + tab.right) / 2.f;
            const float half = std::min((tab.right - tab.left) / 2.f - 8.f, tw / 2.f + 6.f);
            r.fill_round_rect(
                D2D1::RectF(cx - half, kViewTabsH - 5.f, cx + half, kViewTabsH - 2.f), 1.5f,
                r.theme.accent);
        } else {
            if (i == hover) {
                r.fill_round_rect(D2D1::RectF(tab.left, 6.f, tab.right, kViewTabsH - 6.f),
                                  kRadiusSm, r.theme.hover);
            }
            r.text(tab, name, fmt, r.theme.text_dim);
        }
    }
    // 标签行下沿的分隔线（标签条之外的整行都不画，它是拖动区）
    r.fill_rect(D2D1::RectF(0.f, kViewTabsH - 1.f, client.width, kViewTabsH), r.theme.divider);
}

}  // namespace sg
