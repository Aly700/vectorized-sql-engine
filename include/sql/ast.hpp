#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sql {

struct Predicate {
    std::string column;
    std::int64_t equals_i64{0};
};

struct SelectQuery {
    std::vector<std::string> projection;
    std::string table;
    std::optional<Predicate> predicate;
};

SelectQuery parse_select(const std::string& sql);

} // namespace sql
