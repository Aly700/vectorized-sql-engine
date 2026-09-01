#include "bench_common.hpp"
#include "optimizer/explain.hpp"
#include "plan/logical_plan.hpp"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>

namespace {

using namespace bench;

struct Working {
    std::string id;
    std::string sql;
    std::vector<std::pair<std::string, std::size_t>> tables;
};

std::vector<Working> make_workings() {
    return {
        {"coupling",
         "SELECT l.payload AS left_payload, r.payload AS right_payload "
         "FROM join_left AS l JOIN join_right AS r ON l.k1 = r.k1 AND l.k2 = r.k2",
         {{"join_left", kJoinLeftRows}, {"join_right", kJoinRightRows}}},
        {"hump-and-bowl",
         "SELECT l.group_id AS group_id, COUNT(*) AS n, MAX(r.measure) AS top "
         "FROM e2e_left AS l JOIN e2e_right AS r ON l.k = r.k "
         "WHERE l.filter_key < 80 "
         "GROUP BY l.group_id ORDER BY n DESC, group_id ASC LIMIT 20",
         {{"e2e_left", kEndToEndLeftRows}, {"e2e_right", kEndToEndRightRows}}},
        {"three-trains",
         "SELECT l.group_id AS group_id, COUNT(*) AS n, MAX(r.measure) AS top "
         "FROM e2e_left AS l JOIN e2e_right AS r ON l.k = r.k "
         "JOIN e2e_class AS c ON l.group_id = c.group_id "
         "WHERE c.tier = 1 "
         "GROUP BY l.group_id ORDER BY n DESC, group_id ASC LIMIT 20",
         {{"e2e_left", kEndToEndLeftRows}, {"e2e_right", kEndToEndRightRows}, {"e2e_class", kClassRows}}},
        {"special",
         "SELECT l.payload AS payload FROM join_left AS l "
         "WHERE l.k1 IN (SELECT r.k1 FROM join_right AS r WHERE r.payload = 10000)",
         {{"join_left", kJoinLeftRows}, {"join_right", kJoinRightRows}}},
    };
}

struct HumpWorkload {
    std::string id;
    std::string predicate;
    std::string sql;
};

std::vector<HumpWorkload> make_hump_workloads() {
    return {
        {"scan_filter_1pct", "bucket = 7", "SELECT a FROM fact WHERE bucket = 7"},
        {"scan_filter_10pct", "decile = 3", "SELECT a FROM fact WHERE decile = 3"},
        {"scan_filter_50pct", "half = 1", "SELECT a FROM fact WHERE half = 1"},
    };
}

// ---- tiny JSON writer -------------------------------------------------------

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

std::string fixed(double value, int decimals) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}

std::string quoted(const std::string& value) { return "\"" + json_escape(value) + "\""; }

ResultSignature bag_signature_for(const storage::ColumnarBatch& batch) {
    constexpr char kColumnSeparator = '\x1f';
    std::vector<std::string> rows;
    rows.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        std::string canonical;
        for (std::size_t column_index = 0; column_index < batch.column_names().size(); ++column_index) {
            if (column_index != 0) canonical.push_back(kColumnSeparator);
            const auto& name = batch.column_names()[column_index];
            if (batch.column_type(name) == catalog::ColumnType::Int64) {
                const auto& column = batch.column(name);
                if (column.is_null(row)) {
                    canonical += "N";
                } else {
                    canonical += "I" + std::to_string(column.at(row));
                }
            } else {
                const auto& column = batch.string_column(name);
                if (column.is_null(row)) {
                    canonical += "N";
                } else {
                    const auto& value = column.at(row);
                    if (value.find(kColumnSeparator) != std::string::npos) {
                        throw std::logic_error("bag signature separator appeared in string data");
                    }
                    canonical += "S" + value;
                }
            }
        }
        rows.push_back(std::move(canonical));
    }
    std::sort(rows.begin(), rows.end());

    std::uint64_t hash = 1469598103934665603ULL;
    mix_u64(hash, batch.row_count());
    mix_u64(hash, batch.column_names().size());
    for (const auto& name : batch.column_names()) mix_string(hash, name);
    for (const auto& row : rows) mix_string(hash, row);
    return ResultSignature{batch.row_count(), batch.column_names().size(), hash};
}

// ---- plan tree --------------------------------------------------------------

std::string node_line(const plan::LogicalPlan& node) {
    const auto text = plan::to_string(node);
    const auto end = text.find('\n');
    auto line = end == std::string::npos ? text : text.substr(0, end);
    const auto first = line.find_first_not_of(' ');
    return first == std::string::npos ? line : line.substr(first);
}

std::string node_op(const std::string& line) {
    const auto bracket = line.find('[');
    return bracket == std::string::npos ? line : line.substr(0, bracket);
}

std::string node_detail(const std::string& line) {
    const auto bracket = line.find('[');
    if (bracket == std::string::npos || line.back() != ']') return "";
    return line.substr(bracket + 1, line.size() - bracket - 2);
}

std::string tree_json(const plan::LogicalPlan& node, const execution::Catalog& catalog, int indent) {
    const auto estimate = optimizer::estimate_cost(node, catalog);
    const auto line = node_line(node);
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::string out = "{\n" + pad + "  \"op\": " + quoted(node_op(line)) +
                      ",\n" + pad + "  \"detail\": " + quoted(node_detail(line)) +
                      ",\n" + pad + "  \"rows\": " + fixed(estimate.rows, 2) +
                      ",\n" + pad + "  \"cost\": " + fixed(estimate.cost, 2) +
                      ",\n" + pad + "  \"children\": [";
    std::vector<const plan::LogicalPlan*> children;
    if (node.input) children.push_back(node.input.get());
    if (node.left) children.push_back(node.left.get());
    if (node.right) children.push_back(node.right.get());
    for (std::size_t i = 0; i < children.size(); ++i) {
        out += (i == 0 ? "\n" : ",\n") + pad + "    " + tree_json(*children[i], catalog, indent + 4);
    }
    out += children.empty() ? "]" : "\n" + pad + "  ]";
    out += "\n" + pad + "}";
    return out;
}

// ---- measurement ------------------------------------------------------------

struct Alternative {
    std::size_t index{0};
    plan::LogicalPlan plan;
    std::string text;
    double total_cost{0.0};
    ResultSignature ordered_signature;
    ResultSignature bag_signature;
    Timings vectorized;
};

struct WorkingReport {
    Working working;
    std::vector<std::string> explain_lines;
    optimizer::MemoExploreResult explored;
    std::size_t groups{0};
    std::vector<Alternative> alternatives;
    std::size_t chosen{0};
    ResultSignature signature;
    Timings interpreted;
    bool hit_expression_bound{false};
    bool hit_plan_bound{false};
};

std::vector<std::string> explain_lines(const plan::LogicalPlan& logical,
                                       const execution::Catalog& catalog) {
    const auto batch = optimizer::explain(logical, catalog);
    const auto& column = batch.string_column("plan");
    std::vector<std::string> lines;
    for (std::size_t row = 0; row < batch.row_count(); ++row) lines.push_back(column.at(row));
    return lines;
}

WorkingReport run_working(const Working& working, const execution::Catalog& catalog) {
    WorkingReport report;
    report.working = working;
    const auto logical = bind_query(catalog, working.sql);
    report.explain_lines = explain_lines(logical, catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    report.explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    if (!report.explored.reached_fixpoint) {
        throw std::runtime_error(working.id + ": memo exploration did not reach fixpoint");
    }
    report.groups = memo.group_count();

    const auto extracted = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 1024});
    report.hit_expression_bound = extracted.hit_expression_bound;
    report.hit_plan_bound = extracted.hit_plan_bound;
    if (extracted.hit_expression_bound || extracted.hit_plan_bound) {
        throw std::runtime_error(working.id + ": alternative extraction hit a cap");
    }

    const auto best = memo.extract_best(root, catalog);
    const auto best_text = plan::to_string(best);
    bool found = false;
    for (std::size_t i = 0; i < extracted.plans.size(); ++i) {
        Alternative alt;
        alt.index = i;
        alt.plan = extracted.plans[i];
        alt.text = plan::to_string(alt.plan);
        alt.total_cost = optimizer::estimate_cost(alt.plan, catalog).cost;
        const auto result = execution::execute_vectorized(alt.plan, catalog);
        alt.ordered_signature = signature_for(result);
        alt.bag_signature = bag_signature_for(result);
        alt.vectorized = measure(alt.plan, catalog, "vectorized", execution::execute_vectorized);
        if (!found && alt.text == best_text) {
            report.chosen = i;
            found = true;
        }
        report.alternatives.push_back(std::move(alt));
    }
    if (!found) throw std::runtime_error(working.id + ": chosen plan is not among the extracted alternatives");

    std::size_t min_index = 0;
    for (std::size_t i = 1; i < report.alternatives.size(); ++i) {
        if (report.alternatives[i].total_cost < report.alternatives[min_index].total_cost) min_index = i;
    }
    if (min_index != report.chosen) {
        throw std::runtime_error(working.id + ": chosen plan is not the lowest-cost lowest-index alternative");
    }
    report.signature = report.alternatives[report.chosen].ordered_signature;
    for (const auto& alt : report.alternatives) {
        if (!(alt.bag_signature == report.alternatives[report.chosen].bag_signature)) {
            const auto& chosen = report.alternatives[report.chosen];
            std::cerr << working.id << ": chosen alternative " << chosen.index
                      << " rows=" << chosen.bag_signature.row_count
                      << " bag checksum=" << hex_checksum(chosen.bag_signature.checksum) << "\n"
                      << chosen.text << "\n"
                      << working.id << ": mismatching alternative " << alt.index
                      << " rows=" << alt.bag_signature.row_count
                      << " bag checksum=" << hex_checksum(alt.bag_signature.checksum) << "\n"
                      << alt.text << "\n";
            throw std::runtime_error(working.id + ": alternative " + std::to_string(alt.index) + " changed the result");
        }
    }
    const auto interpreted = execution::execute_interpreted(report.alternatives[report.chosen].plan, catalog);
    if (!(signature_for(interpreted) == report.signature)) {
        throw std::runtime_error(working.id + ": interpreted oracle disagrees with the chosen plan");
    }
    report.interpreted =
        measure(report.alternatives[report.chosen].plan, catalog, "interpreted", execution::execute_interpreted);
    return report;
}

void assert_three_trains_has_three_costs(const WorkingReport& report) {
    std::set<std::string> join_costs;
    for (const auto& alt : report.alternatives) {
        if (alt.text.find("Join[") != std::string::npos) join_costs.insert(fixed(alt.total_cost, 2));
    }
    if (join_costs.size() < 3) {
        for (const auto& alt : report.alternatives) {
            std::cerr << "three-trains alternative " << alt.index << " cost=" << fixed(alt.total_cost, 2)
                      << "\n" << alt.text << "\n";
        }
        throw std::runtime_error("three-trains: fewer than three distinct join-order costs (" +
                                 std::to_string(join_costs.size()) + ")");
    }
}

struct HumpReport {
    HumpWorkload workload;
    ResultSignature signature;
    Timings interpreted;
    Timings vectorized;
};

HumpReport run_hump(const HumpWorkload& workload, const execution::Catalog& catalog) {
    const auto logical = bind_query(catalog, workload.sql);
    const auto interpreted = signature_for(execution::execute_interpreted(logical, catalog));
    const auto vectorized = signature_for(execution::execute_vectorized(logical, catalog));
    if (!(interpreted == vectorized)) throw std::runtime_error(workload.id + ": engines disagree");
    return HumpReport{workload, vectorized,
                      measure(logical, catalog, "interpreted", execution::execute_interpreted),
                      measure(logical, catalog, "vectorized", execution::execute_vectorized)};
}

// ---- writers ----------------------------------------------------------------

std::string timings_json(const Timings& t) {
    return "{ \"min\": " + fixed(t.min_ms, 3) + ", \"median\": " + fixed(t.median_ms, 3) + " }";
}

std::string string_array_json(const std::vector<std::string>& values, const std::string& pad) {
    if (values.empty()) return "[]";
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += (i == 0 ? "\n" : ",\n") + pad + "  " + quoted(values[i]);
    }
    return out + "\n" + pad + "]";
}

void write_json(const std::string& path,
                const std::vector<WorkingReport>& workings,
                const std::vector<HumpReport>& humps,
                const std::string& commit,
                const std::string& date,
                const std::string& machine,
                const execution::Catalog& catalog) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "{\n  \"engine\": {\n"
        << "    \"repo\": \"github.com/Aly700/vectorized-sql-engine\",\n"
        << "    \"commit\": " << quoted(commit) << ",\n"
        << "    \"exportedAt\": " << quoted(date) << ",\n"
        << "    \"machine\": " << quoted(machine) << ",\n"
        << "    \"toolchain\": " << quoted(std::string(__VERSION__)) << ",\n"
        << "    \"flags\": \"-O2 -DNDEBUG\",\n"
        << "    \"repetitions\": " << kRepetitions << "\n  },\n"
        << "  \"workings\": [";
    for (std::size_t w = 0; w < workings.size(); ++w) {
        const auto& r = workings[w];
        out << (w == 0 ? "\n" : ",\n") << "    {\n"
            << "      \"id\": " << quoted(r.working.id) << ",\n"
            << "      \"sql\": " << quoted(r.working.sql) << ",\n"
            << "      \"tables\": [";
        for (std::size_t t = 0; t < r.working.tables.size(); ++t) {
            out << (t == 0 ? " " : ", ") << "{ \"name\": " << quoted(r.working.tables[t].first)
                << ", \"rows\": " << r.working.tables[t].second << " }";
        }
        out << " ],\n"
            << "      \"explain\": " << string_array_json(r.explain_lines, "      ") << ",\n"
            << "      \"memo\": { \"groups\": " << r.groups
            << ", \"iterations\": " << r.explored.iterations
            << ", \"reachedFixpoint\": " << (r.explored.reached_fixpoint ? "true" : "false")
            << ", \"firedRules\": " << string_array_json(r.explored.fired_rules, "        ") << " },\n"
            << "      \"alternatives\": [";
        for (std::size_t a = 0; a < r.alternatives.size(); ++a) {
            const auto& alt = r.alternatives[a];
            out << (a == 0 ? "\n" : ",\n") << "        {\n"
                << "          \"index\": " << alt.index << ",\n"
                << "          \"plan\": " << quoted(alt.text) << ",\n"
                << "          \"tree\": " << tree_json(alt.plan, catalog, 10) << ",\n"
                << "          \"totalCost\": " << fixed(alt.total_cost, 2) << ",\n"
                << "          \"rowCount\": " << alt.bag_signature.row_count << ",\n"
                << "          \"bagChecksum\": " << quoted(hex_checksum(alt.bag_signature.checksum)) << ",\n"
                << "          \"vectorizedMs\": " << timings_json(alt.vectorized) << "\n"
                << "        }";
        }
        out << "\n      ],\n"
            << "      \"chosen\": " << r.chosen << ",\n"
            << "      \"checksum\": " << quoted(hex_checksum(r.signature.checksum)) << ",\n"
            << "      \"interpretedMs\": " << timings_json(r.interpreted) << ",\n"
            << "      \"hitExpressionBound\": " << (r.hit_expression_bound ? "true" : "false") << ",\n"
            << "      \"hitPlanBound\": " << (r.hit_plan_bound ? "true" : "false") << "\n"
            << "    }";
    }
    out << "\n  ],\n  \"hump\": [";
    for (std::size_t h = 0; h < humps.size(); ++h) {
        const auto& r = humps[h];
        out << (h == 0 ? "\n" : ",\n") << "    { \"id\": " << quoted(r.workload.id)
            << ", \"sql\": " << quoted(r.workload.sql)
            << ", \"predicate\": " << quoted(r.workload.predicate)
            << ", \"kept\": " << r.signature.row_count
            << ", \"of\": " << kFactRows
            << ", \"checksum\": " << quoted(hex_checksum(r.signature.checksum))
            << ", \"interpretedMs\": " << timings_json(r.interpreted)
            << ", \"vectorizedMs\": " << timings_json(r.vectorized) << " }";
    }
    out << "\n  ]\n}\n";
}

void write_markdown(const std::string& path,
                    const std::vector<WorkingReport>& workings,
                    const std::vector<HumpReport>& humps,
                    const std::string& commit,
                    const std::string& date) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "## Yard Routes (" << date << ")\n\n"
        << "`yard_export` (commit `" << commit << "`) enumerates every memo alternative for four\n"
        << "workings, prices each with `estimate_cost`, and times each through vectorized\n"
        << "execution with the same Release build, five-repetition min/median, deterministic\n"
        << "data, and checksum-before-timing methodology as above. The chosen row is the\n"
        << "`extract_best` winner; every alternative must reproduce its bag checksum. The\n"
        << "interpreted timing is for the chosen plan only.\n\n";
    for (const auto& r : workings) {
        out << "### " << r.working.id << "\n\n```sql\n" << r.working.sql << "\n```\n\n"
            << "memo: " << r.groups << " groups, " << r.explored.iterations << " iterations, fired rules: ";
        for (std::size_t i = 0; i < r.explored.fired_rules.size(); ++i) {
            out << (i == 0 ? "" : ", ") << r.explored.fired_rules[i];
        }
        if (r.explored.fired_rules.empty()) out << "none";
        out << "\n\n| alternative | plan | total cost | vectorized min ms | vectorized median ms |\n"
            << "|---:|---|---:|---:|---:|\n";
        for (const auto& alt : r.alternatives) {
            std::string one_line = alt.text;
            for (auto& ch : one_line) if (ch == '\n') ch = ' ';
            out << "| " << alt.index << (alt.index == r.chosen ? " (chosen)" : "") << " | `" << one_line
                << "` | " << fixed(alt.total_cost, 2) << " | " << fixed(alt.vectorized.min_ms, 3)
                << " | " << fixed(alt.vectorized.median_ms, 3) << " |\n";
        }
        out << "\ninterpreted (chosen plan): " << fixed(r.interpreted.min_ms, 3) << " / "
            << fixed(r.interpreted.median_ms, 3) << " ms; rows="
            << r.signature.row_count
            << " checksum=" << hex_checksum(r.signature.checksum)
            << " bag checksum=" << hex_checksum(r.alternatives[r.chosen].bag_signature.checksum) << "\n\n";
    }
    out << "### hump workloads\n\n"
        << "| workload | predicate | kept | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms |\n"
        << "|---|---|---:|---:|---:|---:|---:|\n";
    for (const auto& r : humps) {
        out << "| " << r.workload.id << " | `" << r.workload.predicate << "` | " << r.signature.row_count
            << " | " << fixed(r.interpreted.min_ms, 3) << " | " << fixed(r.interpreted.median_ms, 3)
            << " | " << fixed(r.vectorized.min_ms, 3) << " | " << fixed(r.vectorized.median_ms, 3) << " |\n";
    }
}

struct Args {
    std::string json, markdown, commit, date, machine;
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--json") args.json = value;
        else if (key == "--markdown") args.markdown = value;
        else if (key == "--commit") args.commit = value;
        else if (key == "--date") args.date = value;
        else if (key == "--machine") args.machine = value;
        else throw std::invalid_argument("unknown argument " + key);
    }
    if (args.json.empty() || args.markdown.empty() || args.commit.empty() || args.date.empty() ||
        args.machine.empty()) {
        throw std::invalid_argument(
            "usage: yard_export --json <path> --markdown <path> --commit <sha> --date <YYYY-MM-DD> --machine <text>");
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto catalog = make_catalog();
        std::vector<WorkingReport> reports;
        for (const auto& working : make_workings()) {
            std::cerr << "working " << working.id << "\n";
            reports.push_back(run_working(working, catalog));
            std::cerr << "  " << reports.back().alternatives.size() << " alternatives, chosen "
                      << reports.back().chosen << "\n";
        }
        assert_three_trains_has_three_costs(reports[2]);
        std::vector<HumpReport> humps;
        for (const auto& workload : make_hump_workloads()) humps.push_back(run_hump(workload, catalog));
        write_json(args.json, reports, humps, args.commit, args.date, args.machine, catalog);
        write_markdown(args.markdown, reports, humps, args.commit, args.date);
        if (benchmark_sink == 0x0123456789abcdefULL) std::cout << "sink=" << benchmark_sink << "\n";
    } catch (const std::invalid_argument& ex) {
        std::cerr << ex.what() << "\n";
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "yard_export failed: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
