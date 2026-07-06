#pragma once

#include "catalog/catalog.hpp"
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
    plan::OrderPermission order_permission{plan::OrderPermission::Deterministic};
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

struct AlternativeExtractionOptions {
    std::size_t max_expressions_per_group{64};
    std::size_t max_plans{256};
};

struct AlternativeExtractionResult {
    std::vector<plan::LogicalPlan> plans;
    std::size_t max_group_expression_count{0};
    bool hit_expression_bound{false};
    bool hit_plan_bound{false};
};

struct CostEstimate {
    double rows{0.0};
    double cost{0.0};
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
    [[nodiscard]] GroupId representative(GroupId id) const;

    [[nodiscard]] plan::LogicalPlan extract(GroupId id) const;
    [[nodiscard]] plan::LogicalPlan extract_best(GroupId id, const catalog::Catalog& catalog) const;
    [[nodiscard]] AlternativeExtractionResult extract_alternatives(
        GroupId id,
        AlternativeExtractionOptions options = {}) const;
    [[nodiscard]] std::string dump() const;

    void assert_invariants() const;

private:
    [[nodiscard]] bool has_group(GroupId id) const;
    [[nodiscard]] std::size_t index_for(GroupId id) const;
    [[nodiscard]] bool is_representative(GroupId id) const;
    [[nodiscard]] GroupId find_existing(const MemoExpression& expression) const;
    [[nodiscard]] bool reaches(GroupId from, GroupId target) const;
    [[nodiscard]] bool reaches(GroupId from, GroupId target, std::vector<GroupId>& stack) const;
    [[nodiscard]] plan::LogicalPlan extract(GroupId id, std::vector<GroupId>& stack) const;
    [[nodiscard]] plan::LogicalPlan extract_expression(const MemoExpression& expression,
                                                       std::vector<GroupId>& stack) const;

    void validate_expression(const MemoExpression& expression) const;
    void validate_no_cycle_from(GroupId group, const MemoExpression& expression) const;
    void index_expression(const MemoExpression& expression, GroupId owner);
    void rebuild_expression_index();
    void canonicalize_child_references();
    void canonicalize_expression_references(MemoExpression& expression) const;
    void deduplicate_group_expressions(GroupId group);
    void mark_non_representative_groups();
    void merge_groups(GroupId first, GroupId second);
    bool merge_representatives_once(GroupId first, GroupId second);
    void normalize_after_merge();
    [[nodiscard]] std::vector<plan::LogicalPlan> extract_alternatives_for_group(
        GroupId id,
        const AlternativeExtractionOptions& options,
        AlternativeExtractionResult& result,
        std::vector<GroupId>& stack) const;
    [[nodiscard]] std::vector<plan::LogicalPlan> extract_alternatives_for_expression(
        const MemoExpression& expression,
        const AlternativeExtractionOptions& options,
        AlternativeExtractionResult& result,
        std::vector<GroupId>& stack) const;

    std::vector<MemoGroup> groups_;
    std::vector<GroupId> representatives_{0};
    std::unordered_map<std::size_t, std::vector<std::pair<MemoExpression, GroupId>>> expression_index_;
};

[[nodiscard]] CostEstimate estimate_cost(const plan::LogicalPlan& logical, const catalog::Catalog& catalog);

} // namespace optimizer
