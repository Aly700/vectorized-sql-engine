#pragma once

#include "plan/logical_plan.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace optimizer {

using GroupId = std::uint64_t;

struct GroupExpression {
    plan::LogicalPlan expression;
    double local_cost{0.0};
};

class Memo {
public:
    GroupId insert_group(plan::LogicalPlan expression);
    void insert_equivalent(GroupId group, plan::LogicalPlan expression, double local_cost);
    [[nodiscard]] const std::vector<GroupExpression>& group(GroupId id) const;
    [[nodiscard]] std::size_t group_count() const { return groups_.size(); }

private:
    GroupId next_{1};
    std::map<GroupId, std::vector<GroupExpression>> groups_;
};

} // namespace optimizer
