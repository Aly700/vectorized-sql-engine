#pragma once

#include "catalog/catalog.hpp"
#include "plan/logical_plan.hpp"
#include "sql/ast.hpp"

namespace sql {

plan::LogicalPlan bind_select(const SelectQuery& query, const catalog::Catalog& catalog);

} // namespace sql
