#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>
#include <vector>

#include "viewapi.h"

namespace sg {

struct App;

void box_render(App& app);
// 返回 true 表示按键已被消费（Esc 清掉筛选时也算消费，调用方就不要收起窗口）
bool box_keydown(App& app, UINT vk);
// 面板收到字符：把输入送进筛选框（筛选词看得见，不再做无提示的“输入即跳”）
void box_char(App& app, wchar_t ch);
void box_mouse_move(App& app, POINT phys);
void box_lbutton_down(App& app, POINT phys);
void box_lbutton_up(App& app, POINT phys);
void box_lbutton_dblclk(App& app, POINT phys);
void box_mouse_leave(App& app);
void box_mouse_wheel(App& app, int step, POINT screen);
void box_size(App& app);
void box_context_menu(App& app, POINT screen_pt, POINT client_pt);
// 筛选框 EN_CHANGE
void box_search_changed(App& app);
// 悬停淡入淡出。返回 true 表示动画还没结束，定时器要留着。
bool box_anim_tick(App& app);
void box_on_show(App& app);
void box_on_hide(App& app);
// 标题栏里、除了「收起」按钮以外的区域：用来拖动窗口
bool box_in_caption(D2D1_SIZE_F client, D2D1_POINT_2F pt);

void box_clamp(AppState& s, D2D1_SIZE_F client);
// 把路径收进当前盒子（拖入、粘贴、文件对话框共用）
void box_ingest(App& app, const std::vector<std::wstring>& paths);

}  // namespace sg
