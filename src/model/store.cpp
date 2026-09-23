#include "model/store.h"

#include <algorithm>
#include <cwchar>

#include "model/paths.h"  // file_name（拖入时的显示名）
#include "model/rowformat.h"

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

static long long to_ll(const std::wstring& s) {
    return static_cast<long long>(std::wcstoll(s.c_str(), nullptr, 10));
}

std::wstring serialize_todos(const std::vector<TodoItem>& todos) {
    std::vector<std::vector<std::wstring>> rows;
    for (const auto& t : todos) {
        rows.push_back({ std::to_wstring(t.id), t.done ? L"1" : L"0",
                         std::to_wstring(t.created), todo_kind_to_string(t.kind), t.text,
                         t.attach });
    }
    return build_text(rows);
}

std::vector<TodoItem> parse_todos(const std::wstring& text, int& bad) {
    const auto rows = parse_rows(text, 6, bad);
    std::vector<TodoItem> todos;
    for (const auto& r : rows) {
        // 空 kind + 空 text + 空 attach = 占位/坏行，不产生幽灵条目
        if (r[3].empty() && r[4].empty() && r[5].empty()) {
            ++bad;
            continue;
        }
        TodoItem t;
        t.id = to_ll(r[0]);
        t.done = (r[1] == L"1");
        t.created = to_ll(r[2]);
        t.kind = todo_kind_from_string(r[3]);
        t.text = r[4];
        t.attach = r[5];
        todos.push_back(std::move(t));
    }
    return todos;
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

void sort_todos(std::vector<TodoItem>& todos) {
    std::stable_sort(todos.begin(), todos.end(), [](const TodoItem& a, const TodoItem& b) {
        if (a.done != b.done) return !a.done;  // 未完成在前
        if (a.created != b.created) return a.created > b.created;  // 新的在前
        return a.id > b.id;  // 同一秒连记多条：id 大的在前（不随排序实现漂移）
    });
}

long long next_todo_id(const std::vector<TodoItem>& todos) {
    long long max_id = 0;
    for (const auto& t : todos) {
        if (t.id > max_id) max_id = t.id;
    }
    return max_id + 1;
}

}  // namespace sg
