#pragma once

#include "execution/interpreter.hpp"
#include "plan/logical_plan.hpp"
#include "sql/ast.hpp"

namespace sql {

plan::LogicalPlan bind_select(const SelectQuery& query, const execution::Catalog& catalog);

} // namespace sql
