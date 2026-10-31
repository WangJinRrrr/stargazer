#pragma once

#include <string>
#include <utility>
#include <vector>

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

// 盒内换位置（拖到另一个格子）：条目最终落在原列表的 to 下标处，
// 原本占着 to 的条目（以及中间的）整体朝 from 方向挪一格 —— 语义是
// “落在哪个格子就住哪个格子”，与拖到标签换盒是同一套心智。
// 越界、from == to 一律不动并返回 false。
bool box_move_within(Box& box, int from, int to);

// 盒子名是否已被**别的**盒子占用（except_index 是要改名的那一个）。
// 改名必须用它拦重名：同名盒子在 parse 时会被合并，等于静默丢一个盒子的组织结构。
bool box_name_taken(const std::vector<Box>& boxes, const std::wstring& name, int except_index);

// 当前盒子里“看得见”的条目下标（真实下标，不是一份拷贝）。
// needle 为空时按原顺序返回全部；否则留下显示名或文件名里含它的项。
// 打开 / 删除 / 换位置都拿这个下标回去，避免筛选后再用“看得见的序号”指错条目。
std::vector<int> box_visible(const std::vector<BoxItem>& items, const std::wstring& needle);

// 行格式：key \t value
std::wstring serialize_config(const Config& kv);
Config parse_config(const std::wstring& text, int& bad);

std::wstring config_get(const Config& kv, const std::wstring& key, const std::wstring& def);
void config_set(Config& kv, const std::wstring& key, const std::wstring& value);

}  // namespace sg
