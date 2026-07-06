#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
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

bool compare_engines(const std::string& sql, const execution::Catalog& catalog, const std::string& table_text) {
    const auto parsed = sql::parse_select(sql);
    const auto logical = sql::bind_select(parsed, catalog);
    const auto interpreted = execution::execute_interpreted(logical, catalog);
    const auto vectorized = execution::execute_vectorized(logical, catalog);
    if (same_batch(interpreted, vectorized)) {
        return true;
    }

    std::cerr << "vectorized divergence\n"
              << "sql: " << sql << "\n"
              << table_text << "\n"
              << "interpreted: " << format_batch(interpreted) << "\n"
              << "vectorized:   " << format_batch(vectorized) << "\n";
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
        ok = compare_engines(sql, catalog, "golden table t: columns=[a,b,c] rows=[[1,10,5],[2,20,6],[3,20,7],[4,40,8]]") && ok;
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
    ok = run_generated_corpus() && ok;
    return ok ? 0 : 1;
}
