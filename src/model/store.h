#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sg {

struct LaunchItem {
    std::wstring name;
    std::wstring target;   // exe / .lnk / 目录 / URL / shell: 协议
    std::wstring args;
    std::wstring workdir;
    std::wstring icon;     // 自定义图标路径，空则用 target 的
};

struct LaunchGroup {
    std::wstring name;
    std::vector<LaunchItem> items;
};

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
    long long due = 0;      // 0 = 未设置
    int prio = 0;           // 0 普通，1 高
    std::wstring text;
};

using Config = std::vector<std::pair<std::wstring, std::wstring>>;

// 行格式：group \t name \t target \t args \t workdir \t icon
std::wstring serialize_launcher(const std::vector<LaunchGroup>& groups);
std::vector<LaunchGroup> parse_launcher(const std::wstring& text, int& bad);

// 行格式：box \t name \t path
std::wstring serialize_boxes(const std::vector<Box>& boxes);
std::vector<Box> parse_boxes(const std::wstring& text, int& bad);

// 把一批绝对路径加进第 box_index 个盒子（显示名取文件名），返回实际添加的条数。
// 不去重：同一文件可以在多个盒子里，也可以在一个盒子里重复出现（都是合法用法）。
// boxes 为空时先建一个“新盒子”——返回值只看条目数，不反映是否新建了盒子。
size_t box_add_paths(std::vector<Box>& boxes, int box_index,
                     const std::vector<std::wstring>& paths);

// 行格式：id \t done \t created \t due \t prio \t text
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
