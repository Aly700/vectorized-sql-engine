#include "sql/ast.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace sql {
namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> split_projection(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto t = trim(item);
        if (t.empty()) {
            throw std::invalid_argument("empty projection item");
        }
        out.push_back(t);
    }
    return out;
}

} // namespace

SelectQuery parse_select(const std::string& input) {
    static const std::regex pattern(
        R"(^\s*SELECT\s+(.+?)\s+FROM\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+WHERE\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(-?[0-9]+))?\s*;?\s*$)",
        std::regex::icase);

    std::smatch match;
    if (!std::regex_match(input, match, pattern)) {
        throw std::invalid_argument("only SELECT <cols> FROM <table> [WHERE col = int] is implemented in phase 1");
    }

    SelectQuery query;
    query.projection = split_projection(match[1].str());
    query.table = match[2].str();
    if (match[3].matched) {
        query.predicate = Predicate{match[3].str(), std::stoll(match[4].str())};
    }
    return query;
}

} // namespace sql
