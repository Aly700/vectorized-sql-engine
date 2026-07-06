#include "differential_verifier.hpp"
#include "sql/errors.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kDefaultSeedCount = 160;

struct GeneratedTable {
    std::string name;
    std::vector<std::string> columns;
    std::vector<std::vector<std::int64_t>> rows;
};

struct ColumnRef {
    std::string alias;
    std::string column;
};

struct RangeItem {
    std::string table;
    std::string alias;
    std::vector<std::string> columns;
};

struct SelectItem {
    std::string expression;
    std::string alias;
    std::optional<ColumnRef> source_column;
};

struct GeneratedCase {
    execution::Catalog catalog;
    std::string sql;
    std::string catalog_dump;
};

std::string sql_text(const ColumnRef& ref) {
    return ref.alias + "." + ref.column;
}

std::string comparison_op_text(sql::ComparisonOp op) {
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

storage::ColumnarBatch make_batch(const GeneratedTable& table) {
    storage::ColumnarBatch batch;
    for (std::size_t column_index = 0; column_index < table.columns.size(); ++column_index) {
        storage::Int64Column column;
        for (const auto& row : table.rows) {
            column.append(row[column_index]);
        }
        batch.add_column(table.columns[column_index], std::move(column));
    }
    return batch;
}

execution::Catalog make_catalog(const std::vector<GeneratedTable>& tables) {
    execution::Catalog catalog;
    for (const auto& table : tables) {
        catalog.add_table(table.name, make_batch(table));
    }
    return catalog;
}

std::string format_catalog(const std::vector<GeneratedTable>& tables) {
    std::ostringstream out;
    out << "catalog:\n";
    for (const auto& table : tables) {
        out << "  table=" << table.name << " columns=[";
        for (std::size_t column = 0; column < table.columns.size(); ++column) {
            if (column != 0) {
                out << ",";
            }
            out << table.columns[column];
        }
        out << "] rows=[";
        for (std::size_t row = 0; row < table.rows.size(); ++row) {
            if (row != 0) {
                out << ",";
            }
            out << "[";
            for (std::size_t column = 0; column < table.rows[row].size(); ++column) {
                if (column != 0) {
                    out << ",";
                }
                out << table.rows[row][column];
            }
            out << "]";
        }
        out << "]\n";
    }
    return out.str();
}

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

class QueryGenerator {
public:
    explicit QueryGenerator(std::uint64_t seed) : rng_(seed) {}

    GeneratedCase generate() {
        auto tables = generate_tables();
        auto catalog = make_catalog(tables);
        auto ranges = choose_ranges(tables);
        const auto all_columns = columns_for_ranges(ranges);
        const auto from_sql = from_clause(ranges);
        const auto aggregate_query = chance(48);
        std::vector<std::string> group_keys;
        std::vector<std::string> aggregate_exprs;
        std::vector<SelectItem> select_items;

        if (aggregate_query) {
            if (chance(70)) {
                group_keys = sample_unique(column_sqls(all_columns), between(1, std::min<std::size_t>(3, all_columns.size())));
            }
            const auto aggregate_count = between(1, 3);
            for (std::size_t i = 0; i < aggregate_count; ++i) {
                aggregate_exprs.push_back(random_aggregate(all_columns));
            }
            select_items = aggregate_select_items(group_keys, aggregate_exprs, all_columns);
        } else {
            select_items = scalar_select_items(all_columns);
        }

        const auto distinct = chance(28);
        std::ostringstream sql;
        sql << "SELECT ";
        if (distinct) {
            sql << "DISTINCT ";
        }
        for (std::size_t i = 0; i < select_items.size(); ++i) {
            if (i != 0) {
                sql << ", ";
            }
            sql << select_items[i].expression << " AS " << select_items[i].alias;
        }
        sql << " FROM " << from_sql;

        if (chance(58)) {
            sql << " WHERE " << predicate_tree(all_columns, 2);
        }
        if (!group_keys.empty()) {
            sql << " GROUP BY ";
            for (std::size_t i = 0; i < group_keys.size(); ++i) {
                if (i != 0) {
                    sql << ", ";
                }
                sql << group_keys[i];
            }
        }
        if (!group_keys.empty() && chance(55)) {
            sql << " HAVING " << having_tree(group_keys, aggregate_exprs, 2);
        }

        const auto order_candidates = order_by_candidates(distinct, aggregate_query, group_keys, select_items, all_columns);
        if (!order_candidates.empty() && chance(62)) {
            sql << " ORDER BY ";
            auto keys = sample_unique(order_candidates, between(1, std::min<std::size_t>(2, order_candidates.size())));
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i != 0) {
                    sql << ", ";
                }
                sql << keys[i] << (chance(50) ? " ASC" : " DESC");
            }
        }
        if (chance(45)) {
            sql << " LIMIT " << between(0, 8);
        }

        return GeneratedCase{std::move(catalog), sql.str(), format_catalog(tables)};
    }

private:
    std::vector<GeneratedTable> generate_tables() {
        std::vector<GeneratedTable> tables;
        const auto table_count = between(2, 4);
        tables.reserve(table_count);
        for (std::size_t table_index = 0; table_index < table_count; ++table_index) {
            GeneratedTable table;
            table.name = "t" + std::to_string(table_index);
            const auto column_count = between(2, 4);
            for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
                table.columns.push_back("c" + std::to_string(column_index));
            }
            const auto row_count = between(0, 12);
            for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
                std::vector<std::int64_t> row;
                row.reserve(column_count);
                for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
                    row.push_back(random_value());
                }
                table.rows.push_back(std::move(row));
            }
            tables.push_back(std::move(table));
        }
        return tables;
    }

    std::vector<RangeItem> choose_ranges(const std::vector<GeneratedTable>& tables) {
        std::vector<RangeItem> ranges;
        const auto range_count = between(1, 4);
        ranges.reserve(range_count);
        for (std::size_t i = 0; i < range_count; ++i) {
            const auto& table = pick(tables);
            ranges.push_back(RangeItem{table.name, "r" + std::to_string(i), table.columns});
        }
        return ranges;
    }

    std::string from_clause(const std::vector<RangeItem>& ranges) {
        std::ostringstream out;
        out << ranges[0].table << " AS " << ranges[0].alias;
        std::vector<RangeItem> joined{ranges[0]};
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            const auto left_columns = columns_for_ranges(joined);
            const auto right_columns = columns_for_ranges(std::vector<RangeItem>{ranges[i]});
            out << " JOIN " << ranges[i].table << " AS " << ranges[i].alias << " ON "
                << join_predicate(left_columns, right_columns);
            joined.push_back(ranges[i]);
        }
        return out.str();
    }

    std::vector<SelectItem> aggregate_select_items(const std::vector<std::string>& group_keys,
                                                   const std::vector<std::string>& aggregate_exprs,
                                                   const std::vector<ColumnRef>& all_columns) {
        std::vector<SelectItem> items;
        const auto projected_group_count =
            group_keys.empty() ? std::size_t{0} : between(1, group_keys.size());
        auto projected_groups = sample_unique(group_keys, projected_group_count);
        for (const auto& key : projected_groups) {
            const auto source = column_ref_for_sql(all_columns, key);
            items.push_back(SelectItem{key, next_alias(all_columns, source), source});
        }

        auto projected_aggregates = sample_with_replacement(aggregate_exprs, between(1, aggregate_exprs.size()));
        for (const auto& aggregate : projected_aggregates) {
            items.push_back(SelectItem{aggregate, next_alias(all_columns), std::nullopt});
        }
        return items;
    }

    std::vector<SelectItem> scalar_select_items(const std::vector<ColumnRef>& all_columns) {
        std::vector<SelectItem> items;
        const auto item_count = between(1, 4);
        for (std::size_t i = 0; i < item_count; ++i) {
            if (chance(82)) {
                const auto column = pick(all_columns);
                items.push_back(SelectItem{sql_text(column), next_alias(all_columns, column), column});
            } else {
                items.push_back(SelectItem{literal(), next_alias(all_columns), std::nullopt});
            }
        }
        return items;
    }

    std::vector<std::string> order_by_candidates(bool distinct,
                                                 bool aggregate_query,
                                                 const std::vector<std::string>& group_keys,
                                                 const std::vector<SelectItem>& select_items,
                                                 const std::vector<ColumnRef>& all_columns) {
        std::vector<std::string> candidates;
        for (const auto& item : select_items) {
            candidates.push_back(item.alias);
        }
        if (distinct) {
            return candidates;
        }
        if (aggregate_query) {
            add_safe_from_scope_order_keys(select_items, &candidates);
            return candidates;
        }
        add_safe_from_scope_order_keys(select_items, &candidates);
        return candidates;
    }

    void add_safe_from_scope_order_keys(const std::vector<SelectItem>& select_items,
                                        std::vector<std::string>* candidates) {
        for (const auto& item : select_items) {
            if (!item.source_column.has_value() || item.alias != item.source_column->column) {
                continue;
            }
            const auto text = sql_text(*item.source_column);
            if (!contains_string(*candidates, text)) {
                candidates->push_back(text);
            }
        }
    }

    std::string join_predicate(const std::vector<ColumnRef>& left_columns, const std::vector<ColumnRef>& right_columns) {
        const auto left = sql_text(pick(left_columns));
        const auto right = sql_text(pick(right_columns));
        auto predicate = left + " = " + right;
        if (chance(65)) {
            std::vector<ColumnRef> all = left_columns;
            all.insert(all.end(), right_columns.begin(), right_columns.end());
            predicate = "(" + predicate + " " + bool_op() + " " + predicate_tree(all, 1) + ")";
        }
        return predicate;
    }

    std::string predicate_tree(const std::vector<ColumnRef>& columns, int depth) {
        if (depth == 0 || chance(45)) {
            return predicate_leaf(columns);
        }
        return "(" + predicate_tree(columns, depth - 1) + " " + bool_op() + " " +
               predicate_tree(columns, depth - 1) + ")";
    }

    std::string predicate_leaf(const std::vector<ColumnRef>& columns) {
        const auto op = comparison_op_text(random_op());
        const auto column_first = chance(70);
        const auto use_two_columns = chance(24);
        if (use_two_columns) {
            return sql_text(pick(columns)) + " " + op + " " + sql_text(pick(columns));
        }
        if (column_first) {
            return sql_text(pick(columns)) + " " + op + " " + literal();
        }
        return literal() + " " + op + " " + sql_text(pick(columns));
    }

    std::string having_tree(const std::vector<std::string>& group_keys,
                            const std::vector<std::string>& aggregate_exprs,
                            int depth) {
        if (depth == 0 || chance(45)) {
            return having_leaf(group_keys, aggregate_exprs);
        }
        return "(" + having_tree(group_keys, aggregate_exprs, depth - 1) + " " + bool_op() + " " +
               having_tree(group_keys, aggregate_exprs, depth - 1) + ")";
    }

    std::string having_leaf(const std::vector<std::string>& group_keys,
                            const std::vector<std::string>& aggregate_exprs) {
        return having_expr(group_keys, aggregate_exprs) + " " + comparison_op_text(random_op()) + " " +
               (chance(70) ? literal() : having_expr(group_keys, aggregate_exprs));
    }

    std::string having_expr(const std::vector<std::string>& group_keys,
                            const std::vector<std::string>& aggregate_exprs) {
        if (chance(45) && !group_keys.empty()) {
            return pick(group_keys);
        }
        if (chance(75) && !aggregate_exprs.empty()) {
            return pick(aggregate_exprs);
        }
        return literal();
    }

    std::string random_aggregate(const std::vector<ColumnRef>& columns) {
        const auto choice = between(0, 4);
        if (choice == 0) {
            return "COUNT(*)";
        }
        const auto argument = sql_text(pick(columns));
        if (choice == 1) {
            return "COUNT(" + argument + ")";
        }
        if (choice == 2) {
            return "SUM(" + argument + ")";
        }
        if (choice == 3) {
            return "MIN(" + argument + ")";
        }
        return "MAX(" + argument + ")";
    }

    std::string next_alias(const std::vector<ColumnRef>& all_columns,
                           std::optional<ColumnRef> preferred_column = std::nullopt) {
        if (preferred_column.has_value() && chance(35) &&
            !contains_string(used_output_aliases_, preferred_column->column)) {
            used_output_aliases_.push_back(preferred_column->column);
            return preferred_column->column;
        }
        if (chance(12)) {
            const auto candidate = pick(all_columns).column;
            if (!contains_string(used_output_aliases_, candidate)) {
                used_output_aliases_.push_back(candidate);
                return candidate;
            }
        }
        std::string alias;
        do {
            alias = "o" + std::to_string(next_output_alias_++);
        } while (contains_string(used_output_aliases_, alias));
        used_output_aliases_.push_back(alias);
        return alias;
    }

    std::vector<ColumnRef> columns_for_ranges(const std::vector<RangeItem>& ranges) const {
        std::vector<ColumnRef> refs;
        for (const auto& range : ranges) {
            for (const auto& column : range.columns) {
                refs.push_back(ColumnRef{range.alias, column});
            }
        }
        return refs;
    }

    std::vector<std::string> column_sqls(const std::vector<ColumnRef>& columns) const {
        std::vector<std::string> values;
        values.reserve(columns.size());
        for (const auto& column : columns) {
            values.push_back(sql_text(column));
        }
        return values;
    }

    std::optional<ColumnRef> column_ref_for_sql(const std::vector<ColumnRef>& columns, const std::string& text) const {
        for (const auto& column : columns) {
            if (sql_text(column) == text) {
                return column;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> sample_unique(std::vector<std::string> values, std::size_t count) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        std::shuffle(values.begin(), values.end(), rng_);
        if (values.size() > count) {
            values.resize(count);
        }
        return values;
    }

    std::vector<std::string> sample_with_replacement(const std::vector<std::string>& values, std::size_t count) {
        std::vector<std::string> sample;
        sample.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            sample.push_back(pick(values));
        }
        return sample;
    }

    sql::ComparisonOp random_op() {
        switch (between(0, 5)) {
        case 0:
            return sql::ComparisonOp::Equal;
        case 1:
            return sql::ComparisonOp::NotEqual;
        case 2:
            return sql::ComparisonOp::Less;
        case 3:
            return sql::ComparisonOp::LessEqual;
        case 4:
            return sql::ComparisonOp::Greater;
        default:
            return sql::ComparisonOp::GreaterEqual;
        }
    }

    std::string bool_op() { return chance(55) ? "AND" : "OR"; }

    std::int64_t random_value() {
        static const std::vector<std::int64_t> pool{
            std::numeric_limits<std::int64_t>::min(),
            std::numeric_limits<std::int64_t>::max(),
            -10,
            -3,
            -1,
            -1,
            0,
            0,
            0,
            1,
            1,
            2,
            2,
            3,
            7,
            10,
        };
        if (chance(88)) {
            return pick(pool);
        }
        return static_cast<std::int64_t>(between(0, 20)) - 10;
    }

    std::string literal() { return std::to_string(random_value()); }

    bool chance(int percent) { return between(1, 100) <= static_cast<std::size_t>(percent); }

    std::size_t between(std::size_t low, std::size_t high) {
        std::uniform_int_distribution<std::size_t> dist(low, high);
        return dist(rng_);
    }

    template <typename T>
    const T& pick(const std::vector<T>& values) {
        return values[between(0, values.size() - 1)];
    }

    std::mt19937_64 rng_;
    std::size_t next_output_alias_{0};
    std::vector<std::string> used_output_aliases_;
};

std::optional<std::uint64_t> parse_seed_arg(int argc, char** argv) {
    if (argc <= 1) {
        return std::nullopt;
    }
    std::uint64_t seed = 0;
    const std::string text = argv[1];
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, seed);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument("seed argument must be an unsigned integer");
    }
    return seed;
}

std::vector<std::uint64_t> seed_list(int argc, char** argv) {
    if (const auto seed = parse_seed_arg(argc, argv); seed.has_value()) {
        return {*seed};
    }
    std::vector<std::uint64_t> seeds;
    seeds.reserve(kDefaultSeedCount);
    for (std::uint64_t seed = 1; seed <= kDefaultSeedCount; ++seed) {
        seeds.push_back(seed);
    }
    return seeds;
}

bool verify_generated_case(std::uint64_t seed, const GeneratedCase& generated, differential::ComparisonStats* stats) {
    try {
        const auto parsed = sql::parse_select(generated.sql);
        (void)sql::bind_select(parsed, generated.catalog);
    } catch (const sql::ParseError& error) {
        std::cerr << "GENERATOR BUG: generated SQL did not parse\n"
                  << "seed: " << seed << "\n"
                  << "sql: " << generated.sql << "\n"
                  << generated.catalog_dump
                  << "parse error: " << error.what() << "\n";
        return false;
    } catch (const sql::BindError& error) {
        std::cerr << "GENERATOR BUG: generated SQL did not bind\n"
                  << "seed: " << seed << "\n"
                  << "sql: " << generated.sql << "\n"
                  << generated.catalog_dump
                  << "bind error: " << error.what() << "\n";
        return false;
    }

    std::ostringstream context;
    context << "seed: " << seed << "\n" << generated.catalog_dump;
    return differential::compare_engines(generated.sql, generated.catalog, context.str(), stats);
}

} // namespace

int main(int argc, char** argv) {
    // Re-run one deterministic case with:
    //   build/sql_fuzz_differential <seed>
    // The ctest target runs the fixed list 1..kDefaultSeedCount.
    const auto seeds = seed_list(argc, argv);
    std::size_t queries = 0;
    std::size_t alternatives = 0;
    std::size_t execution_paths = 0;
    std::size_t accepted_error_paths = 0;
    std::size_t max_group_expression_count = 0;
    bool hit_expression_bound = false;
    bool hit_plan_bound = false;

    for (const auto seed : seeds) {
        QueryGenerator generator(seed);
        const auto generated = generator.generate();
        differential::ComparisonStats stats;
        if (!verify_generated_case(seed, generated, &stats)) {
            return 1;
        }
        ++queries;
        alternatives += stats.alternative_count;
        execution_paths += stats.execution_path_count;
        accepted_error_paths += stats.accepted_error_path_count;
        max_group_expression_count = std::max(max_group_expression_count, stats.max_group_expression_count);
        hit_expression_bound = hit_expression_bound || stats.hit_expression_bound;
        hit_plan_bound = hit_plan_bound || stats.hit_plan_bound;
    }

    std::cout << "sql_fuzz_differential coverage: seeds=" << seeds.size() << " queries=" << queries
              << " alternatives=" << alternatives << " execution_paths=" << execution_paths
              << " accepted_error_paths=" << accepted_error_paths
              << " max_group_expressions=" << max_group_expression_count
              << " hit_expression_bound=" << (hit_expression_bound ? "yes" : "no")
              << " hit_plan_bound=" << (hit_plan_bound ? "yes" : "no") << "\n";
    return 0;
}
