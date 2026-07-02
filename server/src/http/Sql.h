#pragma once
// Sql.h —— 防注入辅助：动态标识符（列名/排序）一律走白名单，绝不拼接用户输入。
// 值永远用 $n 参数绑定；这里只处理"不能被参数化"的标识符位置。

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace aegis::sql {

// 把字符串列表拼成 PostgreSQL text[] 字面量 {"a","b",...}，作为单个参数绑定后 ::text[] 使用。
// 元素加引号并转义，整体是绑定值（非拼接），无注入风险。
inline std::string pgTextArray(const std::vector<std::string>& items) {
    std::string s = "{";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) s += ",";
        s += "\"";
        for (char c : items[i]) { if (c == '"' || c == '\\') s += '\\'; s += c; }
        s += "\"";
    }
    s += "}";
    return s;
}

// 把整型 id 列表拼成 PostgreSQL 数组字面量 {1,2,3}，绑定后 ::bigint[] 使用。
// 元素只取自 long long（无文本注入面），整体作为绑定值，安全。
inline std::string pgBigintArray(const std::vector<long long>& ids) {
    std::string s = "{";
    for (size_t i = 0; i < ids.size(); ++i) { if (i) s += ","; s += std::to_string(ids[i]); }
    s += "}";
    return s;
}

// 把多行/逗号分隔文本切成去重去空的列表（批量搜索输入解析）。
inline std::vector<std::string> splitLines(const std::string& text, size_t maxItems = 2000) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::string cur;
    auto flush = [&]() {
        size_t a = cur.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            size_t b = cur.find_last_not_of(" \t\r\n");
            std::string v = cur.substr(a, b - a + 1);
            if (!v.empty() && seen.insert(v).second && out.size() < maxItems) out.push_back(v);
        }
        cur.clear();
    };
    for (char c : text) { if (c == '\n' || c == '\r' || c == ',') flush(); else cur += c; }
    flush();
    return out;
}

// 生成 IN 占位符： startIndex=3, count=2 -> "$3,$4"。占位符由程序生成，值仍参数绑定，安全。
inline std::string inPlaceholders(size_t count, int startIndex) {
    std::string s;
    for (size_t i = 0; i < count; ++i) {
        if (i) s += ",";
        s += "$" + std::to_string(startIndex + static_cast<int>(i));
    }
    return s;
}

// users 表允许的模糊搜索列（前端传 field，必须命中白名单，否则忽略）。
inline bool isUserSearchField(const std::string& f) {
    static const std::unordered_set<std::string> ok{
        "username", "machine_code", "ip_address", "remark", "extra"};
    return ok.count(f) != 0;
}

// cards 表允许的模糊搜索列。
inline bool isCardSearchField(const std::string& f) {
    static const std::unordered_set<std::string> ok{
        "code", "remark", "batch_id", "maker_name"};
    return ok.count(f) != 0;
}

// 排序方向白名单。
inline std::string sortDir(const std::string& d) {
    return (d == "asc" || d == "ASC") ? "ASC" : "DESC";
}

} // namespace aegis::sql
