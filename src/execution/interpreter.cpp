#include "execution/interpreter.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace execution {
namespace {

std::int64_t evaluate_scalar(const sql::ScalarExpr& expression, const storage::ColumnarBatch& batch, std::size_t row) {
    if (const auto* column = std::get_if<sql::ColumnRef>(&expression)) {
        return batch.column(column->name).at(row);
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

bool evaluate_comparison(const sql::ComparisonExpr& comparison, const storage::ColumnarBatch& batch, std::size_t row) {
    return compare_values(evaluate_scalar(comparison.left, batch, row),
                          comparison.op,
                          evaluate_scalar(comparison.right, batch, row));
}

storage::RowMask evaluate_filter(const std::vector<sql::ComparisonExpr>& predicates, const storage::ColumnarBatch& batch) {
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
        return catalog.table(plan.table);
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
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace execution
