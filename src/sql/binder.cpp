#include "sql/binder.hpp"

#include "sql/errors.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace sql {
namespace {

const storage::ColumnarBatch& require_table(const SelectQuery& query, const execution::Catalog& catalog) {
    try {
        return catalog.table(query.table);
    } catch (const std::out_of_range&) {
        throw BindError(query.table_position, "unknown table '" + query.table + "'");
    }
}

void require_column(const ColumnRef& column, const SelectQuery& query, const storage::ColumnarBatch& table) {
    if (!table.has_column(column.name)) {
        throw BindError(column.position, "unknown column '" + column.name + "' in table '" + query.table + "'");
    }
}

void bind_expression(const ScalarExpr& expression, const SelectQuery& query, const storage::ColumnarBatch& table) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        require_column(*column, query, table);
    }
}

void bind_comparison(const ComparisonExpr& comparison, const SelectQuery& query, const storage::ColumnarBatch& table) {
    bind_expression(comparison.left, query, table);
    bind_expression(comparison.right, query, table);
}

} // namespace

plan::LogicalPlan bind_select(const SelectQuery& query, const execution::Catalog& catalog) {
    const auto& table = require_table(query, catalog);

    std::set<std::string> output_names;
    std::vector<plan::Projection> projections;
    projections.reserve(query.projection.size());
    for (const auto& item : query.projection) {
        bind_expression(item.expression, query, table);

        auto name = output_name(item.expression);
        const auto [_, inserted] = output_names.insert(name);
        if (!inserted) {
            throw BindError(item.position, "duplicate output name '" + name + "'");
        }
        projections.push_back(plan::Projection{std::move(name), item.expression});
    }

    auto plan = plan::LogicalPlan::scan(query.table);
    if (query.predicate.has_value()) {
        for (const auto& comparison : query.predicate->conjuncts) {
            bind_comparison(comparison, query, table);
        }
        plan = plan::LogicalPlan::filter(query.predicate->conjuncts, std::move(plan));
    }
    return plan::LogicalPlan::project(std::move(projections), std::move(plan));
}

} // namespace sql
