#include "execution/vectorized.hpp"

#include "optimizer/explain.hpp"

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

enum class TruthValue { False, True, Unknown };
enum class KernelDispatch { Int64Only, Typed };

using SelectionVector = std::vector<std::size_t>;
using SelectionVectorPtr = std::shared_ptr<const SelectionVector>;

struct BatchView {
    std::shared_ptr<const storage::ColumnarBatch> owned_batch;
    const storage::ColumnarBatch* batch{nullptr};
    SelectionVectorPtr selection;
    bool selection_rows_match_positions{false};
};

struct EquiJoinKey {
    plan::BoundColumnRef left;
    plan::BoundColumnRef right;
};

struct JoinPredicateSplit {
    std::vector<EquiJoinKey> equi_keys;
    std::vector<plan::BoundPredicate> residuals;
};

struct TypedCell {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool is_null{true};
    std::int64_t int_value{0};
    std::string string_value;
};

bool operator==(const TypedCell& left, const TypedCell& right) {
    if (left.type != right.type) {
        return false;
    }
    if (left.is_null || right.is_null) {
        return left.is_null == right.is_null;
    }
    return left.type == catalog::ColumnType::Int64 ? left.int_value == right.int_value
                                                   : left.string_value == right.string_value;
}

TypedCell null_cell(catalog::ColumnType type) {
    TypedCell cell;
    cell.type = type;
    cell.is_null = true;
    return cell;
}

TypedCell int_cell(std::int64_t value) {
    TypedCell cell;
    cell.type = catalog::ColumnType::Int64;
    cell.is_null = false;
    cell.int_value = value;
    return cell;
}

TypedCell string_cell(std::string value) {
    TypedCell cell;
    cell.type = catalog::ColumnType::String;
    cell.is_null = false;
    cell.string_value = std::move(value);
    return cell;
}

struct MaterializedValueSet {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool is_empty{true};
    bool has_null{false};
    std::unordered_set<std::int64_t> int_values;
    std::unordered_set<std::string> string_values;

    [[nodiscard]] bool contains(const TypedCell& cell) const {
        if (cell.type != type || cell.is_null) {
            return false;
        }
        return type == catalog::ColumnType::Int64 ? int_values.contains(cell.int_value)
                                                  : string_values.contains(cell.string_value);
    }
};

struct ExecutionContext {
    const Catalog& catalog;
    std::unordered_map<const plan::LogicalPlan*, storage::ColumnarBatch> subquery_results;
    std::unordered_map<const plan::LogicalPlan*, MaterializedValueSet> subquery_value_sets;
};

const storage::ColumnarBatch& prepared_subquery(
    const std::shared_ptr<const plan::LogicalPlan>& subquery,
    const ExecutionContext& context) {
    if (subquery == nullptr) {
        throw std::invalid_argument("bound subquery is missing its logical plan");
    }
    const auto found = context.subquery_results.find(subquery.get());
    if (found == context.subquery_results.end()) {
        throw std::logic_error("vectorized subquery was not eagerly prepared");
    }
    return found->second;
}

const MaterializedValueSet& prepared_value_set(
    const std::shared_ptr<const plan::LogicalPlan>& subquery,
    const ExecutionContext& context) {
    if (subquery == nullptr) {
        throw std::invalid_argument("bound IN subquery is missing its logical plan");
    }
    const auto found = context.subquery_value_sets.find(subquery.get());
    if (found == context.subquery_value_sets.end()) {
        throw std::logic_error("vectorized IN subquery set was not eagerly prepared");
    }
    return found->second;
}

std::size_t hash_cell(const TypedCell& cell) {
    std::size_t seed = static_cast<std::size_t>(cell.type);
    const auto hashed = [&] {
        if (cell.is_null) {
            return std::size_t{0x517cc1b727220a95ULL};
        }
        if (cell.type == catalog::ColumnType::Int64) {
            return std::hash<std::int64_t>{}(cell.int_value);
        }
        return std::hash<std::string>{}(cell.string_value);
    }();
    seed ^= hashed + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

// HashKey keeps NULL as an explicit typed slot for GROUP BY and DISTINCT, where
// SQL uses distinct-style equality. Hash joins avoid NULL slots with
// make_non_null_key(), preserving comparison semantics where NULL = NULL is
// UNKNOWN rather than TRUE.
struct HashKey {
    std::vector<TypedCell> values;

    bool operator==(const HashKey& other) const { return values == other.values; }
};

struct HashKeyHash {
    std::size_t operator()(const HashKey& key) const {
        std::size_t seed = key.values.size();
        for (const auto& value : key.values) {
            const auto hashed = hash_cell(value);
            seed ^= hashed + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        }
        return seed;
    }
};

struct CompiledColumn {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    const std::vector<std::int64_t>* int_values{nullptr};
    const std::vector<std::string>* string_values{nullptr};
    const std::vector<std::uint8_t>* validity{nullptr};

    [[nodiscard]] bool is_null(std::size_t row) const {
        return validity != nullptr && (*validity)[row] == 0;
    }

    [[nodiscard]] TypedCell cell(std::size_t row) const {
        if (is_null(row)) {
            return null_cell(type);
        }
        if (type == catalog::ColumnType::Int64) {
            return int_cell((*int_values)[row]);
        }
        return string_cell((*string_values)[row]);
    }
};

struct TypedColumnBuilder {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    storage::Int64Column int64;
    storage::StringColumn string;

    explicit TypedColumnBuilder(catalog::ColumnType column_type) : type(column_type) {}

    void reserve(std::size_t count) {
        if (type == catalog::ColumnType::Int64) {
            int64.reserve(count);
        } else {
            string.reserve(count);
        }
    }

    void append(const TypedCell& cell) {
        if (cell.type != type) {
            throw std::logic_error("typed column builder received a cell with the wrong type");
        }
        if (type == catalog::ColumnType::Int64) {
            if (cell.is_null) {
                int64.append_null();
            } else {
                int64.append(cell.int_value);
            }
            return;
        }
        if (cell.is_null) {
            string.append_null();
        } else {
            string.append(cell.string_value);
        }
    }

    void add_to(storage::ColumnarBatch& batch, std::string name) && {
        if (type == catalog::ColumnType::Int64) {
            batch.add_column(std::move(name), std::move(int64));
        } else {
            batch.add_column(std::move(name), std::move(string));
        }
    }
};

struct JoinOutputBuilder {
    std::vector<std::string> names;
    std::vector<TypedColumnBuilder> columns;
    std::vector<CompiledColumn> left_columns;
    std::vector<CompiledColumn> right_columns;
};

struct AggregateValue {
    std::int64_t count{0};
    std::int64_t sum{0};
    std::int64_t min{0};
    std::int64_t max{0};
    std::string string_min;
    std::string string_max;
    bool has_value{false};
};

struct AggregateGroupState {
    HashKey key;
    std::vector<AggregateValue> aggregates;
};

struct CompiledScalar {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    const std::vector<std::int64_t>* column_int_values{nullptr};
    const std::vector<std::string>* column_string_values{nullptr};
    const std::vector<std::uint8_t>* column_validity{nullptr};
    std::int64_t literal{0};
    std::string string_literal;
    bool literal_is_null{false};

    [[nodiscard]] bool is_null(std::size_t row) const {
        if (column_int_values == nullptr && column_string_values == nullptr) {
            return literal_is_null;
        }
        return column_validity != nullptr && (*column_validity)[row] == 0;
    }

    [[nodiscard]] TypedCell cell(std::size_t row) const {
        if (is_null(row)) {
            return null_cell(type);
        }
        if (type == catalog::ColumnType::Int64) {
            return column_int_values == nullptr ? int_cell(literal) : int_cell((*column_int_values)[row]);
        }
        return column_string_values == nullptr ? string_cell(string_literal)
                                               : string_cell((*column_string_values)[row]);
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
    CompiledScalar in_value;
    const MaterializedValueSet* value_set{nullptr};
    bool exists_value{false};
    std::shared_ptr<CompiledPredicate> left;
    std::shared_ptr<CompiledPredicate> right;
};

struct PredicateMask {
    std::vector<std::uint8_t> is_true;
    std::vector<std::uint8_t> is_known;
};

struct PredicateEvaluationDomain {
    const SelectionVector* selection{nullptr};
    std::size_t size{0};
    bool rows_match_positions{false};
};

struct CompiledProjection {
    std::string output_name;
    CompiledScalar expression;
};

struct CompiledSortKey {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    const std::vector<std::int64_t>* int_values{nullptr};
    const std::vector<std::string>* string_values{nullptr};
    const std::vector<std::uint8_t>* validity{nullptr};
    sql::SortDirection direction{sql::SortDirection::Asc};
};

struct CompiledJoinScalar {
    enum class Source { Literal, Left, Right };

    Source source{Source::Literal};
    catalog::ColumnType type{catalog::ColumnType::Int64};
    const std::vector<std::int64_t>* int_values{nullptr};
    const std::vector<std::string>* string_values{nullptr};
    const std::vector<std::uint8_t>* validity{nullptr};
    std::int64_t literal{0};
    std::string string_literal;
    bool literal_is_null{false};

    [[nodiscard]] bool is_null(std::size_t left_row, std::size_t right_row) const {
        if (source != Source::Literal) {
            const auto row = source == Source::Left ? left_row : right_row;
            return validity != nullptr && (*validity)[row] == 0;
        }
        return literal_is_null;
    }

    [[nodiscard]] TypedCell cell(std::size_t left_row, std::size_t right_row) const {
        if (is_null(left_row, right_row)) {
            return null_cell(type);
        }
        if (source == Source::Literal) {
            return type == catalog::ColumnType::Int64 ? int_cell(literal) : string_cell(string_literal);
        }
        const auto row = source == Source::Left ? left_row : right_row;
        return type == catalog::ColumnType::Int64 ? int_cell((*int_values)[row]) : string_cell((*string_values)[row]);
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
    CompiledJoinScalar in_value;
    const MaterializedValueSet* value_set{nullptr};
    bool exists_value{false};
    std::shared_ptr<CompiledJoinPredicate> left;
    std::shared_ptr<CompiledJoinPredicate> right;
};

struct CompiledAggregateExpression {
    const plan::AggregateExpression* aggregate{nullptr};
    const std::vector<std::int64_t>* argument_int_values{nullptr};
    const std::vector<std::string>* argument_string_values{nullptr};
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

void append_cell(TypedColumnBuilder& column, const TypedCell& value) {
    column.append(value);
}

void validate_view(const BatchView& view) {
    if (view.batch == nullptr) {
        throw std::logic_error("vectorized batch view is missing a batch");
    }
    if (!view.selection) {
        throw std::logic_error("vectorized batch view is missing a selection vector");
    }
    for (std::size_t position = 0; position < view.selection->size(); ++position) {
        const auto row = (*view.selection)[position];
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
    const auto name = column_identity_name(column);
    if (batch.column_type(name) != column.type) {
        throw std::logic_error("bound column type does not match storage column: " + name);
    }
    if (column.type == catalog::ColumnType::Int64) {
        const auto& storage_column = batch.column(name);
        return CompiledColumn{column.type,
                              &storage_column.values(),
                              nullptr,
                              storage_column.has_nulls() ? &storage_column.validity() : nullptr};
    }
    const auto& storage_column = batch.string_column(name);
    return CompiledColumn{column.type,
                          nullptr,
                          &storage_column.values(),
                          storage_column.has_nulls() ? &storage_column.validity() : nullptr};
}

CompiledColumn compile_named_column(const storage::ColumnarBatch& batch, const std::string& name) {
    const auto type = batch.column_type(name);
    if (type == catalog::ColumnType::Int64) {
        const auto& storage_column = batch.column(name);
        return CompiledColumn{type,
                              &storage_column.values(),
                              nullptr,
                              storage_column.has_nulls() ? &storage_column.validity() : nullptr};
    }
    const auto& storage_column = batch.string_column(name);
    return CompiledColumn{type,
                          nullptr,
                          &storage_column.values(),
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

CompiledScalar compile_scalar(const plan::BoundScalarExpr& expression,
                              const storage::ColumnarBatch& batch,
                              const ExecutionContext& context) {
    CompiledScalar compiled;
    compiled.type = expression.type;
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        const auto name = column_identity_name(*column);
        if (batch.column_type(name) != expression.type) {
            throw std::logic_error("bound scalar type does not match storage column: " + name);
        }
        if (expression.type == catalog::ColumnType::Int64) {
            const auto& storage_column = batch.column(name);
            compiled.column_int_values = &storage_column.values();
            compiled.column_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            return compiled;
        }
        const auto& storage_column = batch.string_column(name);
        compiled.column_string_values = &storage_column.values();
        compiled.column_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
        return compiled;
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        compiled.literal = literal->value;
        return compiled;
    }
    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        compiled.string_literal = literal->value;
        return compiled;
    }
    if (const auto* subquery = std::get_if<plan::BoundScalarSubquery>(&expression.value)) {
        const auto& result = prepared_subquery(subquery->plan, context);
        if (result.row_count() > 1) {
            throw std::runtime_error(subquery->name + " returned more than one row");
        }
        if (result.row_count() == 0) {
            compiled.literal_is_null = true;
            return compiled;
        }
        if (result.column_names().size() != 1 || result.column_type(result.column_names().front()) != expression.type) {
            throw std::logic_error("scalar subquery materialization has the wrong schema");
        }
        const auto& name = result.column_names().front();
        if (expression.type == catalog::ColumnType::Int64) {
            const auto& column = result.column(name);
            compiled.literal_is_null = column.is_null(0);
            if (!compiled.literal_is_null) {
                compiled.literal = column.at(0);
            }
        } else {
            const auto& column = result.string_column(name);
            compiled.literal_is_null = column.is_null(0);
            if (!compiled.literal_is_null) {
                compiled.string_literal = column.at(0);
            }
        }
        return compiled;
    }
    compiled.literal_is_null = true;
    return compiled;
}

bool compare_int_values(std::int64_t left, sql::ComparisonOp op, std::int64_t right) {
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

bool compare_string_values(const std::string& left, sql::ComparisonOp op, const std::string& right) {
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

bool compare_cells(const TypedCell& left, sql::ComparisonOp op, const TypedCell& right) {
    if (left.type != right.type) {
        throw std::logic_error("typed comparison received mismatched cell types");
    }
    return left.type == catalog::ColumnType::Int64 ? compare_int_values(left.int_value, op, right.int_value)
                                                   : compare_string_values(left.string_value, op, right.string_value);
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

TruthValue evaluate_in_truth(const TypedCell& value, const MaterializedValueSet& set) {
    if (set.is_empty) {
        return TruthValue::False;
    }
    if (value.is_null) {
        return TruthValue::Unknown;
    }
    if (set.contains(value)) {
        return TruthValue::True;
    }
    return set.has_null ? TruthValue::Unknown : TruthValue::False;
}

CompiledComparison compile_comparison(const plan::BoundComparisonExpr& comparison,
                                      const storage::ColumnarBatch& batch,
                                      const ExecutionContext& context) {
    CompiledComparison compiled;
    compiled.left = compile_scalar(comparison.left, batch, context);
    compiled.op = comparison.op;
    compiled.right = compile_scalar(comparison.right, batch, context);
    return compiled;
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

CompiledPredicate compile_predicate(const plan::BoundPredicate& predicate,
                                    const storage::ColumnarBatch& batch,
                                    const ExecutionContext& context) {
    CompiledPredicate compiled;
    compiled.kind = predicate.kind;
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        compiled.comparison = compile_comparison(predicate.comparison, batch, context);
        return compiled;
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        compiled.null_check = compile_scalar(predicate.null_check, batch, context);
        return compiled;
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        compiled.in_value = compile_scalar(predicate.in_value, batch, context);
        compiled.value_set = &prepared_value_set(predicate.subquery, context);
        return compiled;
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        compiled.exists_value = prepared_subquery(predicate.subquery, context).row_count() != 0;
        return compiled;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        compiled.left = std::make_shared<CompiledPredicate>(
            compile_predicate(require_left_predicate(predicate), batch, context));
        compiled.right = std::make_shared<CompiledPredicate>(
            compile_predicate(require_right_predicate(predicate), batch, context));
        return compiled;
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<CompiledPredicate> compile_predicates(const std::vector<plan::BoundPredicate>& predicates,
                                                  const storage::ColumnarBatch& batch,
                                                  const ExecutionContext& context) {
    std::vector<CompiledPredicate> compiled;
    compiled.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        compiled.push_back(compile_predicate(predicate, batch, context));
    }
    return compiled;
}

PredicateMask make_predicate_mask(std::size_t size) {
    return PredicateMask{std::vector<std::uint8_t>(size), std::vector<std::uint8_t>(size)};
}

PredicateMask make_constant_predicate_mask(std::size_t size, std::uint8_t true_value, std::uint8_t known_value) {
    return PredicateMask{std::vector<std::uint8_t>(size, true_value), std::vector<std::uint8_t>(size, known_value)};
}

PredicateEvaluationDomain make_predicate_domain(const BatchView& input) {
    if (!input.selection) {
        throw std::logic_error("cannot build a predicate domain without a selection vector");
    }
    return PredicateEvaluationDomain{input.selection.get(), input.selection->size(), input.selection_rows_match_positions};
}

template <typename Func>
void for_each_domain_row(const PredicateEvaluationDomain& domain, Func&& func) {
    if (domain.selection == nullptr) {
        throw std::logic_error("predicate evaluation domain is missing a selection vector");
    }
    if (domain.rows_match_positions) {
        for (std::size_t position = 0; position < domain.size; ++position) {
            func(position, position);
        }
        return;
    }
    for (std::size_t position = 0; position < domain.size; ++position) {
        func(position, (*domain.selection)[position]);
    }
}

bool scalar_has_column(const CompiledScalar& scalar) {
    return scalar.column_int_values != nullptr || scalar.column_string_values != nullptr;
}

bool scalar_is_always_null(const CompiledScalar& scalar) {
    return !scalar_has_column(scalar) && scalar.literal_is_null;
}

bool scalar_can_be_null(const CompiledScalar& scalar) {
    return scalar_has_column(scalar) ? scalar.column_validity != nullptr : scalar.literal_is_null;
}

template <typename Compare>
PredicateMask evaluate_int_comparison_mask_with(const CompiledComparison& comparison,
                                                const PredicateEvaluationDomain& domain,
                                                Compare&& compare) {
    if (scalar_is_always_null(comparison.left) || scalar_is_always_null(comparison.right)) {
        return make_constant_predicate_mask(domain.size, 0, 0);
    }
    if (!scalar_has_column(comparison.left) && !scalar_has_column(comparison.right)) {
        return make_constant_predicate_mask(
            domain.size,
            compare(comparison.left.literal, comparison.right.literal) ? std::uint8_t{1} : std::uint8_t{0},
            1);
    }

    auto mask = make_predicate_mask(domain.size);
    const auto* left_values = comparison.left.column_int_values;
    const auto* right_values = comparison.right.column_int_values;
    const auto left_literal = comparison.left.literal;
    const auto right_literal = comparison.right.literal;
    const auto nulls_possible = scalar_can_be_null(comparison.left) || scalar_can_be_null(comparison.right);

    auto write_position = [&](std::size_t position, std::size_t row, std::int64_t left, std::int64_t right) {
        const auto known = nulls_possible ? static_cast<std::uint8_t>(!comparison.left.is_null(row) &&
                                                                      !comparison.right.is_null(row))
                                          : std::uint8_t{1};
        mask.is_known[position] = known;
        mask.is_true[position] = static_cast<std::uint8_t>(known & static_cast<std::uint8_t>(compare(left, right)));
    };

    if (left_values != nullptr && right_values != nullptr) {
        for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
            write_position(position, row, (*left_values)[row], (*right_values)[row]);
        });
    } else if (left_values != nullptr) {
        for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
            write_position(position, row, (*left_values)[row], right_literal);
        });
    } else if (right_values != nullptr) {
        for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
            write_position(position, row, left_literal, (*right_values)[row]);
        });
    } else {
        throw std::logic_error("int comparison mask reached an impossible scalar shape");
    }
    return mask;
}

template <typename Compare>
PredicateMask evaluate_string_comparison_mask_with(const CompiledComparison& comparison,
                                                   const PredicateEvaluationDomain& domain,
                                                   Compare&& compare) {
    if (scalar_is_always_null(comparison.left) || scalar_is_always_null(comparison.right)) {
        return make_constant_predicate_mask(domain.size, 0, 0);
    }
    if (!scalar_has_column(comparison.left) && !scalar_has_column(comparison.right)) {
        return make_constant_predicate_mask(
            domain.size,
            compare(comparison.left.string_literal, comparison.right.string_literal) ? std::uint8_t{1}
                                                                                     : std::uint8_t{0},
            1);
    }

    auto mask = make_predicate_mask(domain.size);
    const auto* left_values = comparison.left.column_string_values;
    const auto* right_values = comparison.right.column_string_values;
    const auto& left_literal = comparison.left.string_literal;
    const auto& right_literal = comparison.right.string_literal;
    const auto nulls_possible = scalar_can_be_null(comparison.left) || scalar_can_be_null(comparison.right);

    auto write_position = [&](std::size_t position, std::size_t row, const std::string& left, const std::string& right) {
        const auto known = nulls_possible ? static_cast<std::uint8_t>(!comparison.left.is_null(row) &&
                                                                      !comparison.right.is_null(row))
                                          : std::uint8_t{1};
        mask.is_known[position] = known;
        mask.is_true[position] = static_cast<std::uint8_t>(known & static_cast<std::uint8_t>(compare(left, right)));
    };

    if (left_values != nullptr && right_values != nullptr) {
        for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
            write_position(position, row, (*left_values)[row], (*right_values)[row]);
        });
    } else if (left_values != nullptr) {
        for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
            write_position(position, row, (*left_values)[row], right_literal);
        });
    } else if (right_values != nullptr) {
        for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
            write_position(position, row, left_literal, (*right_values)[row]);
        });
    } else {
        throw std::logic_error("string comparison mask reached an impossible scalar shape");
    }
    return mask;
}

PredicateMask evaluate_int_comparison_mask(const CompiledComparison& comparison,
                                           const PredicateEvaluationDomain& domain) {
    switch (comparison.op) {
    case sql::ComparisonOp::Equal:
        return evaluate_int_comparison_mask_with(comparison, domain, [](auto left, auto right) { return left == right; });
    case sql::ComparisonOp::NotEqual:
        return evaluate_int_comparison_mask_with(comparison, domain, [](auto left, auto right) { return left != right; });
    case sql::ComparisonOp::Less:
        return evaluate_int_comparison_mask_with(comparison, domain, [](auto left, auto right) { return left < right; });
    case sql::ComparisonOp::LessEqual:
        return evaluate_int_comparison_mask_with(comparison, domain, [](auto left, auto right) { return left <= right; });
    case sql::ComparisonOp::Greater:
        return evaluate_int_comparison_mask_with(comparison, domain, [](auto left, auto right) { return left > right; });
    case sql::ComparisonOp::GreaterEqual:
        return evaluate_int_comparison_mask_with(comparison, domain, [](auto left, auto right) { return left >= right; });
    }
    throw std::logic_error("unreachable comparison operator");
}

PredicateMask evaluate_string_comparison_mask(const CompiledComparison& comparison,
                                              const PredicateEvaluationDomain& domain) {
    switch (comparison.op) {
    case sql::ComparisonOp::Equal:
        return evaluate_string_comparison_mask_with(comparison, domain, [](const auto& left, const auto& right) {
            return left == right;
        });
    case sql::ComparisonOp::NotEqual:
        return evaluate_string_comparison_mask_with(comparison, domain, [](const auto& left, const auto& right) {
            return left != right;
        });
    case sql::ComparisonOp::Less:
        return evaluate_string_comparison_mask_with(comparison, domain, [](const auto& left, const auto& right) {
            return left < right;
        });
    case sql::ComparisonOp::LessEqual:
        return evaluate_string_comparison_mask_with(comparison, domain, [](const auto& left, const auto& right) {
            return left <= right;
        });
    case sql::ComparisonOp::Greater:
        return evaluate_string_comparison_mask_with(comparison, domain, [](const auto& left, const auto& right) {
            return left > right;
        });
    case sql::ComparisonOp::GreaterEqual:
        return evaluate_string_comparison_mask_with(comparison, domain, [](const auto& left, const auto& right) {
            return left >= right;
        });
    }
    throw std::logic_error("unreachable comparison operator");
}

PredicateMask evaluate_comparison_mask(const CompiledComparison& comparison,
                                       const PredicateEvaluationDomain& domain) {
    if (comparison.left.type != comparison.right.type) {
        throw std::logic_error("compiled comparison mask received mismatched scalar types");
    }
    return comparison.left.type == catalog::ColumnType::Int64 ? evaluate_int_comparison_mask(comparison, domain)
                                                              : evaluate_string_comparison_mask(comparison, domain);
}

PredicateMask evaluate_null_check_mask(const CompiledScalar& scalar,
                                       const PredicateEvaluationDomain& domain,
                                       bool is_not_null) {
    if (!scalar_can_be_null(scalar)) {
        return make_constant_predicate_mask(domain.size, is_not_null ? std::uint8_t{1} : std::uint8_t{0}, 1);
    }
    if (scalar_is_always_null(scalar)) {
        return make_constant_predicate_mask(domain.size, is_not_null ? std::uint8_t{0} : std::uint8_t{1}, 1);
    }

    auto mask = make_predicate_mask(domain.size);
    for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
        const auto is_null = scalar.is_null(row);
        mask.is_true[position] = static_cast<std::uint8_t>(is_not_null ? !is_null : is_null);
        mask.is_known[position] = 1;
    });
    return mask;
}

void combine_and_in_place(PredicateMask& left, const PredicateMask& right) {
    if (left.is_true.size() != right.is_true.size() || left.is_known.size() != right.is_known.size() ||
        left.is_true.size() != left.is_known.size()) {
        throw std::logic_error("AND mask combine received mismatched mask sizes");
    }
    for (std::size_t i = 0; i < left.is_true.size(); ++i) {
        const auto lt = left.is_true[i];
        const auto lk = left.is_known[i];
        const auto rt = right.is_true[i];
        const auto rk = right.is_known[i];
        const auto true_value = static_cast<std::uint8_t>(lt & rt);
        const auto false_value = static_cast<std::uint8_t>((lk & (lt ^ 1U)) | (rk & (rt ^ 1U)));
        left.is_true[i] = true_value;
        left.is_known[i] = static_cast<std::uint8_t>(true_value | false_value);
    }
}

void combine_or_in_place(PredicateMask& left, const PredicateMask& right) {
    if (left.is_true.size() != right.is_true.size() || left.is_known.size() != right.is_known.size() ||
        left.is_true.size() != left.is_known.size()) {
        throw std::logic_error("OR mask combine received mismatched mask sizes");
    }
    for (std::size_t i = 0; i < left.is_true.size(); ++i) {
        const auto lt = left.is_true[i];
        const auto lk = left.is_known[i];
        const auto rt = right.is_true[i];
        const auto rk = right.is_known[i];
        const auto true_value = static_cast<std::uint8_t>(lt | rt);
        const auto false_value = static_cast<std::uint8_t>((lk & (lt ^ 1U)) & (rk & (rt ^ 1U)));
        left.is_true[i] = true_value;
        left.is_known[i] = static_cast<std::uint8_t>(true_value | false_value);
    }
}

PredicateMask evaluate_in_mask(const CompiledPredicate& predicate,
                               const PredicateEvaluationDomain& domain,
                               bool negate) {
    if (predicate.value_set == nullptr) {
        throw std::logic_error("compiled IN predicate is missing its value set");
    }
    auto mask = make_predicate_mask(domain.size);
    for_each_domain_row(domain, [&](std::size_t position, std::size_t row) {
        const auto value = predicate.in_value.cell(row);
        bool is_true = false;
        bool is_known = true;
        if (predicate.value_set->is_empty) {
            is_true = false;
        } else if (value.is_null) {
            is_known = false;
        } else if (predicate.value_set->contains(value)) {
            is_true = true;
        } else if (predicate.value_set->has_null) {
            is_known = false;
        }
        if (negate && is_known) {
            is_true = !is_true;
        }
        mask.is_true[position] = is_true ? std::uint8_t{1} : std::uint8_t{0};
        mask.is_known[position] = is_known ? std::uint8_t{1} : std::uint8_t{0};
    });
    return mask;
}

PredicateMask evaluate_predicate_mask(const CompiledPredicate& predicate, const PredicateEvaluationDomain& domain) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return evaluate_comparison_mask(predicate.comparison, domain);
    case sql::PredicateKind::IsNull:
        return evaluate_null_check_mask(predicate.null_check, domain, false);
    case sql::PredicateKind::IsNotNull:
        return evaluate_null_check_mask(predicate.null_check, domain, true);
    case sql::PredicateKind::In:
        return evaluate_in_mask(predicate, domain, false);
    case sql::PredicateKind::NotIn:
        return evaluate_in_mask(predicate, domain, true);
    case sql::PredicateKind::Exists:
        return make_constant_predicate_mask(domain.size, predicate.exists_value ? 1 : 0, 1);
    case sql::PredicateKind::NotExists:
        return make_constant_predicate_mask(domain.size, predicate.exists_value ? 0 : 1, 1);
    case sql::PredicateKind::And: {
        auto left = evaluate_predicate_mask(require_left_predicate(predicate), domain);
        auto right = evaluate_predicate_mask(require_right_predicate(predicate), domain);
        combine_and_in_place(left, right);
        return left;
    }
    case sql::PredicateKind::Or: {
        auto left = evaluate_predicate_mask(require_left_predicate(predicate), domain);
        auto right = evaluate_predicate_mask(require_right_predicate(predicate), domain);
        combine_or_in_place(left, right);
        return left;
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

PredicateMask evaluate_predicates_mask(const std::vector<CompiledPredicate>& predicates,
                                       const PredicateEvaluationDomain& domain) {
    if (predicates.empty()) {
        return make_constant_predicate_mask(domain.size, 1, 1);
    }
    auto mask = evaluate_predicate_mask(predicates.front(), domain);
    for (std::size_t i = 1; i < predicates.size(); ++i) {
        auto next = evaluate_predicate_mask(predicates[i], domain);
        combine_and_in_place(mask, next);
    }
    return mask;
}

SelectionVector selection_from_true_mask(const PredicateMask& mask, const PredicateEvaluationDomain& domain) {
    if (mask.is_true.size() != domain.size || mask.is_known.size() != domain.size) {
        throw std::logic_error("predicate mask size does not match evaluation domain");
    }
    SelectionVector rows;
    rows.reserve(domain.size);
    for (std::size_t position = 0; position < domain.size; ++position) {
        if (mask.is_true[position] == 0) {
            continue;
        }
        rows.push_back(domain.rows_match_positions ? position : (*domain.selection)[position]);
    }
    return rows;
}

CompiledJoinScalar compile_join_scalar(const plan::BoundScalarExpr& expression,
                                       const storage::ColumnarBatch& left,
                                       const storage::ColumnarBatch& right,
                                       const ExecutionContext& context) {
    CompiledJoinScalar compiled;
    compiled.type = expression.type;
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        const auto name = column_identity_name(*column);
        if (left.has_column(name)) {
            if (left.column_type(name) != expression.type) {
                throw std::logic_error("bound join scalar type does not match left storage column: " + name);
            }
            compiled.source = CompiledJoinScalar::Source::Left;
            if (expression.type == catalog::ColumnType::Int64) {
                const auto& storage_column = left.column(name);
                compiled.int_values = &storage_column.values();
                compiled.validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            } else {
                const auto& storage_column = left.string_column(name);
                compiled.string_values = &storage_column.values();
                compiled.validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            }
            return compiled;
        }
        if (right.has_column(name)) {
            if (right.column_type(name) != expression.type) {
                throw std::logic_error("bound join scalar type does not match right storage column: " + name);
            }
            compiled.source = CompiledJoinScalar::Source::Right;
            if (expression.type == catalog::ColumnType::Int64) {
                const auto& storage_column = right.column(name);
                compiled.int_values = &storage_column.values();
                compiled.validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            } else {
                const auto& storage_column = right.string_column(name);
                compiled.string_values = &storage_column.values();
                compiled.validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
            }
            return compiled;
        }
        throw std::logic_error("bound join column identity is missing from both inputs: " + name);
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        compiled.literal = literal->value;
        return compiled;
    }
    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        compiled.string_literal = literal->value;
        return compiled;
    }
    if (const auto* subquery = std::get_if<plan::BoundScalarSubquery>(&expression.value)) {
        const auto& result = prepared_subquery(subquery->plan, context);
        if (result.row_count() > 1) {
            throw std::runtime_error(subquery->name + " returned more than one row");
        }
        if (result.row_count() == 0) {
            compiled.literal_is_null = true;
            return compiled;
        }
        if (result.column_names().size() != 1 || result.column_type(result.column_names().front()) != expression.type) {
            throw std::logic_error("scalar subquery materialization has the wrong join schema");
        }
        const auto& name = result.column_names().front();
        if (expression.type == catalog::ColumnType::Int64) {
            const auto& column = result.column(name);
            compiled.literal_is_null = column.is_null(0);
            if (!compiled.literal_is_null) {
                compiled.literal = column.at(0);
            }
        } else {
            const auto& column = result.string_column(name);
            compiled.literal_is_null = column.is_null(0);
            if (!compiled.literal_is_null) {
                compiled.string_literal = column.at(0);
            }
        }
        return compiled;
    }
    compiled.literal_is_null = true;
    return compiled;
}

CompiledJoinComparison compile_join_comparison(const plan::BoundComparisonExpr& comparison,
                                               const storage::ColumnarBatch& left,
                                               const storage::ColumnarBatch& right,
                                               const ExecutionContext& context) {
    CompiledJoinComparison compiled;
    compiled.left = compile_join_scalar(comparison.left, left, right, context);
    compiled.op = comparison.op;
    compiled.right = compile_join_scalar(comparison.right, left, right, context);
    return compiled;
}

TruthValue evaluate_join_comparison(const CompiledJoinComparison& comparison,
                                    std::size_t left_row,
                                    std::size_t right_row) {
    const auto left = comparison.left.cell(left_row, right_row);
    const auto right = comparison.right.cell(left_row, right_row);
    if (left.is_null || right.is_null) {
        return TruthValue::Unknown;
    }
    return truth_from_bool(compare_cells(left, comparison.op, right));
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
                                             const storage::ColumnarBatch& right,
                                             const ExecutionContext& context) {
    CompiledJoinPredicate compiled;
    compiled.kind = predicate.kind;
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        compiled.comparison = compile_join_comparison(predicate.comparison, left, right, context);
        return compiled;
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        compiled.null_check = compile_join_scalar(predicate.null_check, left, right, context);
        return compiled;
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        compiled.in_value = compile_join_scalar(predicate.in_value, left, right, context);
        compiled.value_set = &prepared_value_set(predicate.subquery, context);
        return compiled;
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        compiled.exists_value = prepared_subquery(predicate.subquery, context).row_count() != 0;
        return compiled;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        compiled.left =
            std::make_shared<CompiledJoinPredicate>(
                compile_join_predicate(require_left_predicate(predicate), left, right, context));
        compiled.right =
            std::make_shared<CompiledJoinPredicate>(
                compile_join_predicate(require_right_predicate(predicate), left, right, context));
        return compiled;
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<CompiledJoinPredicate> compile_join_predicates(const std::vector<plan::BoundPredicate>& predicates,
                                                           const storage::ColumnarBatch& left,
                                                           const storage::ColumnarBatch& right,
                                                           const ExecutionContext& context) {
    std::vector<CompiledJoinPredicate> compiled;
    compiled.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        compiled.push_back(compile_join_predicate(predicate, left, right, context));
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
    case sql::PredicateKind::In:
        if (predicate.value_set == nullptr) {
            throw std::logic_error("compiled join IN predicate is missing its value set");
        }
        return evaluate_in_truth(predicate.in_value.cell(left_row, right_row), *predicate.value_set);
    case sql::PredicateKind::NotIn:
        if (predicate.value_set == nullptr) {
            throw std::logic_error("compiled join NOT IN predicate is missing its value set");
        }
        return not_truth(evaluate_in_truth(predicate.in_value.cell(left_row, right_row), *predicate.value_set));
    case sql::PredicateKind::Exists:
        return truth_from_bool(predicate.exists_value);
    case sql::PredicateKind::NotExists:
        return truth_from_bool(!predicate.exists_value);
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
        if (value.is_null) {
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
        const auto name = column_identity_name(*aggregate.argument);
        if (batch.column_type(name) != aggregate.argument->type) {
            throw std::logic_error("aggregate argument type does not match storage column: " + name);
        }
        if (aggregate.argument->type == catalog::ColumnType::Int64) {
            const auto& storage_column = batch.column(name);
            compiled.argument_int_values = &storage_column.values();
            compiled.argument_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
        } else {
            const auto& storage_column = batch.string_column(name);
            compiled.argument_string_values = &storage_column.values();
            compiled.argument_validity = storage_column.has_nulls() ? &storage_column.validity() : nullptr;
        }
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

TypedCell aggregate_argument_value(const CompiledAggregateExpression& aggregate, std::size_t row) {
    const auto& expression = *aggregate.aggregate;
    if (!expression.argument.has_value()) {
        throw std::logic_error("aggregate argument is missing");
    }
    if (aggregate.argument_validity != nullptr && (*aggregate.argument_validity)[row] == 0) {
        return null_cell(expression.argument->type);
    }
    if (expression.argument->type == catalog::ColumnType::Int64) {
        return int_cell((*aggregate.argument_int_values)[row]);
    }
    return string_cell((*aggregate.argument_string_values)[row]);
}

void update_aggregate(AggregateValue& value,
                      const CompiledAggregateExpression& compiled,
                      std::size_t row) {
    const auto& aggregate = *compiled.aggregate;
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        if (aggregate.argument.has_value() && aggregate_argument_value(compiled, row).is_null) {
            return;
        }
        increment_count(value, aggregate.output_name);
        return;
    case sql::AggregateFunction::Sum: {
        const auto argument = aggregate_argument_value(compiled, row);
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
        const auto argument = aggregate_argument_value(compiled, row);
        if (argument.is_null) {
            return;
        }
        if (aggregate.type == catalog::ColumnType::Int64) {
            if (!value.has_value || argument.int_value < value.min) {
                value.min = argument.int_value;
            }
        } else if (!value.has_value || argument.string_value < value.string_min) {
            value.string_min = argument.string_value;
        }
        value.has_value = true;
        return;
    }
    case sql::AggregateFunction::Max: {
        const auto argument = aggregate_argument_value(compiled, row);
        if (argument.is_null) {
            return;
        }
        if (aggregate.type == catalog::ColumnType::Int64) {
            if (!value.has_value || argument.int_value > value.max) {
                value.max = argument.int_value;
            }
        } else if (!value.has_value || argument.string_value > value.string_max) {
            value.string_max = argument.string_value;
        }
        value.has_value = true;
        return;
    }
    }
    throw std::logic_error("unreachable aggregate function");
}

TypedCell finalize_aggregate(const AggregateValue& value, const plan::AggregateExpression& aggregate) {
    switch (aggregate.function) {
    case sql::AggregateFunction::Count:
        return int_cell(value.count);
    case sql::AggregateFunction::Sum:
        if (!value.has_value) {
            return null_cell(catalog::ColumnType::Int64);
        }
        return int_cell(value.sum);
    case sql::AggregateFunction::Min:
        if (!value.has_value) {
            return null_cell(aggregate.type);
        }
        return aggregate.type == catalog::ColumnType::Int64 ? int_cell(value.min) : string_cell(value.string_min);
    case sql::AggregateFunction::Max:
        if (!value.has_value) {
            return null_cell(aggregate.type);
        }
        return aggregate.type == catalog::ColumnType::Int64 ? int_cell(value.max) : string_cell(value.string_max);
    }
    throw std::logic_error("unreachable aggregate function");
}

sql::AggregateFunction aggregate_function_for_window(sql::WindowFunction function) {
    switch (function) {
    case sql::WindowFunction::Count:
        return sql::AggregateFunction::Count;
    case sql::WindowFunction::Sum:
        return sql::AggregateFunction::Sum;
    case sql::WindowFunction::Min:
        return sql::AggregateFunction::Min;
    case sql::WindowFunction::Max:
        return sql::AggregateFunction::Max;
    case sql::WindowFunction::RowNumber:
    case sql::WindowFunction::Rank:
    case sql::WindowFunction::DenseRank:
        break;
    }
    throw std::logic_error("ranking window is not an aggregate");
}

plan::AggregateExpression aggregate_expression_for_window(const plan::WindowExpression& window) {
    return plan::AggregateExpression{window.output_name,
                                     aggregate_function_for_window(window.function),
                                     window.argument,
                                     window.position,
                                     window.type};
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
    builder.columns.reserve(builder.names.size());
    builder.left_columns.reserve(left.column_names().size());
    for (const auto& column_name : left.column_names()) {
        builder.columns.emplace_back(left.column_type(column_name));
        builder.left_columns.push_back(compile_named_column(left, column_name));
    }
    builder.right_columns.reserve(right.column_names().size());
    for (const auto& column_name : right.column_names()) {
        builder.columns.emplace_back(right.column_type(column_name));
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

void append_null_extended_left_row(JoinOutputBuilder& builder, std::size_t left_row) {
    std::size_t output_index = 0;
    for (const auto& column : builder.left_columns) {
        append_cell(builder.columns[output_index++], column.cell(left_row));
    }
    for (const auto& column : builder.right_columns) {
        append_cell(builder.columns[output_index++], null_cell(column.type));
    }
}

storage::ColumnarBatch finish_join_output(JoinOutputBuilder builder) {
    storage::ColumnarBatch out;
    for (std::size_t i = 0; i < builder.names.size(); ++i) {
        std::move(builder.columns[i]).add_to(out, std::move(builder.names[i]));
    }
    return out;
}

storage::ColumnarBatch materialize_left_join_rows(const BatchView& left, const SelectionVector& rows) {
    storage::ColumnarBatch out;
    for (const auto& name : left.batch->column_names()) {
        const auto compiled = compile_named_column(*left.batch, name);
        TypedColumnBuilder column(compiled.type);
        column.reserve(rows.size());
        for (const auto row : rows) {
            append_cell(column, compiled.cell(row));
        }
        std::move(column).add_to(out, name);
    }
    return out;
}

template <bool EmitMatched>
storage::ColumnarBatch execute_nested_loop_existence_join(
    const BatchView& left,
    const BatchView& right,
    const std::vector<plan::BoundPredicate>& predicates,
    const ExecutionContext& context) {
    const auto compiled_predicates = compile_join_predicates(predicates, *left.batch, *right.batch, context);
    SelectionVector output_rows;
    output_rows.reserve(left.selection->size());
    for (const auto left_row : *left.selection) {
        bool matched = false;
        for (const auto right_row : *right.selection) {
            if (evaluate_join_predicates(compiled_predicates, left_row, right_row) == TruthValue::True) {
                matched = true;
                break;
            }
        }
        if (matched == EmitMatched) {
            output_rows.push_back(left_row);
        }
    }
    return materialize_left_join_rows(left, output_rows);
}

template <bool EmitMatched>
storage::ColumnarBatch execute_hash_existence_join(const BatchView& left,
                                                   const BatchView& right,
                                                   const JoinPredicateSplit& predicates,
                                                   const ExecutionContext& context) {
    std::vector<plan::BoundColumnRef> left_key_columns;
    std::vector<plan::BoundColumnRef> right_key_columns;
    left_key_columns.reserve(predicates.equi_keys.size());
    right_key_columns.reserve(predicates.equi_keys.size());
    for (const auto& equi_key : predicates.equi_keys) {
        left_key_columns.push_back(equi_key.left);
        right_key_columns.push_back(equi_key.right);
    }
    const auto compiled_left_keys = compile_columns(left_key_columns, *left.batch);
    const auto compiled_right_keys = compile_columns(right_key_columns, *right.batch);
    const auto compiled_residuals =
        compile_join_predicates(predicates.residuals, *left.batch, *right.batch, context);

    // Lookup-only discipline: the hash table is never iterated for output.
    // Candidate vectors retain right input order for residual evaluation.
    std::unordered_map<HashKey, std::vector<std::size_t>, HashKeyHash> right_rows_by_key;
    for (const auto right_row : *right.selection) {
        auto key = make_non_null_key(right_row, compiled_right_keys);
        if (key.has_value()) {
            right_rows_by_key[*key].push_back(right_row);
        }
    }

    SelectionVector output_rows;
    output_rows.reserve(left.selection->size());
    for (const auto left_row : *left.selection) {
        bool matched = false;
        auto key = make_non_null_key(left_row, compiled_left_keys);
        if (key.has_value()) {
            const auto candidates = right_rows_by_key.find(*key);
            if (candidates != right_rows_by_key.end()) {
                for (const auto right_row : candidates->second) {
                    if (evaluate_join_predicates(compiled_residuals, left_row, right_row) == TruthValue::True) {
                        matched = true;
                        break;
                    }
                }
            }
        }
        if (matched == EmitMatched) {
            output_rows.push_back(left_row);
        }
    }
    return materialize_left_join_rows(left, output_rows);
}

template <bool EmitUnmatchedLeft>
storage::ColumnarBatch execute_nested_loop_join(const BatchView& left,
                                                const BatchView& right,
                                                const std::vector<plan::BoundPredicate>& predicates,
                                                const ExecutionContext& context) {
    const auto compiled_predicates = compile_join_predicates(predicates, *left.batch, *right.batch, context);
    auto builder = make_join_output_builder(*left.batch, *right.batch);
    for (auto left_row : *left.selection) {
        bool matched = false;
        for (auto right_row : *right.selection) {
            if (evaluate_join_predicates(compiled_predicates, left_row, right_row) == TruthValue::True) {
                append_joined_row(builder, left_row, right_row);
                if constexpr (EmitUnmatchedLeft) {
                    matched = true;
                }
            }
        }
        if constexpr (EmitUnmatchedLeft) {
            if (!matched) {
                append_null_extended_left_row(builder, left_row);
            }
        }
    }
    return finish_join_output(std::move(builder));
}

template <bool EmitUnmatchedLeft>
storage::ColumnarBatch execute_hash_join(const BatchView& left,
                                         const BatchView& right,
                                         const JoinPredicateSplit& predicates,
                                         const ExecutionContext& context) {
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
    const auto compiled_residuals =
        compile_join_predicates(predicates.residuals, *left.batch, *right.batch, context);

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
        bool matched = false;
        auto key = make_non_null_key(left_row, compiled_left_key_columns);
        if (key.has_value()) {
            const auto matching_right_rows = right_rows_by_key.find(*key);
            if (matching_right_rows != right_rows_by_key.end()) {
                for (auto right_row : matching_right_rows->second) {
                    if (evaluate_join_predicates(compiled_residuals, left_row, right_row) == TruthValue::True) {
                        append_joined_row(builder, left_row, right_row);
                        if constexpr (EmitUnmatchedLeft) {
                            matched = true;
                        }
                    }
                }
            }
        }
        if constexpr (EmitUnmatchedLeft) {
            if (!matched) {
                append_null_extended_left_row(builder, left_row);
            }
        }
    }
    return finish_join_output(std::move(builder));
}

BatchView execute_scan(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    const auto& input = context.catalog.table(plan.table);
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
    view.selection_rows_match_positions = true;
    validate_view(view);
    return view;
}

BatchView execute_filter(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_join(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_project(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_aggregate(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_window(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_distinct(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_sort(const plan::PhysicalPlan& plan, ExecutionContext& context);
BatchView execute_limit(const plan::PhysicalPlan& plan, ExecutionContext& context);

BatchView execute_to_view(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    switch (plan.kind) {
    case plan::PhysicalKind::Scan:
        return execute_scan(plan, context);
    case plan::PhysicalKind::Join:
        return execute_join(plan, context);
    case plan::PhysicalKind::Filter:
        return execute_filter(plan, context);
    case plan::PhysicalKind::Project:
        return execute_project(plan, context);
    case plan::PhysicalKind::Aggregate:
        return execute_aggregate(plan, context);
    case plan::PhysicalKind::Window:
        return execute_window(plan, context);
    case plan::PhysicalKind::Distinct:
        return execute_distinct(plan, context);
    case plan::PhysicalKind::Sort:
        return execute_sort(plan, context);
    case plan::PhysicalKind::Limit:
        return execute_limit(plan, context);
    }
    throw std::logic_error("unreachable physical plan kind");
}

const plan::PhysicalPlan& require_input(const plan::PhysicalPlan& plan) {
    if (!plan.input) {
        throw std::invalid_argument("physical plan node is missing its input");
    }
    return *plan.input;
}

const plan::LogicalPlan& require_logical_input(const plan::LogicalPlan& plan) {
    if (!plan.input) {
        throw std::invalid_argument("logical plan node is missing its input");
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

BatchView execute_filter(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto input = execute_to_view(require_input(plan), context);
    validate_view(input);

    const auto predicates = compile_predicates(plan.predicates, *input.batch, context);
    const auto domain = make_predicate_domain(input);
    auto rows = selection_from_true_mask(evaluate_predicates_mask(predicates, domain), domain);

    input.selection = make_selection(std::move(rows));
    input.selection_rows_match_positions = false;
    validate_view(input);
    return input;
}

BatchView execute_join(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto left = execute_to_view(require_left(plan), context);
    auto right = execute_to_view(require_right(plan), context);
    validate_view(left);
    validate_view(right);

    const auto predicates = split_join_predicates(plan.predicates, *left.batch, *right.batch);
    const auto materialized_batch = [&] {
        if (plan.join_kind == plan::JoinKind::Semi) {
            return predicates.equi_keys.empty()
                       ? execute_nested_loop_existence_join<true>(left, right, predicates.residuals, context)
                       : execute_hash_existence_join<true>(left, right, predicates, context);
        }
        if (plan.join_kind == plan::JoinKind::Anti) {
            return predicates.equi_keys.empty()
                       ? execute_nested_loop_existence_join<false>(left, right, predicates.residuals, context)
                       : execute_hash_existence_join<false>(left, right, predicates, context);
        }
        if (plan.join_kind == plan::JoinKind::Left) {
            return predicates.equi_keys.empty()
                       ? execute_nested_loop_join<true>(left, right, predicates.residuals, context)
                       : execute_hash_join<true>(left, right, predicates, context);
        }
        return predicates.equi_keys.empty()
                   ? execute_nested_loop_join<false>(left, right, predicates.residuals, context)
                   : execute_hash_join<false>(left, right, predicates, context);
    }();
    auto materialized = std::make_shared<const storage::ColumnarBatch>(std::move(materialized_batch));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    view.selection_rows_match_positions = true;
    validate_view(view);
    return view;
}

storage::ColumnarBatch materialize_projection(const plan::PhysicalPlan& plan,
                                              const BatchView& input,
                                              const ExecutionContext& context) {
    validate_view(input);

    std::vector<CompiledProjection> projections;
    projections.reserve(plan.projections.size());
    for (const auto& projection : plan.projections) {
        projections.push_back(
            CompiledProjection{projection.output_name, compile_scalar(projection.expression, *input.batch, context)});
    }

    storage::ColumnarBatch out;
    for (const auto& projection : projections) {
        TypedColumnBuilder column(projection.expression.type);
        column.reserve(input.selection->size());
        for (auto row : *input.selection) {
            append_cell(column, projection.expression.cell(row));
        }
        std::move(column).add_to(out, projection.output_name);
    }
    return out;
}

void add_materialized_selected_column(storage::ColumnarBatch& out,
                                      const BatchView& input,
                                      std::string output_name,
                                      const std::string& input_name) {
    TypedColumnBuilder column(input.batch->column_type(input_name));
    column.reserve(input.selection->size());
    const auto input_column = compile_named_column(*input.batch, input_name);
    for (auto row : *input.selection) {
        append_cell(column, input_column.cell(row));
    }
    std::move(column).add_to(out, std::move(output_name));
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
        add_materialized_selected_column(sort_input, source, name, name);
    }
}

storage::ColumnarBatch materialize_project_sort_input(const plan::PhysicalPlan& project,
                                                      const BatchView& source,
                                                      const std::vector<plan::SortKey>& sort_keys,
                                                      const ExecutionContext& context) {
    auto sort_input = materialize_projection(project, source, context);
    add_missing_sort_key_columns(sort_input, source, sort_keys);
    return sort_input;
}

storage::ColumnarBatch materialize_project_output_columns(const plan::PhysicalPlan& project,
                                                          const storage::ColumnarBatch& batch,
                                                          const SelectionVector& rows) {
    storage::ColumnarBatch out;
    for (const auto& projection : project.projections) {
        TypedColumnBuilder column(projection.type);
        column.reserve(rows.size());
        const auto input_column = compile_named_column(batch, projection.output_name);
        for (auto row : rows) {
            append_cell(column, input_column.cell(row));
        }
        std::move(column).add_to(out, projection.output_name);
    }
    return out;
}

std::vector<CompiledSortKey> compile_sort_keys(const std::vector<plan::SortKey>& sort_keys,
                                               const storage::ColumnarBatch& batch) {
    std::vector<CompiledSortKey> compiled;
    compiled.reserve(sort_keys.size());
    for (const auto& key : sort_keys) {
        const auto name = column_identity_name(key.column);
        if (batch.column_type(name) != key.column.type) {
            throw std::logic_error("sort key type does not match storage column: " + name);
        }
        if (key.column.type == catalog::ColumnType::Int64) {
            const auto& storage_column = batch.column(name);
            compiled.push_back(CompiledSortKey{key.column.type,
                                               &storage_column.values(),
                                               nullptr,
                                               storage_column.has_nulls() ? &storage_column.validity() : nullptr,
                                               key.direction});
        } else {
            const auto& storage_column = batch.string_column(name);
            compiled.push_back(CompiledSortKey{key.column.type,
                                               nullptr,
                                               &storage_column.values(),
                                               storage_column.has_nulls() ? &storage_column.validity() : nullptr,
                                               key.direction});
        }
    }
    return compiled;
}

TypedCell sort_key_cell(const CompiledSortKey& key, std::size_t row) {
    if (key.validity != nullptr && (*key.validity)[row] == 0) {
        return null_cell(key.type);
    }
    if (key.type == catalog::ColumnType::Int64) {
        return int_cell((*key.int_values)[row]);
    }
    return string_cell((*key.string_values)[row]);
}

bool sort_cell_less(const TypedCell& left, const TypedCell& right, sql::SortDirection direction) {
    if (left == right) {
        return false;
    }
    const auto left_is_null = left.is_null;
    const auto right_is_null = right.is_null;
    if (left_is_null || right_is_null) {
        return direction == sql::SortDirection::Asc ? right_is_null : left_is_null;
    }
    if (left.type != right.type) {
        throw std::logic_error("sort comparison received mismatched cell types");
    }
    if (left.type == catalog::ColumnType::Int64) {
        return direction == sql::SortDirection::Asc ? left.int_value < right.int_value
                                                    : left.int_value > right.int_value;
    }
    return direction == sql::SortDirection::Asc ? left.string_value < right.string_value
                                                : left.string_value > right.string_value;
}

SelectionVectorPtr sort_selection(const std::vector<plan::SortKey>& sort_keys, const BatchView& input) {
    validate_view(input);

    const auto compiled_sort_keys = compile_sort_keys(sort_keys, *input.batch);
    SelectionVector rows = *input.selection;
    std::stable_sort(rows.begin(), rows.end(), [&](std::size_t left, std::size_t right) {
        for (const auto& key : compiled_sort_keys) {
            const auto left_value = sort_key_cell(key, left);
            const auto right_value = sort_key_cell(key, right);
            if (left_value == right_value) {
                continue;
            }
            return sort_cell_less(left_value, right_value, key.direction);
        }
        return false;
    });
    return make_selection(std::move(rows));
}

BatchView execute_project(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto input = execute_to_view(require_input(plan), context);
    auto materialized =
        std::make_shared<const storage::ColumnarBatch>(materialize_projection(plan, input, context));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    view.selection_rows_match_positions = true;
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
        TypedColumnBuilder column(plan.group_keys[key_index].type);
        column.reserve(groups.size());
        for (const auto& group : groups) {
            append_cell(column, group.key.values.at(key_index));
        }
        std::move(column).add_to(out, column_identity_name(plan.group_keys[key_index]));
    }

    for (std::size_t aggregate_index = 0; aggregate_index < plan.aggregate_expressions.size(); ++aggregate_index) {
        const auto& aggregate = plan.aggregate_expressions[aggregate_index];
        TypedColumnBuilder column(aggregate.type);
        column.reserve(groups.size());
        for (const auto& group : groups) {
            append_cell(column, finalize_aggregate(group.aggregates.at(aggregate_index), aggregate));
        }
        std::move(column).add_to(out, aggregate.output_name);
    }
    return out;
}

BatchView execute_aggregate(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto input = execute_to_view(require_input(plan), context);
    auto materialized = std::make_shared<const storage::ColumnarBatch>(materialize_aggregate(plan, input));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    view.selection_rows_match_positions = true;
    validate_view(view);
    return view;
}

struct WindowPartition {
    std::vector<std::size_t> rows;
};

std::vector<WindowPartition> build_window_partitions(const plan::WindowExpression& window,
                                                     const BatchView& input) {
    validate_view(input);
    const auto partition_keys = compile_columns(window.partition_keys, *input.batch);
    std::unordered_map<HashKey, std::size_t, HashKeyHash> partition_index_by_key;
    std::vector<WindowPartition> partitions;

    for (auto row : *input.selection) {
        auto key = make_key(row, partition_keys);
        const auto found = partition_index_by_key.find(key);
        if (found == partition_index_by_key.end()) {
            const auto index = partitions.size();
            partition_index_by_key.emplace(std::move(key), index);
            partitions.push_back(WindowPartition{});
            partitions.back().rows.push_back(row);
        } else {
            partitions.at(found->second).rows.push_back(row);
        }
    }
    return partitions;
}

bool window_rows_are_peers(const std::vector<CompiledSortKey>& order_keys,
                           std::size_t left,
                           std::size_t right) {
    for (const auto& key : order_keys) {
        if (!(sort_key_cell(key, left) == sort_key_cell(key, right))) {
            return false;
        }
    }
    return true;
}

std::int64_t checked_window_ordinal(std::size_t ordinal, const std::string& output_name) {
    if (ordinal > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error(output_name + " overflowed int64");
    }
    return static_cast<std::int64_t>(ordinal);
}

void evaluate_ranking_window(const plan::WindowExpression& window,
                             const storage::ColumnarBatch& batch,
                             const std::vector<WindowPartition>& partitions,
                             std::vector<TypedCell>& values) {
    const auto order_keys = compile_sort_keys(window.order_keys, batch);
    for (const auto& partition : partitions) {
        auto ordered_rows = partition.rows;
        std::stable_sort(ordered_rows.begin(), ordered_rows.end(), [&](std::size_t left, std::size_t right) {
            for (const auto& key : order_keys) {
                const auto left_value = sort_key_cell(key, left);
                const auto right_value = sort_key_cell(key, right);
                if (left_value == right_value) {
                    continue;
                }
                return sort_cell_less(left_value, right_value, key.direction);
            }
            return false;
        });

        std::size_t rank = 1;
        std::size_t dense_rank = 1;
        for (std::size_t index = 0; index < ordered_rows.size(); ++index) {
            if (index != 0 && !window_rows_are_peers(order_keys, ordered_rows[index - 1], ordered_rows[index])) {
                rank = index + 1;
                ++dense_rank;
            }
            const auto ordinal = window.function == sql::WindowFunction::RowNumber
                                     ? index + 1
                                     : (window.function == sql::WindowFunction::Rank ? rank : dense_rank);
            values.at(ordered_rows[index]) = int_cell(checked_window_ordinal(ordinal, window.output_name));
        }
    }
}

void evaluate_aggregate_window(const plan::WindowExpression& window,
                               const storage::ColumnarBatch& batch,
                               const std::vector<WindowPartition>& partitions,
                               std::vector<TypedCell>& values) {
    const auto aggregate = aggregate_expression_for_window(window);
    const auto compiled = compile_aggregate_expression(aggregate, batch);
    for (const auto& partition : partitions) {
        AggregateValue state;
        for (auto row : partition.rows) {
            update_aggregate(state, compiled, row);
        }
        const auto value = finalize_aggregate(state, aggregate);
        for (auto row : partition.rows) {
            values.at(row) = value;
        }
    }
}

void add_window_column(storage::ColumnarBatch& out,
                       const plan::WindowExpression& window,
                       const BatchView& input) {
    const auto partitions = build_window_partitions(window, input);
    std::vector<TypedCell> values(input.batch->row_count(), null_cell(window.type));

    switch (window.function) {
    case sql::WindowFunction::RowNumber:
    case sql::WindowFunction::Rank:
    case sql::WindowFunction::DenseRank:
        evaluate_ranking_window(window, *input.batch, partitions, values);
        break;
    case sql::WindowFunction::Count:
    case sql::WindowFunction::Sum:
    case sql::WindowFunction::Min:
    case sql::WindowFunction::Max:
        evaluate_aggregate_window(window, *input.batch, partitions, values);
        break;
    }

    TypedColumnBuilder column(window.type);
    column.reserve(input.selection->size());
    for (auto row : *input.selection) {
        append_cell(column, values.at(row));
    }
    std::move(column).add_to(out, window.output_name);
}

storage::ColumnarBatch materialize_window(const plan::PhysicalPlan& plan, const BatchView& input) {
    validate_view(input);

    storage::ColumnarBatch out;
    for (const auto& name : input.batch->column_names()) {
        add_materialized_selected_column(out, input, name, name);
    }
    for (const auto& window : plan.window_expressions) {
        add_window_column(out, window, input);
    }
    return out;
}

BatchView execute_window(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto input = execute_to_view(require_input(plan), context);
    auto materialized = std::make_shared<const storage::ColumnarBatch>(materialize_window(plan, input));

    BatchView view;
    view.owned_batch = materialized;
    view.batch = materialized.get();
    view.selection = identity_selection(materialized->row_count());
    view.selection_rows_match_positions = true;
    validate_view(view);
    return view;
}

BatchView execute_distinct(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto input = execute_to_view(require_input(plan), context);
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
    input.selection_rows_match_positions = false;
    validate_view(input);
    return input;
}

BatchView execute_sort(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    const auto& input_plan = require_input(plan);
    if (input_plan.kind == plan::PhysicalKind::Project) {
        auto source = execute_to_view(require_input(input_plan), context);
        auto sort_input =
            std::make_shared<const storage::ColumnarBatch>(
                materialize_project_sort_input(input_plan, source, plan.sort_keys, context));

        BatchView sort_view;
        sort_view.owned_batch = sort_input;
        sort_view.batch = sort_input.get();
        sort_view.selection = identity_selection(sort_input->row_count());
        sort_view.selection_rows_match_positions = true;
        sort_view.selection = sort_selection(plan.sort_keys, sort_view);
        sort_view.selection_rows_match_positions = false;

        auto materialized = std::make_shared<const storage::ColumnarBatch>(
            materialize_project_output_columns(input_plan, *sort_view.batch, *sort_view.selection));

        BatchView view;
        view.owned_batch = materialized;
        view.batch = materialized.get();
        view.selection = identity_selection(materialized->row_count());
        view.selection_rows_match_positions = true;
        validate_view(view);
        return view;
    }

    auto input = execute_to_view(input_plan, context);
    input.selection = sort_selection(plan.sort_keys, input);
    input.selection_rows_match_positions = false;
    validate_view(input);
    return input;
}

BatchView execute_limit(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    auto input = execute_to_view(require_input(plan), context);
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
        TypedColumnBuilder column(view.batch->column_type(name));
        column.reserve(view.selection->size());
        const auto input_column = compile_named_column(*view.batch, name);
        for (auto row : *view.selection) {
            append_cell(column, input_column.cell(row));
        }
        std::move(column).add_to(out, name);
    }
    return out;
}

bool scalar_requires_typed_dispatch(const plan::BoundScalarExpr& expression) {
    return expression.type == catalog::ColumnType::String;
}

bool predicate_requires_typed_dispatch(const plan::BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return scalar_requires_typed_dispatch(predicate.comparison.left) ||
               scalar_requires_typed_dispatch(predicate.comparison.right);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return scalar_requires_typed_dispatch(predicate.null_check);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        return scalar_requires_typed_dispatch(predicate.in_value);
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        return false;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        return predicate_requires_typed_dispatch(*predicate.left) ||
               predicate_requires_typed_dispatch(*predicate.right);
    }
    throw std::logic_error("unreachable predicate kind");
}

bool scanned_table_requires_typed_dispatch(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    const auto schema = catalog.find_table_schema(plan.table);
    if (!schema.has_value()) {
        throw std::out_of_range("unknown table: " + plan.table);
    }
    for (const auto& column : schema->columns) {
        if (column.type == catalog::ColumnType::String) {
            return true;
        }
    }
    return false;
}

bool plan_requires_typed_dispatch(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    for (const auto& projection : plan.projections) {
        if (projection.type == catalog::ColumnType::String ||
            scalar_requires_typed_dispatch(projection.expression)) {
            return true;
        }
    }
    for (const auto& key : plan.group_keys) {
        if (key.type == catalog::ColumnType::String) {
            return true;
        }
    }
    for (const auto& aggregate : plan.aggregate_expressions) {
        if (aggregate.type == catalog::ColumnType::String ||
            (aggregate.argument.has_value() && aggregate.argument->type == catalog::ColumnType::String)) {
            return true;
        }
    }
    for (const auto& window : plan.window_expressions) {
        if (window.type == catalog::ColumnType::String ||
            (window.argument.has_value() && window.argument->type == catalog::ColumnType::String)) {
            return true;
        }
        for (const auto& key : window.partition_keys) {
            if (key.type == catalog::ColumnType::String) {
                return true;
            }
        }
        for (const auto& key : window.order_keys) {
            if (key.column.type == catalog::ColumnType::String) {
                return true;
            }
        }
    }
    for (const auto& key : plan.sort_keys) {
        if (key.column.type == catalog::ColumnType::String) {
            return true;
        }
    }
    for (const auto& predicate : plan.predicates) {
        if (predicate_requires_typed_dispatch(predicate)) {
            return true;
        }
    }

    switch (plan.kind) {
    case plan::PhysicalKind::Scan:
        return scanned_table_requires_typed_dispatch(plan, catalog);
    case plan::PhysicalKind::Join:
        return plan_requires_typed_dispatch(require_left(plan), catalog) ||
               plan_requires_typed_dispatch(require_right(plan), catalog);
    case plan::PhysicalKind::Filter:
    case plan::PhysicalKind::Project:
    case plan::PhysicalKind::Aggregate:
    case plan::PhysicalKind::Window:
    case plan::PhysicalKind::Distinct:
    case plan::PhysicalKind::Sort:
    case plan::PhysicalKind::Limit:
        return plan_requires_typed_dispatch(require_input(plan), catalog);
    }
    throw std::logic_error("unreachable physical plan kind");
}

KernelDispatch select_kernel_dispatch(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    return plan_requires_typed_dispatch(plan, catalog) ? KernelDispatch::Typed : KernelDispatch::Int64Only;
}

storage::ColumnarBatch execute_prepared_physical(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    switch (select_kernel_dispatch(plan, context.catalog)) {
    case KernelDispatch::Int64Only:
    case KernelDispatch::Typed:
        return materialize_view(execute_to_view(plan, context));
    }
    throw std::logic_error("unreachable vectorized kernel dispatch");
}

void prepare_subqueries(const plan::LogicalPlan& plan, ExecutionContext& context);

const storage::ColumnarBatch& materialize_subquery(
    const std::shared_ptr<const plan::LogicalPlan>& subquery,
    ExecutionContext& context) {
    if (subquery == nullptr) {
        throw std::invalid_argument("bound subquery is missing its logical plan");
    }
    if (const auto found = context.subquery_results.find(subquery.get());
        found != context.subquery_results.end()) {
        return found->second;
    }

    prepare_subqueries(*subquery, context);
    auto result = execute_prepared_physical(plan::lower_to_physical(*subquery), context);
    const auto [inserted, _] = context.subquery_results.emplace(subquery.get(), std::move(result));
    return inserted->second;
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

void prepare_value_set(const std::shared_ptr<const plan::LogicalPlan>& subquery, ExecutionContext& context) {
    if (subquery == nullptr) {
        throw std::invalid_argument("bound IN subquery is missing its logical plan");
    }
    if (context.subquery_value_sets.contains(subquery.get())) {
        return;
    }
    const auto& result = materialize_subquery(subquery, context);
    if (result.column_names().size() != 1) {
        throw std::logic_error("IN subquery materialization must have exactly one column");
    }
    const auto& name = result.column_names().front();
    MaterializedValueSet set;
    set.type = result.column_type(name);
    set.is_empty = result.row_count() == 0;
    if (set.type == catalog::ColumnType::Int64) {
        const auto& column = result.column(name);
        for (std::size_t row = 0; row < result.row_count(); ++row) {
            if (column.is_null(row)) {
                set.has_null = true;
            } else {
                set.int_values.insert(column.at(row));
            }
        }
    } else {
        const auto& column = result.string_column(name);
        for (std::size_t row = 0; row < result.row_count(); ++row) {
            if (column.is_null(row)) {
                set.has_null = true;
            } else {
                set.string_values.insert(column.at(row));
            }
        }
    }
    context.subquery_value_sets.emplace(subquery.get(), std::move(set));
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
        prepare_value_set(predicate.subquery, context);
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

template <typename Plan>
void prepare_owned_expressions(const Plan& plan, ExecutionContext& context) {
    for (const auto& projection : plan.projections) {
        prepare_scalar_subquery(projection.expression, context);
    }
    for (const auto& predicate : plan.predicates) {
        prepare_predicate_subqueries(predicate, context);
    }
}

void prepare_subqueries(const plan::LogicalPlan& plan, ExecutionContext& context) {
    prepare_owned_expressions(plan, context);
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

void prepare_subqueries(const plan::PhysicalPlan& plan, ExecutionContext& context) {
    prepare_owned_expressions(plan, context);
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

} // namespace

storage::ColumnarBatch execute_vectorized(const plan::LogicalPlan& plan, const Catalog& catalog) {
    if (plan.kind == plan::LogicalKind::Explain) {
        return optimizer::explain(require_logical_input(plan), catalog);
    }
    ExecutionContext context{catalog, {}, {}};
    prepare_subqueries(plan, context);
    return execute_prepared_physical(plan::lower_to_physical(plan), context);
}

storage::ColumnarBatch execute_vectorized(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    ExecutionContext context{catalog, {}, {}};
    prepare_subqueries(plan, context);
    return execute_prepared_physical(plan, context);
}

} // namespace execution
