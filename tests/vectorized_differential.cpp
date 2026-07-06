#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "plan/physical_plan.hpp"
#include "sql/binder.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
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

bool compare_engines(const std::string& sql, const execution::Catalog& catalog, const std::string& table_text) {
    const auto parsed = sql::parse_select(sql);
    const auto logical = sql::bind_select(parsed, catalog);
    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto extracted = memo.extract(root);

    const auto unrewritten_oracle = execution::execute_interpreted(logical, catalog);
    const auto memo_oracle = execution::execute_interpreted(extracted, catalog);
    const auto memo_vectorized = execution::execute_vectorized(extracted, catalog);
    if (same_batch(unrewritten_oracle, memo_oracle) && same_batch(unrewritten_oracle, memo_vectorized)) {
        return true;
    }

    std::cerr << "memo/vectorized divergence\n"
              << "sql: " << sql << "\n"
              << table_text << "\n"
              << "memo trace: " << format_trace(explored.fired_rules) << "\n"
              << "before plan:\n"
              << plan::to_string(logical) << "\n"
              << "memo dump:\n"
              << memo.dump()
              << "extracted plan:\n"
              << plan::to_string(extracted) << "\n"
              << "unrewritten oracle: " << format_batch(unrewritten_oracle) << "\n"
              << "memo oracle:        " << format_batch(memo_oracle) << "\n"
              << "memo vectorized:    " << format_batch(memo_vectorized) << "\n";
    return false;
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
        "t3 rows=[[200,1],[201,2],[201,3]], extreme_left rows=[[min,10],[-1,11],[max,12]], "
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
        "SELECT extreme_left.a, extreme_left.b, extreme_right.b FROM extreme_left JOIN extreme_right "
        "ON extreme_left.a = extreme_right.a",
    };

    bool ok = true;
    for (const auto& sql : join_sqls) {
        ok = compare_engines(sql, catalog, table_text) && ok;
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
