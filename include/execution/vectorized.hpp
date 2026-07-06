#pragma once

#include "execution/interpreter.hpp"
#include "plan/logical_plan.hpp"
#include "plan/physical_plan.hpp"
#include "storage/columnar_batch.hpp"

namespace execution {

storage::ColumnarBatch execute_vectorized(const plan::LogicalPlan& plan, const Catalog& catalog);
storage::ColumnarBatch execute_vectorized(const plan::PhysicalPlan& plan, const Catalog& catalog);

} // namespace execution
