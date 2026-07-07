#include "execution/vectorized.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace execution {
namespace {

using Cell = std::optional<std::int64_t>;

enum class TruthValue { False, True, Unknown };

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
    std::vector<plan::BoundPredicate> residuals;
};

// HashKey keeps NULL as an explicit slot for GROUP BY and DISTINCT, where SQL
// uses distinct-style equality. Hash joins avoid NULL slots with
// make_non_null_key(), preserving comparison semantics where NULL = NULL is
// UNKNOWN rather than TRUE.
struct HashKey {
    std::vector<Cell> values;

    bool operator==(const HashKey& other) const { return values == other.values; }
};

struct HashKeyHash {
    std::size_t operator()(const HashKey& key) const {
        std::size_t seed = key.values.size();
        for (auto value : key.values) {
            const auto hashed = value.has_value() ? std::hash<std::int64_t>{}(*value) : 0x517cc1b727220a95ULL;
            seed ^= hashed + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        }
        return seed;
    }
};

struct CompiledColumn {
    const std::vector<std::int64_t>* values{nullptr};
    const std::vector<std::uint8_t>* validity{nullptr};

    [[nodiscard]] bool is_null(std::size_t row) const {
        return validity != nullptr && (*validity)[row] == 0;
    }

    [[nodiscard]] Cell cell(std::size_t row) const {
        if (is_null(row)) {
            return std::nullopt;
        }
        return (*values)[row];
    }
};

struct JoinOutputBuilder {
    std::vector<std::string> names;
    std::vector<storage::Int64Column> columns;
    std::vector<CompiledColumn> left_columns;
    std::vector<CompiledColumn> right_columns;
};

struct AggregateValue {
    std::int64_t count{0};
    std::int64_t sum{0};
    std::int64_t min{0};
    std::int64_t max{0};
    bool has_value{false};
};

struct AggregateGroupState {
    HashKey key;
    std::vector<AggregateValue> aggregates;
};

struct CompiledScalar {
    const std::vector<std::int64_t>* column_values{nullptr};
    const std::vector<std::uint8_t>* column_validity{nullptr};
    std::int64_t literal{0};
    bool literal_is_null{false};

    [[nodiscard]] bool is_null(std::size_t row) const {
        if (column_values == nullptr) {
            return literal_is_null;
        }
        return column_validity != nullptr && (*column_validity)[row] == 0;
    }

    [[nodiscard]] Cell cell(std::size_t row) const {
        if (is_null(row)) {
            return std::nullopt;
        }
        return column_values == nullptr ? literal : (*column_values)[row];
    }
};

struct CompiledComparison {
    CompiledScalar left;
    sql::ComparisonOp op{sql::ComparisonOp::Equal};
    CompiledScalar right;
};

struct CompiledPredicate {
    sql::PredicateKind kind{sql::PredicateKind::Comparison};
    CompiledComparison comparison;
    CompiledScalar null_check;
    std::shared_ptr<CompiledPredicate> left;
    std::shared_ptr<CompiledPredicate> right;
};

struct CompiledProjection {
    std::string output_name;
    CompiledScalar expression;
};

struct CompiledSortKey {
    const std::vector<std::int64_t>* values{nullptr};
    const std::vector<std::uint8_t>* validity{nullptr};
    sql::SortDirection direction{sql::SortDirection::Asc};
};

struct CompiledJoinScalar {
    const std::vector<std::int64_t>* left_values{nullptr};
    const std::vector<std::uint8_t>* left_validity{nullptr};
    const std::vector<std::int64_t>* right_values{nullptr};
    const std::vector<std::uint8_t>* right_validity{nullptr};
    std::int64_t literal{0};
    bool literal_is_null{false};

    [[nodiscard]] bool is_null(std::size_t left_row, std::size_t right_row) const {
        if (left_values != nullptr) {
            return left_validity != nullptr && (*left_validity)[left_row] == 0;
        }
        if (right_values != nullptr) {
            return right_validity != nullptr && (*right_validity)[right_row] == 0;
        }
        return literal_is_null;
    }

    [[nodiscard]] Cell cell(std::size_t left_row, std::size_t right_row) const {
        if (is_null(left_row, right_row)) {
            return std::nullopt;
        }
        if (left_values != nullptr) {
            return (*left_values)[left_row];
        }
        if (right_values != nullptr) {
            return (*right_values)[right_row];
        }
        return literal;
    }
};

struct CompiledJoinComparison {
    CompiledJoinScalar left;
    sql::ComparisonOp op{sql::ComparisonOp::Equal};
    CompiledJoinScalar right;
};

struct CompiledJoinPredicate {
    sql::PredicateKind kind{sql::PredicateKind::Comparison};
    CompiledJoinComparison comparison;
    CompiledJoinScalar null_check;
    std::shared_ptr<CompiledJoinPredicate> left;
    std::shared_ptr<CompiledJoinPredicate> right;
};

struct CompiledAggregateExpression {
    const plan::AggregateExpression* aggregate{nullptr};
    const std::vector<std::int64_t>* argument_values{nullptr};
    const std::vector<std::uint8_t>* argument_validity{nullptr};
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

void append_cell(storage::Int64Column& column, Cell value) {
    if (value.has_value()) {
        column.append(*value);
    } else {
        column.append_null();
    }
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
    if (column.binding.empty()) {
        return column.column;
    }
    return column.binding + "." + column.column;
}

CompiledColumn compile_column(const plan::BoundColumnRef& column, const storage::ColumnarBatch& batch) {
    const auto& storage_column = batch.column(column_identity_name(column));
    return CompiledColumn{&storage_column.values(),
                          storage_column.has_nulls() ? &storage_column.validity() : nullptr};
}

CompiledColumn compile_named_column(const storage::ColumnarBatch& batch, const std::string& name) {
    const auto& storage_column = batch.column(name);
    return CompiledColumn{&storage_column.values(),
                          storage_column.has_nulls() ? &storage_column.validity() : nullptr};
}

std::vector<CompiledColumn> compile_columns(const std::vector<plan::BoundColumnRef>& columns,
                                            const storage::ColumnarBatch& batch) {
    std::vector<CompiledColumn> compiled;
    compiled.reserve(columns.size());
    for (const auto& column : columns) {
        compiled.push_back(compile_column(column, batch));
    }
    return compiled;
}

std::vector<CompiledColumn> compile_named_columns(const storage::ColumnarBatch& batch,
                                                  const std::vector<std::string>& names) {
    std::vector<CompiledColumn> compiled;
    compiled.reserve(names.size());
    for (const auto& name : names) {
        compiled.push_back(compile_named_column(batch, name));
    }
    return compiled;
}

CompiledScalar compile_scalar(const plan::BoundScalarExpr& expression, const storage::ColumnarBatch& batch) {
    CompiledScalar compiled;
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        const auto& storage_column = batch.column(column_identity_name(*column));
        compiled.column_values = &storage_column.values();
        compiled.column_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
        return compiled;
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        compiled.literal = literal->value;
        return compiled;
    }
    compiled.literal_is_null = true;
    return compiled;
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

CompiledComparison compile_comparison(const plan::BoundComparisonExpr& comparison,
                                      const storage::ColumnarBatch& batch) {
    CompiledComparison compiled;
    compiled.left = compile_scalar(comparison.left, batch);
    compiled.op = comparison.op;
    compiled.right = compile_scalar(comparison.right, batch);
    return compiled;
}

TruthValue evaluate_comparison(const CompiledComparison& comparison, std::size_t row) {
    const auto left = comparison.left.cell(row);
    const auto right = comparison.right.cell(row);
    if (!left.has_value() || !right.has_value()) {
        return TruthValue::Unknown;
    }
    return truth_from_bool(compare_values(*left, comparison.op, *right));
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

const CompiledPredicate& require_left_predicate(const CompiledPredicate& predicate) {
    if (predicate.left == nullptr) {
        throw std::invalid_argument("compiled predicate is missing its left child");
    }
    return *predicate.left;
}

const CompiledPredicate& require_right_predicate(const CompiledPredicate& predicate) {
    if (predicate.right == nullptr) {
        throw std::invalid_argument("compiled predicate is missing its right child");
    }
    return *predicate.right;
}

CompiledPredicate compile_predicate(const plan::BoundPredicate& predicate, const storage::ColumnarBatch& batch) {
    CompiledPredicate compiled;
    compiled.kind = predicate.kind;
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        compiled.comparison = compile_comparison(predicate.comparison, batch);
        return compiled;
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        compiled.null_check = compile_scalar(predicate.null_check, batch);
        return compiled;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        compiled.left = std::make_shared<CompiledPredicate>(compile_predicate(require_left_predicate(predicate), batch));
        compiled.right = std::make_shared<CompiledPredicate>(compile_predicate(require_right_predicate(predicate), batch));
        return compiled;
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<CompiledPredicate> compile_predicates(const std::vector<plan::BoundPredicate>& predicates,
                                                  const storage::ColumnarBatch& batch) {
    std::vector<CompiledPredicate> compiled;
    compiled.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        compiled.push_back(compile_predicate(predicate, batch));
    }
    return compiled;
}

TruthValue evaluate_predicate(const CompiledPredicate& predicate, std::size_t row) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return evaluate_comparison(predicate.comparison, row);
    case sql::PredicateKind::IsNull:
        return truth_from_bool(predicate.null_check.is_null(row));
    case sql::PredicateKind::IsNotNull:
        return truth_from_bool(!predicate.null_check.is_null(row));
    case sql::PredicateKind::And: {
        // Evaluation is deliberately left-to-right and short-circuit-free so
        // the vectorized path has the same observable order as the oracle.
        const auto left = evaluate_predicate(require_left_predicate(predicate), row);
        const auto right = evaluate_predicate(require_right_predicate(predicate), row);
        return and_truth(left, right);
    }
    case sql::PredicateKind::Or: {
        const auto left = evaluate_predicate(require_left_predicate(predicate), row);
        const auto right = evaluate_predicate(require_right_predicate(predicate), row);
        return or_truth(left, right);
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

TruthValue evaluate_predicates(const std::vector<CompiledPredicate>& predicates, std::size_t row) {
    auto keep = TruthValue::True;
    for (const auto& predicate : predicates) {
        const auto predicate_result = evaluate_predicate(predicate, row);
        keep = and_truth(keep, predicate_result);
    }
    return keep;
}

CompiledJoinScalar compile_join_scalar(const plan::BoundScalarExpr& expression,
                                       const storage::ColumnarBatch& left,
                                       const storage::ColumnarBatch& right) {
    CompiledJoinScalar compiled;
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        const auto name = column_identity_name(*column);
        if (left.has_column(name)) {
            const auto& storage_column = left.column(name);
            compiled.left_values = &storage_column.values();
            compiled.left_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            return compiled;
        }
        if (right.has_column(name)) {
            const auto& storage_column = right.column(name);
            compiled.right_values = &storage_column.values();
            compiled.right_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            return compiled;
        }
        throw std::logic_error("bound join column identity is missing from both inputs: " + name);
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        compiled.literal = literal->value;
        return compiled;
    }
    compiled.literal_is_null = true;
    return compiled;
}

CompiledJoinComparison compile_join_comparison(const plan::BoundComparisonExpr& comparison,
                                               const storage::ColumnarBatch& left,
                                               const storage::ColumnarBatch& right) {
    CompiledJoinComparison compiled;
    compiled.left = compile_join_scalar(comparison.left, left, right);
    compiled.op = comparison.op;
    compiled.right = compile_join_scalar(comparison.right, left, right);
    return compiled;
}

TruthValue evaluate_join_comparison(const CompiledJoinComparison& comparison,
                                    std::size_t left_row,
                                    std::size_t right_row) {
    const auto left = comparison.left.cell(left_row, right_row);
    const auto right = comparison.right.cell(left_row, right_row);
    if (!left.has_value() || !right.has_value()) {
        return TruthValue::Unknown;
    }
    return truth_from_bool(compare_values(*left, comparison.op, *right));
}

const CompiledJoinPredicate& require_left_predicate(const CompiledJoinPredicate& predicate) {
    if (predicate.left == nullptr) {
        throw std::invalid_argument("compiled join predicate is missing its left child");
    }
    return *predicate.left;
}

const CompiledJoinPredicate& require_right_predicate(const CompiledJoinPredicate& predicate) {
    if (predicate.right == nullptr) {
        throw std::invalid_argument("compiled join predicate is missing its right child");
    }
    return *predicate.right;
}

CompiledJoinPredicate compile_join_predicate(const plan::BoundPredicate& predicate,
                                             const storage::ColumnarBatch& left,
                                             const storage::ColumnarBatch& right) {
    CompiledJoinPredicate compiled;
    compiled.kind = predicate.kind;
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        compiled.comparison = compile_join_comparison(predicate.comparison, left, right);
        return compiled;
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        compiled.null_check = compile_join_scalar(predicate.null_check, left, right);
        return compiled;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        compiled.left =
            std::make_shared<CompiledJoinPredicate>(compile_join_predicate(require_left_predicate(predicate), left, right));
        compiled.right =
            std::make_shared<CompiledJoinPredicate>(compile_join_predicate(require_right_predicate(predicate), left, right));
        return compiled;
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<CompiledJoinPredicate> compile_join_predicates(const std::vector<plan::BoundPredicate>& predicates,
                                                           const storage::ColumnarBatch& left,
                                                           const storage::ColumnarBatch& right) {
    std::vector<CompiledJoinPredicate> compiled;
    compiled.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        compiled.push_back(compile_join_predicate(predicate, left, right));
    }
    return compiled;
}

TruthValue evaluate_join_predicate(const CompiledJoinPredicate& predicate,
                                   std::size_t left_row,
                                   std::size_t right_row) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return evaluate_join_comparison(predicate.comparison, left_row, right_row);
    case sql::PredicateKind::IsNull:
        return truth_from_bool(predicate.null_check.is_null(left_row, right_row));
    case sql::PredicateKind::IsNotNull:
        return truth_from_bool(!predicate.null_check.is_null(left_row, right_row));
    case sql::PredicateKind::And: {
        const auto left_result = evaluate_join_predicate(require_left_predicate(predicate), left_row, right_row);
        const auto right_result = evaluate_join_predicate(require_right_predicate(predicate), left_row, right_row);
        return and_truth(left_result, right_result);
    }
    case sql::PredicateKind::Or: {
        const auto left_result = evaluate_join_predicate(require_left_predicate(predicate), left_row, right_row);
        const auto right_result = evaluate_join_predicate(require_right_predicate(predicate), left_row, right_row);
        return or_truth(left_result, right_result);
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

TruthValue evaluate_join_predicates(const std::vector<CompiledJoinPredicate>& predicates,
                                    std::size_t left_row,
                                    std::size_t right_row) {
    auto keep = TruthValue::True;
    for (const auto& predicate : predicates) {
        const auto predicate_result = evaluate_join_predicate(predicate, left_row, right_row);
        keep = and_truth(keep, predicate_result);
    }
    return keep;
}

bool try_add_equi_key(const plan::BoundPredicate& predicate,
                      const storage::ColumnarBatch& left,
                      const storage::ColumnarBatch& right,
                      JoinPredicateSplit& split) {
    if (predicate.kind != sql::PredicateKind::Comparison) {
        return false;
    }

    const auto& comparison = predicate.comparison;
    if (comparison.op != sql::ComparisonOp::Equal) {
        return false;
    }

    const auto* lhs = std::get_if<plan::BoundColumnRef>(&comparison.left.value);
    const auto* rhs = std::get_if<plan::BoundColumnRef>(&comparison.right.value);
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

JoinPredicateSplit split_join_predicates(const std::vector<plan::BoundPredicate>& predicates,
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

HashKey make_key(std::size_t row, const std::vector<CompiledColumn>& key_columns) {
    HashKey key;
    key.values.reserve(key_columns.size());
    for (const auto& column : key_columns) {
        key.values.push_back(column.cell(row));
    }
    return key;
}

std::optional<HashKey> make_non_null_key(std::size_t row, const std::vector<CompiledColumn>& key_columns) {
    HashKey key;
    key.values.reserve(key_columns.size());
    for (const auto& column : key_columns) {
        const auto value = column.cell(row);
        if (!value.has_value()) {
            return std::nullopt;
        }
        key.values.push_back(value);
    }
    return key;
}

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

CompiledAggregateExpression compile_aggregate_expression(const plan::AggregateExpression& aggregate,
                                                         const storage::ColumnarBatch& batch) {
    CompiledAggregateExpression compiled;
    compiled.aggregate = &aggregate;
    if (aggregate.argument.has_value()) {
        const auto& storage_column = batch.column(column_identity_name(*aggregate.argument));
        compiled.argument_values = &storage_column.values();
        compiled.argument_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
    }
    return compiled;
}

std::vector<CompiledAggregateExpression> compile_aggregate_expressions(
    const std::vector<plan::AggregateExpression>& aggregates,
    const storage::ColumnarBatch& batch) {
    std::vector<CompiledAggregateExpression> compiled;
    compiled.reserve(aggregates.size());
    for (const auto& aggregate : aggregates) {
        compiled.push_back(compile_aggregate_expression(aggregate, batch));
    }
    return compiled;
}

Cell aggregate_argument_value(const CompiledAggregateExpression& aggregate, std::size_t row) {
    if (aggregate.argument_values == nullptr) {
        throw std::logic_error("aggregate argument is missing");
    }
    if (aggregate.argument_validity != nullptr && (*aggregate.argument_validity)[row] == 0) {
        return std::nullopt;
    }
    return (*aggregate.argument_values)[row];
}

void update_aggregate(AggregateValue& value,
                      const CompiledAggregateExpression& compiled,
                      std::size_t row) {
    const auto& aggregate = *compiled.aggregate;
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        if (aggregate.argument.has_value() && !aggregate_argument_value(compiled, row).has_value()) {
            return;
        }
        increment_count(value, aggregate.output_name);
        return;
    case sql::AggregateFunction::Sum: {
        const auto argument = aggregate_argument_value(compiled, row);
        if (!argument.has_value()) {
            return;
        }
        if (!value.has_value) {
            value.sum = *argument;
            value.has_value = true;
            return;
        }
        value.sum = checked_sum(value.sum, *argument, aggregate.output_name);
        return;
    }
    case sql::AggregateFunction::Min: {
        const auto argument = aggregate_argument_value(compiled, row);
        if (!argument.has_value()) {
            return;
        }
        if (!value.has_value || *argument < value.min) {
            value.min = *argument;
        }
        value.has_value = true;
        return;
    }
    case sql::AggregateFunction::Max: {
        const auto argument = aggregate_argument_value(compiled, row);
        if (!argument.has_value()) {
            return;
        }
        if (!value.has_value || *argument > value.max) {
            value.max = *argument;
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
        return value.count;
    case sql::AggregateFunction::Sum:
        if (!value.has_value) {
            return std::nullopt;
        }
        return value.sum;
    case sql::AggregateFunction::Min:
        if (!value.has_value) {
            return std::nullopt;
        }
        return value.min;
    case sql::AggregateFunction::Max:
        if (!value.has_value) {
            return std::nullopt;
        }
        return value.max;
    }
    throw std::logic_error("unreachable aggregate function");
}

AggregateGroupState make_aggregate_group(HashKey key, const plan::PhysicalPlan& plan) {
    AggregateGroupState group;
    group.key = std::move(key);
    group.aggregates.resize(plan.aggregate_expressions.size());
    return group;
}

JoinOutputBuilder make_join_output_builder(const storage::ColumnarBatch& left,
                                           const storage::ColumnarBatch& right) {
    JoinOutputBuilder builder;
    builder.names = left.column_names();
    builder.names.insert(builder.names.end(), right.column_names().begin(), right.column_names().end());
    builder.columns.resize(builder.names.size());
    builder.left_columns.reserve(left.column_names().size());
    for (const auto& column_name : left.column_names()) {
        builder.left_columns.push_back(compile_named_column(left, column_name));
    }
    builder.right_columns.reserve(right.column_names().size());
    for (const auto& column_name : right.column_names()) {
        builder.right_columns.push_back(compile_named_column(right, column_name));
    }
    return builder;
}

void append_joined_row(JoinOutputBuilder& builder,
                       std::size_t left_row,
                       std::size_t right_row) {
    std::size_t output_index = 0;
    for (const auto& column : builder.left_columns) {
        append_cell(builder.columns[output_index++], column.cell(left_row));
    }
    for (const auto& column : builder.right_columns) {
        append_cell(builder.columns[output_index++], column.cell(right_row));
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
                                                const std::vector<plan::BoundPredicate>& predicates) {
    const auto compiled_predicates = compile_join_predicates(predicates, *left.batch, *right.batch);
    auto builder = make_join_output_builder(*left.batch, *right.batch);
    for (auto left_row : *left.selection) {
        for (auto right_row : *right.selection) {
            if (evaluate_join_predicates(compiled_predicates, left_row, right_row) == TruthValue::True) {
                append_joined_row(builder, left_row, right_row);
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
    const auto compiled_left_key_columns = compile_columns(left_key_columns, *left.batch);
    const auto compiled_right_key_columns = compile_columns(right_key_columns, *right.batch);
    const auto compiled_residuals = compile_join_predicates(predicates.residuals, *left.batch, *right.batch);

    // Lookup-only hash table: output order must come exclusively from probing
    // left rows in order and from each per-key right-row vector's insertion order.
    std::unordered_map<HashKey, std::vector<std::size_t>, HashKeyHash> right_rows_by_key;
    for (auto right_row : *right.selection) {
        auto key = make_non_null_key(right_row, compiled_right_key_columns);
        if (!key.has_value()) {
            continue;
        }
        right_rows_by_key[*key].push_back(right_row);
    }

    auto builder = make_join_output_builder(*left.batch, *right.batch);
    for (auto left_row : *left.selection) {
        auto key = make_non_null_key(left_row, compiled_left_key_columns);
        if (!key.has_value()) {
            continue;
        }
        const auto matching_right_rows = right_rows_by_key.find(*key);
        if (matching_right_rows == right_rows_by_key.end()) {
            continue;
        }

        for (auto right_row : matching_right_rows->second) {
            if (evaluate_join_predicates(compiled_residuals, left_row, right_row) == TruthValue::True) {
                append_joined_row(builder, left_row, right_row);
            }
        }
    }
    return finish_join_output(std::move(builder));
}

BatchView execute_scan(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    const auto& input = catalog.table(plan.table);
    auto qualified = std::make_shared<storage::ColumnarBatch>();
    for (const auto& column_name : input.column_names()) {
        if (input.column_type(column_name) == catalog::ColumnType::Int64) {
            qualified->add_column(plan.binding_name + "." + column_name, input.column(column_name));
        } else {
            qualified->add_column(plan.binding_name + "." + column_name, input.string_column(column_name));
        }
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
BatchView execute_aggregate(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_distinct(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_sort(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_limit(const plan::PhysicalPlan& plan, const Catalog& catalog);

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
    case plan::PhysicalKind::Aggregate:
        return execute_aggregate(plan, catalog);
    case plan::PhysicalKind::Distinct:
        return execute_distinct(plan, catalog);
    case plan::PhysicalKind::Sort:
        return execute_sort(plan, catalog);
    case plan::PhysicalKind::Limit:
        return execute_limit(plan, catalog);
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

    const auto predicates = compile_predicates(plan.predicates, *input.batch);
    SelectionVector rows;
    rows.reserve(input.selection->size());
    for (auto row : *input.selection) {
        if (evaluate_predicates(predicates, row) == TruthValue::True) {
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

    std::vector<CompiledProjection> projections;
    projections.reserve(plan.projections.size());
    for (const auto& projection : plan.projections) {
        projections.push_back(CompiledProjection{projection.output_name, compile_scalar(projection.expression, *input.batch)});
    }

    storage::ColumnarBatch out;
    for (const auto& projection : projections) {
        storage::Int64Column column;
        column.reserve(input.selection->size());
        for (auto row : *input.selection) {
            append_cell(column, projection.expression.cell(row));
        }
        out.add_column(projection.output_name, std::move(column));
    }
    return out;
}

storage::Int64Column materialize_selected_column(const BatchView& input, const std::string& name) {
    storage::Int64Column column;
    column.reserve(input.selection->size());
    const auto input_column = compile_named_column(*input.batch, name);
    for (auto row : *input.selection) {
        append_cell(column, input_column.cell(row));
    }
    return column;
}

void add_missing_sort_key_columns(storage::ColumnarBatch& sort_input,
                                  const BatchView& source,
                                  const std::vector<plan::SortKey>& sort_keys) {
    validate_view(source);
    for (const auto& key : sort_keys) {
        const auto name = column_identity_name(key.column);
        if (sort_input.has_column(name)) {
            continue;
        }
        if (!source.batch->has_column(name)) {
            throw std::logic_error("sort key column is not available before or after Project: " + name);
        }
        sort_input.add_column(name, materialize_selected_column(source, name));
    }
}

storage::ColumnarBatch materialize_project_sort_input(const plan::PhysicalPlan& project,
                                                      const BatchView& source,
                                                      const std::vector<plan::SortKey>& sort_keys) {
    auto sort_input = materialize_projection(project, source);
    add_missing_sort_key_columns(sort_input, source, sort_keys);
    return sort_input;
}

storage::ColumnarBatch materialize_project_output_columns(const plan::PhysicalPlan& project,
                                                          const storage::ColumnarBatch& batch,
                                                          const SelectionVector& rows) {
    storage::ColumnarBatch out;
    for (const auto& projection : project.projections) {
        storage::Int64Column column;
        column.reserve(rows.size());
        const auto input_column = compile_named_column(batch, projection.output_name);
        for (auto row : rows) {
            append_cell(column, input_column.cell(row));
        }
        out.add_column(projection.output_name, std::move(column));
    }
    return out;
}

std::vector<CompiledSortKey> compile_sort_keys(const std::vector<plan::SortKey>& sort_keys,
                                               const storage::ColumnarBatch& batch) {
    std::vector<CompiledSortKey> compiled;
    compiled.reserve(sort_keys.size());
    for (const auto& key : sort_keys) {
        const auto& storage_column = batch.column(column_identity_name(key.column));
        compiled.push_back(CompiledSortKey{&storage_column.values(),
                                           storage_column.has_nulls() ? &storage_column.validity() : nullptr,
                                           key.direction});
    }
    return compiled;
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

SelectionVectorPtr sort_selection(const std::vector<plan::SortKey>& sort_keys, const BatchView& input) {
    validate_view(input);

    const auto compiled_sort_keys = compile_sort_keys(sort_keys, *input.batch);
    SelectionVector rows = *input.selection;
    std::stable_sort(rows.begin(), rows.end(), [&](std::size_t left, std::size_t right) {
        for (const auto& key : compiled_sort_keys) {
            const auto left_value =
                key.validity != nullptr && (*key.validity)[left] == 0 ? Cell{} : Cell{(*key.values)[left]};
            const auto right_value =
                key.validity != nullptr && (*key.validity)[right] == 0 ? Cell{} : Cell{(*key.values)[right]};
            if (left_value == right_value) {
                continue;
            }
            return sort_cell_less(left_value, right_value, key.direction);
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

storage::ColumnarBatch materialize_aggregate(const plan::PhysicalPlan& plan, const BatchView& input) {
    validate_view(input);

    const auto group_keys = compile_columns(plan.group_keys, *input.batch);
    const auto aggregate_expressions = compile_aggregate_expressions(plan.aggregate_expressions, *input.batch);
    std::unordered_map<HashKey, std::size_t, HashKeyHash> group_index_by_key;
    std::vector<AggregateGroupState> groups;
    if (plan.group_keys.empty()) {
        groups.push_back(make_aggregate_group(HashKey{}, plan));
    }

    for (auto row : *input.selection) {
        // GROUP BY uses distinct-style key equality: explicit NULL slots in
        // HashKey compare equal here. Hash joins call make_non_null_key()
        // instead, so this equality never makes NULL match NULL in joins.
        std::size_t group_index = 0;
        if (!plan.group_keys.empty()) {
            auto key = make_key(row, group_keys);
            const auto found = group_index_by_key.find(key);
            if (found == group_index_by_key.end()) {
                group_index = groups.size();
                group_index_by_key.emplace(key, group_index);
                groups.push_back(make_aggregate_group(std::move(key), plan));
            } else {
                group_index = found->second;
            }
        }

        auto& group = groups.at(group_index);
        for (std::size_t i = 0; i < aggregate_expressions.size(); ++i) {
            update_aggregate(group.aggregates.at(i), aggregate_expressions[i], row);
        }
    }

    storage::ColumnarBatch out;
    for (std::size_t key_index = 0; key_index < plan.group_keys.size(); ++key_index) {
        storage::Int64Column column;
        column.reserve(groups.size());
        for (const auto& group : groups) {
            append_cell(column, group.key.values.at(key_index));
        }
        out.add_column(column_identity_name(plan.group_keys[key_index]), std::move(column));
    }

    for (std::size_t aggregate_index = 0; aggregate_index < plan.aggregate_expressions.size(); ++aggregate_index) {
        const auto& aggregate = plan.aggregate_expressions[aggregate_index];
        storage::Int64Column column;
        column.reserve(groups.size());
        for (const auto& group : groups) {
            append_cell(column, finalize_aggregate(group.aggregates.at(aggregate_index), aggregate));
        }
        out.add_column(aggregate.output_name, std::move(column));
    }
    return out;
}

BatchView execute_aggregate(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto input = execute_to_view(require_input(plan), catalog);
    auto materialized = std::make_shared<const storage::ColumnarBatch>(materialize_aggregate(plan, input));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    validate_view(view);
    return view;
}

BatchView execute_distinct(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto input = execute_to_view(require_input(plan), catalog);
    validate_view(input);

    const auto output_columns = compile_named_columns(*input.batch, input.batch->column_names());
    std::unordered_set<HashKey, HashKeyHash> seen;
    SelectionVector rows;
    rows.reserve(input.selection->size());
    for (auto row : *input.selection) {
        // DISTINCT shares GROUP BY's key equality: rows with matching NULL
        // slots deduplicate, independent of comparison or join semantics.
        auto key = make_key(row, output_columns);
        const auto [_, inserted] = seen.emplace(std::move(key));
        if (inserted) {
            rows.push_back(row);
        }
    }

    input.selection = make_selection(std::move(rows));
    validate_view(input);
    return input;
}

BatchView execute_sort(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    const auto& input_plan = require_input(plan);
    if (input_plan.kind == plan::PhysicalKind::Project) {
        auto source = execute_to_view(require_input(input_plan), catalog);
        auto sort_input =
            std::make_shared<const storage::ColumnarBatch>(materialize_project_sort_input(input_plan, source, plan.sort_keys));

        BatchView sort_view;
        sort_view.owned_batch = sort_input;
        sort_view.batch = sort_input.get();
        sort_view.selection = identity_selection(sort_input->row_count());
        sort_view.selection = sort_selection(plan.sort_keys, sort_view);

        auto materialized = std::make_shared<const storage::ColumnarBatch>(
            materialize_project_output_columns(input_plan, *sort_view.batch, *sort_view.selection));

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

BatchView execute_limit(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto input = execute_to_view(require_input(plan), catalog);
    validate_view(input);

    const auto count = std::min(plan.limit_count, input.selection->size());
    SelectionVector rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(input.selection->at(i));
    }

    input.selection = make_selection(std::move(rows));
    validate_view(input);
    return input;
}

storage::ColumnarBatch materialize_view(const BatchView& view) {
    validate_view(view);

    storage::ColumnarBatch out;
    for (const auto& name : view.batch->column_names()) {
        storage::Int64Column column;
        column.reserve(view.selection->size());
        const auto input_column = compile_named_column(*view.batch, name);
        for (auto row : *view.selection) {
            append_cell(column, input_column.cell(row));
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
