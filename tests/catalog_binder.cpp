#include "catalog/catalog.hpp"
#include "plan/logical_plan.hpp"
#include "sql/binder.hpp"
#include "sql/errors.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class StaticSchemaCatalog final : public catalog::Catalog {
public:
    void add_table(catalog::TableSchema schema) { tables_.push_back(std::move(schema)); }

    [[nodiscard]] std::optional<catalog::TableSchema> find_table_schema(const std::string& name) const override {
        for (const auto& table : tables_) {
            if (table.name == name) {
                return table;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<catalog::TableSchema> tables_;
};

StaticSchemaCatalog make_schema_catalog() {
    StaticSchemaCatalog catalog;
    catalog.add_table(catalog::TableSchema{
        "t",
        {catalog::ColumnSchema{"a", catalog::ColumnType::Int64},
         catalog::ColumnSchema{"b", catalog::ColumnType::Int64}},
    });
    return catalog;
}

void assert_binds_against_schema_only_catalog() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT b, a FROM t WHERE a = 2"), catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.projections.size() == 2);
    assert(logical.projections[0].output_name == "b");
    assert(logical.projections[1].output_name == "a");
    assert(logical.input != nullptr);
    assert(logical.input->kind == plan::LogicalKind::Filter);
    assert(logical.input->predicates.size() == 1);
    assert(logical.input->input != nullptr);
    assert(logical.input->input->kind == plan::LogicalKind::Scan);
    assert(logical.input->input->table == "t");
}

void assert_unknown_column_still_reports_bind_error() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT missing FROM t"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 7);
        assert(error.message() == "unknown column 'missing' in table 't'");
        return;
    }
    throw std::logic_error("expected unknown column bind error");
}

} // namespace

int main() {
    assert_binds_against_schema_only_catalog();
    assert_unknown_column_still_reports_bind_error();
    return 0;
}
