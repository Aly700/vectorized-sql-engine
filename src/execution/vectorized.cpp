#include "execution/vectorized.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace execution {
namespace {

using SelectionVector = std::vector<std::size_t>;
using SelectionVectorPtr = std::shared_ptr<const SelectionVector>;

struct BatchView {
    std::shared_ptr<const storage::ColumnarBatch> owned_batch;
    const storage::ColumnarBatch* batch{nullptr};
    SelectionVectorPtr selection;
};

struct EquiJoinKey {
    plan::BoundColumnRef left;
    plan::BoundColumnRef right;
};

struct JoinPredicateSplit {
    std::vector<EquiJoinKey> equi_keys;
    std::vector<plan::BoundComparisonExpr> residuals;
};

struct HashKey {
    std::vector<std::int64_t> values;

    bool operator==(const HashKey& other) const { return values == other.values; }
};

struct HashKeyHash {
    std::size_t operator()(const HashKey& key) const {
        std::size_t seed = key.values.size();
        for (auto value : key.values) {
            const auto hashed = std::hash<std::int64_t>{}(value);
            seed ^= hashed + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        }
        return seed;
    }
};

struct JoinOutputBuilder {
    std::vector<std::string> names;
    std::vector<storage::Int64Column> columns;
};

SelectionVectorPtr make_selection(SelectionVector rows) {
    return std::make_shared<const SelectionVector>(std::move(rows));
}

SelectionVectorPtr identity_selection(std::size_t row_count) {
    SelectionVector rows;
    rows.reserve(row_count);
    for (std::size_t row = 0; row < row_count; ++row) {
        rows.push_back(row);
    }
    return make_selection(std::move(rows));
}

void validate_view(const BatchView& view) {
    if (view.batch == nullptr) {
        throw std::logic_error("vectorized batch view is missing a batch");
    }
    if (!view.selection) {
        throw std::logic_error("vectorized batch view is missing a selection vector");
    }
    for (auto row : *view.selection) {
        if (row >= view.batch->row_count()) {
            throw std::logic_error("selection vector row is outside the batch");
        }
    }
}

std::string column_identity_name(const plan::BoundColumnRef& column) {
    return column.table + "." + column.column;
}

std::int64_t evaluate_scalar(const plan::BoundScalarExpr& expression,
                             const storage::ColumnarBatch& batch,
                             std::size_t row) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression)) {
        return batch.column(column_identity_name(*column)).at(row);
    }
    return std::get<sql::IntLiteral>(expression).value;
}

bool compare_values(std::int64_t left, sql::ComparisonOp op, std::int64_t right) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return left == right;
    case sql::ComparisonOp::NotEqual:
        return left != right;
    case sql::ComparisonOp::Less:
        return left < right;
    case sql::ComparisonOp::LessEqual:
        return left <= right;
    case sql::ComparisonOp::Greater:
        return left > right;
    case sql::ComparisonOp::GreaterEqual:
        return left >= right;
    }
    throw std::logic_error("unreachable comparison operator");
}

bool evaluate_comparison(const plan::BoundComparisonExpr& comparison,
                         const storage::ColumnarBatch& batch,
                         std::size_t row) {
    return compare_values(evaluate_scalar(comparison.left, batch, row),
                          comparison.op,
                          evaluate_scalar(comparison.right, batch, row));
}

std::int64_t evaluate_join_scalar(const plan::BoundScalarExpr& expression,
                                  const storage::ColumnarBatch& left,
                                  std::size_t left_row,
                                  const storage::ColumnarBatch& right,
                                  std::size_t right_row) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression)) {
        const auto name = column_identity_name(*column);
        if (left.has_column(name)) {
            return left.column(name).at(left_row);
        }
        if (right.has_column(name)) {
            return right.column(name).at(right_row);
        }
        throw std::logic_error("bound join column identity is missing from both inputs: " + name);
    }
    return std::get<sql::IntLiteral>(expression).value;
}

bool evaluate_join_comparison(const plan::BoundComparisonExpr& comparison,
                              const storage::ColumnarBatch& left,
                              std::size_t left_row,
                              const storage::ColumnarBatch& right,
                              std::size_t right_row) {
    return compare_values(evaluate_join_scalar(comparison.left, left, left_row, right, right_row),
                          comparison.op,
                          evaluate_join_scalar(comparison.right, left, left_row, right, right_row));
}

bool evaluate_join_predicates(const std::vector<plan::BoundComparisonExpr>& predicates,
                              const storage::ColumnarBatch& left,
                              std::size_t left_row,
                              const storage::ColumnarBatch& right,
                              std::size_t right_row) {
    for (const auto& predicate : predicates) {
        if (!evaluate_join_comparison(predicate, left, left_row, right, right_row)) {
            return false;
        }
    }
    return true;
}

bool try_add_equi_key(const plan::BoundComparisonExpr& predicate,
                      const storage::ColumnarBatch& left,
                      const storage::ColumnarBatch& right,
                      JoinPredicateSplit& split) {
    if (predicate.op != sql::ComparisonOp::Equal) {
        return false;
    }

    const auto* lhs = std::get_if<plan::BoundColumnRef>(&predicate.left);
    const auto* rhs = std::get_if<plan::BoundColumnRef>(&predicate.right);
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    const auto lhs_name = column_identity_name(*lhs);
    const auto rhs_name = column_identity_name(*rhs);
    const auto lhs_in_left = left.has_column(lhs_name);
    const auto lhs_in_right = right.has_column(lhs_name);
    const auto rhs_in_left = left.has_column(rhs_name);
    const auto rhs_in_right = right.has_column(rhs_name);

    if (lhs_in_left && rhs_in_right && !lhs_in_right && !rhs_in_left) {
        split.equi_keys.push_back(EquiJoinKey{*lhs, *rhs});
        return true;
    }
    if (lhs_in_right && rhs_in_left && !lhs_in_left && !rhs_in_right) {
        split.equi_keys.push_back(EquiJoinKey{*rhs, *lhs});
        return true;
    }
    return false;
}

JoinPredicateSplit split_join_predicates(const std::vector<plan::BoundComparisonExpr>& predicates,
                                         const storage::ColumnarBatch& left,
                                         const storage::ColumnarBatch& right) {
    JoinPredicateSplit split;
    for (const auto& predicate : predicates) {
        if (!try_add_equi_key(predicate, left, right, split)) {
            split.residuals.push_back(predicate);
        }
    }
    return split;
}

HashKey make_key(const storage::ColumnarBatch& batch,
                 std::size_t row,
                 const std::vector<plan::BoundColumnRef>& key_columns) {
    HashKey key;
    key.values.reserve(key_columns.size());
    for (const auto& column : key_columns) {
        key.values.push_back(batch.column(column_identity_name(column)).at(row));
    }
    return key;
}

JoinOutputBuilder make_join_output_builder(const storage::ColumnarBatch& left,
                                           const storage::ColumnarBatch& right) {
    JoinOutputBuilder builder;
    builder.names = left.column_names();
    builder.names.insert(builder.names.end(), right.column_names().begin(), right.column_names().end());
    builder.columns.resize(builder.names.size());
    return builder;
}

void append_joined_row(JoinOutputBuilder& builder,
                       const storage::ColumnarBatch& left,
                       std::size_t left_row,
                       const storage::ColumnarBatch& right,
                       std::size_t right_row) {
    std::size_t output_index = 0;
    for (const auto& column_name : left.column_names()) {
        builder.columns[output_index++].append(left.column(column_name).at(left_row));
    }
    for (const auto& column_name : right.column_names()) {
        builder.columns[output_index++].append(right.column(column_name).at(right_row));
    }
}

storage::ColumnarBatch finish_join_output(JoinOutputBuilder builder) {
    storage::ColumnarBatch out;
    for (std::size_t i = 0; i < builder.names.size(); ++i) {
        out.add_column(builder.names[i], std::move(builder.columns[i]));
    }
    return out;
}

storage::ColumnarBatch execute_nested_loop_join(const BatchView& left,
                                                const BatchView& right,
                                                const std::vector<plan::BoundComparisonExpr>& predicates) {
    auto builder = make_join_output_builder(*left.batch, *right.batch);
    for (auto left_row : *left.selection) {
        for (auto right_row : *right.selection) {
            if (evaluate_join_predicates(predicates, *left.batch, left_row, *right.batch, right_row)) {
                append_joined_row(builder, *left.batch, left_row, *right.batch, right_row);
            }
        }
    }
    return finish_join_output(std::move(builder));
}

storage::ColumnarBatch execute_hash_join(const BatchView& left,
                                         const BatchView& right,
                                         const JoinPredicateSplit& predicates) {
    std::vector<plan::BoundColumnRef> left_key_columns;
    std::vector<plan::BoundColumnRef> right_key_columns;
    left_key_columns.reserve(predicates.equi_keys.size());
    right_key_columns.reserve(predicates.equi_keys.size());
    for (const auto& equi_key : predicates.equi_keys) {
        left_key_columns.push_back(equi_key.left);
        right_key_columns.push_back(equi_key.right);
    }

    // Lookup-only hash table: output order must come exclusively from probing
    // left rows in order and from each per-key right-row vector's insertion order.
    std::unordered_map<HashKey, std::vector<std::size_t>, HashKeyHash> right_rows_by_key;
    for (auto right_row : *right.selection) {
        right_rows_by_key[make_key(*right.batch, right_row, right_key_columns)].push_back(right_row);
    }

    auto builder = make_join_output_builder(*left.batch, *right.batch);
    for (auto left_row : *left.selection) {
        const auto matching_right_rows = right_rows_by_key.find(make_key(*left.batch, left_row, left_key_columns));
        if (matching_right_rows == right_rows_by_key.end()) {
            continue;
        }

        for (auto right_row : matching_right_rows->second) {
            if (evaluate_join_predicates(predicates.residuals, *left.batch, left_row, *right.batch, right_row)) {
                append_joined_row(builder, *left.batch, left_row, *right.batch, right_row);
            }
        }
    }
    return finish_join_output(std::move(builder));
}

BatchView execute_scan(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    const auto& input = catalog.table(plan.table);
    auto qualified = std::make_shared<storage::ColumnarBatch>();
    for (const auto& column_name : input.column_names()) {
        qualified->add_column(plan.table + "." + column_name, input.column(column_name));
    }

    BatchView view;
    view.owned_batch = qualified;
    view.batch = qualified.get();
    view.selection = identity_selection(qualified->row_count());
    validate_view(view);
    return view;
}

BatchView execute_filter(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_join(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_project(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_sort(const plan::PhysicalPlan& plan, const Catalog& catalog);

BatchView execute_to_view(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    switch (plan.kind) {
    case plan::PhysicalKind::Scan:
        return execute_scan(plan, catalog);
    case plan::PhysicalKind::Join:
        return execute_join(plan, catalog);
    case plan::PhysicalKind::Filter:
        return execute_filter(plan, catalog);
    case plan::PhysicalKind::Project:
        return execute_project(plan, catalog);
    case plan::PhysicalKind::Sort:
        return execute_sort(plan, catalog);
    }
    throw std::logic_error("unreachable physical plan kind");
}

const plan::PhysicalPlan& require_input(const plan::PhysicalPlan& plan) {
    if (!plan.input) {
        throw std::invalid_argument("physical plan node is missing its input");
    }
    return *plan.input;
}

const plan::PhysicalPlan& require_left(const plan::PhysicalPlan& plan) {
    if (!plan.left) {
        throw std::invalid_argument("physical join node is missing its left input");
    }
    return *plan.left;
}

const plan::PhysicalPlan& require_right(const plan::PhysicalPlan& plan) {
    if (!plan.right) {
        throw std::invalid_argument("physical join node is missing its right input");
    }
    return *plan.right;
}

BatchView execute_filter(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto input = execute_to_view(require_input(plan), catalog);
    validate_view(input);

    SelectionVector rows;
    rows.reserve(input.selection->size());
    for (auto row : *input.selection) {
        bool keep = true;
        for (const auto& predicate : plan.predicates) {
            if (!evaluate_comparison(predicate, *input.batch, row)) {
                keep = false;
                break;
            }
        }
        if (keep) {
            rows.push_back(row);
        }
    }

    input.selection = make_selection(std::move(rows));
    validate_view(input);
    return input;
}

BatchView execute_join(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto left = execute_to_view(require_left(plan), catalog);
    auto right = execute_to_view(require_right(plan), catalog);
    validate_view(left);
    validate_view(right);

    const auto predicates = split_join_predicates(plan.predicates, *left.batch, *right.batch);
    auto materialized = std::make_shared<const storage::ColumnarBatch>(
        predicates.equi_keys.empty() ? execute_nested_loop_join(left, right, predicates.residuals)
                                     : execute_hash_join(left, right, predicates));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    validate_view(view);
    return view;
}

storage::ColumnarBatch materialize_projection(const plan::PhysicalPlan& plan, const BatchView& input) {
    validate_view(input);

    storage::ColumnarBatch out;
    for (const auto& projection : plan.projections) {
        storage::Int64Column column;
        for (auto row : *input.selection) {
            column.append(evaluate_scalar(projection.expression, *input.batch, row));
        }
        out.add_column(projection.output_name, std::move(column));
    }
    return out;
}

SelectionVectorPtr sort_selection(const std::vector<plan::SortKey>& sort_keys, const BatchView& input) {
    validate_view(input);

    SelectionVector rows = *input.selection;
    std::stable_sort(rows.begin(), rows.end(), [&](std::size_t left, std::size_t right) {
        for (const auto& key : sort_keys) {
            const auto& column = input.batch->column(column_identity_name(key.column));
            const auto left_value = column.at(left);
            const auto right_value = column.at(right);
            if (left_value == right_value) {
                continue;
            }
            return key.direction == sql::SortDirection::Asc ? left_value < right_value : left_value > right_value;
        }
        return false;
    });
    return make_selection(std::move(rows));
}

BatchView execute_project(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto input = execute_to_view(require_input(plan), catalog);
    auto materialized = std::make_shared<const storage::ColumnarBatch>(materialize_projection(plan, input));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    validate_view(view);
    return view;
}

BatchView execute_sort(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    const auto& input_plan = require_input(plan);
    if (input_plan.kind == plan::PhysicalKind::Project) {
        auto source = execute_to_view(require_input(input_plan), catalog);
        source.selection = sort_selection(plan.sort_keys, source);
        auto materialized = std::make_shared<const storage::ColumnarBatch>(materialize_projection(input_plan, source));

        BatchView view;
        view.owned_batch = materialized;
        view.batch = materialized.get();
        view.selection = identity_selection(materialized->row_count());
        validate_view(view);
        return view;
    }

    auto input = execute_to_view(input_plan, catalog);
    input.selection = sort_selection(plan.sort_keys, input);
    validate_view(input);
    return input;
}

storage::ColumnarBatch materialize_view(const BatchView& view) {
    validate_view(view);

    storage::ColumnarBatch out;
    for (const auto& name : view.batch->column_names()) {
        storage::Int64Column column;
        const auto& input_column = view.batch->column(name);
        for (auto row : *view.selection) {
            column.append(input_column.at(row));
        }
        out.add_column(name, std::move(column));
    }
    return out;
}

} // namespace

storage::ColumnarBatch execute_vectorized(const plan::LogicalPlan& plan, const Catalog& catalog) {
    return execute_vectorized(plan::lower_to_physical(plan), catalog);
}

storage::ColumnarBatch execute_vectorized(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    return materialize_view(execute_to_view(plan, catalog));
}

} // namespace execution
