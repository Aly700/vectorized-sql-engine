#pragma once

#include "plan/logical_plan.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace optimizer {

using GroupId = std::uint64_t;

enum class MemoExpressionKind { Scan, Join, Filter, Project, GroupRef };

struct MemoExpression {
    MemoExpressionKind kind{MemoExpressionKind::Scan};
    std::string table;
    std::vector<plan::Projection> projections;
    std::vector<plan::BoundComparisonExpr> predicates;
    std::vector<GroupId> children;
};

struct GroupExpression {
    MemoExpression expression;
};

struct MemoGroup {
    GroupId id{0};
    std::vector<GroupExpression> expressions;
};

class Memo {
public:
    [[nodiscard]] GroupId insert(const plan::LogicalPlan& logical);
    [[nodiscard]] GroupId insert_expression(MemoExpression expression);
    [[nodiscard]] bool insert_equivalent(GroupId group, MemoExpression expression);
    [[nodiscard]] bool insert_group_ref_equivalent(GroupId group, GroupId equivalent_group);

    [[nodiscard]] const MemoGroup& group(GroupId id) const;
    [[nodiscard]] const std::vector<MemoGroup>& groups() const { return groups_; }
    [[nodiscard]] std::size_t group_count() const { return groups_.size(); }

    [[nodiscard]] plan::LogicalPlan extract(GroupId id) const;
    [[nodiscard]] std::string dump() const;

    void assert_invariants() const;

private:
    [[nodiscard]] bool has_group(GroupId id) const;
    [[nodiscard]] std::size_t index_for(GroupId id) const;
    [[nodiscard]] GroupId find_existing(const MemoExpression& expression) const;
    [[nodiscard]] bool reaches(GroupId from, GroupId target) const;
    [[nodiscard]] bool reaches(GroupId from, GroupId target, std::vector<GroupId>& stack) const;
    [[nodiscard]] plan::LogicalPlan extract(GroupId id, std::vector<GroupId>& stack) const;

    void validate_expression(const MemoExpression& expression) const;
    void validate_no_cycle_from(GroupId group, const MemoExpression& expression) const;
    void index_expression(const MemoExpression& expression, GroupId owner);

    std::vector<MemoGroup> groups_;
    std::unordered_map<std::size_t, std::vector<std::pair<MemoExpression, GroupId>>> expression_index_;
};

} // namespace optimizer
