#include "views/todo_layout.h"

#include <algorithm>

namespace sg {

float todo_row_height(TodoKind kind) {
    return kind == TodoKind::Image ? kTodoRowImageH : kTodoRowTextH;
}

std::vector<float> todo_row_offsets(const std::vector<TodoKind>& kinds) {
    std::vector<float> offsets;
    offsets.reserve(kinds.size() + 1);
    float y = 0.f;
    offsets.push_back(y);
    for (const TodoKind k : kinds) {
        y += todo_row_height(k);
        offsets.push_back(y);
    }
    return offsets;
}

int todo_row_at(const std::vector<float>& offsets, float y) {
    if (y < 0.f || offsets.size() < 2) return -1;
    const int n = static_cast<int>(offsets.size()) - 1;
    // 第一个“大于 y”的偏移量的下标减一 = 包含 y 的那一行
    const auto it = std::upper_bound(offsets.begin(), offsets.end(), y);
    const int idx = static_cast<int>(it - offsets.begin()) - 1;
    if (idx < 0 || idx >= n) return -1;
    return idx;
}

float todo_scroll_for(const std::vector<float>& offsets, float viewport_h, float scroll, int sel) {
    const int n = static_cast<int>(offsets.size()) - 1;
    if (sel >= 0 && sel < n) {
        const float top = offsets[static_cast<size_t>(sel)];
        const float bottom = offsets[static_cast<size_t>(sel) + 1];
        if (top < scroll) {
            scroll = top;
        } else if (bottom > scroll + viewport_h) {
            scroll = bottom - viewport_h;
        }
    }
    const float total = offsets.empty() ? 0.f : offsets.back();
    const float max_scroll = std::max(0.f, total - viewport_h);
    return std::clamp(scroll, 0.f, max_scroll);
}

}  // namespace sg
