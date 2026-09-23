#pragma once

#include <windows.h>

#include <climits>
#include <string>
#include <vector>

#include "edit.h"
#include "fs_work.h"  // FsEntry（浏览视图的列表项）
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
    int tab_hover = -1;      // 悬停的盒子标签（分段控件的 hover 状态）
    int scroll = 0;         // 起始行
    int drag_over_tab = -1;  // 内部拖拽时高亮的目标盒子标签
    // 输入即跳（Explorer 的 type-ahead）：连打的字符与时间戳。
    // 盒子条目是手挑的几十条，跳过去比“藏条目的筛选框”直接，也不用多一层下标映射。
    std::wstring typeahead;
    unsigned long long typeahead_ms = 0;
    // 重命名输入框。原来借用启动板的搜索框（全程序只有一个 EDIT 实例），
    // 启动板删掉后盒子自己持有一个。
    InlineEdit edit;
};

// 浏览视图状态（本地目录 / 网盘目录）。列表是异步枚举出来的，所以带 requestId。
struct BrowseState {
    std::wstring path;               // 当前目录（空 = 还没设置过根目录）
    std::vector<FsEntry> entries;    // 当前**显示**的列表（已排序：目录在前 + 自然序）
    // 筛选：entries 始终是“看得见的那份”，all 只在筛选生效期间存全量
    // （未筛选时 all 是空的）。这样 open/rename/delete 那些直接吃下标的地方
    // 一行都不用改 —— 下标直通，就不可能指到被筛掉的项上去。
    std::vector<FsEntry> all;
    std::wstring filter;             // 筛选词（空 = 不筛）
    std::wstring error;              // 非空 = 枚举失败（列表区显示错误行 + F5）
    std::wstring note;               // 最近一次操作的提示（如“跳过 2 个同名文件”）
    int sel = -1;
    int hover = -1;
    int scroll = 0;
    std::vector<std::wstring> history;  // 访问过的目录（含当前），hist_pos 指向当前
    int hist_pos = -1;
    uint64_t request_id = 0;            // 每次枚举递增，用来丢弃过期结果
    uint64_t op_id = 0;                 // 文件操作同样带 id
    bool loading = false;
    // 路径栏的输入框：默认只画文本，点路径栏或 Ctrl+L 才打开它（避免跟列表抢键盘）
    InlineEdit path_edit;
    // 筛选框（路径栏同一行的右端）：输入即筛
    InlineEdit filter_edit;
};

// 待办视图状态。列表是不等高行（文字 28 / 图片 96），offsets 是行偏移前缀和，
// 渲染与命中都用它（不另算一份）。
struct TodoState {
    int sel = -1;
    int hover = -1;
    float scroll = 0.f;                 // 像素
    std::vector<float> offsets;         // 行偏移前缀和（不等高：按条目算）
    InlineEdit input;                   // 底部常驻输入框（新增条目）
    InlineEdit edit;                    // F2 改文字（临时叠在行上）
    long long pending_image_id = 0;     // 正在落盘的图片条目 id（0 = 无），失败时回滚它
    uint64_t op_id = 0;                 // 文件操作请求号（与 fs_take_op 配对）
};

struct AppState {
    std::vector<Box> boxes;
    std::vector<TodoItem> todos;
    Config config;
    int bad_lines = 0;
    View view = View::Box;  // 首次启动（ui.txt 还没有记录时）停在收纳盒
    int nav_hover = -1;     // 顶部视图标签的悬停下标（-1 = 无）
    BoxState box_view;
    TodoState todo;
    BrowseState browse;
    bool data_dirty = false;
    // ui.txt 里想要的窗口尺寸（逻辑像素）与位置（**物理像素**，INT_MIN = 没记过）。
    // 尺寸存起来等面板真正创建时再应用；位置在呼出时用，_MIN 时回退到“光标所在显示器居中”。
    // 位置只能存物理像素：DIP 没有绝对原点，跨不同缩放的显示器时只有物理坐标能用。
    int ui_w = 0;
    int ui_h = 0;
    int ui_x = INT_MIN;
    int ui_y = INT_MIN;
};

}  // namespace sg
