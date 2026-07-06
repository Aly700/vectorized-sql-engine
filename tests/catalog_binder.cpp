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
    catalog.add_table(catalog::TableSchema{
        "t1",
        {catalog::ColumnSchema{"a", catalog::ColumnType::Int64},
         catalog::ColumnSchema{"b", catalog::ColumnType::Int64}},
    });
    catalog.add_table(catalog::TableSchema{
        "t2",
        {catalog::ColumnSchema{"a", catalog::ColumnType::Int64},
         catalog::ColumnSchema{"c", catalog::ColumnType::Int64}},
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

void assert_join_binds_qualified_columns_to_stable_plan() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a WHERE t2.c > 200"),
                         catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.projections.size() == 2);
    assert(logical.projections[0].output_name == "t1.b");
    assert(logical.projections[1].output_name == "t2.c");

    const auto printed = plan::to_string(logical);
    assert(printed.find("Join[") != std::string::npos);
    assert(printed.find("col(t1.a) = col(t2.a)") != std::string::npos);
    assert(printed.find("col(t2.c) > lit(200)") != std::string::npos);
}

void assert_order_by_uses_from_scope_not_projection_outputs() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select(
                             "SELECT t1.b FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY t2.c DESC"),
                         catalog);

    assert(logical.order_permission == plan::OrderPermission::Deterministic);
    assert(logical.input != nullptr);
    assert(logical.input->kind == plan::LogicalKind::Project);
    assert(logical.input->order_permission == plan::OrderPermission::Arbitrary);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Sort[col(t2.c) DESC]") != std::string::npos);
    assert(printed.find("Project[t1.b=col(t1.b)]") != std::string::npos);
    assert(printed.find("Join[col(t1.a) = col(t2.a)]") != std::string::npos);
}

void assert_order_by_ambiguous_unqualified_column_reports_candidates() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT t1.b FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 52);
        assert(error.message() == "ambiguous column 'a' matches tables 't1', 't2'");
        return;
    }
    throw std::logic_error("expected ambiguous ORDER BY bind error");
}

void assert_unknown_qualifier_reports_bind_error() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT nope.a FROM t1 JOIN t2 ON t1.a = t2.a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 7);
        assert(error.message() == "unknown table qualifier 'nope'");
        return;
    }
    throw std::logic_error("expected unknown qualifier bind error");
}

void assert_ambiguous_unqualified_column_reports_candidates() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT a FROM t1 JOIN t2 ON t1.a = t2.a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 7);
        assert(error.message() == "ambiguous column 'a' matches tables 't1', 't2'");
        return;
    }
    throw std::logic_error("expected ambiguous column bind error");
}

} // namespace

int main() {
    assert_binds_against_schema_only_catalog();
    assert_unknown_column_still_reports_bind_error();
    assert_join_binds_qualified_columns_to_stable_plan();
    assert_order_by_uses_from_scope_not_projection_outputs();
    assert_order_by_ambiguous_unqualified_column_reports_candidates();
    assert_unknown_qualifier_reports_bind_error();
    assert_ambiguous_unqualified_column_reports_candidates();
    return 0;
}
