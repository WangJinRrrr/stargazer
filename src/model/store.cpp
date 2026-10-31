#include "model/store.h"

#include <algorithm>

#include "model/paths.h"   // file_name（拖入时的显示名，筛选也认它）
#include "model/rowformat.h"
#include "model/search.h"  // contains_ci

namespace sg {

std::wstring serialize_boxes(const std::vector<Box>& boxes) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& b : boxes) {
        if (b.items.empty()) {
            // 空盒子也要落盘：写一行“盒子名 + 两个空字段”的占位行，
            // parse 认出它只建盒子不建条目（与空分组同一套约定），
            // 否则新建的盒子重启后就消失了
            rows.push_back({ b.name, L"", L"" });
            continue;
        }
        for (const auto& it : b.items) {
            rows.push_back({ b.name, it.name, it.path });
        }
    }
    return build_text(rows);
}

std::vector<Box> parse_boxes(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 3, bad);
    std::vector<Box> boxes;
    for (const auto& r : rows) {
        // 只有条目名与路径都为空的行才算“空盒子占位”。
        // 空盒子名或空路径的行不可用（打开不了、也不知道放哪），跳过并计数，
        // 不让它变成在界面上看得见却没什么可做的幽灵条目。
        const bool placeholder = r[1].empty() && r[2].empty();
        if (r[0].empty() || (!placeholder && r[2].empty())) {
            ++bad;
            continue;
        }
        auto b = std::find_if(boxes.begin(), boxes.end(),
                              [&](const Box& x) { return x.name == r[0]; });
        if (b == boxes.end()) {
            boxes.push_back(Box{ r[0], {} });
            b = boxes.end() - 1;
        }
        if (!placeholder) b->items.push_back(BoxItem{ r[1], r[2] });
    }
    return boxes;
}

size_t box_add_paths(std::vector<Box>& boxes, int box_index,
                     const std::vector<std::wstring>& paths) {
    if (boxes.empty()) boxes.push_back(Box{ L"新盒子", {} });
    const int bi = box_index < 0 ? 0
                                 : (box_index >= static_cast<int>(boxes.size())
                                        ? static_cast<int>(boxes.size()) - 1
                                        : box_index);
    auto& items = boxes[bi].items;
    size_t added = 0;
    for (const auto& p : paths) {
        if (p.empty()) continue;  // 空路径不能变成删不掉的幽灵条目
        items.push_back(BoxItem{ file_name(p), p });
        ++added;
    }
    return added;
}

bool box_move_item(std::vector<Box>& boxes, int src_box, int index, int dst_box) {
    if (src_box < 0 || src_box >= static_cast<int>(boxes.size())) return false;
    if (dst_box < 0 || dst_box >= static_cast<int>(boxes.size())) return false;
    if (src_box == dst_box) return false;
    auto& src = boxes[src_box].items;
    if (index < 0 || index >= static_cast<int>(src.size())) return false;
    BoxItem moved = src[index];
    src.erase(src.begin() + index);
    boxes[dst_box].items.push_back(std::move(moved));
    return true;
}

bool box_move_within(Box& box, int from, int to) {
    const int n = static_cast<int>(box.items.size());
    // to 必须是真实存在的格子：空白格不是位置，不能把条目甩到列表尾部
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    BoxItem moved = box.items[from];
    box.items.erase(box.items.begin() + from);
    box.items.insert(box.items.begin() + to, std::move(moved));
    return true;
}

bool box_name_taken(const std::vector<Box>& boxes, const std::wstring& name, int except_index) {
    if (name.empty()) return false;  // 空名不算被占用（改名本来就不接受空名）
    for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
        if (i != except_index && boxes[i].name == name) return true;
    }
    return false;
}

std::vector<int> box_visible(const std::vector<BoxItem>& items, const std::wstring& needle) {
    std::vector<int> out;
    out.reserve(items.size());
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (needle.empty() || contains_ci(items[i].name, needle) ||
            contains_ci(file_name(items[i].path), needle)) {
            out.push_back(i);
        }
    }
    return out;
}

std::wstring serialize_config(const Config& kv) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& p : kv) rows.push_back({ p.first, p.second });
    return build_text(rows);
}

Config parse_config(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 2, bad);
    Config kv;
    for (const auto& r : rows) kv.emplace_back(r[0], r[1]);
    return kv;
}

std::wstring config_get(const Config& kv, const std::wstring& key, const std::wstring& def) {
    for (const auto& p : kv) {
        if (p.first == key) return p.second;
    }
    return def;
}

void config_set(Config& kv, const std::wstring& key, const std::wstring& value) {
    for (auto& p : kv) {
        if (p.first == key) {
            p.second = value;
            return;
        }
    }
    kv.emplace_back(key, value);
}

}  // namespace sg
