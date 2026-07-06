#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "plan/physical_plan.hpp"
#include "sql/binder.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TableSpec {
    std::string name;
    std::vector<std::int64_t> a;
    std::vector<std::int64_t> b;
};

storage::ColumnarBatch make_batch(const TableSpec& spec) {
    storage::Int64Column a;
    for (auto value : spec.a) {
        a.append(value);
    }

    storage::Int64Column b;
    for (auto value : spec.b) {
        b.append(value);
    }

    storage::ColumnarBatch batch;
    batch.add_column("a", std::move(a));
    batch.add_column("b", std::move(b));
    return batch;
}

execution::Catalog make_catalog(const TableSpec& spec) {
    execution::Catalog catalog;
    catalog.add_table("t", make_batch(spec));
    return catalog;
}

execution::Catalog make_golden_catalog() {
    storage::Int64Column a;
    for (auto value : {1, 2, 3, 4}) {
        a.append(value);
    }

    storage::Int64Column b;
    for (auto value : {10, 20, 20, 40}) {
        b.append(value);
    }

    storage::Int64Column c;
    for (auto value : {5, 6, 7, 8}) {
        c.append(value);
    }

    storage::ColumnarBatch batch;
    batch.add_column("a", std::move(a));
    batch.add_column("b", std::move(b));
    batch.add_column("c", std::move(c));

    execution::Catalog catalog;
    catalog.add_table("t", std::move(batch));

    storage::Int64Column t1_a;
    for (auto value : {1, 2, 2}) {
        t1_a.append(value);
    }
    storage::Int64Column t1_b;
    for (auto value : {10, 20, 30}) {
        t1_b.append(value);
    }
    storage::ColumnarBatch t1;
    t1.add_column("a", std::move(t1_a));
    t1.add_column("b", std::move(t1_b));
    catalog.add_table("t1", std::move(t1));

    storage::Int64Column t2_a;
    for (auto value : {2, 2, 3}) {
        t2_a.append(value);
    }
    storage::Int64Column t2_c;
    for (auto value : {200, 201, 300}) {
        t2_c.append(value);
    }
    storage::ColumnarBatch t2;
    t2.add_column("a", std::move(t2_a));
    t2.add_column("c", std::move(t2_c));
    catalog.add_table("t2", std::move(t2));

    storage::Int64Column t3_c;
    for (auto value : {200, 201, 201}) {
        t3_c.append(value);
    }
    storage::Int64Column t3_d;
    for (auto value : {1, 2, 3}) {
        t3_d.append(value);
    }
    storage::ColumnarBatch t3;
    t3.add_column("c", std::move(t3_c));
    t3.add_column("d", std::move(t3_d));
    catalog.add_table("t3", std::move(t3));

    storage::Int64Column t4_d;
    for (auto value : {2, 3, 3}) {
        t4_d.append(value);
    }
    storage::Int64Column t4_e;
    for (auto value : {20, 30, 31}) {
        t4_e.append(value);
    }
    storage::ColumnarBatch t4;
    t4.add_column("d", std::move(t4_d));
    t4.add_column("e", std::move(t4_e));
    catalog.add_table("t4", std::move(t4));

    const std::vector<std::int64_t> extreme_left_values{
        std::numeric_limits<std::int64_t>::min(),
        -1,
        std::numeric_limits<std::int64_t>::max(),
    };
    storage::Int64Column extreme_left_a;
    for (auto value : extreme_left_values) {
        extreme_left_a.append(value);
    }
    storage::Int64Column extreme_left_b;
    for (auto value : {10, 11, 12}) {
        extreme_left_b.append(value);
    }
    storage::ColumnarBatch extreme_left;
    extreme_left.add_column("a", std::move(extreme_left_a));
    extreme_left.add_column("b", std::move(extreme_left_b));
    catalog.add_table("extreme_left", std::move(extreme_left));

    storage::Int64Column extreme_right_a;
    for (auto value : {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
        extreme_right_a.append(value);
    }
    storage::Int64Column extreme_right_b;
    for (auto value : {20, 21}) {
        extreme_right_b.append(value);
    }
    storage::ColumnarBatch extreme_right;
    extreme_right.add_column("a", std::move(extreme_right_a));
    extreme_right.add_column("b", std::move(extreme_right_b));
    catalog.add_table("extreme_right", std::move(extreme_right));

    storage::ColumnarBatch empty;
    empty.add_column("a", storage::Int64Column{});
    catalog.add_table("empty", std::move(empty));
    return catalog;
}

std::string format_batch(const storage::ColumnarBatch& batch) {
    std::ostringstream out;
    const auto& column_order = batch.column_names();
    out << "columns=[";
    for (std::size_t i = 0; i < column_order.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << column_order[i];
    }
    out << "]; rows=[";
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        if (row != 0) {
            out << ",";
        }
        out << "[";
        for (std::size_t col = 0; col < column_order.size(); ++col) {
            if (col != 0) {
                out << ",";
            }
            out << batch.column(column_order[col]).at(row);
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

std::string format_table(const TableSpec& spec) {
    std::ostringstream out;
    out << "table=" << spec.name << " columns=[a,b] rows=[";
    for (std::size_t row = 0; row < spec.a.size(); ++row) {
        if (row != 0) {
            out << ",";
        }
        out << "[" << spec.a[row] << "," << spec.b[row] << "]";
    }
    out << "]";
    return out.str();
}

bool same_batch(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return format_batch(left) == format_batch(right);
}

std::vector<std::string> sorted_column_names(const storage::ColumnarBatch& batch) {
    auto names = batch.column_names();
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::vector<std::int64_t>> sorted_rows_by_column_identity(const storage::ColumnarBatch& batch) {
    const auto names = sorted_column_names(batch);
    std::vector<std::vector<std::int64_t>> rows;
    rows.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        std::vector<std::int64_t> values;
        values.reserve(names.size());
        for (const auto& name : names) {
            values.push_back(batch.column(name).at(row));
        }
        rows.push_back(std::move(values));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

bool same_column_identity_set(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return sorted_column_names(left) == sorted_column_names(right);
}

bool same_column_order(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return left.column_names() == right.column_names();
}

bool same_sorted_bag(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return same_column_identity_set(left, right) &&
           sorted_rows_by_column_identity(left) == sorted_rows_by_column_identity(right);
}

std::string format_sorted_bag(const storage::ColumnarBatch& batch) {
    std::ostringstream out;
    const auto names = sorted_column_names(batch);
    const auto rows = sorted_rows_by_column_identity(batch);
    out << "columns=[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << names[i];
    }
    out << "]; sorted_rows=[";
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (row != 0) {
            out << ",";
        }
        out << "[";
        for (std::size_t col = 0; col < rows[row].size(); ++col) {
            if (col != 0) {
                out << ",";
            }
            out << rows[row][col];
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

std::string format_trace(const std::vector<std::string>& fired_rules) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < fired_rules.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << fired_rules[i];
    }
    out << "]";
    return out.str();
}

bool contains_join(const plan::LogicalPlan& logical) {
    switch (logical.kind) {
    case plan::LogicalKind::Scan:
        return false;
    case plan::LogicalKind::Join:
        return true;
    case plan::LogicalKind::Filter:
    case plan::LogicalKind::Project:
        return logical.input != nullptr && contains_join(*logical.input);
    }
    throw std::logic_error("unreachable logical plan kind");
}

std::size_t join_keyword_count(const std::string& sql) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = sql.find(" JOIN ", pos)) != std::string::npos) {
        ++count;
        pos += 6;
    }
    return count;
}

struct ComparisonStats {
    std::size_t alternative_count{0};
    std::size_t max_group_expression_count{0};
    bool hit_expression_bound{false};
    bool hit_plan_bound{false};
};

bool compare_engines(const std::string& sql,
                     const execution::Catalog& catalog,
                     const std::string& table_text,
                     ComparisonStats* stats = nullptr) {
    const auto parsed = sql::parse_select(sql);
    const auto logical = sql::bind_select(parsed, catalog);
    const auto is_join_query = contains_join(logical);
    if (is_join_query && logical.order_permission != plan::OrderPermission::Arbitrary) {
        std::cerr << "join query did not carry arbitrary-order permission\n"
                  << "sql: " << sql << "\n"
                  << "plan:\n"
                  << plan::to_string(logical) << "\n";
        return false;
    }

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{64, 512});
    if (stats != nullptr) {
        stats->alternative_count = alternatives.plans.size();
        stats->max_group_expression_count = alternatives.max_group_expression_count;
        stats->hit_expression_bound = alternatives.hit_expression_bound;
        stats->hit_plan_bound = alternatives.hit_plan_bound;
    }

    const auto unrewritten_oracle = execution::execute_interpreted(logical, catalog);
    if (alternatives.plans.empty()) {
        std::cerr << "memo alternative extraction returned no plans\n"
                  << "sql: " << sql << "\n"
                  << table_text << "\n"
                  << "memo trace: " << format_trace(explored.fired_rules) << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        return false;
    }

    for (std::size_t alternative_index = 0; alternative_index < alternatives.plans.size(); ++alternative_index) {
        const auto& extracted = alternatives.plans[alternative_index];
        const auto memo_oracle = execution::execute_interpreted(extracted, catalog);
        const auto memo_vectorized = execution::execute_vectorized(extracted, catalog);
        const auto cross_plan_equal = is_join_query ? same_sorted_bag(unrewritten_oracle, memo_oracle)
                                                    : same_batch(unrewritten_oracle, memo_oracle);
        const auto vectorized_equal = same_batch(memo_oracle, memo_vectorized);
        const auto column_sets_match = same_column_identity_set(unrewritten_oracle, memo_oracle) &&
                                       same_column_identity_set(memo_oracle, memo_vectorized);
        const auto output_order_matches = same_column_order(unrewritten_oracle, memo_oracle) &&
                                          same_column_order(memo_oracle, memo_vectorized);
        if (cross_plan_equal && vectorized_equal && column_sets_match && output_order_matches) {
            continue;
        }

        std::cerr << "memo/vectorized divergence\n"
                  << "sql: " << sql << "\n"
                  << table_text << "\n"
                  << "alternative index: " << alternative_index << " of " << alternatives.plans.size() << "\n"
                  << "hit expression bound: " << (alternatives.hit_expression_bound ? "yes" : "no") << "\n"
                  << "hit plan bound: " << (alternatives.hit_plan_bound ? "yes" : "no") << "\n"
                  << "memo trace: " << format_trace(explored.fired_rules) << "\n"
                  << "before plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "memo dump:\n"
                  << memo.dump()
                  << "extracted plan:\n"
                  << plan::to_string(extracted) << "\n"
                  << "unrewritten oracle:     " << format_batch(unrewritten_oracle) << "\n"
                  << "unrewritten bag:        " << format_sorted_bag(unrewritten_oracle) << "\n"
                  << "alternative oracle:     " << format_batch(memo_oracle) << "\n"
                  << "alternative oracle bag: " << format_sorted_bag(memo_oracle) << "\n"
                  << "alternative vectorized: " << format_batch(memo_vectorized) << "\n";
        return false;
    }

    return true;
}

bool run_result_golden_queries() {
    const auto catalog = make_golden_catalog();
    const std::vector<std::string> result_sqls{
        "SELECT a, b, 99 FROM t WHERE a >= 2 AND b <> 40",
        "SELECT a FROM t WHERE a < 3",
        "SELECT b, a FROM t WHERE a = 2",
        "SELECT b FROM t WHERE a <= 3 AND 10 < b",
        "SELECT a FROM t WHERE a > 2",
    };

    bool ok = true;
    for (const auto& sql : result_sqls) {
        ok = compare_engines(sql,
                             catalog,
                             "golden table t: columns=[a,b,c] rows=[[1,10,5],[2,20,6],[3,20,7],[4,40,8]]") &&
             ok;
    }
    return ok;
}

bool run_join_oracle_corpus() {
    const auto catalog = make_golden_catalog();
    const auto table_text =
        "join tables: t1 rows=[[1,10],[2,20],[2,30]], t2 rows=[[2,200],[2,201],[3,300]], "
        "t3 rows=[[200,1],[201,2],[201,3]], t4 rows=[[2,20],[3,30],[3,31]], "
        "extreme_left rows=[[min,10],[-1,11],[max,12]], "
        "extreme_right rows=[[max,20],[min,21]], empty rows=[]";
    const std::vector<std::string> join_sqls{
        "SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a",
        "SELECT empty.a, t1.b FROM empty JOIN t1 ON empty.a = t1.a",
        "SELECT t1.a FROM t1 JOIN empty ON t1.a = empty.a",
        "SELECT t1.a, t2.c FROM t1 JOIN t2 ON t1.a = t2.c",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON 1 = 1",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a AND t1.b < t2.c",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a WHERE 2 > 1 AND t2.c > 200",
        "SELECT t1.b FROM t1 JOIN t2 ON t1.a = t2.a WHERE 2 < 1",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c WHERE t3.d >= 2",
        "SELECT t1.b, t2.c, t3.d, t4.e FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c JOIN t4 ON t3.d = t4.d",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t1.b = t3.d",
        "SELECT extreme_left.a, extreme_left.b, extreme_right.b FROM extreme_left JOIN extreme_right "
        "ON extreme_left.a = extreme_right.a",
    };

    bool ok = true;
    for (const auto& sql : join_sqls) {
        ComparisonStats stats;
        ok = compare_engines(sql, catalog, table_text, &stats) && ok;
        if (join_keyword_count(sql) >= 2) {
            std::cout << "multi-join alternatives verified: sql=\"" << sql << "\" alternatives="
                      << stats.alternative_count << " max_group_expressions=" << stats.max_group_expression_count
                      << " hit_expression_bound=" << (stats.hit_expression_bound ? "yes" : "no")
                      << " hit_plan_bound=" << (stats.hit_plan_bound ? "yes" : "no") << "\n";
        }
    }
    return ok;
}

std::string comparison_op_text(sql::ComparisonOp op) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return "=";
    case sql::ComparisonOp::NotEqual:
        return "<>";
    case sql::ComparisonOp::Less:
        return "<";
    case sql::ComparisonOp::LessEqual:
        return "<=";
    case sql::ComparisonOp::Greater:
        return ">";
    case sql::ComparisonOp::GreaterEqual:
        return ">=";
    }
    throw std::logic_error("unreachable comparison operator");
}

std::string where_clause(std::size_t conjunct_count,
                         bool literal_on_left,
                         sql::ComparisonOp op,
                         std::int64_t literal) {
    if (conjunct_count == 0) {
        return "";
    }

    const auto lit = std::to_string(literal);
    const auto op_text = comparison_op_text(op);
    const auto first = literal_on_left ? lit + " " + op_text + " a" : "a " + op_text + " " + lit;
    if (conjunct_count == 1) {
        return " WHERE " + first;
    }

    const auto second = literal_on_left ? lit + " " + op_text + " b" : "b " + op_text + " " + lit;
    return " WHERE " + first + " AND " + second;
}

bool run_generated_corpus() {
    const auto min = std::numeric_limits<std::int64_t>::min();
    const auto max = std::numeric_limits<std::int64_t>::max();
    const std::vector<TableSpec> tables{
        TableSpec{"empty table", {}, {}},
        TableSpec{"single row", {7}, {7}},
        TableSpec{"all rows match equality literal 7", {7, 7, 7}, {7, 7, 7}},
        TableSpec{"no rows match equality literal 7", {1, 2, 3}, {4, 5, 6}},
        TableSpec{"negative values", {-5, -1, 0, 3}, {-8, -1, 2, 3}},
        TableSpec{"int64 min max", {min, -1, 0, max}, {max, 0, -1, min}},
    };
    const std::vector<sql::ComparisonOp> ops{
        sql::ComparisonOp::Equal,
        sql::ComparisonOp::NotEqual,
        sql::ComparisonOp::Less,
        sql::ComparisonOp::LessEqual,
        sql::ComparisonOp::Greater,
        sql::ComparisonOp::GreaterEqual,
    };
    const std::vector<std::int64_t> literals{min, -1, 0, 1, 7, max};
    const std::vector<std::string> projections{"a", "b, a", "42"};

    bool ok = true;
    for (const auto& table : tables) {
        const auto catalog = make_catalog(table);
        const auto table_text = format_table(table);

        for (const auto& projection : projections) {
            ok = compare_engines("SELECT " + projection + " FROM t", catalog, table_text) && ok;
        }

        for (const auto op : ops) {
            for (const auto literal : literals) {
                for (const auto literal_on_left : {false, true}) {
                    for (std::size_t conjunct_count : {std::size_t{1}, std::size_t{2}}) {
                        const auto sql = "SELECT b, a FROM t" + where_clause(conjunct_count, literal_on_left, op, literal);
                        ok = compare_engines(sql, catalog, table_text) && ok;
                    }
                }
            }
        }

        for (const auto& projection : projections) {
            ok = compare_engines("SELECT " + projection + " FROM t WHERE 2 > 1", catalog, table_text) && ok;
            ok = compare_engines("SELECT " + projection + " FROM t WHERE 2 < 1", catalog, table_text) && ok;
            ok = compare_engines("SELECT " + projection + " FROM t WHERE 2 > 1 AND a = 7", catalog, table_text) && ok;
            ok = compare_engines("SELECT " + projection + " FROM t WHERE a = 7 AND 2 < 1", catalog, table_text) && ok;
        }
    }

    return ok;
}

void assert_physical_lowering_shape() {
    const auto catalog = make_golden_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT b, a FROM t WHERE a >= 2 AND b <> 40"), catalog);
    const auto physical = plan::lower_to_physical(logical);

    assert(physical.kind == plan::PhysicalKind::Project);
    assert(physical.projections.size() == 2);
    assert(physical.input != nullptr);
    assert(physical.input->kind == plan::PhysicalKind::Filter);
    assert(physical.input->predicates.size() == 2);
    assert(physical.input->input != nullptr);
    assert(physical.input->input->kind == plan::PhysicalKind::Scan);
    assert(physical.input->input->table == "t");
}

} // namespace

int main() {
    assert_physical_lowering_shape();

    bool ok = true;
    ok = run_result_golden_queries() && ok;
    ok = run_join_oracle_corpus() && ok;
    ok = run_generated_corpus() && ok;
    return ok ? 0 : 1;
}
