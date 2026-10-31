#include "views/shell.h"

#include <algorithm>

namespace sg {

namespace {

D2D1_RECT_F rect(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

}  // namespace

bool shell_contains(const D2D1_RECT_F& r, D2D1_POINT_2F pt) {
    if (r.right <= r.left || r.bottom <= r.top) return false;
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

ShellLayout shell_measure(float client_w, float client_h, bool show_banner) {
    ShellLayout sh;
    sh.show_banner = show_banner;
    const float w = std::max(client_w, kSidebarW + 160.f);
    const float h = std::max(client_h, kTitleH + 160.f);

    sh.title = rect(0.f, 0.f, w, kTitleH);
    const float btn_w = 72.f;
    const float btn_h = 32.f;
    sh.dismiss = rect(w - kPad - btn_w, (kTitleH - btn_h) / 2.f, w - kPad,
                      (kTitleH - btn_h) / 2.f + btn_h);

    sh.sidebar = rect(0.f, kTitleH, kSidebarW, h);
    sh.side_label = rect(kSidePad, kTitleH + 18.f, kSidebarW - kSidePad, kTitleH + 36.f);
    sh.new_box = rect(kSidePad, h - 16.f - 40.f, kSidebarW - kSidePad, h - 16.f);
    sh.rows = rect(kSidePad, kTitleH + 48.f, kSidebarW - kSidePad, sh.new_box.top - 12.f);

    const float stride = sh.row_h + sh.row_gap;
    const float rows_h = sh.rows.bottom - sh.rows.top;
    sh.rows_visible = static_cast<int>((rows_h + sh.row_gap) / stride);
    if (sh.rows_visible < 1) sh.rows_visible = 1;

    const float content_l = kSidebarW + kPad;
    const float content_r = w - kPad;
    const float add_w = 116.f;
    sh.add = rect(content_r - add_w, kTitleH + 22.f, content_r, kTitleH + 22.f + kBtnH);
    sh.heading = rect(content_l, kTitleH + 18.f, sh.add.left - 16.f, kTitleH + 22.f + kBtnH);
    sh.search = rect(content_l, sh.add.bottom + 16.f, content_r, sh.add.bottom + 16.f + kSearchH);

    float grid_top = sh.search.bottom + 20.f;
    if (show_banner) {
        sh.banner = rect(content_l, sh.search.bottom + 14.f, content_r,
                         sh.search.bottom + 14.f + kBannerH);
        sh.clean = rect(sh.banner.right - 76.f, sh.banner.top + 8.f, sh.banner.right - 10.f,
                        sh.banner.bottom - 8.f);
        grid_top = sh.banner.bottom + 16.f;
    }
    sh.grid_area = rect(content_l, grid_top, content_r, h - kPad);
    return sh;
}

D2D1_RECT_F shell_row_rect(const ShellLayout& sh, int index, int scroll) {
    const float y = sh.rows.top + (index - scroll) * (sh.row_h + sh.row_gap);
    return rect(sh.rows.left, y, sh.rows.right, y + sh.row_h);
}

int shell_row_hittest(const ShellLayout& sh, int count, int scroll, D2D1_POINT_2F pt) {
    if (count <= 0) return -1;
    if (pt.x < sh.rows.left || pt.x > sh.rows.right) return -1;
    if (pt.y < sh.rows.top || pt.y >= sh.rows.bottom) return -1;
    const float stride = sh.row_h + sh.row_gap;
    const float gy = pt.y - sh.rows.top;
    const int row = static_cast<int>(gy / stride);
    if (gy - row * stride > sh.row_h) return -1;  // 落在行间距里
    if (row < 0 || row >= sh.rows_visible) return -1;
    const int idx = row + scroll;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

int shell_clamp_scroll(const ShellLayout& sh, int count, int scroll) {
    return std::clamp(scroll, 0, std::max(0, count - sh.rows_visible));
}

ShellControl shell_control_at(const ShellLayout& sh, D2D1_POINT_2F pt) {
    if (shell_contains(sh.dismiss, pt)) return ShellControl::Dismiss;
    if (shell_contains(sh.add, pt)) return ShellControl::AddFiles;
    if (shell_contains(sh.clean, pt)) return ShellControl::Clean;
    if (shell_contains(sh.new_box, pt)) return ShellControl::NewBox;
    if (shell_contains(sh.search, pt)) return ShellControl::Search;
    if (shell_contains(sh.heading, pt)) return ShellControl::Heading;
    return ShellControl::None;
}

GridLayout shell_grid(const ShellLayout& sh, float client_w, float client_h) {
    const float right_pad = std::max(0.f, client_w - sh.grid_area.right);
    return grid_measure(client_w, client_h, sh.grid_area.top, sh.grid_area.left, right_pad);
}

}  // namespace sg
