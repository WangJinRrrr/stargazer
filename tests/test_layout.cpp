// 网格布局/命中/导航的测试：纯逻辑，不需要窗口、D2D 设备或图标线程。
// 与 test_model 同一套路（assert + 计数），不引入测试框架。
#include <cstdio>

#include "views/grid.h"

using namespace sg;  // 测试里直接写 kPad/kTabsH/grid_hittest

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

// 回归（代码评审 Important 1）：命中不能超出“画得出来”的行。
// 网格区下方那条不足一行的空隙里没有任何格子，点它必须返回 -1；
// 否则 Enter / 双击会启动一个用户根本看不见的条目。
static void test_hittest_stops_at_last_visible_row() {
    // 默认 960x620 逻辑像素、启动板（视图标签行 + 分组标签行 + 搜索框）下的网格
    const float top = kPad + kTabsH + kSearchH + kPad + 28.f;  // 28 = kViewTabsH
    const sg::GridLayout gl = sg::grid_measure(960.f, 620.f, top);
    const int cols = gl.cols;
    const int rows = gl.rows_visible;
    const int count = cols * (rows + 2);  // 故意有足够多的条目让“看不见的那一行”也有内容

    // 最后一行可见格的格子中心：必须命中
    const float last_row_center_y = top + (rows - 1) * (kCell + kGap) + kCell / 2.f;
    CHECK_EQ(sg::grid_hittest(gl, count, 0, D2D1::Point2F(kPad + kCell / 2.f, last_row_center_y)),
             0 + (rows - 1) * cols);

    // 再往下（可见行之外）：必须不命中，哪怕那里“算得出”一个条目下标
    const float below = top + rows * (kCell + kGap) + kCell / 2.f;
    CHECK(below < 620.f);  // 确保这个点在窗口内：否则测的是“窗口外”而不是这条空隙
    CHECK_EQ(sg::grid_hittest(gl, count, 0, D2D1::Point2F(kPad + kCell / 2.f, below)), -1);

    // 底部一点点也算不命中
    CHECK_EQ(sg::grid_hittest(gl, count, 0, D2D1::Point2F(kPad + kCell / 2.f, 619.f)), -1);

    // 间隙里不命中（阶段 1 定的语义：点空白不启动任何东西）
    CHECK_EQ(sg::grid_hittest(gl, count, 0, D2D1::Point2F(kPad + kCell + kGap / 2.f,
                                                         last_row_center_y)),
             -1);

    // 滚动后同样成立：滚动到只显示最后一屏时，屏幕外的行不该被命中
    const int max_scroll = sg::grid_clamp_scroll(gl, count, 999);
    CHECK_EQ(sg::grid_hittest(gl, count, max_scroll,
                              D2D1::Point2F(kPad + kCell / 2.f, below)),
             -1);
}

// 命中与画出来的格子必须一一对应：每个可见格子的中心都必须命中它自己
static void test_hittest_matches_cells() {
    const float top = kPad + kTabsH + kSearchH + kPad + 28.f;
    const sg::GridLayout gl = sg::grid_measure(960.f, 620.f, top);
    const int visible = gl.cols * gl.rows_visible;
    CHECK(visible > 0);
    for (int i = 0; i < visible; ++i) {
        const D2D1_RECT_F rc = sg::grid_cell_rect(gl, i, 0);
        const D2D1_POINT_2F c = D2D1::Point2F((rc.left + rc.right) / 2.f, (rc.top + rc.bottom) / 2.f);
        CHECK_EQ(sg::grid_hittest(gl, visible + 5, 0, c), i);
    }
}

static void test_keydown_navigation() {
    const sg::GridLayout gl = sg::grid_measure(960.f, 620.f, 120.f);
    const int cols = gl.cols;
    int sel = -1;
    int scroll = 0;
    const int count = cols * 3;

    CHECK(sg::grid_keydown(gl, count, sel, scroll, VK_DOWN));
    CHECK_EQ(sel, 0);  // 从“不在网格”进到第一个
    CHECK(sg::grid_keydown(gl, count, sel, scroll, VK_RIGHT));
    CHECK_EQ(sel, 1);
    CHECK(sg::grid_keydown(gl, count, sel, scroll, VK_DOWN));
    CHECK_EQ(sel, 1 + cols);
    CHECK(sg::grid_keydown(gl, count, sel, scroll, VK_UP));
    CHECK_EQ(sel, 1);
    CHECK(sg::grid_keydown(gl, count, sel, scroll, VK_UP));
    CHECK_EQ(sel, -1);  // 首行再往上 = 离开网格（启动板要回到搜索框）
    CHECK(!sg::grid_keydown(gl, count, sel, scroll, VK_F5));  // 不认识的键不消费
}

int main() {
    test_hittest_stops_at_last_visible_row();
    test_hittest_matches_cells();
    test_keydown_navigation();
    if (g_failed == 0) {
        std::printf("OK: test_layout 全部通过\n");
        return 0;
    }
    std::printf("FAILED: %d 项检查未通过\n", g_failed);
    return 1;
}
