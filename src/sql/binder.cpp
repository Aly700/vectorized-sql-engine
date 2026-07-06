#include "sql/binder.hpp"

#include "sql/errors.hpp"

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

plan::BoundColumnRef bind_column_ref(const ColumnRef& column, const std::vector<TableScope>& scopes) {
    const auto& scope =
        column.qualifier.has_value() ? find_qualified_scope(column, scopes) : find_unqualified_scope(column, scopes);
    return plan::BoundColumnRef{scope.binding_name, column.name, column.position};
}

plan::BoundScalarExpr bind_expression(const ScalarExpr& expression, const std::vector<TableScope>& scopes) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return bind_column_ref(*column, scopes);
    }
    return std::get<IntLiteral>(expression);
}

plan::BoundComparisonExpr bind_comparison(const ComparisonExpr& comparison, const std::vector<TableScope>& scopes) {
    return plan::BoundComparisonExpr{
        bind_expression(comparison.left, scopes),
        comparison.op,
        bind_expression(comparison.right, scopes),
        comparison.operator_position,
    };
}

std::vector<plan::BoundComparisonExpr> bind_comparisons(const std::vector<ComparisonExpr>& comparisons,
                                                        const std::vector<TableScope>& scopes) {
    std::vector<plan::BoundComparisonExpr> bound;
    bound.reserve(comparisons.size());
    for (const auto& comparison : comparisons) {
        bound.push_back(bind_comparison(comparison, scopes));
    }
    return bound;
}

plan::SortKey bind_order_by_key(const OrderByKey& key, const std::vector<TableScope>& scopes) {
    return plan::SortKey{bind_column_ref(key.column, scopes), key.direction};
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

    return plan::AggregateExpression{output_name(aggregate), aggregate.function, std::move(argument), aggregate.position};
}

std::vector<plan::SortKey> bind_order_by_keys(const std::vector<OrderByKey>& keys,
                                              const std::vector<TableScope>& scopes,
                                              const std::vector<plan::BoundColumnRef>& group_keys,
                                              bool aggregate_query) {
    std::vector<plan::SortKey> bound;
    bound.reserve(keys.size());
    for (const auto& key : keys) {
        auto sort_key = bind_order_by_key(key, scopes);
        if (aggregate_query && !contains_group_key(group_keys, sort_key.column)) {
            throw BindError(key.column.position,
                            "ORDER BY column '" + output_name(key.column) +
                                "' must be a GROUP BY column in aggregate queries");
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

} // namespace

plan::LogicalPlan bind_select(const SelectQuery& query, const catalog::Catalog& catalog) {
    const auto scopes = build_scopes(query, catalog);
    auto group_keys = bind_group_by_keys(query.group_by, scopes);
    const auto aggregate_query = !group_keys.empty() || query_has_aggregate(query);

    std::set<std::string> output_names;
    std::vector<plan::Projection> projections;
    std::vector<plan::AggregateExpression> aggregate_expressions;
    projections.reserve(query.projection.size());
    for (const auto& item : query.projection) {
        auto name = output_name(item.expression);
        const auto [_, inserted] = output_names.insert(name);
        if (!inserted) {
            throw BindError(item.position, "duplicate output name '" + name + "'");
        }

        if (const auto* aggregate = std::get_if<AggregateCall>(&item.expression)) {
            aggregate_expressions.push_back(bind_aggregate_call(*aggregate, scopes));
            projections.push_back(plan::Projection{
                name,
                plan::BoundColumnRef{"", name, item.position},
            });
            continue;
        }

        const auto& scalar = std::get<ScalarExpr>(item.expression);
        auto expression = bind_expression(scalar, scopes);
        if (aggregate_query) {
            if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression);
                column != nullptr && !contains_group_key(group_keys, *column)) {
                const auto& parsed_column = std::get<ColumnRef>(scalar);
                throw BindError(parsed_column.position,
                                "non-grouped column '" + output_name(parsed_column) +
                                    "' must appear in GROUP BY or be aggregated");
            }
        }
        projections.push_back(plan::Projection{std::move(name), std::move(expression)});
    }

    auto plan = plan::LogicalPlan::scan(query.table, binding_name(query));
    std::vector<TableScope> visible_scopes;
    visible_scopes.push_back(scopes.front());
    for (std::size_t i = 0; i < query.joins.size(); ++i) {
        visible_scopes.push_back(scopes.at(i + 1));
        plan = plan::LogicalPlan::join(bind_comparisons(query.joins[i].predicates, visible_scopes),
                                       std::move(plan),
                                       plan::LogicalPlan::scan(query.joins[i].table, binding_name(query.joins[i])));
    }

    if (query.predicate.has_value()) {
        plan = plan::LogicalPlan::filter(bind_comparisons(query.predicate->conjuncts, scopes), std::move(plan));
    }
    if (aggregate_query) {
        plan = plan::LogicalPlan::aggregate(std::move(group_keys), std::move(aggregate_expressions), std::move(plan));
    }
    auto bound = plan::LogicalPlan::project(std::move(projections), std::move(plan));
    mark_arbitrary_order(bound);
    if (!query.order_by.empty()) {
        // Non-aggregate queries deliberately bind ORDER BY against FROM scopes,
        // not projection output aliases. Aggregate queries keep the same stable
        // FROM-scope identities but restrict ORDER BY to grouping columns.
        return plan::LogicalPlan::sort(bind_order_by_keys(query.order_by, scopes, bound.input->group_keys, aggregate_query),
                                       std::move(bound));
    }
    return bound;
}

} // namespace sql
