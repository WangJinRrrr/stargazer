// 网格与窗口骨架的布局/命中测试：纯逻辑，不需要窗口、D2D 设备或图标线程。
#include <cstdio>

#include "views/grid.h"
#include "views/shell.h"

using namespace sg;

static int g_failed = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                \
        }                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto _a = (a);                                                         \
        auto _b = (b);                                                         \
        if (!(_a == _b)) {                                                     \
            std::printf("FAIL %s:%d  %s != %s\n", __FILE__, __LINE__, #a, #b);  \
            ++g_failed;                                                        \
        }                                                                      \
    } while (0)

// 命中不能超出“画得出来”的行。网格区下方那条不足一行的空隙里没有任何格子。
static void test_hittest_stops_at_last_visible_row() {
    const float top = 180.f;
    const GridLayout gl = grid_measure(1080.f, 720.f, top);
    const int cols = gl.cols;
    const int rows = gl.rows_visible;
    const int count = cols * (rows + 2);

    const float last_row_center_y = top + (rows - 1) * (kCell + kGap) + kCell / 2.f;
    CHECK_EQ(grid_hittest(gl, count, 0, D2D1::Point2F(gl.left + kCell / 2.f, last_row_center_y)),
             (rows - 1) * cols);

    const float below = top + rows * (kCell + kGap) + kCell / 2.f;
    CHECK(below < 720.f);
    CHECK_EQ(grid_hittest(gl, count, 0, D2D1::Point2F(gl.left + kCell / 2.f, below)), -1);
    CHECK_EQ(grid_hittest(gl, count, 0, D2D1::Point2F(gl.left + kCell / 2.f, 719.f)), -1);

    CHECK_EQ(grid_hittest(gl, count, 0,
                          D2D1::Point2F(gl.left + kCell + kGap / 2.f, last_row_center_y)),
             -1);

    const int max_scroll = grid_clamp_scroll(gl, count, 999);
    CHECK_EQ(grid_hittest(gl, count, max_scroll, D2D1::Point2F(gl.left + kCell / 2.f, below)), -1);
}

static void test_hittest_matches_cells() {
    const GridLayout gl = grid_measure(1080.f, 720.f, 180.f);
    const int visible = gl.cols * gl.rows_visible;
    CHECK(visible > 0);
    for (int i = 0; i < visible; ++i) {
        const D2D1_RECT_F rc = grid_cell_rect(gl, i, 0);
        const D2D1_POINT_2F c =
            D2D1::Point2F((rc.left + rc.right) / 2.f, (rc.top + rc.bottom) / 2.f);
        CHECK_EQ(grid_hittest(gl, visible + 5, 0, c), i);
    }
}

// 网格可以从侧栏右边开始，命中必须跟着 left 走，不能还按窗口左缘算。
static void test_grid_respects_left_inset() {
    const float left = kSidebarW + kPad;
    const GridLayout gl = grid_measure(1080.f, 720.f, 200.f, left, kPad);
    CHECK(gl.left == left);
    CHECK(gl.cols >= 1);
    const D2D1_RECT_F rc = grid_cell_rect(gl, 0, 0);
    CHECK(rc.left == left);
    // 落在侧栏里（格子左边）不能命中第 0 格
    CHECK_EQ(grid_hittest(gl, 10, 0, D2D1::Point2F(left - 8.f, rc.top + 10.f)), -1);
    CHECK_EQ(grid_hittest(gl, 10, 0, D2D1::Point2F((rc.left + rc.right) / 2.f,
                                                  (rc.top + rc.bottom) / 2.f)),
             0);
}

static void test_keydown_navigation() {
    const GridLayout gl = grid_measure(1080.f, 720.f, 180.f);
    const int cols = gl.cols;
    int sel = -1;
    int scroll = 0;
    const int count = cols * 3;

    CHECK(grid_keydown(gl, count, sel, scroll, VK_DOWN));
    CHECK_EQ(sel, 0);
    CHECK(grid_keydown(gl, count, sel, scroll, VK_RIGHT));
    CHECK_EQ(sel, 1);
    CHECK(grid_keydown(gl, count, sel, scroll, VK_DOWN));
    CHECK_EQ(sel, 1 + cols);
    CHECK(grid_keydown(gl, count, sel, scroll, VK_UP));
    CHECK_EQ(sel, 1);
    CHECK(grid_keydown(gl, count, sel, scroll, VK_UP));
    CHECK_EQ(sel, -1);
    CHECK(!grid_keydown(gl, count, sel, scroll, VK_F5));
}

static void test_shell_chrome_hit_targets() {
    const ShellLayout sh = shell_measure(1080.f, 720.f, false);
    CHECK(sh.title.bottom == kTitleH);
    CHECK(sh.sidebar.right == kSidebarW);
    CHECK(sh.dismiss.right < 1080.f);
    CHECK(sh.add.left > kSidebarW);
    CHECK(sh.search.top > sh.add.bottom);
    CHECK(sh.grid_area.top > sh.search.bottom);
    CHECK(sh.banner.bottom <= sh.banner.top);  // 没有失效提示时横幅是空的
    CHECK(shell_control_at(sh, D2D1::Point2F((sh.dismiss.left + sh.dismiss.right) / 2.f,
                                             (sh.dismiss.top + sh.dismiss.bottom) / 2.f)) ==
          ShellControl::Dismiss);
    CHECK(shell_control_at(sh, D2D1::Point2F((sh.add.left + sh.add.right) / 2.f,
                                             (sh.add.top + sh.add.bottom) / 2.f)) ==
          ShellControl::AddFiles);
    CHECK(shell_control_at(sh, D2D1::Point2F((sh.new_box.left + sh.new_box.right) / 2.f,
                                             (sh.new_box.top + sh.new_box.bottom) / 2.f)) ==
          ShellControl::NewBox);
    CHECK(shell_control_at(sh, D2D1::Point2F((sh.search.left + sh.search.right) / 2.f,
                                             (sh.search.top + sh.search.bottom) / 2.f)) ==
          ShellControl::Search);
    // 标题栏空白处不是按钮：用来拖动窗口
    CHECK(shell_control_at(sh, D2D1::Point2F(kSidebarW + 40.f, kTitleH / 2.f)) == ShellControl::None);

    const ShellLayout banner = shell_measure(1080.f, 720.f, true);
    CHECK(banner.grid_area.top > sh.grid_area.top);  // 提示行把网格往下推
    CHECK(shell_control_at(banner, D2D1::Point2F((banner.clean.left + banner.clean.right) / 2.f,
                                                 (banner.clean.top + banner.clean.bottom) / 2.f)) ==
          ShellControl::Clean);
    // 没开提示时，同一点不该误伤成「清理」
    CHECK(shell_control_at(sh, D2D1::Point2F((banner.clean.left + banner.clean.right) / 2.f,
                                             (banner.clean.top + banner.clean.bottom) / 2.f)) !=
          ShellControl::Clean);
}

static void test_sidebar_width_clamps_and_moves_content() {
    const ShellLayout wide = shell_measure(1400.f, 800.f, false, 420.f);
    CHECK(wide.sidebar.right == 420.f);
    CHECK(wide.grid_area.left == 420.f + kPad);
    CHECK(shell_splitter_hit(wide, D2D1::Point2F(420.f, 200.f)));
    CHECK(!shell_splitter_hit(wide, D2D1::Point2F(420.f, 10.f)));  // 标题栏不是分隔条
    CHECK(!shell_splitter_hit(wide, D2D1::Point2F(390.f, 200.f)));

    // 窄窗口里侧栏让出一列卡片，不会按请求的 500 画出去
    const float content_min = kPad + kCell + kPad;
    const ShellLayout cramped = shell_measure(700.f, 500.f, false, 500.f);
    CHECK(cramped.sidebar.right <= 700.f - content_min + 0.5f);
    CHECK(cramped.sidebar.right >= kSidebarMin);
    CHECK(cramped.grid_area.left == cramped.sidebar.right + kPad);

    // 0 = 默认宽度
    const ShellLayout def = shell_measure(1080.f, 720.f, false, 0.f);
    CHECK(def.sidebar.right == kSidebarW);

    // 双击复位用的默认值必须落在允许范围内
    CHECK(shell_side_width(kSidebarW, 1080.f) == kSidebarW);
    CHECK(shell_side_width(40.f, 1080.f) == kSidebarMin);
    CHECK(shell_side_width(900.f, 1400.f) == kSidebarMax);
}

static void test_sidebar_rows_skip_gaps() {
    const ShellLayout sh = shell_measure(1080.f, 720.f, false);
    CHECK(sh.rows_visible >= 1);
    const int count = sh.rows_visible + 3;
    const D2D1_RECT_F row0 = shell_row_rect(sh, 0, 0);
    CHECK_EQ(shell_row_hittest(sh, count, 0,
                               D2D1::Point2F((row0.left + row0.right) / 2.f,
                                             (row0.top + row0.bottom) / 2.f)),
             0);
    // 行与行之间的缝不算命中
    const float gap_y = row0.bottom + sh.row_gap / 2.f;
    CHECK_EQ(shell_row_hittest(sh, count, 0, D2D1::Point2F((row0.left + row0.right) / 2.f, gap_y)),
             -1);
    // 滚出可视区的行不能靠“算得出下标”被点到
    const int max_scroll = shell_clamp_scroll(sh, count, 999);
    CHECK(max_scroll > 0);
    const float below = sh.rows.bottom + 4.f;
    CHECK_EQ(shell_row_hittest(sh, count, max_scroll,
                               D2D1::Point2F((row0.left + row0.right) / 2.f, below)),
             -1);
    // 滚到顶之后，第 max_scroll 行出现在列表顶端
    const D2D1_RECT_F scrolled = shell_row_rect(sh, max_scroll, max_scroll);
    CHECK_EQ(shell_row_hittest(sh, count, max_scroll,
                               D2D1::Point2F((scrolled.left + scrolled.right) / 2.f,
                                             (scrolled.top + scrolled.bottom) / 2.f)),
             max_scroll);
}

int main() {
    test_hittest_stops_at_last_visible_row();
    test_hittest_matches_cells();
    test_grid_respects_left_inset();
    test_keydown_navigation();
    test_shell_chrome_hit_targets();
    test_sidebar_width_clamps_and_moves_content();
    test_sidebar_rows_skip_gaps();
    if (g_failed == 0) {
        std::printf("OK: test_layout 全部通过\n");
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    return 1;
}
