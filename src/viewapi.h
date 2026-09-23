#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "edit.h"
#include "model/store.h"
#include "render.h"

namespace sg {

enum class View { Launcher, Box, Todo, Explorer };

// 启动板视图状态（只属于启动板，不污染全局）
struct LauncherState {
    int group = 0;
    int sel = -1;  // filtered 中的下标；-1 = 焦点在搜索框
    int hover = -1;
    int scroll = 0;          // 起始行
    int drag_over_tab = -1;  // 内部拖拽时高亮的目标分组标签
    std::wstring query;
    std::vector<int> filtered;  // 当前分组里通过过滤的条目下标
    // 搜索框、重命名框、新建输入框共用同一个 EDIT 实例（同一时刻只会存在一个）
    InlineEdit search;
};

// 文件收纳盒视图状态（只属于盒子，不污染全局）
struct BoxState {
    int box = 0;            // 当前盒子下标
    int sel = -1;           // 网格选中下标；-1 = 无
    int hover = -1;
    int scroll = 0;         // 起始行
    int drag_over_tab = -1;  // 内部拖拽时高亮的目标盒子标签
};

struct AppState {
    std::vector<LaunchGroup> groups;
    std::vector<Box> boxes;
    std::vector<TodoItem> todos;
    Config config;
    int bad_lines = 0;
    View view = View::Launcher;
    LauncherState launcher;
    BoxState box_view;
    bool data_dirty = false;
    // ui.txt 里想要的窗口尺寸（逻辑像素）。0 = 用默认值。
    // 存起来等面板真正创建时再应用：app_load 跑在面板存在之前，那里改尺寸是死代码。
    int ui_w = 0;
    int ui_h = 0;  // 变更后由 app 层落盘
};

}  // namespace sg
