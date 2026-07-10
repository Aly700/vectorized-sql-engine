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
};

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
    scopes.push_back(TableScope{binding, table_name, position, std::move(schema)});
}

std::vector<TableScope> build_scopes(const SelectQuery& query, const catalog::Catalog& catalog) {
    std::vector<TableScope> scopes;
    scopes.reserve(1 + query.joins.size());
    std::set<std::string> seen_bindings;
    add_scope(scopes, seen_bindings, query.table, query.table_position, query.alias, query.alias_position, catalog);
    for (const auto& join : query.joins) {
        add_scope(scopes, seen_bindings, join.table, join.table_position, join.alias, join.alias_position, catalog);
    }
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

const TableScope& find_qualified_scope(const ColumnRef& column, const std::vector<TableScope>& scopes) {
    for (const auto& scope : scopes) {
        if (scope.binding_name == *column.qualifier) {
            if (!scope.schema.has_column(column.name)) {
                throw BindError(column.position,
                                "unknown column '" + column.name + "' in table '" + scope.binding_name + "'");
            }
            return scope;
        }
    }
    throw BindError(column.position, "unknown table qualifier '" + *column.qualifier + "'");
}

const TableScope& find_unqualified_scope(const ColumnRef& column, const std::vector<TableScope>& scopes) {
    std::vector<const TableScope*> matches;
    std::vector<std::string> match_names;
    for (const auto& scope : scopes) {
        if (scope.schema.has_column(column.name)) {
            matches.push_back(&scope);
            match_names.push_back(scope.binding_name);
        }
    }

    if (matches.empty()) {
        if (scopes.size() == 1) {
            throw BindError(column.position,
                            "unknown column '" + column.name + "' in table '" + scopes.front().binding_name + "'");
        }
        throw BindError(column.position, "unknown column '" + column.name + "'");
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

plan::BoundColumnRef bind_column_ref(const ColumnRef& column, const std::vector<TableScope>& scopes) {
    const auto& scope =
        column.qualifier.has_value() ? find_qualified_scope(column, scopes) : find_unqualified_scope(column, scopes);
    return plan::BoundColumnRef{scope.binding_name, column.name, column.position, column_type_in_scope(scope, column)};
}

plan::BoundScalarExpr bind_expression(const ScalarExpr& expression, const std::vector<TableScope>& scopes) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        auto bound = bind_column_ref(*column, scopes);
        return plan::BoundScalarExpr{bound, bound.type};
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::Int64};
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return plan::BoundScalarExpr{*literal, catalog::ColumnType::String};
    }
    return plan::BoundScalarExpr{std::get<NullLiteral>(expression), catalog::ColumnType::Int64};
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

plan::BoundComparisonExpr bind_comparison(const ComparisonExpr& comparison, const std::vector<TableScope>& scopes) {
    auto left = bind_expression(comparison.left, scopes);
    auto right = bind_expression(comparison.right, scopes);
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

plan::BoundPredicate bind_predicate(const PredicateExpr& predicate, const std::vector<TableScope>& scopes) {
    switch (predicate.kind) {
    case PredicateKind::Comparison:
        return plan::BoundPredicate::comparison_expr(bind_comparison(predicate.comparison, scopes));
    case PredicateKind::IsNull:
    case PredicateKind::IsNotNull:
        return plan::BoundPredicate::null_check_expr(predicate.kind,
                                                    bind_expression(predicate.null_check, scopes),
                                                    predicate.operator_position);
    case PredicateKind::And:
    case PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("parsed boolean predicate is missing a child");
        }
        return plan::BoundPredicate::binary(predicate.kind,
                                           bind_predicate(*predicate.left, scopes),
                                           bind_predicate(*predicate.right, scopes),
                                           predicate.operator_position);
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<plan::BoundPredicate> bind_predicates(const std::vector<PredicateExpr>& predicates,
                                                  const std::vector<TableScope>& scopes) {
    std::vector<plan::BoundPredicate> bound;
    bound.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        bound.push_back(bind_predicate(predicate, scopes));
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
                                                     const std::vector<TableScope>& scopes) {
    std::vector<plan::BoundColumnRef> bound;
    bound.reserve(keys.size());
    std::set<std::string> seen;
    for (const auto& key : keys) {
        auto column = bind_column_ref(key, scopes);
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

bool query_has_aggregate(const SelectQuery& query) {
    for (const auto& item : query.projection) {
        if (select_item_is_aggregate(item)) {
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
                                              const std::vector<TableScope>& scopes) {
    if (aggregate.nested_aggregate) {
        throw BindError(aggregate.nested_position,
                        "nested aggregate '" + aggregate_function_name(aggregate.nested_function) + "' is not allowed");
    }

    std::optional<plan::BoundColumnRef> argument;
    if (aggregate.argument.has_value()) {
        argument = bind_column_ref(*aggregate.argument, scopes);
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
                                                 std::vector<plan::AggregateExpression>& aggregate_expressions) {
    auto bound = bind_aggregate_call(aggregate, scopes);
    for (const auto& existing : aggregate_expressions) {
        if (existing.output_name == bound.output_name) {
            return aggregate_output_ref(existing, aggregate.position);
        }
    }

    const auto ref = aggregate_output_ref(bound, aggregate.position);
    aggregate_expressions.push_back(std::move(bound));
    return ref;
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
                                             std::vector<plan::AggregateExpression>& aggregate_expressions) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        auto bound = bind_column_ref(*column, scopes);
        if (!contains_group_key(group_keys, bound)) {
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
    auto aggregate_ref = ensure_aggregate_expression(std::get<AggregateCall>(expression), scopes, aggregate_expressions);
    return plan::BoundScalarExpr{aggregate_ref, aggregate_ref.type};
}

plan::BoundComparisonExpr bind_having_comparison(const HavingComparisonExpr& comparison,
                                                 const std::vector<TableScope>& scopes,
                                                 const std::vector<plan::BoundColumnRef>& group_keys,
                                                 std::vector<plan::AggregateExpression>& aggregate_expressions) {
    auto left = bind_having_expression(comparison.left, scopes, group_keys, aggregate_expressions);
    auto right = bind_having_expression(comparison.right, scopes, group_keys, aggregate_expressions);
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
                                           std::vector<plan::AggregateExpression>& aggregate_expressions) {
    switch (predicate.kind) {
    case PredicateKind::Comparison:
        return plan::BoundPredicate::comparison_expr(
            bind_having_comparison(predicate.comparison, scopes, group_keys, aggregate_expressions));
    case PredicateKind::IsNull:
    case PredicateKind::IsNotNull:
        return plan::BoundPredicate::null_check_expr(
            predicate.kind,
            bind_having_expression(predicate.null_check, scopes, group_keys, aggregate_expressions),
            predicate.operator_position);
    case PredicateKind::And:
    case PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("parsed HAVING predicate is missing a child");
        }
        return plan::BoundPredicate::binary(predicate.kind,
                                           bind_having_predicate(*predicate.left,
                                                                 scopes,
                                                                 group_keys,
                                                                 aggregate_expressions),
                                           bind_having_predicate(*predicate.right,
                                                                 scopes,
                                                                 group_keys,
                                                                 aggregate_expressions),
                                           predicate.operator_position);
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<plan::BoundPredicate> bind_having_predicates(
    const std::vector<HavingPredicateExpr>& predicates,
    const std::vector<TableScope>& scopes,
    const std::vector<plan::BoundColumnRef>& group_keys,
    std::vector<plan::AggregateExpression>& aggregate_expressions) {
    std::vector<plan::BoundPredicate> bound;
    bound.reserve(predicates.size());
    for (const auto& predicate : predicates) {
        bound.push_back(bind_having_predicate(predicate, scopes, group_keys, aggregate_expressions));
    }
    return bound;
}

std::vector<plan::SortKey> bind_order_by_keys(const std::vector<OrderByKey>& keys,
                                              const std::vector<TableScope>& scopes,
                                              const std::vector<plan::BoundColumnRef>& group_keys,
                                              bool aggregate_query,
                                              bool distinct_query,
                                              const std::vector<std::string>& output_names,
                                              const std::vector<plan::Projection>& projections) {
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
            (void)bind_aggregate_call(*aggregate, scopes);
            throw BindError(aggregate->position, "ORDER BY output '" + name + "' must appear in SELECT list");
        }

        const auto& column = std::get<ColumnRef>(key.expression);
        auto sort_key = plan::SortKey{bind_column_ref(column, scopes), key.direction};
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

} // namespace

plan::LogicalPlan bind_select(const SelectQuery& query, const catalog::Catalog& catalog) {
    const auto scopes = build_scopes(query, catalog);
    auto group_keys = bind_group_by_keys(query.group_by, scopes);
    const auto aggregate_query = !group_keys.empty() || query_has_aggregate(query) || query.having.has_value();

    std::set<std::string> output_names;
    std::vector<std::string> output_name_order;
    std::vector<plan::Projection> projections;
    std::vector<plan::AggregateExpression> aggregate_expressions;
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
            const auto aggregate_ref = ensure_aggregate_expression(*aggregate, scopes, aggregate_expressions);
            projections.push_back(plan::Projection{
                name,
                plan::BoundScalarExpr{aggregate_ref, aggregate_ref.type},
                aggregate_ref.type,
            });
            continue;
        }

        const auto& scalar = std::get<ScalarExpr>(item.expression);
        auto expression = bind_expression(scalar, scopes);
        if (aggregate_query) {
            if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value);
                column != nullptr && !contains_group_key(group_keys, *column)) {
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
        having_predicates = bind_having_predicates(query.having->conjuncts, scopes, group_keys, aggregate_expressions);
        if (group_keys.empty() && aggregate_expressions.empty()) {
            aggregate_expressions.push_back(hidden_count_star_aggregate());
        }
    }

    auto sort_keys =
        bind_order_by_keys(query.order_by, scopes, group_keys, aggregate_query, query.distinct, output_name_order, projections);

    auto plan = plan::LogicalPlan::scan(query.table, binding_name(query));
    std::vector<TableScope> visible_scopes;
    visible_scopes.push_back(scopes.front());
    for (std::size_t i = 0; i < query.joins.size(); ++i) {
        visible_scopes.push_back(scopes.at(i + 1));
        auto predicates = bind_predicates(query.joins[i].predicates, visible_scopes);
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
        plan = plan::LogicalPlan::filter(bind_predicates(query.predicate->conjuncts, scopes), std::move(plan));
    }
    if (aggregate_query) {
        plan = plan::LogicalPlan::aggregate(std::move(group_keys), std::move(aggregate_expressions), std::move(plan));
    }
    if (!having_predicates.empty()) {
        plan = plan::LogicalPlan::filter(std::move(having_predicates), std::move(plan));
    }
    auto bound = plan::LogicalPlan::project(std::move(projections), std::move(plan));
    if (query.distinct) {
        bound = plan::LogicalPlan::distinct(std::move(bound));
    }
    mark_arbitrary_order(bound);
    if (!sort_keys.empty()) {
        bound = plan::LogicalPlan::sort(std::move(sort_keys), std::move(bound));
    }
    if (query.limit.has_value()) {
        bound = plan::LogicalPlan::limit(*query.limit, std::move(bound));
        bound.order_permission = bound.input->order_permission;
    }
    if (query.explain) {
        return plan::LogicalPlan::explain(std::move(bound));
    }
    return bound;
}

} // namespace sql
