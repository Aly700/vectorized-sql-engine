#include "execution/interpreter.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace execution {
namespace {

const plan::LogicalPlan& require_input(const plan::LogicalPlan& plan) {
    if (!plan.input) {
        throw std::invalid_argument("logical plan node is missing its input");
    }
    return *plan.input;
}

const plan::LogicalPlan& require_left(const plan::LogicalPlan& plan) {
    if (!plan.left) {
        throw std::invalid_argument("logical join node is missing its left input");
    }
    return *plan.left;
}

const plan::LogicalPlan& require_right(const plan::LogicalPlan& plan) {
    if (!plan.right) {
        throw std::invalid_argument("logical join node is missing its right input");
    }
    return *plan.right;
}

std::string column_identity_name(const plan::BoundColumnRef& column) {
    if (column.binding.empty()) {
        return column.column;
    }
    return column.binding + "." + column.column;
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

storage::RowMask evaluate_filter(const std::vector<plan::BoundComparisonExpr>& predicates,
                                 const storage::ColumnarBatch& batch) {
    storage::RowMask mask;
    mask.keep.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        bool keep = true;
        for (const auto& predicate : predicates) {
            if (!evaluate_comparison(predicate, batch, row)) {
                keep = false;
                break;
            }
        }
        mask.keep.push_back(keep ? 1 : 0);
    }
    return mask;
}

storage::Int64Column evaluate_projection(const plan::Projection& projection, const storage::ColumnarBatch& batch) {
    storage::Int64Column column;
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        column.append(evaluate_scalar(projection.expression, batch, row));
    }
    return column;
}

storage::Int64Column evaluate_projection(const plan::Projection& projection,
                                         const storage::ColumnarBatch& batch,
                                         const std::vector<std::size_t>& rows) {
    storage::Int64Column column;
    for (auto row : rows) {
        column.append(evaluate_scalar(projection.expression, batch, row));
    }
    return column;
}

std::vector<std::size_t> stable_sorted_rows(const std::vector<plan::SortKey>& sort_keys,
                                            const storage::ColumnarBatch& batch) {
    std::vector<std::size_t> rows;
    rows.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        rows.push_back(row);
    }

    std::stable_sort(rows.begin(), rows.end(), [&](std::size_t left, std::size_t right) {
        for (const auto& key : sort_keys) {
            const auto& column = batch.column(column_identity_name(key.column));
            const auto left_value = column.at(left);
            const auto right_value = column.at(right);
            if (left_value == right_value) {
                continue;
            }
            return key.direction == sql::SortDirection::Asc ? left_value < right_value : left_value > right_value;
        }
        return false;
    });
    return rows;
}

storage::ColumnarBatch materialize_rows(const storage::ColumnarBatch& batch, const std::vector<std::size_t>& rows) {
    storage::ColumnarBatch out;
    for (const auto& name : batch.column_names()) {
        storage::Int64Column column;
        const auto& input_column = batch.column(name);
        for (auto row : rows) {
            column.append(input_column.at(row));
        }
        out.add_column(name, std::move(column));
    }
    return out;
}

storage::ColumnarBatch materialize_project_rows(const plan::LogicalPlan& project,
                                                const storage::ColumnarBatch& batch,
                                                const std::vector<std::size_t>& rows) {
    storage::ColumnarBatch out;
    for (const auto& projection : project.projections) {
        out.add_column(projection.output_name, evaluate_projection(projection, batch, rows));
    }
    return out;
}

struct AggregateValue {
    std::int64_t count{0};
    std::int64_t sum{0};
    std::int64_t min{0};
    std::int64_t max{0};
    bool has_value{false};
};

struct GroupState {
    std::vector<std::int64_t> key_values;
    std::vector<AggregateValue> aggregates;
};

void increment_count(AggregateValue& value, const std::string& output_name) {
    if (value.count == std::numeric_limits<std::int64_t>::max()) {
        throw std::runtime_error(output_name + " overflowed int64");
    }
    ++value.count;
}

std::int64_t checked_sum(std::int64_t left, std::int64_t right, const std::string& output_name) {
    std::int64_t result = 0;
    if (__builtin_add_overflow(left, right, &result)) {
        throw std::runtime_error(output_name + " overflowed int64");
    }
    return result;
}

std::int64_t aggregate_argument_value(const plan::AggregateExpression& aggregate,
                                      const storage::ColumnarBatch& batch,
                                      std::size_t row) {
    if (!aggregate.argument.has_value()) {
        throw std::logic_error("aggregate argument is missing");
    }
    return batch.column(column_identity_name(*aggregate.argument)).at(row);
}

void update_aggregate(AggregateValue& value,
                      const plan::AggregateExpression& aggregate,
                      const storage::ColumnarBatch& batch,
                      std::size_t row) {
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        if (aggregate.argument.has_value()) {
            (void)aggregate_argument_value(aggregate, batch, row);
        }
        increment_count(value, aggregate.output_name);
        return;
    case sql::AggregateFunction::Sum: {
        const auto argument = aggregate_argument_value(aggregate, batch, row);
        if (!value.has_value) {
            value.sum = argument;
            value.has_value = true;
            return;
        }
        value.sum = checked_sum(value.sum, argument, aggregate.output_name);
        return;
    }
    case sql::AggregateFunction::Min: {
        const auto argument = aggregate_argument_value(aggregate, batch, row);
        if (!value.has_value || argument < value.min) {
            value.min = argument;
        }
        value.has_value = true;
        return;
    }
    case sql::AggregateFunction::Max: {
        const auto argument = aggregate_argument_value(aggregate, batch, row);
        if (!value.has_value || argument > value.max) {
            value.max = argument;
        }
        value.has_value = true;
        return;
    }
    }
    throw std::logic_error("unreachable aggregate function");
}

std::int64_t finalize_aggregate(const AggregateValue& value, const plan::AggregateExpression& aggregate) {
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        return value.count;
    case sql::AggregateFunction::Sum:
        if (!value.has_value) {
            throw std::runtime_error(aggregate.output_name + " over empty input has no NULL-free result");
        }
        return value.sum;
    case sql::AggregateFunction::Min:
        if (!value.has_value) {
            throw std::runtime_error(aggregate.output_name + " over empty input has no NULL-free result");
        }
        return value.min;
    case sql::AggregateFunction::Max:
        if (!value.has_value) {
            throw std::runtime_error(aggregate.output_name + " over empty input has no NULL-free result");
        }
        return value.max;
    }
    throw std::logic_error("unreachable aggregate function");
}

std::vector<std::int64_t> group_key_values(const std::vector<plan::BoundColumnRef>& group_keys,
                                           const storage::ColumnarBatch& batch,
                                           std::size_t row) {
    std::vector<std::int64_t> values;
    values.reserve(group_keys.size());
    for (const auto& key : group_keys) {
        values.push_back(batch.column(column_identity_name(key)).at(row));
    }
    return values;
}

GroupState make_group(std::vector<std::int64_t> key_values, const plan::LogicalPlan& plan) {
    GroupState group;
    group.key_values = std::move(key_values);
    group.aggregates.resize(plan.aggregate_expressions.size());
    return group;
}

storage::ColumnarBatch execute_scan(const plan::LogicalPlan& plan, const Catalog& catalog) {
    const auto& input = catalog.table(plan.table);
    storage::ColumnarBatch out;
    for (const auto& column_name : input.column_names()) {
        out.add_column(plan.binding_name + "." + column_name, input.column(column_name));
    }
    return out;
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

storage::ColumnarBatch execute_join(const plan::LogicalPlan& plan, const Catalog& catalog) {
    const auto left = execute_interpreted(require_left(plan), catalog);
    const auto right = execute_interpreted(require_right(plan), catalog);

    std::vector<std::string> output_names = left.column_names();
    output_names.insert(output_names.end(), right.column_names().begin(), right.column_names().end());
    std::vector<storage::Int64Column> output_columns(output_names.size());

    // Inner join order is part of the oracle contract: for each left row in
    // input order, scan every right row in input order. Future vectorized join
    // work must reproduce this left-row-major bag order exactly.
    for (std::size_t left_row = 0; left_row < left.row_count(); ++left_row) {
        for (std::size_t right_row = 0; right_row < right.row_count(); ++right_row) {
            if (!evaluate_join_predicates(plan.predicates, left, left_row, right, right_row)) {
                continue;
            }

            std::size_t output_index = 0;
            for (const auto& column_name : left.column_names()) {
                output_columns[output_index++].append(left.column(column_name).at(left_row));
            }
            for (const auto& column_name : right.column_names()) {
                output_columns[output_index++].append(right.column(column_name).at(right_row));
            }
        }
    }

    storage::ColumnarBatch out;
    for (std::size_t i = 0; i < output_names.size(); ++i) {
        out.add_column(output_names[i], std::move(output_columns[i]));
    }
    return out;
}

storage::ColumnarBatch execute_aggregate(const plan::LogicalPlan& plan, const Catalog& catalog) {
    const auto input = execute_interpreted(require_input(plan), catalog);

    std::map<std::vector<std::int64_t>, std::size_t> group_index_by_key;
    std::vector<GroupState> groups;
    if (plan.group_keys.empty()) {
        groups.push_back(make_group({}, plan));
    }

    for (std::size_t row = 0; row < input.row_count(); ++row) {
        std::size_t group_index = 0;
        if (!plan.group_keys.empty()) {
            auto key = group_key_values(plan.group_keys, input, row);
            const auto found = group_index_by_key.find(key);
            if (found == group_index_by_key.end()) {
                group_index = groups.size();
                group_index_by_key.emplace(key, group_index);
                groups.push_back(make_group(std::move(key), plan));
            } else {
                group_index = found->second;
            }
        }

        auto& group = groups.at(group_index);
        for (std::size_t i = 0; i < plan.aggregate_expressions.size(); ++i) {
            update_aggregate(group.aggregates.at(i), plan.aggregate_expressions[i], input, row);
        }
    }

    storage::ColumnarBatch out;
    for (std::size_t key_index = 0; key_index < plan.group_keys.size(); ++key_index) {
        storage::Int64Column column;
        for (const auto& group : groups) {
            column.append(group.key_values.at(key_index));
        }
        out.add_column(column_identity_name(plan.group_keys[key_index]), std::move(column));
    }

    for (std::size_t aggregate_index = 0; aggregate_index < plan.aggregate_expressions.size(); ++aggregate_index) {
        const auto& aggregate = plan.aggregate_expressions[aggregate_index];
        storage::Int64Column column;
        for (const auto& group : groups) {
            column.append(finalize_aggregate(group.aggregates.at(aggregate_index), aggregate));
        }
        out.add_column(aggregate.output_name, std::move(column));
    }
    return out;
}

storage::ColumnarBatch execute_sort(const plan::LogicalPlan& plan, const Catalog& catalog) {
    const auto& input_plan = require_input(plan);
    if (input_plan.kind == plan::LogicalKind::Project) {
        const auto source = execute_interpreted(require_input(input_plan), catalog);
        const auto rows = stable_sorted_rows(plan.sort_keys, source);
        return materialize_project_rows(input_plan, source, rows);
    }

    auto input = execute_interpreted(input_plan, catalog);
    const auto rows = stable_sorted_rows(plan.sort_keys, input);
    return materialize_rows(input, rows);
}

} // namespace

void Catalog::add_table(std::string name, storage::ColumnarBatch batch) {
    auto [_, inserted] = tables_.emplace(std::move(name), std::move(batch));
    if (!inserted) {
        throw std::invalid_argument("duplicate table");
    }
}

std::optional<catalog::TableSchema> Catalog::find_table_schema(const std::string& name) const {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        return std::nullopt;
    }

    catalog::TableSchema schema;
    schema.name = it->first;
    schema.row_count = it->second.row_count();
    for (const auto& column_name : it->second.column_names()) {
        schema.columns.push_back(catalog::ColumnSchema{column_name, catalog::ColumnType::Int64});
    }
    return schema;
}

const storage::ColumnarBatch& Catalog::table(const std::string& name) const {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        throw std::out_of_range("unknown table: " + name);
    }
    return it->second;
}

storage::ColumnarBatch execute_interpreted(const plan::LogicalPlan& plan, const Catalog& catalog) {
    switch (plan.kind) {
    case plan::LogicalKind::Scan:
        return execute_scan(plan, catalog);
    case plan::LogicalKind::Join:
        return execute_join(plan, catalog);
    case plan::LogicalKind::Filter: {
        auto input = execute_interpreted(*plan.input, catalog);
        return input.filter(evaluate_filter(plan.predicates, input));
    }
    case plan::LogicalKind::Project: {
        auto input = execute_interpreted(*plan.input, catalog);
        storage::ColumnarBatch out;
        for (const auto& projection : plan.projections) {
            out.add_column(projection.output_name, evaluate_projection(projection, input));
        }
        return out;
    }
    case plan::LogicalKind::Aggregate:
        return execute_aggregate(plan, catalog);
    case plan::LogicalKind::Sort:
        return execute_sort(plan, catalog);
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace execution
