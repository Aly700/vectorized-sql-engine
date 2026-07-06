#include "execution/vectorized.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace execution {
namespace {

using SelectionVector = std::vector<std::size_t>;
using SelectionVectorPtr = std::shared_ptr<const SelectionVector>;

struct BatchView {
    std::shared_ptr<const storage::ColumnarBatch> owned_batch;
    const storage::ColumnarBatch* batch{nullptr};
    SelectionVectorPtr selection;
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

BatchView execute_scan(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    const auto& batch = catalog.table(plan.table);
    BatchView view;
    view.batch = &batch;
    view.selection = identity_selection(batch.row_count());
    validate_view(view);
    return view;
}

BatchView execute_filter(const plan::PhysicalPlan& plan, const Catalog& catalog);
BatchView execute_project(const plan::PhysicalPlan& plan, const Catalog& catalog);

BatchView execute_to_view(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    switch (plan.kind) {
    case plan::PhysicalKind::Scan:
        return execute_scan(plan, catalog);
    case plan::PhysicalKind::Filter:
        return execute_filter(plan, catalog);
    case plan::PhysicalKind::Project:
        return execute_project(plan, catalog);
    }
    throw std::logic_error("unreachable physical plan kind");
}

const plan::PhysicalPlan& require_input(const plan::PhysicalPlan& plan) {
    if (!plan.input) {
        throw std::invalid_argument("physical plan node is missing its input");
    }
    return *plan.input;
}

BatchView execute_filter(const plan::PhysicalPlan& plan, const Catalog& catalog) {
    auto input = execute_to_view(require_input(plan), catalog);
    validate_view(input);

    SelectionVector rows;
    rows.reserve(input.selection->size());
    for (auto row : *input.selection) {
        bool keep = true;
        for (const auto& predicate : plan.predicates) {
            if (!evaluate_comparison(predicate, *input.batch, row)) {
                keep = false;
                break;
            }
        }
        if (keep) {
            rows.push_back(row);
        }
    }

    input.selection = make_selection(std::move(rows));
    validate_view(input);
    return input;
}

storage::ColumnarBatch materialize_projection(const plan::PhysicalPlan& plan, const BatchView& input) {
    validate_view(input);

    storage::ColumnarBatch out;
    for (const auto& projection : plan.projections) {
        storage::Int64Column column;
        for (auto row : *input.selection) {
            column.append(evaluate_scalar(projection.expression, *input.batch, row));
        }
        out.add_column(projection.output_name, std::move(column));
    }
    return out;
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

storage::ColumnarBatch materialize_view(const BatchView& view) {
    validate_view(view);

    storage::ColumnarBatch out;
    for (const auto& name : view.batch->column_names()) {
        storage::Int64Column column;
        const auto& input_column = view.batch->column(name);
        for (auto row : *view.selection) {
            column.append(input_column.at(row));
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
