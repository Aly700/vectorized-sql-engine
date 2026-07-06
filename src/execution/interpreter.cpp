#include "execution/interpreter.hpp"

#include <stdexcept>
#include <utility>

namespace execution {

void Catalog::add_table(std::string name, storage::ColumnarBatch batch) {
    auto [_, inserted] = tables_.emplace(std::move(name), std::move(batch));
    if (!inserted) {
        throw std::invalid_argument("duplicate table");
    }
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
        return input.filter(storage::equals_i64(input, plan.predicate.column, plan.predicate.equals_i64));
    }
    case plan::LogicalKind::Project: {
        auto input = execute_interpreted(*plan.input, catalog);
        storage::ColumnarBatch out;
        for (const auto& name : plan.columns) {
            out.add_column(name, input.column(name));
        }
        return out;
    }
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace execution
