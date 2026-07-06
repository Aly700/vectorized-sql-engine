#include "differential_verifier.hpp"
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
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Cell = std::optional<std::int64_t>;

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

    storage::Int64Column nullable_k;
    nullable_k.append(1);
    nullable_k.append_null();
    nullable_k.append(2);
    nullable_k.append_null();
    storage::Int64Column nullable_v;
    nullable_v.append(10);
    nullable_v.append(20);
    nullable_v.append_null();
    nullable_v.append(30);
    storage::ColumnarBatch nullable;
    nullable.add_column("k", std::move(nullable_k));
    nullable.add_column("v", std::move(nullable_v));
    catalog.add_table("nullable", std::move(nullable));

    storage::Int64Column j1_k;
    j1_k.append(1);
    j1_k.append_null();
    j1_k.append(2);
    j1_k.append_null();
    storage::Int64Column j1_v;
    for (auto value : {10, 20, 30, 40}) {
        j1_v.append(value);
    }
    storage::ColumnarBatch j1;
    j1.add_column("k", std::move(j1_k));
    j1.add_column("v", std::move(j1_v));
    catalog.add_table("j1", std::move(j1));

    storage::Int64Column j2_k;
    j2_k.append_null();
    j2_k.append(1);
    j2_k.append(2);
    j2_k.append_null();
    storage::Int64Column j2_w;
    for (auto value : {100, 101, 102, 103}) {
        j2_w.append(value);
    }
    storage::ColumnarBatch j2;
    j2.add_column("k", std::move(j2_k));
    j2.add_column("w", std::move(j2_w));
    catalog.add_table("j2", std::move(j2));

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
            const auto& column = batch.column(column_order[col]);
            if (column.is_null(row)) {
                out << "NULL";
            } else {
                out << column.at(row);
            }
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

Cell cell_at(const storage::Int64Column& column, std::size_t row) {
    if (column.is_null(row)) {
        return std::nullopt;
    }
    return column.at(row);
}

std::vector<std::vector<Cell>> sorted_rows_by_column_identity(const storage::ColumnarBatch& batch) {
    const auto names = sorted_column_names(batch);
    std::vector<std::vector<Cell>> rows;
    rows.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        std::vector<Cell> values;
        values.reserve(names.size());
        for (const auto& name : names) {
            values.push_back(cell_at(batch.column(name), row));
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
            if (rows[row][col].has_value()) {
                out << *rows[row][col];
            } else {
                out << "NULL";
            }
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

std::optional<std::string> output_column_for_sort_key(const plan::SortKey& key, const storage::ColumnarBatch& batch) {
    const auto qualified = key.column.binding + "." + key.column.column;
    if (batch.has_column(qualified)) {
        return qualified;
    }
    if (batch.has_column(key.column.column)) {
        return key.column.column;
    }
    return std::nullopt;
}

bool is_sorted_by_keys(const storage::ColumnarBatch& batch, const std::vector<plan::SortKey>& keys) {
    for (const auto& key : keys) {
        if (!output_column_for_sort_key(key, batch).has_value()) {
            return false;
        }
    }
    for (std::size_t row = 1; row < batch.row_count(); ++row) {
        bool decided = false;
        for (const auto& key : keys) {
            const auto column_name = *output_column_for_sort_key(key, batch);
            const auto& column = batch.column(column_name);
            const auto previous = cell_at(column, row - 1);
            const auto current = cell_at(column, row);
            if (previous == current) {
                continue;
            }
            if (key.direction == sql::SortDirection::Asc && previous > current) {
                return false;
            }
            if (key.direction == sql::SortDirection::Desc && previous < current) {
                return false;
            }
            decided = true;
            break;
        }
        (void)decided;
    }
    return true;
}

const std::vector<plan::SortKey>* root_sort_keys(const plan::LogicalPlan& logical) {
    if (logical.kind == plan::LogicalKind::Limit) {
        if (logical.input == nullptr) {
            throw std::logic_error("Limit missing input");
        }
        return root_sort_keys(*logical.input);
    }
    if (logical.kind == plan::LogicalKind::Sort) {
        return &logical.sort_keys;
    }
    return nullptr;
}

std::optional<std::size_t> root_limit_count(const plan::LogicalPlan& logical) {
    if (logical.kind == plan::LogicalKind::Limit) {
        return logical.limit_count;
    }
    return std::nullopt;
}

const plan::LogicalPlan& without_root_limit(const plan::LogicalPlan& logical) {
    if (logical.kind != plan::LogicalKind::Limit) {
        return logical;
    }
    if (logical.input == nullptr) {
        throw std::logic_error("Limit missing input");
    }
    return *logical.input;
}

bool root_order_boundary_is_valid(const plan::LogicalPlan& logical) {
    const auto& boundary = without_root_limit(logical);
    if (boundary.kind != plan::LogicalKind::Sort || boundary.input == nullptr) {
        return false;
    }
    return logical.order_permission == plan::OrderPermission::Deterministic &&
           boundary.order_permission == plan::OrderPermission::Deterministic &&
           boundary.input->order_permission == plan::OrderPermission::Arbitrary;
}

std::vector<Cell> row_by_output_order(const storage::ColumnarBatch& batch, std::size_t row) {
    std::vector<Cell> values;
    values.reserve(batch.column_names().size());
    for (const auto& name : batch.column_names()) {
        values.push_back(cell_at(batch.column(name), row));
    }
    return values;
}

std::map<std::vector<Cell>, std::size_t> row_multiset_by_output_order(const storage::ColumnarBatch& batch) {
    std::map<std::vector<Cell>, std::size_t> counts;
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        ++counts[row_by_output_order(batch, row)];
    }
    return counts;
}

bool multiset_contains_rows(const storage::ColumnarBatch& superset, const storage::ColumnarBatch& subset) {
    auto counts = row_multiset_by_output_order(superset);
    for (std::size_t row = 0; row < subset.row_count(); ++row) {
        const auto key = row_by_output_order(subset, row);
        const auto it = counts.find(key);
        if (it == counts.end() || it->second == 0) {
            return false;
        }
        --it->second;
    }
    return true;
}

std::optional<std::vector<Cell>> key_tuple_for_row(const storage::ColumnarBatch& batch,
                                                   const std::vector<plan::SortKey>& keys,
                                                   std::size_t row) {
    std::vector<Cell> tuple;
    tuple.reserve(keys.size());
    for (const auto& key : keys) {
        const auto column_name = output_column_for_sort_key(key, batch);
        if (!column_name.has_value()) {
            return std::nullopt;
        }
        tuple.push_back(cell_at(batch.column(*column_name), row));
    }
    return tuple;
}

std::optional<std::map<std::vector<Cell>, std::size_t>>
key_tuple_multiset_prefix(const storage::ColumnarBatch& batch,
                          const std::vector<plan::SortKey>& keys,
                          std::size_t row_count) {
    std::map<std::vector<Cell>, std::size_t> counts;
    for (std::size_t row = 0; row < row_count; ++row) {
        const auto tuple = key_tuple_for_row(batch, keys, row);
        if (!tuple.has_value()) {
            return std::nullopt;
        }
        ++counts[*tuple];
    }
    return counts;
}

bool valid_limit_answer(const storage::ColumnarBatch& candidate,
                        const storage::ColumnarBatch& full_unlimited_oracle,
                        std::size_t limit_count,
                        const std::vector<plan::SortKey>* order_keys) {
    if (!same_column_order(candidate, full_unlimited_oracle)) {
        return false;
    }
    const auto expected_count = std::min(limit_count, full_unlimited_oracle.row_count());
    if (candidate.row_count() != expected_count) {
        return false;
    }
    if (!multiset_contains_rows(full_unlimited_oracle, candidate)) {
        return false;
    }
    if (order_keys == nullptr) {
        return true;
    }
    if (!is_sorted_by_keys(candidate, *order_keys) || !is_sorted_by_keys(full_unlimited_oracle, *order_keys)) {
        return false;
    }
    const auto expected_key_counts = key_tuple_multiset_prefix(full_unlimited_oracle, *order_keys, expected_count);
    const auto actual_key_counts = key_tuple_multiset_prefix(candidate, *order_keys, candidate.row_count());
    return expected_key_counts.has_value() && actual_key_counts.has_value() &&
           *expected_key_counts == *actual_key_counts;
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
    case plan::LogicalKind::Aggregate:
    case plan::LogicalKind::Sort:
    case plan::LogicalKind::Distinct:
    case plan::LogicalKind::Limit:
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
    differential::ComparisonStats shared_stats;
    const auto ok =
        differential::compare_engines(sql, catalog, table_text, stats == nullptr ? nullptr : &shared_stats);
    if (stats != nullptr) {
        stats->alternative_count = shared_stats.alternative_count;
        stats->max_group_expression_count = shared_stats.max_group_expression_count;
        stats->hit_expression_bound = shared_stats.hit_expression_bound;
        stats->hit_plan_bound = shared_stats.hit_plan_bound;
    }
    return ok;
}

bool run_result_golden_queries() {
    const auto catalog = make_golden_catalog();
    const std::vector<std::string> result_sqls{
        "SELECT a, b, 99 FROM t WHERE a >= 2 AND b <> 40",
        "SELECT a FROM t WHERE a < 3",
        "SELECT b, a FROM t WHERE a = 2",
        "SELECT b FROM t WHERE a <= 3 AND 10 < b",
        "SELECT a FROM t WHERE a > 2",
        "SELECT a, b FROM t ORDER BY b DESC",
        "SELECT a, b FROM t ORDER BY b ASC, a DESC",
        "SELECT b FROM t ORDER BY b ASC",
        "SELECT a AS x, b AS y FROM t ORDER BY x DESC",
        "SELECT b AS a, a AS original FROM t ORDER BY a DESC",
        "SELECT a, b FROM t WHERE a = 1 OR b = 20 AND c = 7",
        "SELECT a FROM t WHERE (a = 1 OR b = 20) AND c >= 6",
        "SELECT a FROM t WHERE 2 > 1 OR a = 5",
        "SELECT a FROM t WHERE 2 < 1 OR a = 2",
        "SELECT NULL FROM t LIMIT 2",
        "SELECT k, v FROM nullable",
        "SELECT k, v FROM nullable WHERE k IS NULL",
        "SELECT k, v FROM nullable WHERE k IS NOT NULL",
        "SELECT k FROM nullable WHERE k = NULL",
        "SELECT k FROM nullable WHERE k = NULL OR 1 = 1",
        "SELECT v FROM nullable WHERE k = NULL OR v = 30",
        "SELECT k FROM nullable WHERE k = NULL AND 1 = 0",
        "SELECT v FROM nullable WHERE k = NULL AND v = 30",
        "SELECT COUNT(*), COUNT(b), SUM(a), MIN(b), MAX(b) FROM t",
        "SELECT b, COUNT(*), SUM(a), MIN(a), MAX(a) FROM t GROUP BY b",
        "SELECT b, COUNT(*) FROM t GROUP BY b ORDER BY b DESC",
        "SELECT a FROM t GROUP BY a HAVING SUM(b) > 20",
        "SELECT a, COUNT(*) FROM t GROUP BY a HAVING SUM(b) > 1000",
        "SELECT b, COUNT(*) FROM t GROUP BY b HAVING b = 10 OR COUNT(*) > 1",
        "SELECT a, COUNT(*) FROM t GROUP BY a HAVING COUNT(*) = NULL",
        "SELECT a, SUM(b) AS total FROM t GROUP BY a HAVING SUM(b) >= 20 ORDER BY total DESC",
        "SELECT a, SUM(b) FROM t GROUP BY a ORDER BY SUM(b) DESC",
        "SELECT DISTINCT b FROM t",
        "SELECT DISTINCT b FROM t ORDER BY b DESC",
        "SELECT a FROM t LIMIT 0",
        "SELECT a FROM t LIMIT 1",
        "SELECT a FROM t LIMIT 2",
        "SELECT a FROM t LIMIT 4",
        "SELECT a FROM t LIMIT 99",
        "SELECT a, b FROM t ORDER BY b DESC LIMIT 2",
        "SELECT DISTINCT b FROM t ORDER BY b ASC LIMIT 2",
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
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a "
        "WHERE t1.b = 20 AND t2.c > 200 AND t1.b < t2.c",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a "
        "WHERE (t1.b = 20 OR t1.b = 30) AND t2.c = 201",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a "
        "WHERE t1.b = 20 OR t2.c = 201",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a AND (t2.c = 200 OR t1.b = 30)",
        "SELECT t1.b FROM t1 JOIN t2 ON t1.a = t2.a WHERE 2 < 1",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c WHERE t3.d >= 2",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c "
        "WHERE ((t1.b = 20 OR t3.d = 3) AND (t2.c = 201 OR t3.d = 2))",
        "SELECT t1.b, t2.c, t3.d, t4.e FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c JOIN t4 ON t3.d = t4.d",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t1.b = t3.d",
        "SELECT extreme_left.a, extreme_left.b, extreme_right.b FROM extreme_left JOIN extreme_right "
        "ON extreme_left.a = extreme_right.a",
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY t2.c DESC, t1.b ASC",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c ORDER BY t3.d DESC, t2.c ASC",
        "SELECT t1.b, t2.c, t3.d, t4.e FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c JOIN t4 ON t3.d = t4.d ORDER BY t4.e DESC, t2.c ASC",
        "SELECT x.a FROM t AS x WHERE x.a >= 2 ORDER BY x.a DESC",
        "SELECT x.b, y.c FROM t1 AS x JOIN t2 AS y ON x.a = y.a ORDER BY y.c DESC, x.b ASC",
        "SELECT x.a, y.b FROM t1 AS x JOIN t1 AS y ON x.a = y.a WHERE x.b = 20 ORDER BY y.b ASC",
        "SELECT l.v, r.w FROM j1 AS l JOIN j2 AS r ON l.k = r.k",
        "SELECT x.v, y.v FROM j1 AS x JOIN j1 AS y ON x.k = y.k",
        "SELECT x.b, y.b, z.b FROM t1 AS x JOIN t1 AS y ON x.a = y.a "
        "JOIN t1 AS z ON y.a = z.a WHERE z.b >= 20",
        "SELECT x.b, y.c, z.d FROM t1 AS x JOIN t2 AS y ON x.a = y.a "
        "JOIN t3 AS z ON y.c = z.c ORDER BY z.d DESC, x.b ASC",
        "SELECT t1.a, COUNT(*), SUM(t2.c), MIN(t2.c), MAX(t2.c) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a",
        "SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a ORDER BY t1.a DESC",
        "SELECT t1.a FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a HAVING SUM(t2.c) > 500",
        "SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "GROUP BY t1.a HAVING t1.a = 2 AND COUNT(*) > 1",
        "SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "GROUP BY t1.a HAVING t1.a = 1 OR t1.a = 2",
        "SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "GROUP BY t1.a HAVING t1.a = 1 OR COUNT(*) > 1",
        "SELECT t1.a AS key, SUM(t2.c) AS total FROM t1 JOIN t2 ON t1.a = t2.a "
        "GROUP BY t1.a HAVING SUM(t2.c) > 500 ORDER BY total DESC",
        "SELECT t1.a, t3.d, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c GROUP BY t1.a, t3.d ORDER BY t3.d DESC, t1.a ASC",
        "SELECT t1.a AS key, SUM(t3.d) AS total FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c GROUP BY t1.a HAVING SUM(t3.d) > 4 ORDER BY total DESC",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c LIMIT 3",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c ORDER BY t3.d DESC LIMIT 3",
        "SELECT DISTINCT t1.a FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c LIMIT 1",
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

    const auto stress_sql =
        "SELECT t1.a, t3.d, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "JOIN t3 ON t2.c = t3.c WHERE t1.b >= 20 AND t3.d >= 2 "
        "GROUP BY t1.a, t3.d HAVING t1.a = 2 AND COUNT(*) > 0";
    ComparisonStats stress_stats;
    ok = compare_engines(stress_sql, catalog, table_text, &stress_stats) && ok;
    std::cout << "pushdown stress alternatives verified: sql=\"" << stress_sql << "\" alternatives="
              << stress_stats.alternative_count << " max_group_expressions=" << stress_stats.max_group_expression_count
              << " hit_expression_bound=" << (stress_stats.hit_expression_bound ? "yes" : "no")
              << " hit_plan_bound=" << (stress_stats.hit_plan_bound ? "yes" : "no") << "\n";
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
        ok = compare_engines("SELECT a FROM t ORDER BY a ASC", catalog, table_text) && ok;
        ok = compare_engines("SELECT b, a FROM t ORDER BY b DESC, a ASC", catalog, table_text) && ok;
        ok = compare_engines("SELECT a AS key FROM t ORDER BY key DESC", catalog, table_text) && ok;
        ok = compare_engines("SELECT b AS a, a AS original FROM t ORDER BY a DESC", catalog, table_text) && ok;
        ok = compare_engines("SELECT COUNT(*) FROM t", catalog, table_text) && ok;
        ok = compare_engines("SELECT a, COUNT(*) FROM t GROUP BY a", catalog, table_text) && ok;
        ok = compare_engines("SELECT a, b, COUNT(*) FROM t GROUP BY a, b", catalog, table_text) && ok;
        ok = compare_engines("SELECT a, COUNT(*), COUNT(b), SUM(b), MIN(b), MAX(b) FROM t GROUP BY a",
                             catalog,
                             table_text) &&
             ok;
        ok = compare_engines("SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY a DESC", catalog, table_text) && ok;
        ok = compare_engines("SELECT a AS key FROM t GROUP BY a HAVING COUNT(*) > 0 ORDER BY key ASC",
                             catalog,
                             table_text) &&
             ok;
        ok = compare_engines("SELECT a, SUM(b) AS total FROM t GROUP BY a HAVING COUNT(*) > 0 ORDER BY total DESC",
                             catalog,
                             table_text) &&
             ok;
        ok = compare_engines("SELECT DISTINCT a FROM t", catalog, table_text) && ok;
        ok = compare_engines("SELECT DISTINCT a, b FROM t ORDER BY a ASC LIMIT 2", catalog, table_text) && ok;
        ok = compare_engines("SELECT a FROM t LIMIT 0", catalog, table_text) && ok;
        ok = compare_engines("SELECT a FROM t LIMIT 1", catalog, table_text) && ok;
        ok = compare_engines("SELECT a FROM t LIMIT 3", catalog, table_text) && ok;

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
            ok = compare_engines("SELECT " + projection + " FROM t WHERE 2 > 1 OR a = 7", catalog, table_text) && ok;
            ok = compare_engines("SELECT " + projection + " FROM t WHERE 2 < 1 OR a = 7", catalog, table_text) && ok;
            ok = compare_engines("SELECT " + projection + " FROM t WHERE (a = 7 OR b = 7) AND 2 > 1",
                                 catalog,
                                 table_text) &&
                 ok;
            ok = compare_engines("SELECT " + projection + " FROM t WHERE a = " + std::to_string(min) +
                                     " OR b = " + std::to_string(max),
                                 catalog,
                                 table_text) &&
                 ok;
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
