#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace catalog {

enum class ColumnType { Int64 };

struct ColumnSchema {
    std::string name;
    ColumnType type{ColumnType::Int64};
};

struct TableSchema {
    std::string name;
    std::vector<ColumnSchema> columns;

    [[nodiscard]] bool has_column(const std::string& column_name) const {
        for (const auto& column : columns) {
            if (column.name == column_name) {
                return true;
            }
        }
        return false;
    }
};

class Catalog {
public:
    virtual ~Catalog() = default;

    [[nodiscard]] virtual std::optional<TableSchema> find_table_schema(const std::string& name) const = 0;
};

} // namespace catalog
