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

    storage::Int64Column agg_g;
    agg_g.append_null();
    agg_g.append_null();
    agg_g.append(1);
    agg_g.append(1);
    agg_g.append(2);
    agg_g.append(2);
    agg_g.append_null();
    storage::Int64Column agg_h;
    for (auto value : {1, 1, 1, 2, 1, 1, 2}) {
        agg_h.append(value);
    }
    storage::Int64Column agg_x;
    agg_x.append_null();
    agg_x.append(10);
    agg_x.append_null();
    agg_x.append(5);
    agg_x.append_null();
    agg_x.append_null();
    agg_x.append(20);
    storage::ColumnarBatch agg_null;
    agg_null.add_column("g", std::move(agg_g));
    agg_null.add_column("h", std::move(agg_h));
    agg_null.add_column("x", std::move(agg_x));
    catalog.add_table("agg_null", std::move(agg_null));

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

    storage::Int64Column range_order_id;
    storage::Int64Column range_order_peer;
    for (auto value : {0, 1, 2}) {
        range_order_id.append(value);
        range_order_peer.append(0);
    }
    storage::ColumnarBatch range_order;
    range_order.add_column("id", std::move(range_order_id));
    range_order.add_column("peer", std::move(range_order_peer));
    catalog.add_table("range_order", std::move(range_order));

    storage::Int64Column range_values_id;
    for (auto value : {0, 2, 1}) {
        range_values_id.append(value);
    }
    storage::Int64Column range_values_v;
    range_values_v.append(std::numeric_limits<std::int64_t>::max());
    range_values_v.append(-1);
    range_values_v.append(1);
    storage::ColumnarBatch range_values;
    range_values.add_column("id", std::move(range_values_id));
    range_values.add_column("v", std::move(range_values_v));
    catalog.add_table("range_values", std::move(range_values));

    storage::Int64Column range_overflow_k;
    range_overflow_k.append(0);
    range_overflow_k.append(1);
    storage::Int64Column range_overflow_v;
    range_overflow_v.append(std::numeric_limits<std::int64_t>::max());
    range_overflow_v.append(1);
    storage::ColumnarBatch range_overflow;
    range_overflow.add_column("k", std::move(range_overflow_k));
    range_overflow.add_column("v", std::move(range_overflow_v));
    catalog.add_table("range_overflow", std::move(range_overflow));

    storage::Int64Column checked_sum_outer_k;
    checked_sum_outer_k.append(0);
    storage::ColumnarBatch checked_sum_outer;
    checked_sum_outer.add_column("k", std::move(checked_sum_outer_k));
    catalog.add_table("checked_sum_outer", std::move(checked_sum_outer));

    storage::Int64Column checked_sum_left_id;
    storage::Int64Column checked_sum_left_v;
    checked_sum_left_id.append(0);
    checked_sum_left_v.append(std::numeric_limits<std::int64_t>::max());
    checked_sum_left_id.append(1);
    checked_sum_left_v.append(-1);
    checked_sum_left_id.append(2);
    checked_sum_left_v.append(1);
    storage::ColumnarBatch checked_sum_left;
    checked_sum_left.add_column("id", std::move(checked_sum_left_id));
    checked_sum_left.add_column("v", std::move(checked_sum_left_v));
    catalog.add_table("checked_sum_left", std::move(checked_sum_left));

    storage::Int64Column checked_sum_right_id;
    for (auto value : {0, 2, 1}) {
        checked_sum_right_id.append(value);
    }
    storage::ColumnarBatch checked_sum_right;
    checked_sum_right.add_column("id", std::move(checked_sum_right_id));
    catalog.add_table("checked_sum_right", std::move(checked_sum_right));
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

bool sort_cell_less(Cell left, Cell right, sql::SortDirection direction) {
    if (left == right) {
        return false;
    }
    const auto left_is_null = !left.has_value();
    const auto right_is_null = !right.has_value();
    if (left_is_null || right_is_null) {
        return direction == sql::SortDirection::Asc ? right_is_null : left_is_null;
    }
    return direction == sql::SortDirection::Asc ? *left < *right : *left > *right;
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
            if (sort_cell_less(current, previous, key.direction)) {
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
    case plan::LogicalKind::Window:
    case plan::LogicalKind::Sort:
    case plan::LogicalKind::Distinct:
    case plan::LogicalKind::Limit:
        return logical.input != nullptr && contains_join(*logical.input);
    case plan::LogicalKind::Explain:
        return false;
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
        "SELECT COUNT(*), COUNT(k), COUNT(v), SUM(k), MIN(v), MAX(v) FROM nullable",
        "SELECT k, v FROM nullable ORDER BY k ASC",
        "SELECT k, v FROM nullable ORDER BY k DESC",
        "SELECT COUNT(*), COUNT(b), SUM(a), MIN(b), MAX(b) FROM t",
        "SELECT SUM(a) FROM t HAVING COUNT(*) > 0",
        "SELECT SUM(a) FROM t HAVING COUNT(*) > 100",
        "SELECT b, COUNT(*), SUM(a), MIN(a), MAX(a) FROM t GROUP BY b",
        "SELECT b, COUNT(*) FROM t GROUP BY b ORDER BY b DESC",
        "SELECT g, COUNT(*), COUNT(x), SUM(x), MIN(x), MAX(x) FROM agg_null GROUP BY g",
        "SELECT g, h, COUNT(*), SUM(x) FROM agg_null GROUP BY g, h",
        "SELECT g, SUM(x) FROM agg_null GROUP BY g HAVING SUM(x) > 5",
        "SELECT g, SUM(x) FROM agg_null GROUP BY g HAVING SUM(x) IS NULL",
        "SELECT SUM(x), MIN(x), MAX(x), COUNT(x), COUNT(*) FROM agg_null WHERE g = 2",
        "SELECT a FROM t GROUP BY a HAVING SUM(b) > 20",
        "SELECT a, COUNT(*) FROM t GROUP BY a HAVING SUM(b) > 1000",
        "SELECT b, COUNT(*) FROM t GROUP BY b HAVING b = 10 OR COUNT(*) > 1",
        "SELECT a, COUNT(*) FROM t GROUP BY a HAVING COUNT(*) = NULL",
        "SELECT a, SUM(b) AS total FROM t GROUP BY a HAVING SUM(b) >= 20 ORDER BY total DESC",
        "SELECT a, SUM(b) FROM t GROUP BY a ORDER BY SUM(b) DESC",
        "SELECT DISTINCT b FROM t",
        "SELECT DISTINCT g FROM agg_null",
        "SELECT DISTINCT g, h FROM agg_null",
        "SELECT DISTINCT b FROM t ORDER BY b DESC",
        "SELECT a FROM t LIMIT 0",
        "SELECT a FROM t LIMIT 1",
        "SELECT a FROM t LIMIT 2",
        "SELECT a FROM t LIMIT 4",
        "SELECT a FROM t LIMIT 99",
        "SELECT a, b FROM t ORDER BY b DESC LIMIT 2",
        "SELECT DISTINCT b FROM t ORDER BY b ASC LIMIT 2",
        "SELECT SUM(empty.a), MIN(empty.a), MAX(empty.a), COUNT(empty.a), COUNT(*) FROM empty",
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

bool run_outer_join_corpus() {
    const auto catalog = make_golden_catalog();
    const auto table_text =
        "outer join tables: t1 rows=[[1,10],[2,20],[2,30]], "
        "t2 rows=[[2,200],[2,201],[3,300]], t3 rows=[[200,1],[201,2],[201,3]], "
        "j1 rows=[[1,10],[NULL,20],[2,30],[NULL,40]], "
        "j2 rows=[[NULL,100],[1,101],[2,102],[NULL,103]]";
    const std::vector<std::string> outer_sqls{
        "SELECT t1.a, t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a AND t2.c = 201",
        "SELECT t1.a, t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a AND t2.c = 999",
        "SELECT t1.a, t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.b < t2.c AND t2.c < 0",
        "SELECT t1.a, t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t2.c = 201",
        "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t2.c IS NULL",
        "SELECT t2.c, COUNT(*), COUNT(t2.c), SUM(t2.c) FROM t1 LEFT JOIN t2 ON t1.a = t2.a "
        "GROUP BY t2.c ORDER BY t2.c ASC",
        "SELECT l.v, r.w FROM j1 AS l LEFT JOIN j2 AS r ON l.k = r.k",
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a "
        "LEFT JOIN t3 ON t2.c = t3.c AND t3.d = 3 ORDER BY t1.b ASC, t2.c ASC",
        "SELECT t2.c, t1.b FROM t1 RIGHT JOIN t2 ON t1.a = t2.a",
    };

    bool ok = true;
    for (const auto& sql : outer_sqls) {
        ComparisonStats stats;
        ok = compare_engines(sql, catalog, table_text, &stats) && ok;
        std::cout << "outer-join alternatives verified: sql=\"" << sql << "\" alternatives="
                  << stats.alternative_count << " max_group_expressions=" << stats.max_group_expression_count
                  << " hit_expression_bound=" << (stats.hit_expression_bound ? "yes" : "no")
                  << " hit_plan_bound=" << (stats.hit_plan_bound ? "yes" : "no") << "\n";
    }
    return ok;
}

execution::Catalog make_string_catalog() {
    storage::Int64Column id;
    for (auto value : {1, 2, 3, 4, 5}) {
        id.append(value);
    }
    storage::StringColumn s;
    s.append("b");
    s.append("");
    s.append_null();
    s.append("a");
    s.append("");
    storage::ColumnarBatch strings;
    strings.add_column("id", std::move(id));
    strings.add_column("s", std::move(s));

    storage::StringColumn l_k;
    l_k.append("a");
    l_k.append_null();
    l_k.append("b");
    storage::Int64Column l_v;
    for (auto value : {1, 2, 3}) {
        l_v.append(value);
    }
    storage::ColumnarBatch left;
    left.add_column("k", std::move(l_k));
    left.add_column("v", std::move(l_v));

    storage::StringColumn r_k;
    r_k.append_null();
    r_k.append("b");
    r_k.append("a");
    storage::Int64Column r_w;
    for (auto value : {10, 11, 12}) {
        r_w.append(value);
    }
    storage::ColumnarBatch right;
    right.add_column("k", std::move(r_k));
    right.add_column("w", std::move(r_w));

    execution::Catalog catalog;
    catalog.add_table("strings", std::move(strings));
    catalog.add_table("string_left", std::move(left));
    catalog.add_table("string_right", std::move(right));
    return catalog;
}

bool run_vectorized_string_corpus() {
    const auto catalog = make_string_catalog();
    const auto table_text =
        "string tables: strings rows=[[1,b],[2,],[3,NULL],[4,a],[5,]], "
        "string_left rows=[[a,1],[NULL,2],[b,3]], "
        "string_right rows=[[NULL,10],[b,11],[a,12]]";
    const std::vector<std::string> sqls{
        "SELECT id, s FROM strings WHERE s = 'a' OR s = '' ORDER BY s ASC, id ASC",
        "SELECT l.v, r.w FROM string_left AS l JOIN string_right AS r ON l.k = r.k ORDER BY l.v ASC",
        "SELECT x.v, y.v FROM string_left AS x JOIN string_left AS y ON x.k = y.k ORDER BY x.v ASC, y.v ASC",
        "SELECT MIN(s), MAX(s), COUNT(s), COUNT(*) FROM strings",
        "SELECT s, COUNT(*) FROM strings GROUP BY s ORDER BY s ASC",
        "SELECT DISTINCT s FROM strings ORDER BY s DESC LIMIT 3",
        "SELECT s FROM strings ORDER BY s ASC LIMIT 3",
        "SELECT id, s, 'x' AS literal FROM strings WHERE s IS NOT NULL ORDER BY id ASC",
        "SELECT id, s FROM strings WHERE s = '' OR s IS NULL ORDER BY id ASC",
        "EXPLAIN SELECT id, s FROM strings WHERE s IS NULL",
    };

    bool ok = true;
    for (const auto& sql : sqls) {
        ok = differential::compare_engines(sql, catalog, table_text) && ok;
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

bool run_subquery_execution_corpus() {
    const auto catalog = make_golden_catalog();
    const std::string table_text = "phase21a deterministic subquery catalog";
    bool ok = true;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a = (SELECT MAX(a) FROM t1)", catalog, table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a NOT IN (SELECT k FROM nullable)", catalog, table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE NOT EXISTS (SELECT a FROM t LIMIT 0)", catalog, table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a IN "
             "(SELECT t1.a FROM t1 JOIN t2 ON t1.a = t2.a WHERE EXISTS (SELECT a FROM empty))",
             catalog,
             table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a = 4 OR a IN (SELECT k FROM nullable)", catalog, table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a = 4 OR EXISTS (SELECT a FROM empty)", catalog, table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a = 4 OR a = (SELECT MAX(a) FROM t1)", catalog, table_text) &&
         ok;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE a NOT IN (SELECT k FROM nullable) OR a = 4", catalog, table_text) &&
         ok;

    differential::ComparisonStats error_stats;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE (SELECT a FROM t) = a", catalog, table_text, &error_stats) &&
         ok;
    if (error_stats.accepted_error_path_count != error_stats.execution_path_count) {
        std::cerr << "scalar subquery cardinality error was not equivalent across every execution path\n"
                  << "accepted errors: " << error_stats.accepted_error_path_count << "\n"
                  << "execution paths: " << error_stats.execution_path_count << "\n";
        ok = false;
    }

    differential::ComparisonStats empty_outer_error_stats;
    ok = differential::compare_engines(
             "SELECT a FROM empty WHERE (SELECT a FROM t) = a",
             catalog,
             table_text,
             &empty_outer_error_stats) &&
         ok;
    if (empty_outer_error_stats.accepted_error_path_count !=
        empty_outer_error_stats.execution_path_count) {
        std::cerr << "empty outer input suppressed an eager scalar subquery cardinality error\n"
                  << "accepted errors: " << empty_outer_error_stats.accepted_error_path_count << "\n"
                  << "execution paths: " << empty_outer_error_stats.execution_path_count << "\n";
        ok = false;
    }


    differential::ComparisonStats folded_error_stats;
    ok = differential::compare_engines(
             "SELECT a FROM t WHERE 1 = 0 AND (SELECT a FROM t) = a",
             catalog,
             table_text,
             &folded_error_stats) &&
         ok;
    if (folded_error_stats.accepted_error_path_count != folded_error_stats.execution_path_count) {
        std::cerr << "constant folding erased an eager scalar subquery cardinality error\n"
                  << "accepted errors: " << folded_error_stats.accepted_error_path_count << "\n"
                  << "oracle paths: " << folded_error_stats.execution_path_count << "\n";
        ok = false;
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

void assert_physical_lowering_accepts_string_touching_plans() {
    storage::StringColumn s;
    s.append("a");
    s.append_null();
    storage::ColumnarBatch batch;
    batch.add_column("s", std::move(s));

    execution::Catalog catalog;
    catalog.add_table("strings", std::move(batch));

    const auto string_column_plan =
        sql::bind_select(sql::parse_select("SELECT s FROM strings WHERE s = 'a'"), catalog);
    const auto physical = plan::lower_to_physical(string_column_plan);
    assert(physical.kind == plan::PhysicalKind::Project);
    const auto interpreted = execution::execute_interpreted(string_column_plan, catalog);
    const auto vectorized = execution::execute_vectorized(string_column_plan, catalog);
    assert(differential::same_batch(interpreted, vectorized));

    const auto int_catalog = make_golden_catalog();
    const auto string_literal_plan =
        sql::bind_select(sql::parse_select("SELECT 'x' AS x FROM t LIMIT 1"), int_catalog);
    const auto literal_interpreted = execution::execute_interpreted(string_literal_plan, int_catalog);
    const auto literal_vectorized = execution::execute_vectorized(string_literal_plan, int_catalog);
    assert(differential::same_batch(literal_interpreted, literal_vectorized));
}

void assert_outer_join_physical_lowering_preserves_kind() {
    const auto catalog = make_golden_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a"), catalog);

    const auto physical = plan::lower_to_physical(logical);
    assert(physical.kind == plan::PhysicalKind::Project);
    assert(physical.input != nullptr);
    assert(physical.input->kind == plan::PhysicalKind::Join);
    assert(physical.input->join_kind == plan::JoinKind::Left);
}

plan::BoundPredicate equi_join_predicate(std::string left_binding,
                                         std::string left_column,
                                         std::string right_binding,
                                         std::string right_column,
                                         catalog::ColumnType type = catalog::ColumnType::Int64) {
    return plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundColumnRef{std::move(left_binding), std::move(left_column), 0, type},
        sql::ComparisonOp::Equal,
        plan::BoundColumnRef{std::move(right_binding), std::move(right_column), 0, type},
        0,
    });
}

plan::BoundPredicate literal_membership_predicate(std::int64_t value,
                                                  std::string right_binding,
                                                  std::string right_column) {
    return plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundScalarExpr{sql::IntLiteral{value, 0}},
        sql::ComparisonOp::Equal,
        plan::BoundColumnRef{std::move(right_binding), std::move(right_column), 0},
        0,
    });
}

void assert_interpreted_semi_anti_join_contract() {
    const auto catalog = make_golden_catalog();

    const auto semi = plan::LogicalPlan::join(
        {equi_join_predicate("t1", "a", "t2", "a")},
        plan::LogicalPlan::scan("t1"),
        plan::LogicalPlan::scan("t2"),
        plan::JoinKind::Semi);
    const auto semi_result = execution::execute_interpreted(semi, catalog);
    const auto semi_vectorized = execution::execute_vectorized(semi, catalog);
    assert(format_batch(semi_result) == "columns=[t1.a,t1.b]; rows=[[2,20],[2,30]]");
    assert(differential::same_batch(semi_result, semi_vectorized));
    assert(plan::to_string(semi).find("SemiJoin[") == 0);
    assert(plan::lower_to_physical(semi).join_kind == plan::JoinKind::Semi);

    const auto anti = plan::LogicalPlan::join(
        {equi_join_predicate("j1", "k", "j2", "k")},
        plan::LogicalPlan::scan("j1"),
        plan::LogicalPlan::scan("j2"),
        plan::JoinKind::Anti);
    const auto anti_result = execution::execute_interpreted(anti, catalog);
    const auto anti_vectorized = execution::execute_vectorized(anti, catalog);
    assert(format_batch(anti_result) == "columns=[j1.k,j1.v]; rows=[[NULL,20],[NULL,40]]");
    assert(differential::same_batch(anti_result, anti_vectorized));
    assert(plan::to_string(anti).find("AntiJoin[") == 0);
    assert(plan::lower_to_physical(anti).join_kind == plan::JoinKind::Anti);
}

void assert_null_aware_anti_join_contract() {
    const auto catalog = make_golden_catalog();
    const auto assert_same = [&](const plan::LogicalPlan& logical, const std::string& expected) {
        const auto interpreted = execution::execute_interpreted(logical, catalog);
        const auto vectorized = execution::execute_vectorized(logical, catalog);
        const auto physical_vectorized = execution::execute_vectorized(plan::lower_to_physical(logical), catalog);
        assert(format_batch(interpreted) == expected);
        assert(differential::same_batch(interpreted, vectorized));
        assert(differential::same_batch(interpreted, physical_vectorized));
    };

    const auto empty_right = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate("nullable", "k", "empty", "a"),
        plan::LogicalPlan::scan("nullable"),
        plan::LogicalPlan::scan("empty"));
    assert_same(empty_right,
                "columns=[nullable.k,nullable.v]; "
                "rows=[[1,10],[NULL,20],[2,NULL],[NULL,30]]");
    assert(plan::to_string(empty_right) ==
           "NullAwareAntiJoin[candidates=[], membership=col(nullable.k) = col(empty.a)]\n"
           "  Scan[nullable]\n"
           "  Scan[empty]");
    const auto empty_physical = plan::lower_to_physical(empty_right);
    assert(empty_physical.join_kind == plan::JoinKind::NullAwareAnti);
    assert(empty_physical.null_aware_predicate.has_value());

    const auto null_bearing_right = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate("j1", "k", "j2", "k"),
        plan::LogicalPlan::scan("j1"),
        plan::LogicalPlan::scan("j2"));
    assert_same(null_bearing_right, "columns=[j1.k,j1.v]; rows=[]");

    const auto null_free_right = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate("t", "a", "t1", "a"),
        plan::LogicalPlan::scan("t"),
        plan::LogicalPlan::scan("t1"));
    assert_same(null_free_right, "columns=[t.a,t.b,t.c]; rows=[[3,20,7],[4,40,8]]");

    const auto null_left_nonempty_right = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate("nullable", "k", "t1", "a"),
        plan::LogicalPlan::scan("nullable"),
        plan::LogicalPlan::scan("t1"));
    assert_same(null_left_nonempty_right, "columns=[nullable.k,nullable.v]; rows=[]");

    const auto correlated = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate("l", "v", "r", "v"),
        plan::LogicalPlan::scan("nullable", "l"),
        plan::LogicalPlan::scan("nullable", "r"),
        {equi_join_predicate("l", "k", "r", "k")});
    assert_same(correlated, "columns=[l.k,l.v]; rows=[[NULL,20],[NULL,30]]");

    const auto right_non_null = plan::LogicalPlan::filter(
        {plan::BoundPredicate::null_check_expr(
            sql::PredicateKind::IsNotNull,
            plan::BoundScalarExpr{plan::BoundColumnRef{"r", "v", 0}},
            0)},
        plan::LogicalPlan::scan("nullable", "r"));
    const auto correlated_empty_for_null_membership = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate("l", "v", "r", "v"),
        plan::LogicalPlan::scan("nullable", "l"),
        right_non_null,
        {equi_join_predicate("l", "k", "r", "k")});
    assert_same(correlated_empty_for_null_membership,
                "columns=[l.k,l.v]; rows=[[NULL,20],[2,NULL],[NULL,30]]");

    const auto fallback = plan::LogicalPlan::null_aware_anti(
        literal_membership_predicate(5, "t1", "a"),
        plan::LogicalPlan::scan("t"),
        plan::LogicalPlan::scan("t1"));
    assert_same(fallback,
                "columns=[t.a,t.b,t.c]; rows=[[1,10,5],[2,20,6],[3,20,7],[4,40,8]]");

    const auto plain_anti = plan::LogicalPlan::join(
        {equi_join_predicate("j1", "k", "j2", "k")},
        plan::LogicalPlan::scan("j1"),
        plan::LogicalPlan::scan("j2"),
        plan::JoinKind::Anti);
    assert(format_batch(execution::execute_interpreted(plain_anti, catalog)) ==
           "columns=[j1.k,j1.v]; rows=[[NULL,20],[NULL,40]]");
    assert(differential::same_batch(execution::execute_interpreted(plain_anti, catalog),
                                    execution::execute_vectorized(plain_anti, catalog)));
    assert(format_batch(execution::execute_interpreted(null_bearing_right, catalog)) ==
           "columns=[j1.k,j1.v]; rows=[]");

    try {
        (void)plan::LogicalPlan::join({},
                                     plan::LogicalPlan::scan("t"),
                                     plan::LogicalPlan::scan("t1"),
                                     plan::JoinKind::NullAwareAnti);
        throw std::logic_error("expected explicit NullAwareAnti factory guard");
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()) ==
               "NullAwareAnti must be constructed with an explicit membership equality");
    }

    const auto non_equality = plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundColumnRef{"t", "a", 0},
        sql::ComparisonOp::Less,
        plan::BoundColumnRef{"t1", "a", 0},
        0,
    });
    try {
        (void)plan::LogicalPlan::null_aware_anti(
            non_equality, plan::LogicalPlan::scan("t"), plan::LogicalPlan::scan("t1"));
        throw std::logic_error("expected NullAwareAnti equality guard");
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()) ==
               "NullAwareAnti membership predicate must be an equality");
    }

    const auto mismatched_equality = plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundColumnRef{"t", "a", 0, catalog::ColumnType::Int64},
        sql::ComparisonOp::Equal,
        plan::BoundColumnRef{"string_right", "k", 0, catalog::ColumnType::String},
        0,
    });
    try {
        (void)plan::LogicalPlan::null_aware_anti(
            mismatched_equality,
            plan::LogicalPlan::scan("t"),
            plan::LogicalPlan::scan("string_right"));
        throw std::logic_error("expected NullAwareAnti type guard");
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()) ==
               "NullAwareAnti membership equality must have matching types");
    }

    auto missing_membership = plan::LogicalPlan::join(
        {}, plan::LogicalPlan::scan("t"), plan::LogicalPlan::scan("t1"));
    missing_membership.join_kind = plan::JoinKind::NullAwareAnti;
    try {
        (void)plan::lower_to_physical(missing_membership);
        throw std::logic_error("expected malformed NullAwareAnti lowering guard");
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()) ==
               "NullAwareAnti join is missing its membership equality");
    }

    auto extra_membership = plan::LogicalPlan::join(
        {}, plan::LogicalPlan::scan("t"), plan::LogicalPlan::scan("t1"));
    extra_membership.null_aware_predicate = equi_join_predicate("t", "a", "t1", "a");
    try {
        (void)plan::lower_to_physical(extra_membership);
        throw std::logic_error("expected non-NullAwareAnti membership-field guard");
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()) ==
               "non-NullAwareAnti join owns a membership equality");
    }
}

void assert_string_null_aware_anti_join_contract() {
    storage::StringColumn left_k;
    left_k.append("a");
    left_k.append_null();
    left_k.append("c");
    storage::Int64Column left_v;
    for (auto value : {1, 2, 3}) {
        left_v.append(value);
    }
    storage::ColumnarBatch left;
    left.add_column("k", std::move(left_k));
    left.add_column("v", std::move(left_v));

    storage::StringColumn right_k;
    right_k.append("a");
    right_k.append("b");
    storage::ColumnarBatch right;
    right.add_column("k", std::move(right_k));

    execution::Catalog catalog;
    catalog.add_table("string_na_left", std::move(left));
    catalog.add_table("string_na_right", std::move(right));
    const auto logical = plan::LogicalPlan::null_aware_anti(
        equi_join_predicate(
            "string_na_left", "k", "string_na_right", "k", catalog::ColumnType::String),
        plan::LogicalPlan::scan("string_na_left"),
        plan::LogicalPlan::scan("string_na_right"));
    const auto interpreted = execution::execute_interpreted(logical, catalog);
    const auto vectorized = execution::execute_vectorized(logical, catalog);
    assert(differential::same_batch(interpreted, vectorized));
    assert(interpreted.row_count() == 1);
    assert(interpreted.string_column("string_na_left.k").at(0) == "c");
    assert(interpreted.column("string_na_left.v").at(0) == 3);
}

void assert_subquery_physical_lowering_and_vectorized_execution_are_supported() {
    const auto catalog = make_golden_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT a FROM t WHERE a IN (SELECT a FROM t1)"), catalog);
    const auto physical = plan::lower_to_physical(logical);
    assert(physical.kind == plan::PhysicalKind::Project);
    const auto interpreted = execution::execute_interpreted(logical, catalog);
    const auto vectorized = execution::execute_vectorized(logical, catalog);
    const auto physical_vectorized = execution::execute_vectorized(physical, catalog);
    assert(differential::same_batch(interpreted, vectorized));
    assert(differential::same_batch(interpreted, physical_vectorized));
}

void assert_residual_correlated_subquery_is_rejected_at_physical_lowering() {
    const auto catalog = make_golden_catalog();
    const std::vector<std::pair<std::string, std::size_t>> blocked{
        {"SELECT a FROM t WHERE EXISTS (SELECT t1.a FROM t1 WHERE t1.a > t.a)", 22},
        {"SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE t1.a > t.a)", 28},
    };
    for (const auto& [sql_text, position] : blocked) {
        const auto logical = sql::bind_select(sql::parse_select(sql_text), catalog);
        try {
            (void)plan::lower_to_physical(logical);
        } catch (const std::exception& error) {
            const auto expected =
                "vectorized execution does not support residual correlated subqueries at position " +
                std::to_string(position);
            if (std::string(error.what()) != expected) {
                throw std::logic_error("unexpected residual correlation guard: " +
                                       std::string(error.what()));
            }
            continue;
        }
        throw std::logic_error("expected residual correlated subquery lowering guard");
    }
}

void assert_correlated_scalar_error_is_equivalent_on_every_oracle_path() {
    const auto catalog = make_golden_catalog();
    const auto sql_text =
        "SELECT a FROM t WHERE (SELECT t1.b FROM t1 WHERE t1.a = t.a) = b";
    const auto logical = sql::bind_select(sql::parse_select(sql_text), catalog);
    const auto expected_error = "scalar subquery at position 22 returned more than one row";
    const auto assert_oracle_error = [&](const plan::LogicalPlan& candidate) {
        try {
            (void)execution::execute_interpreted(candidate, catalog);
        } catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == expected_error);
            return;
        }
        throw std::logic_error("expected per-row correlated scalar cardinality error");
    };
    const auto assert_guard = [&](const plan::LogicalPlan& candidate) {
        try {
            (void)plan::lower_to_physical(candidate);
        } catch (const std::runtime_error& error) {
            assert(std::string(error.what()) ==
                   "vectorized execution does not support residual correlated subqueries at position 22");
            return;
        }
        throw std::logic_error("expected correlated scalar lowering guard");
    };

    assert_oracle_error(logical);
    assert_guard(logical);
    const auto rewritten = optimizer::rewrite_to_fixpoint(logical, optimizer::default_rules());
    assert_oracle_error(rewritten.plan);
    assert_guard(rewritten.plan);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    (void)optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto alternatives = memo.extract_alternatives(root);
    for (const auto& alternative : alternatives.plans) {
        assert_oracle_error(alternative);
        assert_guard(alternative);
    }
    const auto best = memo.extract_best(root, catalog);
    assert_oracle_error(best);
    assert_guard(best);
}

void assert_window_paths_and_vectorized_execution() {
    const auto catalog = make_golden_catalog();
    const auto sql_text =
        "SELECT t1.b, t2.c, RANK() OVER (ORDER BY t2.c) AS r "
        "FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY t2.c, t1.b";
    const auto logical = sql::bind_select(sql::parse_select(sql_text), catalog);
    const auto expected = execution::execute_interpreted(logical, catalog);

    const auto assert_path = [&](const plan::LogicalPlan& candidate) {
        const auto oracle = execution::execute_interpreted(candidate, catalog);
        assert(differential::same_batch(expected, oracle));
        const auto physical = plan::lower_to_physical(candidate);
        assert(physical.kind == plan::PhysicalKind::Sort);
        assert(physical.input != nullptr);
        assert(physical.input->kind == plan::PhysicalKind::Project);
        assert(physical.input->input != nullptr);
        assert(physical.input->input->kind == plan::PhysicalKind::Window);
        const auto vectorized = execution::execute_vectorized(candidate, catalog);
        const auto physical_vectorized = execution::execute_vectorized(physical, catalog);
        assert(differential::same_batch(expected, vectorized));
        assert(differential::same_batch(expected, physical_vectorized));
    };

    assert_path(logical);
    const auto rewritten = optimizer::rewrite_to_fixpoint(logical, optimizer::default_rules());
    assert_path(rewritten.plan);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    (void)optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto alternatives = memo.extract_alternatives(root);
    assert(alternatives.plans.size() > 1);
    for (const auto& alternative : alternatives.plans) {
        assert_path(alternative);
    }
    assert_path(memo.extract_best(root, catalog));
}

void assert_running_window_paths_and_error_equivalence() {
    const auto catalog = make_golden_catalog();
    const std::string context = "phase23 deterministic running-frame catalog";

    differential::ComparisonStats range_stats;
    const auto range_sql =
        "SELECT o.id, v.v, SUM(v.v) OVER (ORDER BY o.peer) AS running_sum "
        "FROM range_order AS o JOIN range_values AS v ON o.id = v.id";
    assert(differential::compare_engines(range_sql, catalog, context, &range_stats));
    assert(range_stats.alternative_count > 1);
    assert(range_stats.join_commute_firings > 0);
    assert(range_stats.accepted_error_path_count == 0);

    differential::ComparisonStats rows_overflow_stats;
    const auto rows_overflow_sql =
        "SELECT o.id, v.v, "
        "SUM(v.v) OVER (ORDER BY o.peer ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_sum "
        "FROM range_order AS o JOIN range_values AS v ON o.id = v.id";
    assert(differential::compare_engines(
        rows_overflow_sql, catalog, context, &rows_overflow_stats));
    assert(rows_overflow_stats.join_commute_firings == 0);
    assert(rows_overflow_stats.execution_path_count > 0);
    assert(rows_overflow_stats.accepted_error_path_count ==
           rows_overflow_stats.execution_path_count);

    differential::ComparisonStats range_overflow_stats;
    const auto range_overflow_sql =
        "SELECT SUM(v) OVER (ORDER BY k) AS running_sum FROM range_overflow";
    assert(differential::compare_engines(
        range_overflow_sql, catalog, context, &range_overflow_stats));
    assert(range_overflow_stats.execution_path_count > 0);
    assert(range_overflow_stats.accepted_error_path_count ==
           range_overflow_stats.execution_path_count);

    assert(differential::compare_engines(
        "SELECT a, "
        "SUM(a) OVER (ORDER BY b ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_sum "
        "FROM t WHERE a <> 2",
        catalog,
        context));
}

void assert_plain_checked_sum_pins_join_order() {
    const auto catalog = make_golden_catalog();
    const std::string context =
        "checked_sum_left(id,v)=[(0,INT64_MAX),(1,-1),(2,1)]; "
        "checked_sum_right(id)=[(0),(2),(1)]";
    differential::ComparisonStats stats;
    const auto sql_text =
        "SELECT SUM(a.v) AS total "
        "FROM checked_sum_left AS a "
        "JOIN checked_sum_right AS b ON a.id = b.id";

    assert(differential::compare_engines(sql_text, catalog, context, &stats));
    assert(stats.execution_path_count > 0);
    assert(stats.join_commute_firings == 0);
    assert(stats.accepted_error_path_count == 0);
}

void assert_opaque_checked_sum_subquery_pins_after_decorrelation() {
    const auto catalog = make_golden_catalog();
    const std::string context =
        "checked_sum_outer(k)=[(0)]; "
        "checked_sum_left(id,v)=[(0,INT64_MAX),(1,-1),(2,1)]; "
        "checked_sum_right(id)=[(0),(2),(1)]";
    differential::ComparisonStats stats;
    const auto sql_text =
        "SELECT o.k, COUNT(*) OVER (ORDER BY o.k) AS running_n "
        "FROM checked_sum_outer AS o "
        "WHERE EXISTS ("
        "SELECT SUM(a.v) FROM checked_sum_left AS a "
        "JOIN checked_sum_right AS b ON a.id = b.id)";

    assert(differential::compare_engines(sql_text, catalog, context, &stats));
    assert(stats.execution_path_count > 0);
    assert(stats.alternative_count > 1);
    assert(stats.exists_to_semi_firings > 0);
    assert(stats.join_commute_firings == 0);
    assert(stats.accepted_error_path_count == 0);
}

const plan::LogicalPlan* find_null_aware_anti(const plan::LogicalPlan& logical) {
    if (logical.kind == plan::LogicalKind::Join &&
        logical.join_kind == plan::JoinKind::NullAwareAnti) {
        return &logical;
    }
    if (logical.input != nullptr) {
        if (const auto* found = find_null_aware_anti(*logical.input); found != nullptr) {
            return found;
        }
    }
    if (logical.left != nullptr) {
        if (const auto* found = find_null_aware_anti(*logical.left); found != nullptr) {
            return found;
        }
    }
    if (logical.right != nullptr) {
        return find_null_aware_anti(*logical.right);
    }
    return nullptr;
}

const plan::LogicalPlan* find_aggregate(const plan::LogicalPlan& logical) {
    if (logical.kind == plan::LogicalKind::Aggregate) {
        return &logical;
    }
    if (logical.input != nullptr) {
        if (const auto* found = find_aggregate(*logical.input); found != nullptr) {
            return found;
        }
    }
    if (logical.left != nullptr) {
        if (const auto* found = find_aggregate(*logical.left); found != nullptr) {
            return found;
        }
    }
    if (logical.right != nullptr) {
        return find_aggregate(*logical.right);
    }
    return nullptr;
}

void assert_not_in_checked_sum_subplan_keeps_order_pin() {
    const auto catalog = make_golden_catalog();
    const auto sql_text =
        "SELECT o.k FROM checked_sum_outer AS o WHERE o.k NOT IN ("
        "SELECT SUM(a.v) FROM checked_sum_left AS a "
        "JOIN checked_sum_right AS b ON a.id = b.id)";
    const auto logical = sql::bind_select(sql::parse_select(sql_text), catalog);
    const auto expected = execution::execute_interpreted(logical, catalog);
    assert(format_batch(expected) == "columns=[o.k]; rows=[[0]]");

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored =
        optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(std::find(explored.fired_rules.begin(),
                     explored.fired_rules.end(),
                     "NotInToNullAwareAntiJoinRule") != explored.fired_rules.end());
    assert(std::find(explored.fired_rules.begin(), explored.fired_rules.end(), "JoinCommuteRule") ==
           explored.fired_rules.end());
    assert(std::find(explored.fired_rules.begin(), explored.fired_rules.end(), "JoinAssociateRule") ==
           explored.fired_rules.end());

    const auto alternatives = memo.extract_alternatives(
        root, optimizer::AlternativeExtractionOptions{128, 1024});
    bool saw_materialized = false;
    bool saw_native = false;
    for (const auto& alternative : alternatives.plans) {
        saw_materialized = saw_materialized ||
                           plan::to_string(alternative).find("NOT IN subquery(") != std::string::npos;
        const auto* null_aware = find_null_aware_anti(alternative);
        if (null_aware != nullptr) {
            saw_native = true;
            assert(null_aware->right != nullptr);
            const auto* aggregate = find_aggregate(*null_aware->right);
            assert(aggregate != nullptr && aggregate->input != nullptr);
            assert(aggregate->input->kind == plan::LogicalKind::Join);
            assert(aggregate->input->order_permission == plan::OrderPermission::Deterministic);
        }
        const auto oracle = execution::execute_interpreted(alternative, catalog);
        const auto vectorized = execution::execute_vectorized(alternative, catalog);
        assert(differential::same_batch(expected, oracle));
        assert(differential::same_batch(expected, vectorized));
    }
    assert(saw_materialized && saw_native);
    memo.assert_invariants();
}

void assert_order_insensitive_aggregate_keeps_join_alternatives() {
    const auto catalog = make_golden_catalog();
    const std::string context = "checked SUM pin negative control";
    differential::ComparisonStats stats;
    const auto sql_text =
        "SELECT COUNT(*) AS n "
        "FROM checked_sum_left AS a "
        "JOIN checked_sum_right AS b ON a.id = b.id";

    assert(differential::compare_engines(sql_text, catalog, context, &stats));
    assert(stats.alternative_count > 1);
    assert(stats.join_commute_firings > 0);
    assert(stats.accepted_error_path_count == 0);
}

} // namespace

int main() {
    assert_physical_lowering_shape();
    assert_physical_lowering_accepts_string_touching_plans();
    assert_outer_join_physical_lowering_preserves_kind();
    assert_interpreted_semi_anti_join_contract();
    assert_null_aware_anti_join_contract();
    assert_string_null_aware_anti_join_contract();
    assert_subquery_physical_lowering_and_vectorized_execution_are_supported();
    assert_residual_correlated_subquery_is_rejected_at_physical_lowering();
    assert_correlated_scalar_error_is_equivalent_on_every_oracle_path();
    assert_window_paths_and_vectorized_execution();
    assert_running_window_paths_and_error_equivalence();
    assert_opaque_checked_sum_subquery_pins_after_decorrelation();
    assert_not_in_checked_sum_subplan_keeps_order_pin();
    assert_plain_checked_sum_pins_join_order();
    assert_order_insensitive_aggregate_keeps_join_alternatives();

    bool ok = true;
    ok = run_result_golden_queries() && ok;
    ok = run_join_oracle_corpus() && ok;
    ok = run_outer_join_corpus() && ok;
    ok = run_vectorized_string_corpus() && ok;
    ok = run_subquery_execution_corpus() && ok;
    ok = run_generated_corpus() && ok;
    return ok ? 0 : 1;
}
