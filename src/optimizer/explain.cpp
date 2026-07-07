#include "optimizer/explain.hpp"

#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace optimizer {
namespace {

void append_indented_lines(std::vector<std::string>& lines, const std::string& text, const std::string& indent) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line = end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
        lines.push_back(indent + line);
        if (end == std::string::npos) {
            return;
        }
        start = end + 1;
    }
}

std::string format_double(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string estimate_annotation(const plan::LogicalPlan& logical, const catalog::Catalog& catalog) {
    const auto estimate = estimate_cost(logical, catalog);
    return "rows=" + format_double(estimate.rows) + " cost=" + format_double(estimate.cost);
}

storage::ColumnarBatch lines_to_batch(const std::vector<std::string>& lines) {
    storage::StringColumn plan;
    plan.reserve(lines.size());
    for (const auto& line : lines) {
        plan.append(line);
    }

    storage::ColumnarBatch batch;
    batch.add_column("plan", std::move(plan));
    return batch;
}

} // namespace

storage::ColumnarBatch explain(const plan::LogicalPlan& select_plan, const catalog::Catalog& catalog) {
    if (select_plan.kind == plan::LogicalKind::Explain) {
        throw std::invalid_argument("EXPLAIN renderer expects the wrapped SELECT logical plan");
    }

    std::vector<std::string> lines;
    lines.push_back("EXPLAIN");
    lines.push_back("bound logical plan:");
    append_indented_lines(lines, plan::to_string(select_plan), "  ");

    Memo memo;
    const auto root = memo.insert(select_plan);
    const auto explored = explore_memo_to_fixpoint(memo, default_memo_rules());

    lines.push_back("memo exploration:");
    lines.push_back("  groups: " + std::to_string(memo.group_count()));
    lines.push_back("  iterations: " + std::to_string(explored.iterations));
    lines.push_back(std::string("  reached_fixpoint: ") + (explored.reached_fixpoint ? "yes" : "no"));
    if (explored.fired_rules.empty()) {
        lines.push_back("  fired rules: <none>");
    } else {
        lines.push_back("  fired rules:");
        for (std::size_t i = 0; i < explored.fired_rules.size(); ++i) {
            lines.push_back("    " + std::to_string(i) + ": " + explored.fired_rules[i]);
        }
    }

    const auto best = memo.extract_best(root, catalog);
    const auto chosen = plan::to_string_annotated(best, [&](const auto& node) {
        return estimate_annotation(node, catalog);
    });
    lines.push_back("chosen plan:");
    append_indented_lines(lines, chosen, "  ");
    lines.push_back("total cost: " + format_double(estimate_cost(best, catalog).cost));

    return lines_to_batch(lines);
}

} // namespace optimizer
