#pragma once

#include <string>
#include <utility>
#include <vector>

#include "model/todo_kind.h"

namespace sg {

struct BoxItem {
    std::wstring name;  // 显示名（可与真实文件名不同）
    std::wstring path;  // 绝对路径，仅引用，永不移动
    // 失效标记：运行期状态，由工作线程 B 校验后填，**不参与序列化**（不写进 boxes.txt）
    bool missing = false;
};

struct Box {
    std::wstring name;
    std::vector<BoxItem> items;
};

struct TodoItem {
    long long id = 0;
    bool done = false;
    long long created = 0;  // Unix 秒
    TodoKind kind = TodoKind::Text;
    std::wstring text;    // 文字内容；link 时是 URL；image 时可为空
    std::wstring attach;  // image 时的图片来源路径，其余为空
    // 失效标记（仅 image 的引用型用）：运行期状态，由存在性校验回填，**不参与序列化**
    bool missing = false;
};

using Config = std::vector<std::pair<std::wstring, std::wstring>>;

// 行格式：box \t name \t path
std::wstring serialize_boxes(const std::vector<Box>& boxes);
std::vector<Box> parse_boxes(const std::wstring& text, int& bad);

// 把一批绝对路径加进第 box_index 个盒子（显示名取文件名），返回实际添加的条数。
// 不去重：同一文件可以在多个盒子里，也可以在一个盒子里重复出现（都是合法用法）。
// boxes 为空时先建一个“新盒子”——返回值只看条目数，不反映是否新建了盒子。
size_t box_add_paths(std::vector<Box>& boxes, int box_index,
                     const std::vector<std::wstring>& paths);

// 把一个条目从 src_box 移到 dst_box（拖到别的盒子标签）。
// 越界、同盒、空列表一律不动并返回 false，调用方不必先自查。
bool box_move_item(std::vector<Box>& boxes, int src_box, int index, int dst_box);

// 盒子名是否已被**别的**盒子占用（except_index 是要改名的那一个）。
// 改名必须用它拦重名：同名盒子在 parse 时会被合并，等于静默丢一个盒子的组织结构。
bool box_name_taken(const std::vector<Box>& boxes, const std::wstring& name, int except_index);

// 行格式：id \t done \t created \t due \t prio \t text
// 行格式：id \t done \t created \t kind \t text \t attach
std::wstring serialize_todos(const std::vector<TodoItem>& todos);
std::vector<TodoItem> parse_todos(const std::wstring& text, int& bad);

// 行格式：key \t value
std::wstring serialize_config(const Config& kv);
Config parse_config(const std::wstring& text, int& bad);

std::wstring config_get(const Config& kv, const std::wstring& key, const std::wstring& def);
void config_set(Config& kv, const std::wstring& key, const std::wstring& value);

// 未完成在上：prio 降序 -> created 降序；已完成全部沉底（同样按 prio/create 排）
void sort_todos(std::vector<TodoItem>& todos);

// 空列表返回 1
long long next_todo_id(const std::vector<TodoItem>& todos);

}  // namespace sg
