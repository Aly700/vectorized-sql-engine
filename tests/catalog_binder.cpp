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
    catalog.add_table(catalog::TableSchema{
        "strings",
        {catalog::ColumnSchema{"s", catalog::ColumnType::String},
         catalog::ColumnSchema{"i", catalog::ColumnType::Int64}},
    });
    catalog.add_table(catalog::TableSchema{
        "strings2",
        {catalog::ColumnSchema{"s", catalog::ColumnType::String},
         catalog::ColumnSchema{"payload", catalog::ColumnType::Int64}},
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

void assert_outer_join_kinds_parse_and_bind() {
    auto catalog = make_schema_catalog();

    const auto left_parsed =
        sql::parse_select("SELECT t1.b, t2.c FROM t1 LEFT OUTER JOIN t2 ON t1.a = t2.a");
    assert(left_parsed.joins.size() == 1);
    assert(left_parsed.joins[0].kind == sql::JoinKind::Left);

    const auto left_logical = sql::bind_select(left_parsed, catalog);
    const auto left_printed = plan::to_string(left_logical);
    assert(left_printed.find("LeftJoin[col(t1.a) = col(t2.a)]") != std::string::npos);

    const auto right_parsed =
        sql::parse_select("SELECT t2.c, t1.b FROM t1 RIGHT JOIN t2 ON t1.a = t2.a");
    assert(right_parsed.joins.size() == 1);
    assert(right_parsed.joins[0].kind == sql::JoinKind::Right);

    const auto right_logical = sql::bind_select(right_parsed, catalog);
    const auto right_printed = plan::to_string(right_logical);
    assert(right_printed.find("LeftJoin[col(t1.a) = col(t2.a)]") != std::string::npos);
    assert(right_printed.find("    Scan[t2]\n    Scan[t1]") != std::string::npos);
    assert(right_logical.input != nullptr);
    assert(right_logical.input->kind == plan::LogicalKind::Join);
    assert(right_logical.input->join_kind == plan::JoinKind::Left);
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

void assert_global_having_binds_over_single_global_group() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT SUM(a) FROM t HAVING COUNT(*) > 0"), catalog);

    const auto printed = plan::to_string(logical);
    const auto expected =
        std::string("Project[SUM(a)=col(SUM(a))]\n") +
        "  Filter[col(COUNT(*)) > lit(0)]\n"
        "    Aggregate[group_keys=[], aggregates=[SUM(a)=col(t.a), COUNT(*)]]\n"
        "      Scan[t]";
    if (printed != expected) {
        throw std::logic_error("global HAVING plan shape mismatch:\n" + printed);
    }
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

void assert_string_columns_and_literals_bind_types() {
    auto catalog = make_schema_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT s, 'a''b' AS escaped FROM strings WHERE s = 'alpha'"), catalog);

    assert(logical.kind == plan::LogicalKind::Project);
    assert(logical.projections.size() == 2);
    assert(logical.projections[0].type == catalog::ColumnType::String);
    assert(logical.projections[1].type == catalog::ColumnType::String);
    assert(logical.input != nullptr);
    assert(logical.input->kind == plan::LogicalKind::Filter);
    assert(logical.input->predicates.front().comparison.left.type == catalog::ColumnType::String);
    assert(logical.input->predicates.front().comparison.right.type == catalog::ColumnType::String);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Project[s=col(strings.s), escaped=lit('a''b')]") != std::string::npos);
    assert(printed.find("Filter[col(strings.s) = lit('alpha')]") != std::string::npos);
}

void assert_string_group_order_join_and_minmax_are_legal() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select("SELECT l.s, MIN(r.s), MAX(r.s), COUNT(l.s) FROM strings AS l JOIN strings2 AS r ON l.s = r.s GROUP BY l.s ORDER BY l.s"),
        catalog);

    const auto printed = plan::to_string(logical);
    assert(printed.find("Join[col(l.s) = col(r.s)]") != std::string::npos);
    assert(printed.find("Aggregate[group_keys=[col(l.s)], aggregates=[MIN(r.s)=col(r.s), MAX(r.s)=col(r.s), COUNT(l.s)=col(l.s)]]") != std::string::npos);
    assert(printed.find("Sort[col(l.s) ASC]") != std::string::npos);
}

void assert_comparison_type_mismatch_is_rejected() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT s FROM strings WHERE s = 1"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 30);
        assert(error.message() == "comparison operands must have the same type: string vs int64");
        return;
    }
    throw std::logic_error("expected string/int comparison bind error");
}

void assert_reverse_comparison_type_mismatch_is_rejected() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT i FROM strings WHERE i = '1'"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 30);
        assert(error.message() == "comparison operands must have the same type: int64 vs string");
        return;
    }
    throw std::logic_error("expected int/string comparison bind error");
}

void assert_sum_rejects_string_argument() {
    auto catalog = make_schema_catalog();
    try {
        (void)sql::bind_select(sql::parse_select("SELECT SUM(s) FROM strings"), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == 7);
        assert(error.message() == "SUM requires int64 argument, got string");
        return;
    }
    throw std::logic_error("expected SUM(string) bind error");
}

void assert_unterminated_string_literal_reports_start_position() {
    try {
        (void)sql::parse_select("SELECT 'abc FROM strings");
    } catch (const sql::ParseError& error) {
        assert(error.position() == 7);
        assert(error.message() == "unterminated string literal");
        return;
    }
    throw std::logic_error("expected unterminated string literal parse error");
}

void assert_uncorrelated_subquery_forms_bind_with_nested_full_selects() {
    auto catalog = make_schema_catalog();

    const auto scalar_right = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE a = (SELECT b FROM t1 ORDER BY b LIMIT 1)"), catalog);
    const auto scalar_left = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE (SELECT b FROM t1 ORDER BY b LIMIT 1) = a"), catalog);
    const auto in = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE a IN (SELECT t1.a FROM t1 JOIN t2 ON t1.a = t2.a)"), catalog);
    const auto not_in = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE a NOT IN (SELECT a FROM t1 GROUP BY a HAVING COUNT(*) > 0)"),
        catalog);
    const auto nested_exists = sql::bind_select(
        sql::parse_select(
            "SELECT a FROM t WHERE EXISTS (SELECT b FROM t1 WHERE b IN (SELECT c FROM t2))"),
        catalog);
    const auto not_exists = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE NOT EXISTS (SELECT a FROM t2 LIMIT 0)"), catalog);

    for (const auto* logical : {&scalar_right, &scalar_left, &in, &not_in, &nested_exists, &not_exists}) {
        const auto printed = plan::to_string(*logical);
        assert(printed.find("Subquery[") != std::string::npos);
    }
}

void assert_scalar_and_in_subquery_width_rules_are_positioned() {
    auto catalog = make_schema_catalog();

    const std::string scalar_sql = "SELECT a FROM t WHERE a = (SELECT a, b FROM t1)";
    try {
        (void)sql::bind_select(sql::parse_select(scalar_sql), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == scalar_sql.find('('));
        assert(error.message() == "scalar subquery must produce exactly one output column, got 2");
        goto in_width;
    }
    throw std::logic_error("expected scalar subquery width bind error");

in_width:
    const std::string in_sql = "SELECT a FROM t WHERE a IN (SELECT a, b FROM t1)";
    try {
        (void)sql::bind_select(sql::parse_select(in_sql), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == in_sql.find("IN"));
        assert(error.message() == "IN subquery must produce exactly one output column, got 2");
        return;
    }
    throw std::logic_error("expected IN subquery width bind error");
}

void assert_subquery_type_mismatches_are_positioned() {
    auto catalog = make_schema_catalog();

    const auto null_in_string = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE NULL IN (SELECT s FROM strings)"), catalog);
    assert(plan::to_string(null_in_string).find("lit(NULL) IN subquery(") != std::string::npos);

    const std::string scalar_sql = "SELECT a FROM t WHERE a = (SELECT s FROM strings LIMIT 1)";
    try {
        (void)sql::bind_select(sql::parse_select(scalar_sql), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == scalar_sql.find('='));
        assert(error.message() == "comparison operands must have the same type: int64 vs string");
        goto in_type;
    }
    throw std::logic_error("expected scalar subquery type bind error");

in_type:
    const std::string in_sql = "SELECT s FROM strings WHERE s IN (SELECT a FROM t)";
    try {
        (void)sql::bind_select(sql::parse_select(in_sql), catalog);
    } catch (const sql::BindError& error) {
        assert(error.position() == in_sql.find("IN"));
        assert(error.message() == "IN operand and subquery column must have the same type: string vs int64");
        return;
    }
    throw std::logic_error("expected IN subquery type bind error");
}

void assert_correlated_subquery_binds_outer_identity_and_records_correlation() {
    auto catalog = make_schema_catalog();
    const std::string query =
        "SELECT outer_t.a FROM t AS outer_t WHERE EXISTS "
        "(SELECT b FROM t1 WHERE t1.a = outer_t.a)";
    const auto logical = sql::bind_select(sql::parse_select(query), catalog);
    assert(logical.input != nullptr && logical.input->kind == plan::LogicalKind::Filter);
    const auto& subquery = logical.input->predicates.at(0).subquery;
    assert(subquery != nullptr);
    assert(subquery->correlation_columns.size() == 1);
    const auto& correlation = subquery->correlation_columns.front();
    assert(correlation.binding == "outer_t");
    assert(correlation.column == "a");
    assert(correlation.outer_depth == 1);
    const auto printed = plan::to_string(logical);
    assert(printed.find("correlation=[outer(1):col(outer_t.a)]") != std::string::npos);
}

void assert_subquery_local_scope_shadows_same_named_outer_scope() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select("SELECT x.a FROM t AS x WHERE EXISTS (SELECT x.b FROM t1 AS x WHERE x.a = 1)"),
        catalog);
    const auto& subquery = logical.input->predicates.at(0).subquery;
    assert(subquery != nullptr);
    assert(subquery->correlation_columns.empty());
}

void assert_unqualified_subquery_reference_falls_back_to_outer_scope() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select("SELECT o.b FROM t AS o WHERE EXISTS (SELECT t2.c FROM t2 WHERE t2.c = b)"),
        catalog);
    const auto& subquery = logical.input->predicates.at(0).subquery;
    assert(subquery != nullptr && subquery->correlation_columns.size() == 1);
    assert(subquery->correlation_columns.front().binding == "o");
    assert(subquery->correlation_columns.front().column == "b");
    assert(subquery->correlation_columns.front().outer_depth == 1);
}

void assert_inner_level_ambiguity_does_not_fall_back_to_outer_scope() {
    auto catalog = make_schema_catalog();
    const std::string query =
        "SELECT o.a FROM t AS o WHERE EXISTS "
        "(SELECT x.b FROM t1 AS x JOIN t2 AS y ON x.a = y.a WHERE a = o.a)";
    try {
        (void)sql::bind_select(sql::parse_select(query), catalog);
    } catch (const sql::BindError& error) {
        assert(error.message() == "ambiguous column 'a' matches tables 'x', 'y'");
        return;
    }
    throw std::logic_error("expected innermost-level ambiguity bind error");
}

void assert_nested_subquery_records_grandparent_dependency() {
    auto catalog = make_schema_catalog();
    const auto logical = sql::bind_select(sql::parse_select(
                                              "SELECT o.a FROM t AS o WHERE EXISTS "
                                              "(SELECT m.b FROM t1 AS m WHERE EXISTS "
                                              "(SELECT n.c FROM t2 AS n WHERE n.a = o.a))"),
                                          catalog);
    const auto& middle = logical.input->predicates.at(0).subquery;
    assert(middle != nullptr);
    assert(middle->correlation_columns.size() == 1);
    assert(middle->correlation_columns.front().binding == "o");
    assert(middle->correlation_columns.front().outer_depth == 1);
    assert(middle->input != nullptr && middle->input->kind == plan::LogicalKind::Filter);
    const auto& inner = middle->input->predicates.at(0).subquery;
    assert(inner != nullptr);
    assert(inner->correlation_columns.size() == 1);
    assert(inner->correlation_columns.front().binding == "o");
    assert(inner->correlation_columns.front().outer_depth == 2);
}

void assert_subquery_parse_errors_are_positioned() {
    const std::string missing_right = "SELECT a FROM t WHERE EXISTS (SELECT a FROM t";
    try {
        (void)sql::parse_select(missing_right);
    } catch (const sql::ParseError& error) {
        assert(error.position() == missing_right.size());
        assert(error.message() == "expected ')' after subquery");
        goto missing_select;
    }
    throw std::logic_error("expected missing subquery right parenthesis parse error");

missing_select:
    const std::string invalid_in = "SELECT a FROM t WHERE a IN (a)";
    try {
        (void)sql::parse_select(invalid_in);
    } catch (const sql::ParseError& error) {
        assert(error.position() == invalid_in.find('('));
        assert(error.message() == "expected subquery after IN");
        return;
    }
    throw std::logic_error("expected IN subquery parse error");
}

} // namespace

int main() {
    assert_binds_against_schema_only_catalog();
    assert_unknown_column_still_reports_bind_error();
    assert_join_binds_qualified_columns_to_stable_plan();
    assert_outer_join_kinds_parse_and_bind();
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
    assert_global_having_binds_over_single_global_group();
    assert_having_non_grouped_column_is_rejected();
    assert_grouped_order_by_accepts_grouping_column();
    assert_physical_table_qualifier_is_unknown_when_alias_exists();
    assert_duplicate_binding_reports_alias_scope();
    assert_unaliased_self_join_still_requires_aliases();
    assert_ambiguous_unqualified_column_reports_alias_candidates();
    assert_unknown_qualifier_reports_bind_error();
    assert_ambiguous_unqualified_column_reports_candidates();
    assert_string_columns_and_literals_bind_types();
    assert_string_group_order_join_and_minmax_are_legal();
    assert_comparison_type_mismatch_is_rejected();
    assert_reverse_comparison_type_mismatch_is_rejected();
    assert_sum_rejects_string_argument();
    assert_unterminated_string_literal_reports_start_position();
    assert_uncorrelated_subquery_forms_bind_with_nested_full_selects();
    assert_scalar_and_in_subquery_width_rules_are_positioned();
    assert_subquery_type_mismatches_are_positioned();
    assert_correlated_subquery_binds_outer_identity_and_records_correlation();
    assert_subquery_local_scope_shadows_same_named_outer_scope();
    assert_unqualified_subquery_reference_falls_back_to_outer_scope();
    assert_inner_level_ambiguity_does_not_fall_back_to_outer_scope();
    assert_nested_subquery_records_grandparent_dependency();
    assert_subquery_parse_errors_are_positioned();
    return 0;
}
