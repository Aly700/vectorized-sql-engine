#include "sql/binder.hpp"

#include "sql/errors.hpp"

#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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

plan::BoundScalarExpr bind_expression(const ScalarExpr& expression, const std::vector<TableScope>& scopes) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        const auto& scope =
            column->qualifier.has_value() ? find_qualified_scope(*column, scopes) : find_unqualified_scope(*column, scopes);
        return plan::BoundColumnRef{scope.binding_name, column->name, column->position};
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
    const auto& column = key.column;
    const auto& scope =
        column.qualifier.has_value() ? find_qualified_scope(column, scopes) : find_unqualified_scope(column, scopes);
    return plan::SortKey{plan::BoundColumnRef{scope.binding_name, column.name, column.position}, key.direction};
}

std::vector<plan::SortKey> bind_order_by_keys(const std::vector<OrderByKey>& keys,
                                              const std::vector<TableScope>& scopes) {
    std::vector<plan::SortKey> bound;
    bound.reserve(keys.size());
    for (const auto& key : keys) {
        bound.push_back(bind_order_by_key(key, scopes));
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

    std::set<std::string> output_names;
    std::vector<plan::Projection> projections;
    projections.reserve(query.projection.size());
    for (const auto& item : query.projection) {
        auto expression = bind_expression(item.expression, scopes);

        auto name = output_name(item.expression);
        const auto [_, inserted] = output_names.insert(name);
        if (!inserted) {
            throw BindError(item.position, "duplicate output name '" + name + "'");
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
    auto bound = plan::LogicalPlan::project(std::move(projections), std::move(plan));
    mark_arbitrary_order(bound);
    if (!query.order_by.empty()) {
        // This SQL slice deliberately binds ORDER BY against FROM scopes, not
        // projection output aliases. That keeps ordered keys as stable bound
        // column identities even when the SELECT list renames or omits them.
        return plan::LogicalPlan::sort(bind_order_by_keys(query.order_by, scopes), std::move(bound));
    }
    return bound;
}

} // namespace sql
