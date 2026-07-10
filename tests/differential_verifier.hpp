#pragma once

#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "plan/physical_plan.hpp"
#include "sql/binder.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace differential {

struct Cell {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool is_null{true};
    std::int64_t int_value{0};
    std::string string_value;
};

inline bool operator==(const Cell& left, const Cell& right) {
    if (left.is_null || right.is_null) {
        return left.is_null == right.is_null && left.type == right.type;
    }
    if (left.type != right.type) {
        return false;
    }
    return left.type == catalog::ColumnType::Int64 ? left.int_value == right.int_value
                                                   : left.string_value == right.string_value;
}

inline bool operator<(const Cell& left, const Cell& right) {
    if (left.type != right.type) {
        return static_cast<int>(left.type) < static_cast<int>(right.type);
    }
    if (left.is_null || right.is_null) {
        return left.is_null && !right.is_null;
    }
    return left.type == catalog::ColumnType::Int64 ? left.int_value < right.int_value
                                                   : left.string_value < right.string_value;
}

inline Cell cell_at(const storage::ColumnarBatch& batch, const std::string& name, std::size_t row) {
    const auto type = batch.column_type(name);
    if (type == catalog::ColumnType::Int64) {
        const auto& column = batch.column(name);
        if (column.is_null(row)) {
            return Cell{type, true, 0, ""};
        }
        return Cell{type, false, column.at(row), ""};
    }

    const auto& column = batch.string_column(name);
    if (column.is_null(row)) {
        return Cell{type, true, 0, ""};
    }
    return Cell{type, false, 0, column.at(row)};
}

inline std::string format_cell(const Cell& cell) {
    if (cell.is_null) {
        return "NULL";
    }
    if (cell.type == catalog::ColumnType::String) {
        return "'" + cell.string_value + "'";
    }
    return std::to_string(cell.int_value);
}

struct ComparisonStats {
    std::size_t alternative_count{0};
    std::size_t max_group_expression_count{0};
    std::size_t execution_path_count{0};
    std::size_t accepted_error_path_count{0};
    std::size_t exists_to_semi_firings{0};
    std::size_t not_exists_to_anti_firings{0};
    std::size_t in_to_semi_firings{0};
    std::size_t correlated_exists_to_semi_firings{0};
    std::size_t correlated_not_exists_to_anti_firings{0};
    std::size_t correlated_in_to_semi_firings{0};
    std::size_t residual_correlated_guard_paths{0};
    std::size_t left_join_to_inner_firings{0};
    std::size_t join_commute_firings{0};
    std::size_t join_associate_firings{0};
    bool hit_expression_bound{false};
    bool hit_plan_bound{false};
};

inline std::string format_batch(const storage::ColumnarBatch& batch) {
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
            out << format_cell(cell_at(batch, column_order[col], row));
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

inline bool same_batch(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return format_batch(left) == format_batch(right);
}

inline std::vector<std::string> sorted_column_names(const storage::ColumnarBatch& batch) {
    auto names = batch.column_names();
    std::sort(names.begin(), names.end());
    return names;
}

inline std::vector<std::vector<Cell>> sorted_rows_by_column_identity(const storage::ColumnarBatch& batch) {
    const auto names = sorted_column_names(batch);
    std::vector<std::vector<Cell>> rows;
    rows.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        std::vector<Cell> values;
        values.reserve(names.size());
        for (const auto& name : names) {
            values.push_back(cell_at(batch, name, row));
        }
        rows.push_back(std::move(values));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

inline bool same_column_identity_set(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return sorted_column_names(left) == sorted_column_names(right);
}

inline bool same_column_order(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return left.column_names() == right.column_names();
}

inline bool same_sorted_bag(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return same_column_identity_set(left, right) &&
           sorted_rows_by_column_identity(left) == sorted_rows_by_column_identity(right);
}

inline std::string format_sorted_bag(const storage::ColumnarBatch& batch) {
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
            out << format_cell(rows[row][col]);
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

inline std::optional<std::string> output_column_for_sort_key(const plan::SortKey& key,
                                                             const storage::ColumnarBatch& batch) {
    const auto qualified = key.column.binding + "." + key.column.column;
    if (batch.has_column(qualified)) {
        return qualified;
    }
    if (batch.has_column(key.column.column)) {
        return key.column.column;
    }
    return std::nullopt;
}

inline bool sort_cell_less(Cell left, Cell right, sql::SortDirection direction) {
    if (left == right) {
        return false;
    }
    const auto left_is_null = left.is_null;
    const auto right_is_null = right.is_null;
    if (left_is_null || right_is_null) {
        return direction == sql::SortDirection::Asc ? right_is_null : left_is_null;
    }
    return direction == sql::SortDirection::Asc ? left < right : right < left;
}

inline bool is_sorted_by_keys(const storage::ColumnarBatch& batch, const std::vector<plan::SortKey>& keys) {
    for (const auto& key : keys) {
        if (!output_column_for_sort_key(key, batch).has_value()) {
            return false;
        }
    }
    for (std::size_t row = 1; row < batch.row_count(); ++row) {
        for (const auto& key : keys) {
            const auto column_name = *output_column_for_sort_key(key, batch);
            const auto previous = cell_at(batch, column_name, row - 1);
            const auto current = cell_at(batch, column_name, row);
            if (previous == current) {
                continue;
            }
            if (sort_cell_less(current, previous, key.direction)) {
                return false;
            }
            break;
        }
    }
    return true;
}

inline const std::vector<plan::SortKey>* root_sort_keys(const plan::LogicalPlan& logical) {
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

inline std::optional<std::size_t> root_limit_count(const plan::LogicalPlan& logical) {
    if (logical.kind == plan::LogicalKind::Limit) {
        return logical.limit_count;
    }
    return std::nullopt;
}

inline const plan::LogicalPlan& without_root_limit(const plan::LogicalPlan& logical) {
    if (logical.kind != plan::LogicalKind::Limit) {
        return logical;
    }
    if (logical.input == nullptr) {
        throw std::logic_error("Limit missing input");
    }
    return *logical.input;
}

inline bool root_order_boundary_is_valid(const plan::LogicalPlan& logical) {
    const auto& boundary = without_root_limit(logical);
    if (boundary.kind != plan::LogicalKind::Sort || boundary.input == nullptr) {
        return false;
    }
    return logical.order_permission == plan::OrderPermission::Deterministic &&
           boundary.order_permission == plan::OrderPermission::Deterministic &&
           boundary.input->order_permission == plan::OrderPermission::Arbitrary;
}

inline std::vector<Cell> row_by_output_order(const storage::ColumnarBatch& batch, std::size_t row) {
    std::vector<Cell> values;
    values.reserve(batch.column_names().size());
    for (const auto& name : batch.column_names()) {
        values.push_back(cell_at(batch, name, row));
    }
    return values;
}

inline std::map<std::vector<Cell>, std::size_t> row_multiset_by_output_order(
    const storage::ColumnarBatch& batch) {
    std::map<std::vector<Cell>, std::size_t> counts;
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        ++counts[row_by_output_order(batch, row)];
    }
    return counts;
}

inline bool multiset_contains_rows(const storage::ColumnarBatch& superset, const storage::ColumnarBatch& subset) {
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

inline std::optional<std::vector<Cell>> key_tuple_for_row(const storage::ColumnarBatch& batch,
                                                          const std::vector<plan::SortKey>& keys,
                                                          std::size_t row) {
    std::vector<Cell> tuple;
    tuple.reserve(keys.size());
    for (const auto& key : keys) {
        const auto column_name = output_column_for_sort_key(key, batch);
        if (!column_name.has_value()) {
            return std::nullopt;
        }
        tuple.push_back(cell_at(batch, *column_name, row));
    }
    return tuple;
}

inline std::optional<std::map<std::vector<Cell>, std::size_t>> key_tuple_multiset_prefix(
    const storage::ColumnarBatch& batch,
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

inline bool valid_limit_answer(const storage::ColumnarBatch& candidate,
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

inline std::string format_trace(const std::vector<std::string>& fired_rules) {
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

inline bool contains_join(const plan::LogicalPlan& logical) {
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

inline std::size_t join_keyword_count(const std::string& sql) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = sql.find(" JOIN ", pos)) != std::string::npos) {
        ++count;
        pos += 6;
    }
    return count;
}

inline std::optional<std::string> accepted_runtime_error_category(const std::string& message) {
    if (message.find("overflowed int64") != std::string::npos) {
        return std::string{"int64-overflow"};
    }
    if (message.find("scalar subquery at position ") != std::string::npos &&
        message.find(" returned more than one row") != std::string::npos) {
        return std::string{"scalar-subquery-cardinality"};
    }
    return std::nullopt;
}

struct ExecutionOutcome {
    std::string path_name;
    std::optional<storage::ColumnarBatch> batch;
    std::optional<std::string> error_category;
    std::string error_message;
    bool unexpected_error{false};
};

inline std::string format_outcome(const ExecutionOutcome& outcome) {
    if (outcome.batch.has_value()) {
        return format_batch(*outcome.batch);
    }
    std::ostringstream out;
    out << "error category=" << (outcome.error_category.has_value() ? *outcome.error_category : "<unclassified>")
        << " unexpected=" << (outcome.unexpected_error ? "yes" : "no") << " message=\"" << outcome.error_message
        << "\"";
    return out.str();
}

template <typename Fn>
ExecutionOutcome run_execution_path(std::string path_name, Fn&& fn, ComparisonStats* stats) {
    ExecutionOutcome outcome;
    outcome.path_name = std::move(path_name);
    if (stats != nullptr) {
        ++stats->execution_path_count;
    }
    try {
        outcome.batch = std::forward<Fn>(fn)();
    } catch (const std::runtime_error& error) {
        outcome.error_message = error.what();
        outcome.error_category = accepted_runtime_error_category(outcome.error_message);
        if (!outcome.error_category.has_value()) {
            outcome.unexpected_error = true;
            outcome.error_category = std::string{"unexpected-runtime"};
        } else if (stats != nullptr) {
            ++stats->accepted_error_path_count;
        }
    } catch (const std::exception& error) {
        outcome.error_message = error.what();
        outcome.error_category = std::string{"unexpected-exception"};
        outcome.unexpected_error = true;
    }
    return outcome;
}

inline bool same_accepted_error(const ExecutionOutcome& expected, const ExecutionOutcome& actual) {
    return !expected.unexpected_error && !actual.unexpected_error && expected.error_category.has_value() &&
           actual.error_category.has_value() && *expected.error_category == *actual.error_category;
}

inline bool verify_error_pair(const ExecutionOutcome& unrewritten_oracle,
                              const ExecutionOutcome& candidate_oracle,
                              const ExecutionOutcome& candidate_vectorized,
                              const std::string& sql,
                              const std::string& table_text,
                              const std::string& path_label,
                              const plan::LogicalPlan& original_plan,
                              const plan::LogicalPlan& candidate_plan,
                              const std::string& memo_text = {}) {
    if (same_accepted_error(unrewritten_oracle, candidate_oracle) &&
        same_accepted_error(candidate_oracle, candidate_vectorized)) {
        return true;
    }

    std::cerr << "runtime error-equivalence divergence\n"
              << "path: " << path_label << "\n"
              << "sql: " << sql << "\n"
              << table_text << "\n"
              << "original plan:\n"
              << plan::to_string(original_plan) << "\n";
    if (!memo_text.empty()) {
        std::cerr << memo_text;
    }
    std::cerr << "candidate plan:\n"
              << plan::to_string(candidate_plan) << "\n"
              << "unrewritten oracle: " << format_outcome(unrewritten_oracle) << "\n"
              << "candidate oracle:   " << format_outcome(candidate_oracle) << "\n"
              << "candidate vector:   " << format_outcome(candidate_vectorized) << "\n";
    return false;
}

inline bool verify_result_pair(const storage::ColumnarBatch& unrewritten_oracle,
                               const storage::ColumnarBatch& full_unlimited_oracle,
                               const ExecutionOutcome& candidate_oracle,
                               const ExecutionOutcome& candidate_vectorized,
                               const std::string& sql,
                               const std::string& table_text,
                               const std::string& path_label,
                               const plan::LogicalPlan& original_plan,
                               const plan::LogicalPlan& candidate_plan,
                               bool is_join_query,
                               bool is_ordered_query,
                               const std::vector<plan::SortKey>* order_keys,
                               std::optional<std::size_t> limit_count,
                               const std::string& memo_text = {}) {
    const auto* candidate_order_keys = root_sort_keys(candidate_plan);
    const auto candidate_has_results = candidate_oracle.batch.has_value() && candidate_vectorized.batch.has_value();
    const auto ordered_outputs_are_sorted =
        candidate_has_results &&
        (!is_ordered_query ||
         (candidate_order_keys != nullptr && is_sorted_by_keys(unrewritten_oracle, *order_keys) &&
          is_sorted_by_keys(*candidate_oracle.batch, *candidate_order_keys)));
    const auto cross_plan_equal =
        candidate_has_results &&
        (limit_count.has_value()
             ? valid_limit_answer(*candidate_oracle.batch, full_unlimited_oracle, *limit_count, order_keys)
             : is_ordered_query || is_join_query ? same_sorted_bag(unrewritten_oracle, *candidate_oracle.batch)
                                                 : same_batch(unrewritten_oracle, *candidate_oracle.batch));
    const auto vectorized_equal =
        candidate_has_results && same_batch(*candidate_oracle.batch, *candidate_vectorized.batch);
    const auto column_sets_match =
        candidate_has_results && same_column_identity_set(unrewritten_oracle, *candidate_oracle.batch) &&
        same_column_identity_set(*candidate_oracle.batch, *candidate_vectorized.batch);
    const auto output_order_matches =
        candidate_has_results && same_column_order(unrewritten_oracle, *candidate_oracle.batch) &&
        same_column_order(*candidate_oracle.batch, *candidate_vectorized.batch);

    if (cross_plan_equal && vectorized_equal && column_sets_match && output_order_matches &&
        ordered_outputs_are_sorted) {
        return true;
    }

    std::cerr << "memo/rewrite/vectorized divergence\n"
              << "path: " << path_label << "\n"
              << "sql: " << sql << "\n"
              << table_text << "\n"
              << "original plan:\n"
              << plan::to_string(original_plan) << "\n";
    if (!memo_text.empty()) {
        std::cerr << memo_text;
    }
    std::cerr << "candidate plan:\n"
              << plan::to_string(candidate_plan) << "\n"
              << "unrewritten oracle:     " << format_batch(unrewritten_oracle) << "\n"
              << "full unlimited oracle:  " << format_batch(full_unlimited_oracle) << "\n"
              << "unrewritten bag:        " << format_sorted_bag(unrewritten_oracle) << "\n"
              << "candidate oracle:       " << format_outcome(candidate_oracle) << "\n";
    if (candidate_oracle.batch.has_value()) {
        std::cerr << "candidate oracle bag:   " << format_sorted_bag(*candidate_oracle.batch) << "\n";
    }
    std::cerr << "candidate vectorized:   " << format_outcome(candidate_vectorized) << "\n"
              << "cross plan equal:       " << (cross_plan_equal ? "yes" : "no") << "\n"
              << "vectorized equal:       " << (vectorized_equal ? "yes" : "no") << "\n"
              << "column sets match:      " << (column_sets_match ? "yes" : "no") << "\n"
              << "output order matches:   " << (output_order_matches ? "yes" : "no") << "\n"
              << "ordered outputs sorted: " << (ordered_outputs_are_sorted ? "yes" : "no") << "\n";
    return false;
}

inline bool compare_engines(const std::string& sql,
                            const execution::Catalog& catalog,
                            const std::string& table_text,
                            ComparisonStats* stats = nullptr) {
    try {
        const auto parsed = sql::parse_select(sql);
        const auto logical = sql::bind_select(parsed, catalog);
        if (logical.kind == plan::LogicalKind::Explain) {
            const auto interpreted = run_execution_path(
                "EXPLAIN interpreted",
                [&] { return execution::execute_interpreted(logical, catalog); },
                stats);
            const auto vectorized = run_execution_path(
                "EXPLAIN vectorized",
                [&] { return execution::execute_vectorized(logical, catalog); },
                stats);
            if (interpreted.batch.has_value() && vectorized.batch.has_value() &&
                same_batch(*interpreted.batch, *vectorized.batch)) {
                return true;
            }
            std::cerr << "EXPLAIN engine divergence\n"
                      << "sql: " << sql << "\n"
                      << table_text << "\n"
                      << "plan:\n"
                      << plan::to_string(logical) << "\n"
                      << "interpreted: " << format_outcome(interpreted) << "\n"
                      << "vectorized:  " << format_outcome(vectorized) << "\n";
            return false;
        }
        const auto is_join_query = contains_join(logical);
        const auto* order_keys = root_sort_keys(logical);
        const auto limit_count = root_limit_count(logical);
        const auto is_ordered_query = order_keys != nullptr;
        if (is_join_query && !is_ordered_query && logical.order_permission != plan::OrderPermission::Arbitrary) {
            std::cerr << "join query did not carry arbitrary-order permission\n"
                      << "sql: " << sql << "\n"
                      << "plan:\n"
                      << plan::to_string(logical) << "\n";
            return false;
        }
        if (is_ordered_query && !root_order_boundary_is_valid(logical)) {
            std::cerr << "ordered query did not keep a required root above arbitrary-order input\n"
                      << "sql: " << sql << "\n"
                      << "plan:\n"
                      << plan::to_string(logical) << "\n";
            return false;
        }

        const auto rewritten = optimizer::rewrite_to_fixpoint(logical, optimizer::default_rules());

        optimizer::Memo memo;
        const auto root = memo.insert(logical);
        const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{256, 4096});
        if (stats != nullptr) {
            stats->alternative_count = alternatives.plans.size();
            stats->max_group_expression_count = alternatives.max_group_expression_count;
            stats->left_join_to_inner_firings =
                std::count(explored.fired_rules.begin(), explored.fired_rules.end(), "LeftJoinToInnerRule");
            stats->exists_to_semi_firings =
                std::count(explored.fired_rules.begin(), explored.fired_rules.end(), "ExistsToSemiJoinRule");
            stats->not_exists_to_anti_firings =
                std::count(explored.fired_rules.begin(), explored.fired_rules.end(), "NotExistsToAntiJoinRule");
            stats->in_to_semi_firings =
                std::count(explored.fired_rules.begin(), explored.fired_rules.end(), "InToSemiJoinRule");
            stats->correlated_exists_to_semi_firings = std::count(
                explored.fired_rules.begin(), explored.fired_rules.end(), "CorrelatedExistsToSemiJoinRule");
            stats->correlated_not_exists_to_anti_firings = std::count(
                explored.fired_rules.begin(), explored.fired_rules.end(), "CorrelatedNotExistsToAntiJoinRule");
            stats->correlated_in_to_semi_firings = std::count(
                explored.fired_rules.begin(), explored.fired_rules.end(), "CorrelatedInToSemiJoinRule");
            stats->join_commute_firings =
                std::count(explored.fired_rules.begin(), explored.fired_rules.end(), "JoinCommuteRule");
            stats->join_associate_firings =
                std::count(explored.fired_rules.begin(), explored.fired_rules.end(), "JoinAssociateRule");
            stats->hit_expression_bound = alternatives.hit_expression_bound;
            stats->hit_plan_bound = alternatives.hit_plan_bound;
        }
        if (alternatives.plans.empty()) {
            std::cerr << "memo alternative extraction returned no plans\n"
                      << "sql: " << sql << "\n"
                      << table_text << "\n"
                      << "memo trace: " << format_trace(explored.fired_rules) << "\n"
                      << "memo dump:\n"
                      << memo.dump();
            return false;
        }

        const auto unrewritten_oracle = run_execution_path(
            "unrewritten interpreted",
            [&] { return execution::execute_interpreted(logical, catalog); },
            stats);
        std::optional<ExecutionOutcome> full_unlimited_oracle;
        if (unrewritten_oracle.batch.has_value()) {
            full_unlimited_oracle =
                limit_count.has_value()
                    ? run_execution_path(
                          "unlimited interpreted",
                          [&] { return execution::execute_interpreted(without_root_limit(logical), catalog); },
                          stats)
                    : unrewritten_oracle;
            if (!full_unlimited_oracle->batch.has_value()) {
                std::cerr << "unlimited oracle diverged from limited oracle\n"
                          << "sql: " << sql << "\n"
                          << table_text << "\n"
                          << "limited oracle:   " << format_outcome(unrewritten_oracle) << "\n"
                          << "unlimited oracle: " << format_outcome(*full_unlimited_oracle) << "\n"
                          << "plan:\n"
                          << plan::to_string(logical) << "\n";
                return false;
            }
        }
        const auto verify_path_pair = [&](const std::string& path_label,
                                          const plan::LogicalPlan& candidate_plan,
                                          const std::string& memo_text = {}) {
            const auto candidate_oracle = run_execution_path(
                path_label + " interpreted",
                [&] { return execution::execute_interpreted(candidate_plan, catalog); },
                stats);
            const auto candidate_vectorized = run_execution_path(
                path_label + " vectorized",
                [&] { return execution::execute_vectorized(candidate_plan, catalog); },
                stats);
            if (!unrewritten_oracle.batch.has_value()) {
                return verify_error_pair(unrewritten_oracle,
                                         candidate_oracle,
                                         candidate_vectorized,
                                         sql,
                                         table_text,
                                         path_label,
                                         logical,
                                         candidate_plan,
                                         memo_text);
            }
            if (!full_unlimited_oracle.has_value() || !full_unlimited_oracle->batch.has_value()) {
                throw std::logic_error("missing unlimited oracle result");
            }

            return verify_result_pair(*unrewritten_oracle.batch,
                                      *full_unlimited_oracle->batch,
                                      candidate_oracle,
                                      candidate_vectorized,
                                      sql,
                                      table_text,
                                      path_label,
                                      logical,
                                      candidate_plan,
                                      is_join_query,
                                      is_ordered_query,
                                      order_keys,
                                      limit_count,
                                      memo_text);
        };

        const auto rewrite_text =
            "standalone rewrite trace: " + format_trace(rewritten.trace.fired_rules) + "\n";
        if (!verify_path_pair("standalone rewrite", rewritten.plan, rewrite_text)) {
            return false;
        }

        const auto memo_text_prefix = "memo trace: " + format_trace(explored.fired_rules) + "\n" +
                                      "memo dump:\n" + memo.dump();
        for (std::size_t alternative_index = 0; alternative_index < alternatives.plans.size(); ++alternative_index) {
            std::ostringstream path_label;
            path_label << "memo alternative " << alternative_index << " of " << alternatives.plans.size();
            std::ostringstream memo_text;
            memo_text << "hit expression bound: " << (alternatives.hit_expression_bound ? "yes" : "no") << "\n"
                      << "hit plan bound: " << (alternatives.hit_plan_bound ? "yes" : "no") << "\n"
                      << memo_text_prefix;
            if (!verify_path_pair(path_label.str(), alternatives.plans[alternative_index], memo_text.str())) {
                return false;
            }
        }

        const auto best = memo.extract_best(root, catalog);
        if (!verify_path_pair("memo extract_best", best, memo_text_prefix)) {
            return false;
        }

        return true;
    } catch (const std::exception& error) {
        std::cerr << "verification setup failed\n"
                  << "sql: " << sql << "\n"
                  << table_text << "\n"
                  << "exception: " << error.what() << "\n";
        return false;
    }
}

} // namespace differential
