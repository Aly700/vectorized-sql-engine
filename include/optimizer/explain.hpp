#pragma once

#include "catalog/catalog.hpp"
#include "plan/logical_plan.hpp"
#include "storage/columnar_batch.hpp"

namespace optimizer {

[[nodiscard]] storage::ColumnarBatch explain(const plan::LogicalPlan& select_plan, const catalog::Catalog& catalog);

} // namespace optimizer
