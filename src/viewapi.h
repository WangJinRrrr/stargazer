#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "edit.h"
#include "model/store.h"
#include "render.h"

namespace sg {

// 视图集合。启动板已删除：它的位置由“浏览”（找文件）与“收纳盒”（归档）合起来取代。
enum class View { Box, Todo, Browse };

// 文件收纳盒视图状态（只属于盒子，不污染全局）
struct BoxState {
    int box = 0;            // 当前盒子下标
    int sel = -1;           // 网格选中下标；-1 = 无
    int hover = -1;
    int scroll = 0;         // 起始行
    int drag_over_tab = -1;  // 内部拖拽时高亮的目标盒子标签
    // 重命名输入框。原来借用启动板的搜索框（全程序只有一个 EDIT 实例），
    // 启动板删掉后盒子自己持有一个。
    InlineEdit edit;
};

struct AppState {
    std::vector<Box> boxes;
    std::vector<TodoItem> todos;
    Config config;
    int bad_lines = 0;
    View view = View::Box;  // 首次启动（ui.txt 还没有记录时）停在收纳盒
    BoxState box_view;
    bool data_dirty = false;
    // ui.txt 里想要的窗口尺寸（逻辑像素）。0 = 用默认值。
    // 存起来等面板真正创建时再应用：app_load 跑在面板存在之前，那里改尺寸是死代码。
    int ui_w = 0;
    int ui_h = 0;  // 变更后由 app 层落盘
};

}  // namespace sg
