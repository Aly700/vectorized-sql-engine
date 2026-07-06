#include "optimizer/memo.hpp"

#include <stdexcept>
#include <utility>

namespace optimizer {

GroupId Memo::insert_group(plan::LogicalPlan expression) {
    auto id = next_++;
    groups_[id].push_back(GroupExpression{std::move(expression), 0.0});
    return id;
}

void Memo::insert_equivalent(GroupId group, plan::LogicalPlan expression, double local_cost) {
    auto it = groups_.find(group);
    if (it == groups_.end()) {
        throw std::out_of_range("unknown memo group");
    }
    it->second.push_back(GroupExpression{std::move(expression), local_cost});
}

const std::vector<GroupExpression>& Memo::group(GroupId id) const {
    auto it = groups_.find(id);
    if (it == groups_.end()) {
        throw std::out_of_range("unknown memo group");
    }
    return it->second;
}

} // namespace optimizer
