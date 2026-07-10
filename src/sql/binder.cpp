#include "sql/binder.hpp"

#include "sql/errors.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sql {
namespace {

struct TableScope {
    std::string binding_name;
    std::string physical_table;
    std::size_t position{0};
    catalog::TableSchema schema;
    std::size_t outer_depth{0};
};

plan::LogicalPlan bind_select_impl(const SelectQuery& query,
                                   const catalog::Catalog& catalog,
                                   const std::vector<TableScope>& outer_scopes);

[[noreturn]] void throw_unresolved_column(const ColumnRef& column,
                                          const std::vector<TableScope>& scopes,
                                          bool inside_subquery) {
    (void)inside_subquery;
    if (column.qualifier.has_value()) {
        throw BindError(column.position, "unknown table qualifier '" + *column.qualifier + "'");
    }
    if (scopes.size() == 1) {
        throw BindError(column.position,
                        "unknown column '" + column.name + "' in table '" + scopes.front().binding_name + "'");
    }
    throw BindError(column.position, "unknown column '" + column.name + "'");
}

catalog::TableSchema require_table(const std::string& name, std::size_t position, const catalog::Catalog& catalog) {
    auto table = catalog.find_table_schema(name);
    if (!table.has_value()) {
        throw BindError(position, "unknown table '" + name + "'");
    }
    return std::move(*table);
}

void add_scope(std::vector<TableScope>& scopes,
               std::set<std::string>& seen_bindings,
               const std::string& table_name,
               std::size_t position,
               const std::optional<std::string>& alias,
               std::size_t alias_position,
               const catalog::Catalog& catalog) {
    auto schema = require_table(table_name, position, catalog);
    const auto& binding = alias.has_value() ? *alias : table_name;
    const auto binding_position = alias.has_value() ? alias_position : position;
    const auto [_, inserted] = seen_bindings.insert(binding);
    if (!inserted) {
        throw BindError(binding_position, "duplicate table binding '" + binding + "' requires a unique alias");
    }
    scopes.push_back(TableScope{binding, table_name, position, std::move(schema), 0});
}

std::vector<TableScope> build_scopes(const SelectQuery& query,
                                     const catalog::Catalog& catalog,
                                     const std::vector<TableScope>& outer_scopes) {
    std::vector<TableScope> scopes;
    scopes.reserve(1 + query.joins.size());
    std::set<std::string> seen_bindings;
    add_scope(scopes, seen_bindings, query.table, query.table_position, query.alias, query.alias_position, catalog);
    for (const auto& join : query.joins) {
        add_scope(scopes, seen_bindings, join.table, join.table_position, join.alias, join.alias_position, catalog);
    }
    scopes.insert(scopes.end(), outer_scopes.begin(), outer_scopes.end());
    return scopes;
}

std::string quoted_table_list(const std::vector<std::string>& tables) {
    std::ostringstream out;
    for (std::size_t i = 0; i < tables.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "'" << tables[i] << "'";
    }
    return out.str();
}

const TableScope& find_qualified_scope(const ColumnRef& column,
                                       const std::vector<TableScope>& scopes,
                                       bool inside_subquery) {
    for (const auto& scope : scopes) {
        if (scope.binding_name == *column.qualifier) {
            if (!scope.schema.has_column(column.name)) {
                throw BindError(column.position,
                                "unknown column '" + column.name + "' in table '" + scope.binding_name + "'");
            }
            return scope;
        }
    }
    throw_unresolved_column(column, scopes, inside_subquery);
}

const TableScope& find_unqualified_scope(const ColumnRef& column,
                                         const std::vector<TableScope>& scopes,
                                         bool inside_subquery) {
    std::vector<const TableScope*> matches;
    std::vector<std::string> match_names;
    std::optional<std::size_t> matched_depth;
    for (const auto& scope : scopes) {
        if (matched_depth.has_value() && scope.outer_depth != *matched_depth) {
            break;
        }
        if (scope.schema.has_column(column.name)) {
            matched_depth = scope.outer_depth;
            matches.push_back(&scope);
            match_names.push_back(scope.binding_name);
        }
    }

    if (matches.empty()) {
        throw_unresolved_column(column, scopes, inside_subquery);
    }

    if (matches.size() > 1) {
        throw BindError(column.position,
                        "ambiguous column '" + column.name + "' matches tables " + quoted_table_list(match_names));
    }

    return *matches.front();
}

catalog::ColumnType column_type_in_scope(const TableScope& scope, const ColumnRef& column) {
    for (const auto& schema_column : scope.schema.columns) {
        if (schema_column.name == column.name) {
            return schema_column.type;
        }
    }
    throw std::logic_error("resolved column is missing from scope schema");
}

plan::BoundColumnRef bind_column_ref(const ColumnRef& column,
                                     const std::vector<TableScope>& scopes,
                                     bool inside_subquery) {
    const auto& scope = column.qualifier.has_value() ? find_qualified_scope(column, scopes, inside_subquery)
                                                     : find_unqualified_scope(column, scopes, inside_subquery);
    return plan::BoundColumnRef{
        scope.binding_name, column.name, column.position, column_type_in_scope(scope, column), scope.outer_depth};
}

const std::vector<plan::Projection>& output_projections(const plan::LogicalPlan& logical) {
    switch (logical.kind) {
    case plan::LogicalKind::Project:
        return logical.projections;
    case plan::LogicalKind::Distinct:
    case plan::LogicalKind::Sort:
    case plan::LogicalKind::Limit:
        if (logical.input == nullptr) {
            throw std::logic_error("subquery result-shaping node is missing its input");
        }
        return output_projections(*logical.input);
    case plan::LogicalKind::Explain:
        throw std::logic_error("EXPLAIN cannot be used as a subquery");
    case plan::LogicalKind::Scan:
    case plan::LogicalKind::Join:
    case plan::LogicalKind::Filter:
    case plan::LogicalKind::Aggregate:
    case plan::LogicalKind::Window:
        break;
    }
    throw std::logic_error("bound subquery is missing its final projection");
}

std::vector<TableScope> child_outer_scopes(const std::vector<TableScope>& visible_scopes) {
    auto outer = visible_scopes;
    for (auto& scope : outer) {
        ++scope.outer_depth;
    }
    return outer;
}

std::shared_ptr<const plan::LogicalPlan> bind_subquery(const ScalarSubquery& subquery,
                                                      const catalog::Catalog& catalog,
                                                      const std::vector<TableScope>& visible_scopes) {
    if (subquery.query == nullptr) {
        throw std::logic_error("parsed scalar subquery is missing its SELECT query");
    }
    return std::make_shared<const plan::LogicalPlan>(
        bind_select_impl(*subquery.query, catalog, child_outer_scopes(visible_scopes)));
}

plan::BoundScalarExpr bind_expression(const ScalarExpr& expression,
                                      const std::vector<TableScope>& scopes,
                                      const catalog::Catalog& catalog,
                                      bool inside_subquery) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        auto bound = bind_column_ref(*column, scopes, inside_subquery);
        return plan::BoundScalarExpr{bound, bound.type};
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::Int64};
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::String};
    }
    if (const auto* literal = std::get_if<NullLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::Int64};
    }
    const auto& subquery = std::get<ScalarSubquery>(expression);
    auto bound_plan = bind_subquery(subquery, catalog, scopes);
    const auto& projections = output_projections(*bound_plan);
    if (projections.size() != 1) {
        throw BindError(subquery.position,
                        "scalar subquery must produce exactly one output column, got " +
                            std::to_string(projections.size()));
    }
    const auto type = projections.front().type;
    return plan::BoundScalarExpr{
        plan::BoundScalarSubquery{
            std::move(bound_plan), subquery.position, "scalar subquery at position " + std::to_string(subquery.position)},
        type};
}

std::string type_name(catalog::ColumnType type) {
    switch (type) {
    case catalog::ColumnType::Int64:
        return "int64";
    case catalog::ColumnType::String:
        return "string";
    }
    throw std::logic_error("unreachable column type");
}

bool is_null_literal(const plan::BoundScalarExpr& expression) {
    return std::holds_alternative<NullLiteral>(expression.value);
}

void align_null_comparison_types(plan::BoundScalarExpr& left, plan::BoundScalarExpr& right) {
    if (is_null_literal(left) && !is_null_literal(right)) {
        left.type = right.type;
    }
    if (is_null_literal(right) && !is_null_literal(left)) {
        right.type = left.type;
    }
}

plan::BoundComparisonExpr bind_comparison(const ComparisonExpr& comparison,
                                          const std::vector<TableScope>& scopes,
                                          const catalog::Catalog& catalog,
                                          bool inside_subquery) {
    auto left = bind_expression(comparison.left, scopes, catalog, inside_subquery);
    auto right = bind_expression(comparison.right, scopes, catalog, inside_subquery);
    align_null_comparison_types(left, right);
    if (left.type != right.type) {
        throw BindError(comparison.operator_position,
                        "comparison operands must have the same type: " + type_name(left.type) + " vs " +
                            type_name(right.type));
    }
    return plan::BoundComparisonExpr{
        std::move(left),
        comparison.op,
        std::move(right),
        comparison.operator_position,
    };
}

plan::BoundPredicate bind_predicate(const PredicateExpr& predicate,
                                    const std::vector<TableScope>& scopes,
                                    const catalog::Catalog& catalog,
                                    bool inside_subquery) {
    switch (predicate.kind) {
    case PredicateKind::Comparison:
        return plan::BoundPredicate::comparison_expr(
            bind_comparison(predicate.comparison, scopes, catalog, inside_subquery));
    case PredicateKind::IsNull:
    case PredicateKind::IsNotNull:
        return plan::BoundPredicate::null_check_expr(predicate.kind,
                                                    bind_expression(predicate.null_check, scopes, catalog, inside_subquery),
                                                    predicate.operator_position);
    case PredicateKind::In:
    case PredicateKind::NotIn: {
        if (predicate.subquery == nullptr) {
            throw std::logic_error("parsed IN predicate is missing its subquery");
        }
        auto value = bind_expression(predicate.in_value, scopes, catalog, inside_subquery);
        const auto parsed_subquery = ScalarSubquery{predicate.subquery, predicate.operator_position};
        auto subplan = bind_subquery(parsed_subquery, catalog, scopes);
        const auto& projections = output_projections(*subplan);
        if (projections.size() != 1) {
            throw BindError(predicate.operator_position,
                            "IN subquery must produce exactly one output column, got " +
                                std::to_string(projections.size()));
        }
        if (is_null_literal(value)) {
            value.type = projections.front().type;
        }
        if (value.type != projections.front().type) {
            throw BindError(predicate.operator_position,
                            "IN operand and subquery column must have the same type: " + type_name(value.type) +
                                " vs " + type_name(projections.front().type));
        }
        return plan::BoundPredicate::in_expr(
            predicate.kind,
            std::move(value),
            std::move(subplan),
            predicate.operator_position,
            "IN subquery at position " + std::to_string(predicate.operator_position));
    }
    case PredicateKind::Exists:
    case PredicateKind::NotExists: {
        if (predicate.subquery == nullptr) {
            throw std::logic_error("parsed EXISTS predicate is missing its subquery");
        }
        auto subplan =
            bind_subquery(ScalarSubquery{predicate.subquery, predicate.operator_position}, catalog, scopes);
        return plan::BoundPredicate::exists_expr(
            predicate.kind,
            std::move(subplan),
            predicate.operator_position,
            "EXISTS subquery at position " + std::to_string(predicate.operator_position));
    }
    case PredicateKind::And:
    case PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("parsed boolean predicate is missing a child");
        }
        return plan::BoundPredicate::binary(predicate.kind,
                                           bind_predicate(*predicate.left, scopes, catalog, inside_subquery),
                                           bind_predicate(*predicate.right, scopes, catalog, inside_subquery),
                                           predicate.operator_position);
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<plan::BoundPredicate> bind_predicates(const std::vector<PredicateExpr>& predicates,
                                                  const std::vector<TableScope>& scopes,
                                                  const catalog::Catalog& catalog,
                                                  bool inside_subquery) {
    std::vector<plan::BoundPredicate> bound;
    bound.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        bound.push_back(bind_predicate(predicate, scopes, catalog, inside_subquery));
    }
    return bound;
}

bool bound_column_equal(const plan::BoundColumnRef& left, const plan::BoundColumnRef& right) {
    return left.binding == right.binding && left.column == right.column;
}

bool contains_group_key(const std::vector<plan::BoundColumnRef>& group_keys, const plan::BoundColumnRef& column) {
    for (const auto& key : group_keys) {
        if (bound_column_equal(key, column)) {
            return true;
        }
    }
    return false;
}

std::vector<plan::BoundColumnRef> bind_group_by_keys(const std::vector<ColumnRef>& keys,
                                                     const std::vector<TableScope>& scopes,
                                                     bool inside_subquery) {
    std::vector<plan::BoundColumnRef> bound;
    bound.reserve(keys.size());
    std::set<std::string> seen;
    for (const auto& key : keys) {
        auto column = bind_column_ref(key, scopes, inside_subquery);
        const auto identity = column.binding + "." + column.column;
        const auto [_, inserted] = seen.insert(identity);
        if (!inserted) {
            throw BindError(key.position, "duplicate GROUP BY column '" + output_name(key) + "'");
        }
        bound.push_back(std::move(column));
    }
    return bound;
}

bool select_item_is_aggregate(const SelectItem& item) {
    return std::holds_alternative<AggregateCall>(item.expression);
}

bool window_input_is_aggregate(const WindowInputExpr& expression) {
    return std::holds_alternative<AggregateCall>(expression);
}

bool window_has_input_aggregate(const WindowCall& window) {
    if (window.argument.has_value() && window_input_is_aggregate(*window.argument)) {
        return true;
    }
    for (const auto& key : window.partition_by) {
        if (window_input_is_aggregate(key)) {
            return true;
        }
    }
    for (const auto& key : window.order_by) {
        if (window_input_is_aggregate(key.expression)) {
            return true;
        }
    }
    return false;
}

bool query_has_aggregate(const SelectQuery& query) {
    for (const auto& item : query.projection) {
        if (select_item_is_aggregate(item)) {
            return true;
        }
        if (const auto* window = std::get_if<WindowCall>(&item.expression);
            window != nullptr && window_has_input_aggregate(*window)) {
            return true;
        }
    }
    return false;
}

plan::AggregateExpression hidden_count_star_aggregate() {
    return plan::AggregateExpression{
        "COUNT(*)",
        AggregateFunction::Count,
        std::nullopt,
        0,
        catalog::ColumnType::Int64,
    };
}

plan::AggregateExpression bind_aggregate_call(const AggregateCall& aggregate,
                                              const std::vector<TableScope>& scopes,
                                              bool inside_subquery) {
    if (aggregate.nested_aggregate) {
        throw BindError(aggregate.nested_position,
                        "nested aggregate '" + aggregate_function_name(aggregate.nested_function) + "' is not allowed");
    }

    std::optional<plan::BoundColumnRef> argument;
    if (aggregate.argument.has_value()) {
        argument = bind_column_ref(*aggregate.argument, scopes, inside_subquery);
    }

    auto output_type = catalog::ColumnType::Int64;
    switch (aggregate.function) {
    case AggregateFunction::Count:
        output_type = catalog::ColumnType::Int64;
        break;
    case AggregateFunction::Sum:
        if (!argument.has_value()) {
            throw BindError(aggregate.position, "SUM requires int64 argument, got <missing>");
        }
        if (argument->type != catalog::ColumnType::Int64) {
            throw BindError(aggregate.position, "SUM requires int64 argument, got " + type_name(argument->type));
        }
        output_type = catalog::ColumnType::Int64;
        break;
    case AggregateFunction::Min:
    case AggregateFunction::Max:
        if (!argument.has_value()) {
            throw BindError(aggregate.position,
                            aggregate_function_name(aggregate.function) + " requires an argument");
        }
        output_type = argument->type;
        break;
    }

    return plan::AggregateExpression{
        output_name(aggregate),
        aggregate.function,
        std::move(argument),
        aggregate.position,
        output_type,
    };
}

plan::BoundColumnRef aggregate_output_ref(const plan::AggregateExpression& aggregate, std::size_t position) {
    return plan::BoundColumnRef{"", aggregate.output_name, position, aggregate.type};
}

plan::BoundColumnRef ensure_aggregate_expression(const AggregateCall& aggregate,
                                                 const std::vector<TableScope>& scopes,
                                                 std::vector<plan::AggregateExpression>& aggregate_expressions,
                                                 bool inside_subquery) {
    auto bound = bind_aggregate_call(aggregate, scopes, inside_subquery);
    for (const auto& existing : aggregate_expressions) {
        if (existing.output_name == bound.output_name) {
            return aggregate_output_ref(existing, aggregate.position);
        }
    }

    const auto ref = aggregate_output_ref(bound, aggregate.position);
    aggregate_expressions.push_back(std::move(bound));
    return ref;
}

plan::BoundColumnRef bind_window_input(const WindowInputExpr& expression,
                                       const std::vector<TableScope>& scopes,
                                       const std::vector<plan::BoundColumnRef>& group_keys,
                                       bool aggregate_query,
                                       std::vector<plan::AggregateExpression>& aggregate_expressions,
                                       bool inside_subquery) {
    if (const auto* aggregate = std::get_if<AggregateCall>(&expression)) {
        return ensure_aggregate_expression(*aggregate, scopes, aggregate_expressions, inside_subquery);
    }

    const auto& column = std::get<ColumnRef>(expression);
    auto bound = bind_column_ref(column, scopes, inside_subquery);
    if (aggregate_query && bound.outer_depth == 0 && !contains_group_key(group_keys, bound)) {
        throw BindError(column.position,
                        "window column '" + output_name(column) +
                            "' must appear in GROUP BY or be aggregated");
    }
    return bound;
}

catalog::ColumnType bind_window_output_type(const WindowCall& window,
                                            const std::optional<plan::BoundColumnRef>& argument) {
    switch (window.function) {
    case WindowFunction::RowNumber:
    case WindowFunction::Rank:
    case WindowFunction::DenseRank:
    case WindowFunction::Count:
        return catalog::ColumnType::Int64;
    case WindowFunction::Sum:
        if (!argument.has_value()) {
            throw BindError(window.position, "SUM requires int64 argument, got <missing>");
        }
        if (argument->type != catalog::ColumnType::Int64) {
            throw BindError(window.position, "SUM requires int64 argument, got " + type_name(argument->type));
        }
        return catalog::ColumnType::Int64;
    case WindowFunction::Min:
    case WindowFunction::Max:
        if (!argument.has_value()) {
            throw BindError(window.position,
                            window_function_name(window.function) + " requires an argument");
        }
        return argument->type;
    }
    throw std::logic_error("unreachable window function");
}

plan::BoundColumnRef ensure_window_expression(const WindowCall& window,
                                              const std::vector<TableScope>& scopes,
                                              const std::vector<plan::BoundColumnRef>& group_keys,
                                              bool aggregate_query,
                                              std::vector<plan::AggregateExpression>& aggregate_expressions,
                                              std::vector<plan::WindowExpression>& window_expressions,
                                              bool inside_subquery) {
    std::optional<plan::BoundColumnRef> argument;
    if (window.argument.has_value()) {
        argument = bind_window_input(*window.argument,
                                     scopes,
                                     group_keys,
                                     aggregate_query,
                                     aggregate_expressions,
                                     inside_subquery);
    }

    std::vector<plan::BoundColumnRef> partition_keys;
    partition_keys.reserve(window.partition_by.size());
    for (const auto& key : window.partition_by) {
        partition_keys.push_back(bind_window_input(
            key, scopes, group_keys, aggregate_query, aggregate_expressions, inside_subquery));
    }

    std::vector<plan::SortKey> order_keys;
    order_keys.reserve(window.order_by.size());
    for (const auto& key : window.order_by) {
        order_keys.push_back(plan::SortKey{
            bind_window_input(key.expression,
                              scopes,
                              group_keys,
                              aggregate_query,
                              aggregate_expressions,
                              inside_subquery),
            key.direction});
    }

    const auto name = output_name(window);
    const auto type = bind_window_output_type(window, argument);
    for (const auto& existing : window_expressions) {
        if (existing.output_name == name) {
            return plan::BoundColumnRef{"", existing.output_name, window.position, existing.type};
        }
    }

    window_expressions.push_back(plan::WindowExpression{name,
                                                        window.function,
                                                        window.count_star,
                                                        std::move(argument),
                                                        std::move(partition_keys),
                                                        std::move(order_keys),
                                                        window.position,
                                                        type});
    return plan::BoundColumnRef{"", name, window.position, type};
}

std::string select_item_output_name(const SelectItem& item) {
    return item.alias.has_value() ? *item.alias : output_name(item.expression);
}

std::size_t select_item_output_position(const SelectItem& item) {
    return item.alias.has_value() ? item.alias_position : item.position;
}

bool has_output_name(const std::vector<std::string>& output_names, const std::string& name) {
    return std::find(output_names.begin(), output_names.end(), name) != output_names.end();
}

catalog::ColumnType output_type_for_name(const std::vector<plan::Projection>& projections, const std::string& name) {
    for (const auto& projection : projections) {
        if (projection.output_name == name) {
            return projection.type;
        }
    }
    throw std::logic_error("resolved output name is missing from projections");
}

plan::BoundScalarExpr bind_having_expression(const HavingExpr& expression,
                                             const std::vector<TableScope>& scopes,
                                             const std::vector<plan::BoundColumnRef>& group_keys,
                                             std::vector<plan::AggregateExpression>& aggregate_expressions,
                                             const catalog::Catalog& catalog,
                                             bool inside_subquery) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        auto bound = bind_column_ref(*column, scopes, inside_subquery);
        if (bound.outer_depth == 0 && !contains_group_key(group_keys, bound)) {
            throw BindError(column->position,
                            "HAVING column '" + output_name(*column) +
                                "' must be a GROUP BY column or aggregate expression");
        }
        return plan::BoundScalarExpr{bound, bound.type};
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::Int64};
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::String};
    }
    if (const auto* literal = std::get_if<NullLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::Int64};
    }
    if (const auto* subquery = std::get_if<ScalarSubquery>(&expression)) {
        auto bound_plan = bind_subquery(*subquery, catalog, scopes);
        const auto& projections = output_projections(*bound_plan);
        if (projections.size() != 1) {
            throw BindError(subquery->position,
                            "scalar subquery must produce exactly one output column, got " +
                                std::to_string(projections.size()));
        }
        const auto type = projections.front().type;
        return plan::BoundScalarExpr{
            plan::BoundScalarSubquery{std::move(bound_plan),
                                      subquery->position,
                                      "scalar subquery at position " + std::to_string(subquery->position)},
            type};
    }
    auto aggregate_ref = ensure_aggregate_expression(
        std::get<AggregateCall>(expression), scopes, aggregate_expressions, inside_subquery);
    return plan::BoundScalarExpr{aggregate_ref, aggregate_ref.type};
}

plan::BoundComparisonExpr bind_having_comparison(const HavingComparisonExpr& comparison,
                                                 const std::vector<TableScope>& scopes,
                                                 const std::vector<plan::BoundColumnRef>& group_keys,
                                                 std::vector<plan::AggregateExpression>& aggregate_expressions,
                                                 const catalog::Catalog& catalog,
                                                 bool inside_subquery) {
    auto left = bind_having_expression(
        comparison.left, scopes, group_keys, aggregate_expressions, catalog, inside_subquery);
    auto right = bind_having_expression(
        comparison.right, scopes, group_keys, aggregate_expressions, catalog, inside_subquery);
    align_null_comparison_types(left, right);
    if (left.type != right.type) {
        throw BindError(comparison.operator_position,
                        "comparison operands must have the same type: " + type_name(left.type) + " vs " +
                            type_name(right.type));
    }
    return plan::BoundComparisonExpr{std::move(left), comparison.op, std::move(right), comparison.operator_position};
}

plan::BoundPredicate bind_having_predicate(const HavingPredicateExpr& predicate,
                                           const std::vector<TableScope>& scopes,
                                           const std::vector<plan::BoundColumnRef>& group_keys,
                                           std::vector<plan::AggregateExpression>& aggregate_expressions,
                                           const catalog::Catalog& catalog,
                                           bool inside_subquery) {
    switch (predicate.kind) {
    case PredicateKind::Comparison:
        return plan::BoundPredicate::comparison_expr(
            bind_having_comparison(
                predicate.comparison, scopes, group_keys, aggregate_expressions, catalog, inside_subquery));
    case PredicateKind::IsNull:
    case PredicateKind::IsNotNull:
        return plan::BoundPredicate::null_check_expr(
            predicate.kind,
            bind_having_expression(
                predicate.null_check, scopes, group_keys, aggregate_expressions, catalog, inside_subquery),
            predicate.operator_position);
    case PredicateKind::In:
    case PredicateKind::NotIn: {
        if (predicate.subquery == nullptr) {
            throw std::logic_error("parsed HAVING IN predicate is missing its subquery");
        }
        auto value = bind_having_expression(
            predicate.in_value, scopes, group_keys, aggregate_expressions, catalog, inside_subquery);
        auto subplan =
            bind_subquery(ScalarSubquery{predicate.subquery, predicate.operator_position}, catalog, scopes);
        const auto& projections = output_projections(*subplan);
        if (projections.size() != 1) {
            throw BindError(predicate.operator_position,
                            "IN subquery must produce exactly one output column, got " +
                                std::to_string(projections.size()));
        }
        if (is_null_literal(value)) {
            value.type = projections.front().type;
        }
        if (value.type != projections.front().type) {
            throw BindError(predicate.operator_position,
                            "IN operand and subquery column must have the same type: " + type_name(value.type) +
                                " vs " + type_name(projections.front().type));
        }
        return plan::BoundPredicate::in_expr(
            predicate.kind,
            std::move(value),
            std::move(subplan),
            predicate.operator_position,
            "IN subquery at position " + std::to_string(predicate.operator_position));
    }
    case PredicateKind::Exists:
    case PredicateKind::NotExists:
        if (predicate.subquery == nullptr) {
            throw std::logic_error("parsed HAVING EXISTS predicate is missing its subquery");
        }
        return plan::BoundPredicate::exists_expr(
            predicate.kind,
            bind_subquery(ScalarSubquery{predicate.subquery, predicate.operator_position}, catalog, scopes),
            predicate.operator_position,
            "EXISTS subquery at position " + std::to_string(predicate.operator_position));
    case PredicateKind::And:
    case PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("parsed HAVING predicate is missing a child");
        }
        return plan::BoundPredicate::binary(predicate.kind,
                                           bind_having_predicate(*predicate.left,
                                                                 scopes,
                                                                 group_keys,
                                                                 aggregate_expressions,
                                                                 catalog,
                                                                 inside_subquery),
                                           bind_having_predicate(*predicate.right,
                                                                 scopes,
                                                                 group_keys,
                                                                 aggregate_expressions,
                                                                 catalog,
                                                                 inside_subquery),
                                           predicate.operator_position);
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<plan::BoundPredicate> bind_having_predicates(
    const std::vector<HavingPredicateExpr>& predicates,
    const std::vector<TableScope>& scopes,
    const std::vector<plan::BoundColumnRef>& group_keys,
    std::vector<plan::AggregateExpression>& aggregate_expressions,
    const catalog::Catalog& catalog,
    bool inside_subquery) {
    std::vector<plan::BoundPredicate> bound;
    bound.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        bound.push_back(
            bind_having_predicate(predicate, scopes, group_keys, aggregate_expressions, catalog, inside_subquery));
    }
    return bound;
}

std::vector<plan::SortKey> bind_order_by_keys(const std::vector<OrderByKey>& keys,
                                              const std::vector<TableScope>& scopes,
                                              const std::vector<plan::BoundColumnRef>& group_keys,
                                              bool aggregate_query,
                                              bool distinct_query,
                                              const std::vector<std::string>& output_names,
                                              const std::vector<plan::Projection>& projections,
                                              bool inside_subquery) {
    std::vector<plan::SortKey> bound;
    bound.reserve(keys.size());
    for (const auto& key : keys) {
        const auto name = output_name(key.expression);
        if (has_output_name(output_names, name)) {
            bound.push_back(plan::SortKey{
                plan::BoundColumnRef{"", name, expression_position(key.expression), output_type_for_name(projections, name)},
                key.direction});
            continue;
        }

        if (const auto* aggregate = std::get_if<AggregateCall>(&key.expression)) {
            (void)bind_aggregate_call(*aggregate, scopes, inside_subquery);
            throw BindError(aggregate->position, "ORDER BY output '" + name + "' must appear in SELECT list");
        }

        const auto& column = std::get<ColumnRef>(key.expression);
        auto sort_key = plan::SortKey{bind_column_ref(column, scopes, inside_subquery), key.direction};
        if (distinct_query) {
            throw BindError(column.position,
                            "ORDER BY column '" + output_name(column) +
                                "' must appear in SELECT list for SELECT DISTINCT");
        }
        if (aggregate_query && !contains_group_key(group_keys, sort_key.column)) {
            throw BindError(column.position,
                            "ORDER BY column '" + output_name(column) +
                                "' must be a GROUP BY column or SELECT output name in aggregate queries");
        }
        bound.push_back(std::move(sort_key));
    }
    return bound;
}

bool same_correlation(const plan::BoundColumnRef& left, const plan::BoundColumnRef& right) {
    return left.binding == right.binding && left.column == right.column && left.type == right.type &&
           left.outer_depth == right.outer_depth;
}

void add_correlation(std::vector<plan::BoundColumnRef>& correlations, plan::BoundColumnRef column) {
    if (column.outer_depth == 0) {
        return;
    }
    if (std::none_of(correlations.begin(), correlations.end(), [&](const auto& existing) {
            return same_correlation(existing, column);
        })) {
        correlations.push_back(std::move(column));
    }
}

void collect_scalar_correlations(const plan::BoundScalarExpr& expression,
                                 std::vector<plan::BoundColumnRef>& correlations);

void collect_nested_subplan_correlations(const std::shared_ptr<const plan::LogicalPlan>& subplan,
                                         std::vector<plan::BoundColumnRef>& correlations) {
    if (subplan == nullptr) {
        throw std::logic_error("bound subquery is missing its logical plan");
    }
    for (auto column : subplan->correlation_columns) {
        // A depth-one dependency is satisfied by this query block's current
        // row. Deeper dependencies remain correlations of this owner after
        // rebasing one lexical level.
        if (column.outer_depth > 1) {
            --column.outer_depth;
            add_correlation(correlations, std::move(column));
        }
    }
}

void collect_scalar_correlations(const plan::BoundScalarExpr& expression,
                                 std::vector<plan::BoundColumnRef>& correlations) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        add_correlation(correlations, *column);
        return;
    }
    if (const auto* subquery = std::get_if<plan::BoundScalarSubquery>(&expression.value)) {
        collect_nested_subplan_correlations(subquery->plan, correlations);
    }
}

void collect_predicate_correlations(const plan::BoundPredicate& predicate,
                                    std::vector<plan::BoundColumnRef>& correlations) {
    switch (predicate.kind) {
    case PredicateKind::Comparison:
        collect_scalar_correlations(predicate.comparison.left, correlations);
        collect_scalar_correlations(predicate.comparison.right, correlations);
        return;
    case PredicateKind::IsNull:
    case PredicateKind::IsNotNull:
        collect_scalar_correlations(predicate.null_check, correlations);
        return;
    case PredicateKind::In:
    case PredicateKind::NotIn:
        collect_scalar_correlations(predicate.in_value, correlations);
        collect_nested_subplan_correlations(predicate.subquery, correlations);
        return;
    case PredicateKind::Exists:
    case PredicateKind::NotExists:
        collect_nested_subplan_correlations(predicate.subquery, correlations);
        return;
    case PredicateKind::And:
    case PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        collect_predicate_correlations(*predicate.left, correlations);
        collect_predicate_correlations(*predicate.right, correlations);
        return;
    }
    throw std::logic_error("unreachable predicate kind");
}

void collect_plan_correlations(const plan::LogicalPlan& logical,
                               std::vector<plan::BoundColumnRef>& correlations) {
    for (const auto& projection : logical.projections) {
        collect_scalar_correlations(projection.expression, correlations);
    }
    for (const auto& key : logical.group_keys) {
        add_correlation(correlations, key);
    }
    for (const auto& aggregate : logical.aggregate_expressions) {
        if (aggregate.argument.has_value()) {
            add_correlation(correlations, *aggregate.argument);
        }
    }
    for (const auto& window : logical.window_expressions) {
        if (window.argument.has_value()) {
            add_correlation(correlations, *window.argument);
        }
        for (const auto& key : window.partition_keys) {
            add_correlation(correlations, key);
        }
        for (const auto& key : window.order_keys) {
            add_correlation(correlations, key.column);
        }
    }
    for (const auto& key : logical.sort_keys) {
        add_correlation(correlations, key.column);
    }
    for (const auto& predicate : logical.predicates) {
        collect_predicate_correlations(predicate, correlations);
    }
    if (logical.input != nullptr) {
        collect_plan_correlations(*logical.input, correlations);
    }
    if (logical.left != nullptr) {
        collect_plan_correlations(*logical.left, correlations);
    }
    if (logical.right != nullptr) {
        collect_plan_correlations(*logical.right, correlations);
    }
}

std::vector<plan::BoundColumnRef> correlation_set(const plan::LogicalPlan& logical) {
    std::vector<plan::BoundColumnRef> correlations;
    collect_plan_correlations(logical, correlations);
    return correlations;
}

void mark_arbitrary_order(plan::LogicalPlan& logical) {
    logical.order_permission = plan::OrderPermission::Arbitrary;
    if (logical.input != nullptr) {
        mark_arbitrary_order(*logical.input);
    }
    if (logical.left != nullptr) {
        mark_arbitrary_order(*logical.left);
    }
    if (logical.right != nullptr) {
        mark_arbitrary_order(*logical.right);
    }
}

void mark_deterministic_order(plan::LogicalPlan& logical) {
    logical.order_permission = plan::OrderPermission::Deterministic;
    if (logical.input != nullptr) {
        mark_deterministic_order(*logical.input);
    }
    if (logical.left != nullptr) {
        mark_deterministic_order(*logical.left);
    }
    if (logical.right != nullptr) {
        mark_deterministic_order(*logical.right);
    }
}

bool window_requires_deterministic_input(const plan::LogicalPlan& logical) {
    return logical.kind == plan::LogicalKind::Window &&
           std::any_of(logical.window_expressions.begin(), logical.window_expressions.end(), [](const auto& window) {
               return window.function == WindowFunction::RowNumber || window.function == WindowFunction::Sum;
           });
}

void protect_order_sensitive_window_inputs(plan::LogicalPlan& logical) {
    if (window_requires_deterministic_input(logical)) {
        if (logical.input == nullptr) {
            throw std::logic_error("Window logical plan is missing its input");
        }
        // ROW_NUMBER exposes stable child order whenever OVER keys tie. SUM's
        // checked intermediate overflow can likewise depend on accumulation
        // order even when the mathematical total is representable. Without a
        // uniqueness or order-independent overflow proof, order-changing join
        // commute/associate rules must fail closed throughout this child.
        mark_deterministic_order(*logical.input);
        return;
    }
    if (logical.input != nullptr) {
        protect_order_sensitive_window_inputs(*logical.input);
    }
    if (logical.left != nullptr) {
        protect_order_sensitive_window_inputs(*logical.left);
    }
    if (logical.right != nullptr) {
        protect_order_sensitive_window_inputs(*logical.right);
    }
}

plan::JoinKind bound_join_kind(JoinKind parsed) {
    switch (parsed) {
    case JoinKind::Inner:
        return plan::JoinKind::Inner;
    case JoinKind::Left:
    case JoinKind::Right:
        return plan::JoinKind::Left;
    }
    throw std::logic_error("unreachable parsed join kind");
}

plan::LogicalPlan bind_select_impl(const SelectQuery& query,
                                   const catalog::Catalog& catalog,
                                   const std::vector<TableScope>& outer_scopes) {
    const auto scopes = build_scopes(query, catalog, outer_scopes);
    const auto inside_subquery = !outer_scopes.empty();
    auto group_keys = bind_group_by_keys(query.group_by, scopes, inside_subquery);
    const auto aggregate_query = !group_keys.empty() || query_has_aggregate(query) || query.having.has_value();

    std::set<std::string> output_names;
    std::vector<std::string> output_name_order;
    std::vector<plan::Projection> projections;
    std::vector<plan::AggregateExpression> aggregate_expressions;
    std::vector<plan::WindowExpression> window_expressions;
    projections.reserve(query.projection.size());
    output_name_order.reserve(query.projection.size());
    for (const auto& item : query.projection) {
        auto name = select_item_output_name(item);
        const auto [_, inserted] = output_names.insert(name);
        if (!inserted) {
            throw BindError(select_item_output_position(item), "duplicate output name '" + name + "'");
        }
        output_name_order.push_back(name);

        if (const auto* aggregate = std::get_if<AggregateCall>(&item.expression)) {
            const auto aggregate_ref =
                ensure_aggregate_expression(*aggregate, scopes, aggregate_expressions, inside_subquery);
            projections.push_back(plan::Projection{
                name,
                plan::BoundScalarExpr{aggregate_ref, aggregate_ref.type},
                aggregate_ref.type,
            });
            continue;
        }

        if (const auto* window = std::get_if<WindowCall>(&item.expression)) {
            const auto window_ref = ensure_window_expression(*window,
                                                             scopes,
                                                             group_keys,
                                                             aggregate_query,
                                                             aggregate_expressions,
                                                             window_expressions,
                                                             inside_subquery);
            projections.push_back(plan::Projection{
                name,
                plan::BoundScalarExpr{window_ref, window_ref.type},
                window_ref.type,
            });
            continue;
        }

        const auto& scalar = std::get<ScalarExpr>(item.expression);
        auto expression = bind_expression(scalar, scopes, catalog, inside_subquery);
        if (aggregate_query) {
            if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value);
                column != nullptr && column->outer_depth == 0 && !contains_group_key(group_keys, *column)) {
                const auto& parsed_column = std::get<ColumnRef>(scalar);
                throw BindError(parsed_column.position,
                                "non-grouped column '" + output_name(parsed_column) +
                                    "' must appear in GROUP BY or be aggregated");
            }
        }
        const auto type = expression.type;
        projections.push_back(plan::Projection{std::move(name), std::move(expression), type});
    }

    std::vector<plan::BoundPredicate> having_predicates;
    if (query.having.has_value()) {
        having_predicates = bind_having_predicates(
            query.having->conjuncts, scopes, group_keys, aggregate_expressions, catalog, inside_subquery);
        if (group_keys.empty() && aggregate_expressions.empty()) {
            aggregate_expressions.push_back(hidden_count_star_aggregate());
        }
    }

    auto sort_keys = bind_order_by_keys(query.order_by,
                                        scopes,
                                        group_keys,
                                        aggregate_query,
                                        query.distinct,
                                        output_name_order,
                                        projections,
                                        inside_subquery);

    auto plan = plan::LogicalPlan::scan(query.table, binding_name(query));
    std::vector<TableScope> visible_scopes;
    visible_scopes.push_back(scopes.front());
    for (std::size_t i = 0; i < query.joins.size(); ++i) {
        visible_scopes.push_back(scopes.at(i + 1));
        auto predicate_scopes = visible_scopes;
        predicate_scopes.insert(predicate_scopes.end(), outer_scopes.begin(), outer_scopes.end());
        auto predicates = bind_predicates(
            query.joins[i].predicates, predicate_scopes, catalog, inside_subquery);
        auto right_scan = plan::LogicalPlan::scan(query.joins[i].table, binding_name(query.joins[i]));
        if (query.joins[i].kind == JoinKind::Right) {
            plan = plan::LogicalPlan::join(std::move(predicates),
                                           std::move(right_scan),
                                           std::move(plan),
                                           bound_join_kind(query.joins[i].kind));
            continue;
        }
        plan = plan::LogicalPlan::join(std::move(predicates),
                                       std::move(plan),
                                       std::move(right_scan),
                                       bound_join_kind(query.joins[i].kind));
    }

    if (query.predicate.has_value()) {
        plan = plan::LogicalPlan::filter(
            bind_predicates(query.predicate->conjuncts, scopes, catalog, inside_subquery), std::move(plan));
    }
    if (aggregate_query) {
        plan = plan::LogicalPlan::aggregate(std::move(group_keys), std::move(aggregate_expressions), std::move(plan));
    }
    if (!having_predicates.empty()) {
        plan = plan::LogicalPlan::filter(std::move(having_predicates), std::move(plan));
    }
    if (!window_expressions.empty()) {
        plan = plan::LogicalPlan::window(std::move(window_expressions), std::move(plan));
    }
    auto bound = plan::LogicalPlan::project(std::move(projections), std::move(plan));
    if (query.distinct) {
        bound = plan::LogicalPlan::distinct(std::move(bound));
    }
    mark_arbitrary_order(bound);
    protect_order_sensitive_window_inputs(bound);
    if (!sort_keys.empty()) {
        bound = plan::LogicalPlan::sort(std::move(sort_keys), std::move(bound));
    }
    if (query.limit.has_value()) {
        bound = plan::LogicalPlan::limit(*query.limit, std::move(bound));
        bound.order_permission = bound.input->order_permission;
    }
    bound.correlation_columns = correlation_set(bound);
    if (query.explain) {
        return plan::LogicalPlan::explain(std::move(bound));
    }
    return bound;
}

} // namespace

plan::LogicalPlan bind_select(const SelectQuery& query, const catalog::Catalog& catalog) {
    return bind_select_impl(query, catalog, {});
}

} // namespace sql
