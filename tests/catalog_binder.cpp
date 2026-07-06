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

void assert_alias_binding_replaces_physical_qualifier() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT x.b FROM t1 AS x WHERE x.a = 2"), catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.projections.size() == 1);
    assert(logical.projections[0].output_name == "x.b");

    const auto printed = plan::to_string(logical);
    assert(printed.find("Project[x.b=col(x.b)]") != std::string::npos);
    assert(printed.find("Filter[col(x.a) = lit(2)]") != std::string::npos);
    assert(printed.find("Scan[t1 AS x]") != std::string::npos);
}

void assert_unaliased_scan_keeps_existing_identity_text() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT t1.b FROM t1"), catalog);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Project[t1.b=col(t1.b)]") != std::string::npos);
    assert(printed.find("Scan[t1]") != std::string::npos);
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

void assert_order_by_output_name_takes_precedence_over_from_scope() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT b AS a, a AS original FROM t ORDER BY a DESC"), catalog);

    assert(logical.kind == plan::LogicalKind::Sort);
    const auto printed = plan::to_string(logical);
    assert(printed.find("Sort[col(a) DESC]") != std::string::npos);
    assert(printed.find("Project[a=col(t.b), original=col(t.a)]") != std::string::npos);
}

void assert_alias_order_by_uses_binding_scope() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select(
                             "SELECT x.b FROM t1 AS x JOIN t2 AS y ON x.a = y.a ORDER BY y.c DESC"),
                         catalog);

    assert(logical.order_permission == plan::OrderPermission::Deterministic);
    const auto printed = plan::to_string(logical);
    assert(printed.find("Sort[col(y.c) DESC]") != std::string::npos);
    assert(printed.find("Project[x.b=col(x.b)]") != std::string::npos);
    assert(printed.find("Join[col(x.a) = col(y.a)]") != std::string::npos);
    assert(printed.find("Scan[t1 AS x]") != std::string::npos);
    assert(printed.find("Scan[t2 AS y]") != std::string::npos);
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

void assert_group_by_binds_aggregate_between_filter_and_project() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT a, COUNT(*), SUM(b) FROM t WHERE b >= 20 GROUP BY a"), catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.projections.size() == 3);
    assert(logical.projections[0].output_name == "a");
    assert(logical.projections[1].output_name == "COUNT(*)");
    assert(logical.projections[2].output_name == "SUM(b)");

    const auto printed = plan::to_string(logical);
    assert(printed.find("Project[a=col(t.a), COUNT(*)=col(COUNT(*)), SUM(b)=col(SUM(b))]") != std::string::npos);
    assert(printed.find("Aggregate[group_keys=[col(t.a)], aggregates=[COUNT(*), SUM(b)=col(t.b)]]") !=
           std::string::npos);
    assert(printed.find("Filter[col(t.b) >= lit(20)]") != std::string::npos);
}

void assert_having_binds_between_aggregate_and_project() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT a FROM t GROUP BY a HAVING SUM(b) > 20"), catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.input != nullptr);
    assert(logical.input->kind == plan::LogicalKind::Filter);
    assert(logical.input->input != nullptr);
    assert(logical.input->input->kind == plan::LogicalKind::Aggregate);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Project[a=col(t.a)]") != std::string::npos);
    assert(printed.find("Filter[col(SUM(b)) > lit(20)]") != std::string::npos);
    assert(printed.find("Aggregate[group_keys=[col(t.a)], aggregates=[SUM(b)=col(t.b)]]") != std::string::npos);
}

void assert_boolean_tree_predicates_bind_and_split_top_level_and() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT a FROM t WHERE a = 1 AND (b = 20 OR a = 3)"), catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.input != nullptr);
    assert(logical.input->kind == plan::LogicalKind::Filter);
    assert(logical.input->predicates.size() == 2);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Filter[col(t.a) = lit(1) AND (col(t.b) = lit(20) OR col(t.a) = lit(3))]") !=
           std::string::npos);
}

void assert_having_boolean_tree_binds_group_keys_and_aggregate_outputs() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT a FROM t GROUP BY a HAVING a = 1 OR COUNT(*) > 1"), catalog);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Filter[(col(t.a) = lit(1) OR col(COUNT(*)) > lit(1))]") != std::string::npos);
    assert(printed.find("Aggregate[group_keys=[col(t.a)], aggregates=[COUNT(*)]]") != std::string::npos);
}

void assert_select_aliases_name_projected_aggregate_outputs() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select("SELECT a AS key, SUM(b) AS total FROM t GROUP BY a ORDER BY total DESC"),
        catalog);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Sort[col(total) DESC]") != std::string::npos);
    assert(printed.find("Project[key=col(t.a), total=col(SUM(b))]") != std::string::npos);
    assert(printed.find("Aggregate[group_keys=[col(t.a)], aggregates=[SUM(b)=col(t.b)]]") != std::string::npos);
}

void assert_distinct_and_limit_bind_above_project_below_sort() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT DISTINCT a FROM t ORDER BY a DESC LIMIT 2"), catalog);

    const auto printed = plan::to_string(logical);
    const auto expected =
        std::string("Limit[2]\n") +
        "  Sort[col(a) DESC]\n"
        "    Distinct\n"
        "      Project[a=col(t.a)]\n"
        "        Scan[t]";
    if (printed != expected) {
        throw std::logic_error("DISTINCT/LIMIT plan shape mismatch:\n" + printed);
    }
}

void assert_ungrouped_aggregate_binds_single_global_group() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT COUNT(*), MIN(t.b), MAX(t.b) FROM t"), catalog);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Aggregate[group_keys=[], aggregates=[COUNT(*), MIN(t.b)=col(t.b), MAX(t.b)=col(t.b)]]") !=
           std::string::npos);
    assert(printed.find("Project[COUNT(*)=col(COUNT(*)), MIN(t.b)=col(MIN(t.b)), MAX(t.b)=col(MAX(t.b))]") !=
           std::string::npos);
}

void assert_non_grouped_projection_column_is_rejected() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT a, b, COUNT(*) FROM t GROUP BY a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 10);
        assert(error.message() == "non-grouped column 'b' must appear in GROUP BY or be aggregated");
        return;
    }
    throw std::logic_error("expected non-grouped projection bind error");
}

void assert_nested_aggregate_is_rejected() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT SUM(COUNT(*)) FROM t"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 11);
        assert(error.message() == "nested aggregate 'COUNT' is not allowed");
        return;
    }
    throw std::logic_error("expected nested aggregate bind error");
}

void assert_grouped_order_by_must_use_grouping_column() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY b"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 46);
        assert(error.message() == "ORDER BY column 'b' must be a GROUP BY column or SELECT output name in aggregate queries");
        return;
    }
    throw std::logic_error("expected grouped ORDER BY bind error");
}

void assert_having_without_group_by_is_rejected() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT COUNT(*) FROM t HAVING COUNT(*) > 0"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 23);
        assert(error.message() == "HAVING requires GROUP BY in this SQL slice");
        return;
    }
    throw std::logic_error("expected HAVING without GROUP BY bind error");
}

void assert_having_non_grouped_column_is_rejected() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT a, COUNT(*) FROM t GROUP BY a HAVING b = 20"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 44);
        assert(error.message() == "HAVING column 'b' must be a GROUP BY column or aggregate expression");
        return;
    }
    throw std::logic_error("expected non-grouped HAVING column bind error");
}

void assert_grouped_order_by_accepts_grouping_column() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY a DESC"), catalog);

    assert(logical.kind == plan::LogicalKind::Sort);
    const auto printed = plan::to_string(logical);
    assert(printed.find("Sort[col(a) DESC]") != std::string::npos);
    assert(printed.find("Aggregate[group_keys=[col(t.a)], aggregates=[COUNT(*)]]") != std::string::npos);
}

void assert_physical_table_qualifier_is_unknown_when_alias_exists() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT t1.a FROM t1 AS x"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 7);
        assert(error.message() == "unknown table qualifier 't1'");
        return;
    }
    throw std::logic_error("expected physical-name qualifier bind error");
}

void assert_duplicate_binding_reports_alias_scope() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT x.a FROM t AS x JOIN t1 AS x ON x.a = x.a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 34);
        assert(error.message() == "duplicate table binding 'x' requires a unique alias");
        return;
    }
    throw std::logic_error("expected duplicate binding bind error");
}

void assert_unaliased_self_join_still_requires_aliases() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT t.a FROM t JOIN t ON t.a = t.a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 23);
        assert(error.message() == "duplicate table binding 't' requires a unique alias");
        return;
    }
    throw std::logic_error("expected unaliased self join duplicate binding error");
}

void assert_ambiguous_unqualified_column_reports_alias_candidates() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT a FROM t AS x JOIN t2 AS y ON x.a = y.a"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 7);
        assert(error.message() == "ambiguous column 'a' matches tables 'x', 'y'");
        return;
    }
    throw std::logic_error("expected alias-scope ambiguous column bind error");
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
    assert_alias_binding_replaces_physical_qualifier();
    assert_unaliased_scan_keeps_existing_identity_text();
    assert_order_by_uses_from_scope_not_projection_outputs();
    assert_order_by_output_name_takes_precedence_over_from_scope();
    assert_alias_order_by_uses_binding_scope();
    assert_order_by_ambiguous_unqualified_column_reports_candidates();
    assert_group_by_binds_aggregate_between_filter_and_project();
    assert_having_binds_between_aggregate_and_project();
    assert_boolean_tree_predicates_bind_and_split_top_level_and();
    assert_having_boolean_tree_binds_group_keys_and_aggregate_outputs();
    assert_select_aliases_name_projected_aggregate_outputs();
    assert_distinct_and_limit_bind_above_project_below_sort();
    assert_ungrouped_aggregate_binds_single_global_group();
    assert_non_grouped_projection_column_is_rejected();
    assert_nested_aggregate_is_rejected();
    assert_grouped_order_by_must_use_grouping_column();
    assert_having_without_group_by_is_rejected();
    assert_having_non_grouped_column_is_rejected();
    assert_grouped_order_by_accepts_grouping_column();
    assert_physical_table_qualifier_is_unknown_when_alias_exists();
    assert_duplicate_binding_reports_alias_scope();
    assert_unaliased_self_join_still_requires_aliases();
    assert_ambiguous_unqualified_column_reports_alias_candidates();
    assert_unknown_qualifier_reports_bind_error();
    assert_ambiguous_unqualified_column_reports_candidates();
    return 0;
}
