#pragma once

#include <vector>

#include "model/todo_kind.h"

namespace sg {

// 待办列表的布局常量（逻辑 DIP）。文字与图片的行高不同 —— 这是待办与网格视图最大的区别，
// 所以滚动/命中不能再用“等行高”的假设。
constexpr float kTodoInputH = 34.f;     // 底部常驻输入框
constexpr float kTodoRowTextH = 28.f;   // 文字/链接行
constexpr float kTodoRowImageH = 96.f;  // 图片行（缩略图 + 文件名）
constexpr float kTodoThumbW = 160.f;    // 缩略图框
constexpr float kTodoThumbH = 88.f;

float todo_row_height(TodoKind kind);

// offsets[i] = 第 i 行的上边（相对列表顶部），offsets[n] = 列表总高 → size = n + 1
std::vector<float> todo_row_offsets(const std::vector<TodoKind>& kinds);

// y（相对列表顶部）落在哪一行；越界返回 -1。正好落在边界上算下一行。
int todo_row_at(const std::vector<float>& offsets, float y);

// 让 sel 这一行可见，返回夹紧后的 scroll（像素）。sel < 0 时原样返回。
float todo_scroll_for(const std::vector<float>& offsets, float viewport_h, float scroll, int sel);

}  // namespace sg
