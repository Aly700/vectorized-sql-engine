#include "execution/interpreter.hpp"

#include "optimizer/explain.hpp"

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

struct Cell {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool is_null{true};
    std::int64_t int_value{0};
    std::string string_value;
};

bool operator==(const Cell& left, const Cell& right) {
    if (left.is_null || right.is_null) {
        return left.is_null == right.is_null && left.type == right.type;
    }
    if (left.type != right.type) {
        return false;
    }
    return left.type == catalog::ColumnType::Int64 ? left.int_value == right.int_value
                                                   : left.string_value == right.string_value;
}

bool operator<(const Cell& left, const Cell& right) {
    if (left.type != right.type) {
        return static_cast<int>(left.type) < static_cast<int>(right.type);
    }
    if (left.is_null || right.is_null) {
        return left.is_null && !right.is_null;
    }
    return left.type == catalog::ColumnType::Int64 ? left.int_value < right.int_value
                                                   : left.string_value < right.string_value;
}

Cell null_cell(catalog::ColumnType type) {
    return Cell{type, true, 0, ""};
}

Cell int64_cell(std::int64_t value) {
    return Cell{catalog::ColumnType::Int64, false, value, ""};
}

Cell string_cell(std::string value) {
    Cell cell;
    cell.type = catalog::ColumnType::String;
    cell.is_null = false;
    cell.string_value = std::move(value);
    return cell;
}

struct OutputColumn {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    storage::Int64Column int64;
    storage::StringColumn string;

    void reserve(std::size_t count) {
        if (type == catalog::ColumnType::Int64) {
            int64.reserve(count);
        } else {
            string.reserve(count);
        }
    }

    void append(Cell value) {
        if (type == catalog::ColumnType::Int64) {
            if (value.is_null) {
                int64.append_null();
            } else {
                int64.append(value.int_value);
            }
            return;
        }
        if (value.is_null) {
            string.append_null();
        } else {
            string.append(std::move(value.string_value));
        }
    }

    void add_to(storage::ColumnarBatch& batch, const std::string& name) && {
        if (type == catalog::ColumnType::Int64) {
            batch.add_column(name, std::move(int64));
        } else {
            batch.add_column(name, std::move(string));
        }
    }
};

enum class TruthValue { False, True, Unknown };

struct ExecutionContext {
    const Catalog& catalog;
    std::map<const plan::LogicalPlan*, storage::ColumnarBatch> subquery_results;
};

storage::ColumnarBatch execute_node(const plan::LogicalPlan& plan, ExecutionContext& context);
void prepare_subqueries(const plan::LogicalPlan& plan, ExecutionContext& context);

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

Cell cell_at(const storage::ColumnarBatch& batch, const std::string& name, std::size_t row) {
    const auto type = batch.column_type(name);
    if (type == catalog::ColumnType::Int64) {
        const auto& column = batch.column(name);
        if (column.is_null(row)) {
            return null_cell(type);
        }
        return int64_cell(column.at(row));
    }

    const auto& column = batch.string_column(name);
    if (column.is_null(row)) {
        return null_cell(type);
    }
    return string_cell(column.at(row));
}

const storage::ColumnarBatch& materialize_subquery(const std::shared_ptr<const plan::LogicalPlan>& subquery,
                                                  ExecutionContext& context) {
    if (subquery == nullptr) {
        throw std::invalid_argument("bound subquery is missing its logical plan");
    }
    if (const auto existing = context.subquery_results.find(subquery.get());
        existing != context.subquery_results.end()) {
        return existing->second;
    }

    // Phase 21a subplans are uncorrelated and immutable. Prepare their nested
    // subqueries first, then execute this plan exactly once for the top-level
    // query and retain the materialized batch for every outer row.
    prepare_subqueries(*subquery, context);
    auto result = execute_node(*subquery, context);
    const auto [inserted, _] = context.subquery_results.emplace(subquery.get(), std::move(result));
    return inserted->second;
}

Cell evaluate_scalar_subquery(const plan::BoundScalarSubquery& subquery,
                              catalog::ColumnType type,
                              ExecutionContext& context) {
    const auto& result = materialize_subquery(subquery.plan, context);
    if (result.row_count() == 0) {
        return null_cell(type);
    }
    if (result.row_count() > 1) {
        throw std::runtime_error(subquery.name + " returned more than one row");
    }
    if (result.column_names().size() != 1) {
        throw std::logic_error("bound scalar subquery did not produce exactly one output column");
    }
    return cell_at(result, result.column_names().front(), 0);
}

Cell evaluate_scalar(const plan::BoundScalarExpr& expression,
                     const storage::ColumnarBatch& batch,
                     std::size_t row,
                     ExecutionContext& context) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        return cell_at(batch, column_identity_name(*column), row);
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        return int64_cell(literal->value);
    }
    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        return string_cell(literal->value);
    }
    if (std::holds_alternative<sql::NullLiteral>(expression.value)) {
        return null_cell(expression.type);
    }
    const auto& subquery = std::get<plan::BoundScalarSubquery>(expression.value);
    const auto value = evaluate_scalar_subquery(subquery, expression.type, context);
    return value.is_null ? null_cell(expression.type) : value;
}

bool compare_values(const Cell& left, sql::ComparisonOp op, const Cell& right) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return left == right;
    case sql::ComparisonOp::NotEqual:
        return left != right;
    case sql::ComparisonOp::Less:
        return left < right;
    case sql::ComparisonOp::LessEqual:
        return left < right || left == right;
    case sql::ComparisonOp::Greater:
        return right < left;
    case sql::ComparisonOp::GreaterEqual:
        return right < left || left == right;
    }
    throw std::logic_error("unreachable comparison operator");
}

TruthValue truth_from_bool(bool value) {
    return value ? TruthValue::True : TruthValue::False;
}

TruthValue and_truth(TruthValue left, TruthValue right) {
    if (left == TruthValue::False || right == TruthValue::False) {
        return TruthValue::False;
    }
    if (left == TruthValue::True && right == TruthValue::True) {
        return TruthValue::True;
    }
    return TruthValue::Unknown;
}

TruthValue or_truth(TruthValue left, TruthValue right) {
    if (left == TruthValue::True || right == TruthValue::True) {
        return TruthValue::True;
    }
    if (left == TruthValue::False && right == TruthValue::False) {
        return TruthValue::False;
    }
    return TruthValue::Unknown;
}

TruthValue not_truth(TruthValue value) {
    if (value == TruthValue::Unknown) {
        return TruthValue::Unknown;
    }
    return value == TruthValue::True ? TruthValue::False : TruthValue::True;
}

TruthValue evaluate_comparison(const plan::BoundComparisonExpr& comparison,
                               const storage::ColumnarBatch& batch,
                               std::size_t row,
                               ExecutionContext& context) {
    const auto left = evaluate_scalar(comparison.left, batch, row, context);
    const auto right = evaluate_scalar(comparison.right, batch, row, context);
    if (left.is_null || right.is_null) {
        return TruthValue::Unknown;
    }
    return truth_from_bool(compare_values(left, comparison.op, right));
}

TruthValue evaluate_in(const Cell& value,
                       const plan::BoundPredicate& predicate,
                       ExecutionContext& context) {
    const auto& result = materialize_subquery(predicate.subquery, context);
    if (result.row_count() == 0) {
        return TruthValue::False;
    }
    if (result.column_names().size() != 1) {
        throw std::logic_error("bound IN subquery did not produce exactly one output column");
    }
    if (value.is_null) {
        return TruthValue::Unknown;
    }

    bool contains_null = false;
    const auto& column = result.column_names().front();
    for (std::size_t row = 0; row < result.row_count(); ++row) {
        const auto member = cell_at(result, column, row);
        if (member.is_null) {
            contains_null = true;
            continue;
        }
        if (value == member) {
            return TruthValue::True;
        }
    }
    return contains_null ? TruthValue::Unknown : TruthValue::False;
}

const plan::BoundPredicate& require_left_predicate(const plan::BoundPredicate& predicate) {
    if (predicate.left == nullptr) {
        throw std::invalid_argument("bound predicate is missing its left child");
    }
    return *predicate.left;
}

const plan::BoundPredicate& require_right_predicate(const plan::BoundPredicate& predicate) {
    if (predicate.right == nullptr) {
        throw std::invalid_argument("bound predicate is missing its right child");
    }
    return *predicate.right;
}

TruthValue evaluate_predicate(const plan::BoundPredicate& predicate,
                              const storage::ColumnarBatch& batch,
                              std::size_t row,
                              ExecutionContext& context) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return evaluate_comparison(predicate.comparison, batch, row, context);
    case sql::PredicateKind::IsNull:
        return truth_from_bool(evaluate_scalar(predicate.null_check, batch, row, context).is_null);
    case sql::PredicateKind::IsNotNull:
        return truth_from_bool(!evaluate_scalar(predicate.null_check, batch, row, context).is_null);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn: {
        const auto result = evaluate_in(evaluate_scalar(predicate.in_value, batch, row, context), predicate, context);
        return predicate.kind == sql::PredicateKind::NotIn ? not_truth(result) : result;
    }
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists: {
        const auto exists = truth_from_bool(materialize_subquery(predicate.subquery, context).row_count() != 0);
        return predicate.kind == sql::PredicateKind::NotExists ? not_truth(exists) : exists;
    }
    case sql::PredicateKind::And: {
        // Predicate trees are pure in this SQL slice, but evaluation order is
        // still fixed: left child, right child, then the boolean operator.
        const auto left = evaluate_predicate(require_left_predicate(predicate), batch, row, context);
        const auto right = evaluate_predicate(require_right_predicate(predicate), batch, row, context);
        return and_truth(left, right);
    }
    case sql::PredicateKind::Or: {
        const auto left = evaluate_predicate(require_left_predicate(predicate), batch, row, context);
        const auto right = evaluate_predicate(require_right_predicate(predicate), batch, row, context);
        return or_truth(left, right);
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

TruthValue evaluate_predicates(const std::vector<plan::BoundPredicate>& predicates,
                               const storage::ColumnarBatch& batch,
                               std::size_t row,
                               ExecutionContext& context) {
    auto keep = TruthValue::True;
    for (const auto& predicate : predicates) {
        const auto predicate_result = evaluate_predicate(predicate, batch, row, context);
        keep = and_truth(keep, predicate_result);
    }
    return keep;
}

storage::RowMask evaluate_filter(const std::vector<plan::BoundPredicate>& predicates,
                                 const storage::ColumnarBatch& batch,
                                 ExecutionContext& context) {
    storage::RowMask mask;
    mask.keep.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        mask.keep.push_back(evaluate_predicates(predicates, batch, row, context) == TruthValue::True ? 1 : 0);
    }
    return mask;
}

OutputColumn evaluate_projection(const plan::Projection& projection,
                                 const storage::ColumnarBatch& batch,
                                 ExecutionContext& context) {
    OutputColumn column;
    column.type = projection.type;
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        column.append(evaluate_scalar(projection.expression, batch, row, context));
    }
    return column;
}

OutputColumn evaluate_projection(const plan::Projection& projection,
                                 const storage::ColumnarBatch& batch,
                                 const std::vector<std::size_t>& rows,
                                 ExecutionContext& context) {
    OutputColumn column;
    column.type = projection.type;
    for (auto row : rows) {
        column.append(evaluate_scalar(projection.expression, batch, row, context));
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
            const auto name = column_identity_name(key.column);
            const auto left_value = cell_at(batch, name, left);
            const auto right_value = cell_at(batch, name, right);
            if (left_value == right_value) {
                continue;
            }
            const auto left_is_null = left_value.is_null;
            const auto right_is_null = right_value.is_null;
            if (left_is_null || right_is_null) {
                return key.direction == sql::SortDirection::Asc ? right_is_null : left_is_null;
            }
            return key.direction == sql::SortDirection::Asc ? left_value < right_value
                                                            : right_value < left_value;
        }
        return false;
    });
    return rows;
}

storage::ColumnarBatch materialize_rows(const storage::ColumnarBatch& batch, const std::vector<std::size_t>& rows) {
    storage::ColumnarBatch out;
    for (const auto& name : batch.column_names()) {
        OutputColumn column;
        column.type = batch.column_type(name);
        for (auto row : rows) {
            column.append(cell_at(batch, name, row));
        }
        std::move(column).add_to(out, name);
    }
    return out;
}

std::vector<Cell> row_values(const storage::ColumnarBatch& batch, std::size_t row) {
    std::vector<Cell> values;
    values.reserve(batch.column_names().size());
    for (const auto& name : batch.column_names()) {
        values.push_back(cell_at(batch, name, row));
    }
    return values;
}

storage::ColumnarBatch materialize_project_rows(const plan::LogicalPlan& project,
                                                const storage::ColumnarBatch& batch,
                                                const std::vector<std::size_t>& rows,
                                                ExecutionContext& context) {
    storage::ColumnarBatch out;
    for (const auto& projection : project.projections) {
        auto column = evaluate_projection(projection, batch, rows, context);
        std::move(column).add_to(out, projection.output_name);
    }
    return out;
}

storage::ColumnarBatch materialize_project_output_rows(const plan::LogicalPlan& project,
                                                       const storage::ColumnarBatch& batch,
                                                       const std::vector<std::size_t>& rows) {
    storage::ColumnarBatch out;
    for (const auto& projection : project.projections) {
        OutputColumn column;
        column.type = batch.column_type(projection.output_name);
        for (auto row : rows) {
            column.append(cell_at(batch, projection.output_name, row));
        }
        std::move(column).add_to(out, projection.output_name);
    }
    return out;
}

void add_missing_sort_key_columns(storage::ColumnarBatch& sort_input,
                                  const storage::ColumnarBatch& source,
                                  const std::vector<plan::SortKey>& sort_keys) {
    for (const auto& key : sort_keys) {
        const auto name = column_identity_name(key.column);
        if (sort_input.has_column(name)) {
            continue;
        }
        if (!source.has_column(name)) {
            throw std::logic_error("sort key column is not available before or after Project: " + name);
        }
        if (source.column_type(name) == catalog::ColumnType::Int64) {
            sort_input.add_column(name, source.column(name));
        } else {
            sort_input.add_column(name, source.string_column(name));
        }
    }
}

storage::ColumnarBatch materialize_project_sort_input(const plan::LogicalPlan& project,
                                                      const storage::ColumnarBatch& source,
                                                      const std::vector<plan::SortKey>& sort_keys,
                                                      ExecutionContext& context) {
    std::vector<std::size_t> rows;
    rows.reserve(source.row_count());
    for (std::size_t row = 0; row < source.row_count(); ++row) {
        rows.push_back(row);
    }

    auto sort_input = materialize_project_rows(project, source, rows, context);
    add_missing_sort_key_columns(sort_input, source, sort_keys);
    return sort_input;
}

struct AggregateValue {
    std::int64_t count{0};
    std::int64_t sum{0};
    Cell value;
    bool has_value{false};
};

struct GroupState {
    std::vector<Cell> key_values;
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

Cell aggregate_argument_value(const plan::AggregateExpression& aggregate,
                              const storage::ColumnarBatch& batch,
                              std::size_t row) {
    if (!aggregate.argument.has_value()) {
        throw std::logic_error("aggregate argument is missing");
    }
    return cell_at(batch, column_identity_name(*aggregate.argument), row);
}

void update_aggregate(AggregateValue& value,
                      const plan::AggregateExpression& aggregate,
                      const storage::ColumnarBatch& batch,
                      std::size_t row) {
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        if (aggregate.argument.has_value()) {
            if (aggregate_argument_value(aggregate, batch, row).is_null) {
                return;
            }
        }
        increment_count(value, aggregate.output_name);
        return;
    case sql::AggregateFunction::Sum: {
        const auto argument = aggregate_argument_value(aggregate, batch, row);
        if (argument.is_null) {
            return;
        }
        if (!value.has_value) {
            value.sum = argument.int_value;
            value.has_value = true;
            return;
        }
        value.sum = checked_sum(value.sum, argument.int_value, aggregate.output_name);
        return;
    }
    case sql::AggregateFunction::Min: {
        const auto argument = aggregate_argument_value(aggregate, batch, row);
        if (argument.is_null) {
            return;
        }
        if (!value.has_value || argument < value.value) {
            value.value = argument;
        }
        value.has_value = true;
        return;
    }
    case sql::AggregateFunction::Max: {
        const auto argument = aggregate_argument_value(aggregate, batch, row);
        if (argument.is_null) {
            return;
        }
        if (!value.has_value || value.value < argument) {
            value.value = argument;
        }
        value.has_value = true;
        return;
    }
    }
    throw std::logic_error("unreachable aggregate function");
}

Cell finalize_aggregate(const AggregateValue& value, const plan::AggregateExpression& aggregate) {
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        return int64_cell(value.count);
    case sql::AggregateFunction::Sum:
        if (!value.has_value) {
            return null_cell(catalog::ColumnType::Int64);
        }
        return int64_cell(value.sum);
    case sql::AggregateFunction::Min:
        if (!value.has_value) {
            return null_cell(aggregate.type);
        }
        return value.value;
    case sql::AggregateFunction::Max:
        if (!value.has_value) {
            return null_cell(aggregate.type);
        }
        return value.value;
    }
    throw std::logic_error("unreachable aggregate function");
}

std::vector<Cell> group_key_values(const std::vector<plan::BoundColumnRef>& group_keys,
                                   const storage::ColumnarBatch& batch,
                                   std::size_t row) {
    std::vector<Cell> values;
    values.reserve(group_keys.size());
    for (const auto& key : group_keys) {
        values.push_back(cell_at(batch, column_identity_name(key), row));
    }
    return values;
}

GroupState make_group(std::vector<Cell> key_values, const plan::LogicalPlan& plan) {
    GroupState group;
    group.key_values = std::move(key_values);
    group.aggregates.resize(plan.aggregate_expressions.size());
    return group;
}

void prepare_scalar_subquery(const plan::BoundScalarExpr& expression, ExecutionContext& context) {
    const auto* subquery = std::get_if<plan::BoundScalarSubquery>(&expression.value);
    if (subquery == nullptr) {
        return;
    }
    const auto& result = materialize_subquery(subquery->plan, context);
    if (result.row_count() > 1) {
        throw std::runtime_error(subquery->name + " returned more than one row");
    }
}

void prepare_predicate_subqueries(const plan::BoundPredicate& predicate, ExecutionContext& context) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        prepare_scalar_subquery(predicate.comparison.left, context);
        prepare_scalar_subquery(predicate.comparison.right, context);
        return;
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        prepare_scalar_subquery(predicate.null_check, context);
        return;
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        prepare_scalar_subquery(predicate.in_value, context);
        (void)materialize_subquery(predicate.subquery, context);
        return;
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        (void)materialize_subquery(predicate.subquery, context);
        return;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        prepare_predicate_subqueries(require_left_predicate(predicate), context);
        prepare_predicate_subqueries(require_right_predicate(predicate), context);
        return;
    }
    throw std::logic_error("unreachable predicate kind");
}

void prepare_subqueries(const plan::LogicalPlan& plan, ExecutionContext& context) {
    for (const auto& projection : plan.projections) {
        prepare_scalar_subquery(projection.expression, context);
    }
    for (const auto& predicate : plan.predicates) {
        prepare_predicate_subqueries(predicate, context);
    }
    if (plan.input != nullptr) {
        prepare_subqueries(*plan.input, context);
    }
    if (plan.left != nullptr) {
        prepare_subqueries(*plan.left, context);
    }
    if (plan.right != nullptr) {
        prepare_subqueries(*plan.right, context);
    }
}

storage::ColumnarBatch execute_scan(const plan::LogicalPlan& plan, const Catalog& catalog) {
    const auto& input = catalog.table(plan.table);
    storage::ColumnarBatch out;
    for (const auto& column_name : input.column_names()) {
        if (input.column_type(column_name) == catalog::ColumnType::Int64) {
            out.add_column(plan.binding_name + "." + column_name, input.column(column_name));
        } else {
            out.add_column(plan.binding_name + "." + column_name, input.string_column(column_name));
        }
    }
    return out;
}

Cell evaluate_join_scalar(const plan::BoundScalarExpr& expression,
                          const storage::ColumnarBatch& left,
                          std::size_t left_row,
                          const storage::ColumnarBatch& right,
                          std::size_t right_row,
                          ExecutionContext& context) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        const auto name = column_identity_name(*column);
        if (left.has_column(name)) {
            return cell_at(left, name, left_row);
        }
        if (right.has_column(name)) {
            return cell_at(right, name, right_row);
        }
        throw std::logic_error("bound join column identity is missing from both inputs: " + name);
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        return int64_cell(literal->value);
    }
    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        return string_cell(literal->value);
    }
    if (std::holds_alternative<sql::NullLiteral>(expression.value)) {
        return null_cell(expression.type);
    }
    return evaluate_scalar_subquery(
        std::get<plan::BoundScalarSubquery>(expression.value), expression.type, context);
}

TruthValue evaluate_join_comparison(const plan::BoundComparisonExpr& comparison,
                                    const storage::ColumnarBatch& left,
                                    std::size_t left_row,
                                    const storage::ColumnarBatch& right,
                                    std::size_t right_row,
                                    ExecutionContext& context) {
    const auto left_value = evaluate_join_scalar(comparison.left, left, left_row, right, right_row, context);
    const auto right_value = evaluate_join_scalar(comparison.right, left, left_row, right, right_row, context);
    if (left_value.is_null || right_value.is_null) {
        return TruthValue::Unknown;
    }
    return truth_from_bool(compare_values(left_value, comparison.op, right_value));
}

TruthValue evaluate_join_predicate(const plan::BoundPredicate& predicate,
                                   const storage::ColumnarBatch& left,
                                   std::size_t left_row,
                                   const storage::ColumnarBatch& right,
                                   std::size_t right_row,
                                   ExecutionContext& context) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return evaluate_join_comparison(predicate.comparison, left, left_row, right, right_row, context);
    case sql::PredicateKind::IsNull:
        return truth_from_bool(
            evaluate_join_scalar(predicate.null_check, left, left_row, right, right_row, context).is_null);
    case sql::PredicateKind::IsNotNull:
        return truth_from_bool(
            !evaluate_join_scalar(predicate.null_check, left, left_row, right, right_row, context).is_null);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn: {
        const auto value = evaluate_join_scalar(predicate.in_value, left, left_row, right, right_row, context);
        const auto result = evaluate_in(value, predicate, context);
        return predicate.kind == sql::PredicateKind::NotIn ? not_truth(result) : result;
    }
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists: {
        const auto exists = truth_from_bool(materialize_subquery(predicate.subquery, context).row_count() != 0);
        return predicate.kind == sql::PredicateKind::NotExists ? not_truth(exists) : exists;
    }
    case sql::PredicateKind::And: {
        const auto left_result =
            evaluate_join_predicate(require_left_predicate(predicate), left, left_row, right, right_row, context);
        const auto right_result =
            evaluate_join_predicate(require_right_predicate(predicate), left, left_row, right, right_row, context);
        return and_truth(left_result, right_result);
    }
    case sql::PredicateKind::Or: {
        const auto left_result =
            evaluate_join_predicate(require_left_predicate(predicate), left, left_row, right, right_row, context);
        const auto right_result =
            evaluate_join_predicate(require_right_predicate(predicate), left, left_row, right, right_row, context);
        return or_truth(left_result, right_result);
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

TruthValue evaluate_join_predicates(const std::vector<plan::BoundPredicate>& predicates,
                                    const storage::ColumnarBatch& left,
                                    std::size_t left_row,
                                    const storage::ColumnarBatch& right,
                                    std::size_t right_row,
                                    ExecutionContext& context) {
    auto keep = TruthValue::True;
    for (const auto& predicate : predicates) {
        const auto predicate_result =
            evaluate_join_predicate(predicate, left, left_row, right, right_row, context);
        keep = and_truth(keep, predicate_result);
    }
    return keep;
}

storage::ColumnarBatch execute_join(const plan::LogicalPlan& plan, ExecutionContext& context) {
    const auto left = execute_node(require_left(plan), context);
    const auto right = execute_node(require_right(plan), context);

    std::vector<std::string> output_names = left.column_names();
    output_names.insert(output_names.end(), right.column_names().begin(), right.column_names().end());
    std::vector<OutputColumn> output_columns(output_names.size());
    for (std::size_t i = 0; i < left.column_names().size(); ++i) {
        output_columns[i].type = left.column_type(left.column_names()[i]);
    }
    for (std::size_t i = 0; i < right.column_names().size(); ++i) {
        output_columns[left.column_names().size() + i].type = right.column_type(right.column_names()[i]);
    }

    auto append_output_row = [&](std::size_t left_row, std::optional<std::size_t> right_row) {
        std::size_t output_index = 0;
        for (const auto& column_name : left.column_names()) {
            output_columns[output_index++].append(cell_at(left, column_name, left_row));
        }
        for (const auto& column_name : right.column_names()) {
            output_columns[output_index++].append(right_row.has_value()
                                                      ? cell_at(right, column_name, *right_row)
                                                      : null_cell(right.column_type(column_name)));
        }
    };

    // Join order is part of the oracle contract: for each preserved left row
    // in input order, scan every right row in input order. LEFT JOIN emits one
    // NULL-extended row only when no right row's ON predicates are TRUE.
    for (std::size_t left_row = 0; left_row < left.row_count(); ++left_row) {
        bool matched = false;
        for (std::size_t right_row = 0; right_row < right.row_count(); ++right_row) {
            if (evaluate_join_predicates(plan.predicates, left, left_row, right, right_row, context) !=
                TruthValue::True) {
                continue;
            }

            matched = true;
            append_output_row(left_row, right_row);
        }
        if (!matched && plan.join_kind == plan::JoinKind::Left) {
            append_output_row(left_row, std::nullopt);
        }
    }

    storage::ColumnarBatch out;
    for (std::size_t i = 0; i < output_names.size(); ++i) {
        std::move(output_columns[i]).add_to(out, output_names[i]);
    }
    return out;
}

storage::ColumnarBatch execute_aggregate(const plan::LogicalPlan& plan, ExecutionContext& context) {
    const auto input = execute_node(require_input(plan), context);

    std::map<std::vector<Cell>, std::size_t> group_index_by_key;
    std::vector<GroupState> groups;
    if (plan.group_keys.empty()) {
        groups.push_back(make_group({}, plan));
    }

    for (std::size_t row = 0; row < input.row_count(); ++row) {
        std::size_t group_index = 0;
        if (!plan.group_keys.empty()) {
            // GROUP BY uses distinct-style key equality: NULL slots compare
            // equal here, while predicate and join equality still use SQL 3VL.
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
        OutputColumn column;
        column.type = plan.group_keys[key_index].type;
        for (const auto& group : groups) {
            column.append(group.key_values.at(key_index));
        }
        std::move(column).add_to(out, column_identity_name(plan.group_keys[key_index]));
    }

    for (std::size_t aggregate_index = 0; aggregate_index < plan.aggregate_expressions.size(); ++aggregate_index) {
        const auto& aggregate = plan.aggregate_expressions[aggregate_index];
        OutputColumn column;
        column.type = aggregate.type;
        for (const auto& group : groups) {
            column.append(finalize_aggregate(group.aggregates.at(aggregate_index), aggregate));
        }
        std::move(column).add_to(out, aggregate.output_name);
    }
    return out;
}

storage::ColumnarBatch execute_sort(const plan::LogicalPlan& plan, ExecutionContext& context) {
    const auto& input_plan = require_input(plan);
    if (input_plan.kind == plan::LogicalKind::Project) {
        const auto source = execute_node(require_input(input_plan), context);
        const auto sort_input = materialize_project_sort_input(input_plan, source, plan.sort_keys, context);
        const auto rows = stable_sorted_rows(plan.sort_keys, sort_input);
        return materialize_project_output_rows(input_plan, sort_input, rows);
    }

    auto input = execute_node(input_plan, context);
    const auto rows = stable_sorted_rows(plan.sort_keys, input);
    return materialize_rows(input, rows);
}

storage::ColumnarBatch execute_distinct(const plan::LogicalPlan& plan, ExecutionContext& context) {
    const auto input = execute_node(require_input(plan), context);
    std::map<std::vector<Cell>, bool> seen;
    std::vector<std::size_t> rows;
    rows.reserve(input.row_count());
    for (std::size_t row = 0; row < input.row_count(); ++row) {
        auto key = row_values(input, row);
        const auto [_, inserted] = seen.emplace(std::move(key), true);
        if (inserted) {
            rows.push_back(row);
        }
    }
    return materialize_rows(input, rows);
}

storage::ColumnarBatch execute_limit(const plan::LogicalPlan& plan, ExecutionContext& context) {
    const auto input = execute_node(require_input(plan), context);
    const auto count = std::min(plan.limit_count, input.row_count());
    std::vector<std::size_t> rows;
    rows.reserve(count);
    for (std::size_t row = 0; row < count; ++row) {
        rows.push_back(row);
    }
    return materialize_rows(input, rows);
}

storage::ColumnarBatch execute_node(const plan::LogicalPlan& plan, ExecutionContext& context) {
    switch (plan.kind) {
    case plan::LogicalKind::Scan:
        return execute_scan(plan, context.catalog);
    case plan::LogicalKind::Join:
        return execute_join(plan, context);
    case plan::LogicalKind::Filter: {
        auto input = execute_node(require_input(plan), context);
        return input.filter(evaluate_filter(plan.predicates, input, context));
    }
    case plan::LogicalKind::Project: {
        auto input = execute_node(require_input(plan), context);
        storage::ColumnarBatch out;
        for (const auto& projection : plan.projections) {
            auto column = evaluate_projection(projection, input, context);
            std::move(column).add_to(out, projection.output_name);
        }
        return out;
    }
    case plan::LogicalKind::Aggregate:
        return execute_aggregate(plan, context);
    case plan::LogicalKind::Distinct:
        return execute_distinct(plan, context);
    case plan::LogicalKind::Sort:
        return execute_sort(plan, context);
    case plan::LogicalKind::Limit:
        return execute_limit(plan, context);
    case plan::LogicalKind::Explain:
        return optimizer::explain(require_input(plan), context.catalog);
    }
    throw std::logic_error("unreachable logical plan kind");
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
        schema.columns.push_back(catalog::ColumnSchema{column_name, it->second.column_type(column_name)});
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
    ExecutionContext context{catalog, {}};
    if (plan.kind != plan::LogicalKind::Explain) {
        prepare_subqueries(plan, context);
    }
    return execute_node(plan, context);
}

} // namespace execution
