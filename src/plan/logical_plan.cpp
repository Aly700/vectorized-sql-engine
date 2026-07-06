#include "plan/logical_plan.hpp"

#include <sstream>
#include <stdexcept>
#include <variant>

namespace plan {
namespace {

const LogicalPlan& require_input(const LogicalPlan& logical) {
    if (!logical.input) {
        throw std::invalid_argument("logical plan node is missing its input");
    }
    return *logical.input;
}

std::string expression_to_string(const sql::ScalarExpr& expression) {
    if (const auto* column = std::get_if<sql::ColumnRef>(&expression)) {
        return "col(" + column->name + ")";
    }
    return "lit(" + std::to_string(std::get<sql::IntLiteral>(expression).value) + ")";
}

std::string comparison_op_to_string(sql::ComparisonOp op) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return "=";
    case sql::ComparisonOp::NotEqual:
        return "<>";
    case sql::ComparisonOp::Less:
        return "<";
    case sql::ComparisonOp::LessEqual:
        return "<=";
    case sql::ComparisonOp::Greater:
        return ">";
    case sql::ComparisonOp::GreaterEqual:
        return ">=";
    }
    throw std::logic_error("unreachable comparison operator");
}

std::string comparison_to_string(const sql::ComparisonExpr& comparison) {
    return expression_to_string(comparison.left) + " " + comparison_op_to_string(comparison.op) + " " +
           expression_to_string(comparison.right);
}

void append_indent(std::ostringstream& out, std::size_t depth) {
    for (std::size_t i = 0; i < depth; ++i) {
        out << "  ";
    }
}

void append_plan(std::ostringstream& out, const LogicalPlan& logical, std::size_t depth) {
    append_indent(out, depth);
    switch (logical.kind) {
    case LogicalKind::Scan:
        out << "Scan[" << logical.table << "]";
        return;
    case LogicalKind::Filter:
        out << "Filter[";
        for (std::size_t i = 0; i < logical.predicates.size(); ++i) {
            if (i != 0) {
                out << " AND ";
            }
            out << comparison_to_string(logical.predicates[i]);
        }
        out << "]\n";
        append_plan(out, require_input(logical), depth + 1);
        return;
    case LogicalKind::Project:
        out << "Project[";
        for (std::size_t i = 0; i < logical.projections.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << logical.projections[i].output_name << "=" << expression_to_string(logical.projections[i].expression);
        }
        out << "]\n";
        append_plan(out, require_input(logical), depth + 1);
        return;
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace

std::string to_string(const LogicalPlan& logical) {
    std::ostringstream out;
    append_plan(out, logical, 0);
    return out.str();
}

} // namespace plan
