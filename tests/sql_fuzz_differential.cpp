#include "differential_verifier.hpp"
#include "sql/ast.hpp"
#include "sql/errors.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
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

struct Cell {
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool is_null{true};
    std::int64_t int_value{0};
    std::string string_value;
};

struct GeneratedTable {
    std::string name;
    std::vector<std::string> columns;
    std::vector<catalog::ColumnType> types;
    std::vector<bool> nullable;
    std::vector<bool> contains_null;
    std::vector<std::vector<Cell>> rows;
};

struct ColumnRef {
    std::string alias;
    std::string column;
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool nullable{false};
    bool contains_null{false};
};

struct RangeItem {
    std::string table;
    std::string alias;
    std::vector<std::string> columns;
    std::vector<catalog::ColumnType> types;
    std::vector<bool> nullable;
    std::vector<bool> contains_null;
};

struct SelectItem {
    std::string expression;
    std::string alias;
    std::optional<ColumnRef> source_column;
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool nullable{false};
};

struct AggregateExpr {
    std::string expression;
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool nullable{false};
    bool contains_null{false};
};

struct WindowInput {
    std::string expression;
    catalog::ColumnType type{catalog::ColumnType::Int64};
    bool nullable{false};
    bool contains_null{false};
};

struct GeneratedCase {
    execution::Catalog catalog;
    std::string sql;
    std::string catalog_dump;
    std::size_t string_column_count{0};
    bool has_string_literal{false};
    bool has_string_join_key{false};
    bool has_string_group_key{false};
    bool has_string_distinct_output{false};
    bool has_string_order_key{false};
    std::size_t inner_join_count{0};
    std::size_t left_join_count{0};
    std::size_t right_join_count{0};
    std::size_t null_key_join_count{0};
    bool has_mixed_inner_outer_chain{false};
    bool has_scalar_subquery{false};
    bool has_in_subquery{false};
    bool has_not_in_subquery{false};
    bool has_exists_subquery{false};
    bool has_not_exists_subquery{false};
    bool has_nested_subquery{false};
    bool has_subquery_join{false};
    bool has_subquery_aggregate{false};
    bool has_null_bearing_subquery{false};
    bool has_empty_subquery{false};
    bool has_or_embedded_subquery{false};
    std::size_t window_definition_count{0};
    std::size_t row_number_window_count{0};
    std::size_t rank_window_count{0};
    std::size_t dense_rank_window_count{0};
    std::size_t count_window_count{0};
    std::size_t sum_window_count{0};
    std::size_t min_window_count{0};
    std::size_t max_window_count{0};
    std::size_t partitioned_window_count{0};
    std::size_t unpartitioned_window_count{0};
    std::size_t ordered_rank_window_count{0};
    std::size_t unordered_rank_window_count{0};
    bool has_window{false};
    bool has_null_window_key{false};
    bool has_string_window_key{false};
    bool has_string_window_argument{false};
    bool has_joined_window_input{false};
    bool has_grouped_window_input{false};
    bool has_having_window_input{false};
    bool has_multiple_windows{false};
    bool has_window_outer_order{false};
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
        if (table.types[column_index] == catalog::ColumnType::Int64) {
            storage::Int64Column column;
            for (const auto& row : table.rows) {
                const auto& value = row[column_index];
                if (value.is_null) {
                    column.append_null();
                } else {
                    column.append(value.int_value);
                }
            }
            batch.add_column(table.columns[column_index], std::move(column));
        } else {
            storage::StringColumn column;
            for (const auto& row : table.rows) {
                const auto& value = row[column_index];
                if (value.is_null) {
                    column.append_null();
                } else {
                    column.append(value.string_value);
                }
            }
            batch.add_column(table.columns[column_index], std::move(column));
        }
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
            out << table.columns[column] << ":"
                << (table.types[column] == catalog::ColumnType::Int64 ? "int64" : "string");
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
                const auto& value = table.rows[row][column];
                if (value.is_null) {
                    out << "NULL";
                } else if (value.type == catalog::ColumnType::Int64) {
                    out << value.int_value;
                } else {
                    out << sql::quote_string_literal(value.string_value);
                }
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
    explicit QueryGenerator(std::uint64_t seed) : seed_(seed), rng_(seed) {}

    GeneratedCase generate() {
        saw_string_join_key_ = false;
        inner_join_count_ = 0;
        left_join_count_ = 0;
        right_join_count_ = 0;
        null_key_join_count_ = 0;
        reset_subquery_coverage();
        reset_window_coverage();
        const auto window_mode = static_cast<std::size_t>((seed_ - 1) % 8);
        const auto window_query = static_cast<std::size_t>((seed_ - 1) % 16) < 8;
        const auto force_window_join = window_query && (window_mode == 0 || window_mode == 7);
        const auto force_grouped_window = window_query && (window_mode == 4 || window_mode == 7);
        auto tables = generate_tables();
        auto catalog = make_catalog(tables);
        auto ranges = choose_ranges(tables, force_window_join);
        const auto all_columns = columns_for_ranges(ranges);
        const auto from_sql = from_clause(ranges);
        const auto aggregate_query = force_grouped_window || (window_query ? chance(36) : chance(48));
        std::vector<ColumnRef> group_keys;
        std::vector<AggregateExpr> aggregate_exprs;
        std::vector<SelectItem> select_items;
        std::vector<std::string> window_aliases;

        if (aggregate_query) {
            if (force_grouped_window || chance(70)) {
                group_keys = sample_unique_columns(all_columns,
                                           between(1, std::min<std::size_t>(3, all_columns.size())));
            }
            const auto aggregate_count = between(1, 3);
            for (std::size_t i = 0; i < aggregate_count; ++i) {
                aggregate_exprs.push_back(random_aggregate(all_columns));
            }
            select_items = aggregate_select_items(group_keys, aggregate_exprs, all_columns);
        } else {
            select_items = scalar_select_items(all_columns);
        }
        if (window_query) {
            auto window_items = window_select_items(
                window_mode, aggregate_query, group_keys, aggregate_exprs, all_columns, &window_aliases);
            select_items.insert(select_items.end(),
                                std::make_move_iterator(window_items.begin()),
                                std::make_move_iterator(window_items.end()));
        }

        const auto distinct = chance(28);
        // EXPLAIN is intentionally excluded from generation: it observes the
        // optimizer report rather than query semantics. The shared differential
        // verifier still handles EXPLAIN exactly if a fixed corpus case uses it.
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

        sql << " WHERE ";
        if (chance(58)) {
            sql << predicate_tree(all_columns, 2) << " AND ";
        }
        sql << subquery_predicate(all_columns, tables);
        if (!group_keys.empty()) {
            sql << " GROUP BY ";
            for (std::size_t i = 0; i < group_keys.size(); ++i) {
                if (i != 0) {
                    sql << ", ";
                }
                sql << sql_text(group_keys[i]);
            }
        }
        bool has_having = false;
        if (!group_keys.empty() && (force_grouped_window || chance(55))) {
            sql << " HAVING " << having_tree(group_keys, aggregate_exprs, 2);
            has_having = true;
        }

        const auto order_candidates = order_by_candidates(distinct, aggregate_query, group_keys, select_items, all_columns);
        const auto force_window_outer_order = window_query && !window_aliases.empty() && (window_mode == 1 || window_mode == 7);
        if (!order_candidates.empty() && (force_window_outer_order || chance(62))) {
            sql << " ORDER BY ";
            auto keys = force_window_outer_order
                            ? std::vector<std::string>{pick(window_aliases)}
                            : sample_unique(order_candidates,
                                            between(1, std::min<std::size_t>(2, order_candidates.size())));
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i != 0) {
                    sql << ", ";
                }
                sql << keys[i] << (chance(50) ? " ASC" : " DESC");
            }
            saw_string_order_key_ = any_string_order_key(keys, select_items, all_columns);
            has_window_outer_order_ = has_window_outer_order_ || force_window_outer_order;
        } else {
            saw_string_order_key_ = false;
        }
        if (chance(45)) {
            sql << " LIMIT " << between(0, 8);
        }

        const auto sql_string = sql.str();
        return GeneratedCase{std::move(catalog),
                             sql_string,
                             format_catalog(tables),
                             string_column_count(tables),
                             sql_string.find('\'') != std::string::npos,
                             saw_string_join_key_,
                             any_string_group_key(group_keys),
                             distinct && any_string_select_item(select_items),
                             saw_string_order_key_,
                             inner_join_count_,
                             left_join_count_,
                             right_join_count_,
                             null_key_join_count_,
                             inner_join_count_ != 0 && (left_join_count_ != 0 || right_join_count_ != 0),
                             has_scalar_subquery_,
                             has_in_subquery_,
                             has_not_in_subquery_,
                             has_exists_subquery_,
                             has_not_exists_subquery_,
                             has_nested_subquery_,
                             has_subquery_join_,
                             has_subquery_aggregate_,
                             has_null_bearing_subquery_,
                             has_empty_subquery_,
                             has_or_embedded_subquery_,
                             window_definition_count_,
                             row_number_window_count_,
                             rank_window_count_,
                             dense_rank_window_count_,
                             count_window_count_,
                             sum_window_count_,
                             min_window_count_,
                             max_window_count_,
                             partitioned_window_count_,
                             unpartitioned_window_count_,
                             ordered_rank_window_count_,
                             unordered_rank_window_count_,
                             window_definition_count_ != 0,
                             has_null_window_key_,
                             has_string_window_key_,
                             has_string_window_argument_,
                             window_definition_count_ != 0 && ranges.size() > 1,
                             window_definition_count_ != 0 && aggregate_query,
                             window_definition_count_ != 0 && has_having,
                             window_definition_count_ > 1,
                             has_window_outer_order_};
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
                table.types.push_back(column_index == 0 || (column_index != 1 && chance(55))
                                          ? catalog::ColumnType::Int64
                                          : catalog::ColumnType::String);
                table.nullable.push_back(column_index != 0 && chance(65));
            }
            const auto row_count = between(0, 12);
            for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
                std::vector<Cell> row;
                row.reserve(column_count);
                for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
                    row.push_back(random_cell(table.types[column_index], table.nullable[column_index]));
                }
                table.rows.push_back(std::move(row));
            }
            table.contains_null.assign(column_count, false);
            for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
                for (const auto& row : table.rows) {
                    table.contains_null[column_index] =
                        table.contains_null[column_index] || row[column_index].is_null;
                }
                if (table.nullable[column_index] && !table.rows.empty() && !table.contains_null[column_index]) {
                    table.rows[0][column_index] = null_cell(table.types[column_index]);
                    table.contains_null[column_index] = true;
                }
            }
            tables.push_back(std::move(table));
        }
        return tables;
    }

    std::vector<RangeItem> choose_ranges(const std::vector<GeneratedTable>& tables, bool force_join) {
        std::vector<RangeItem> ranges;
        const auto range_count = force_join ? between(2, 4) : between(1, 4);
        ranges.reserve(range_count);
        for (std::size_t i = 0; i < range_count; ++i) {
            const auto& table = pick(tables);
            ranges.push_back(RangeItem{table.name,
                                       "r" + std::to_string(i),
                                       table.columns,
                                       table.types,
                                       table.nullable,
                                       table.contains_null});
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
            const auto join_draw = between(1, 100);
            if (join_draw <= 45) {
                out << " JOIN ";
                ++inner_join_count_;
            } else if (join_draw <= 73) {
                out << " LEFT JOIN ";
                ++left_join_count_;
            } else {
                out << " RIGHT JOIN ";
                ++right_join_count_;
            }
            out << ranges[i].table << " AS " << ranges[i].alias << " ON "
                << join_predicate(left_columns, right_columns);
            joined.push_back(ranges[i]);
        }
        return out.str();
    }

    std::vector<SelectItem> aggregate_select_items(const std::vector<ColumnRef>& group_keys,
                                                   const std::vector<AggregateExpr>& aggregate_exprs,
                                                   const std::vector<ColumnRef>& all_columns) {
        std::vector<SelectItem> items;
        const auto projected_group_count =
            group_keys.empty() ? std::size_t{0} : between(1, group_keys.size());
        auto projected_groups = sample_unique_columns(group_keys, projected_group_count);
        for (const auto& key : projected_groups) {
            items.push_back(SelectItem{sql_text(key), next_alias(all_columns, key), key, key.type, key.nullable});
        }

        auto projected_aggregates = sample_with_replacement(aggregate_exprs, between(1, aggregate_exprs.size()));
        for (const auto& aggregate : projected_aggregates) {
            items.push_back(
                SelectItem{aggregate.expression, next_alias(all_columns), std::nullopt, aggregate.type, aggregate.nullable});
        }
        return items;
    }

    std::vector<SelectItem> scalar_select_items(const std::vector<ColumnRef>& all_columns) {
        std::vector<SelectItem> items;
        const auto item_count = between(1, 4);
        for (std::size_t i = 0; i < item_count; ++i) {
            if (chance(82)) {
                const auto column = pick(all_columns);
                items.push_back(
                    SelectItem{sql_text(column), next_alias(all_columns, column), column, column.type, column.nullable});
            } else {
                const auto type = random_literal_type();
                const auto value = literal(type);
                items.push_back(SelectItem{value,
                                           next_alias(all_columns),
                                           std::nullopt,
                                           value == "NULL" ? catalog::ColumnType::Int64 : type,
                                           value == "NULL"});
            }
        }
        return items;
    }

    std::vector<SelectItem> window_select_items(std::size_t mode,
                                                bool aggregate_query,
                                                const std::vector<ColumnRef>& group_keys,
                                                const std::vector<AggregateExpr>& aggregate_exprs,
                                                const std::vector<ColumnRef>& all_columns,
                                                std::vector<std::string>* aliases) {
        const auto inputs = window_inputs(aggregate_query, group_keys, aggregate_exprs, all_columns);
        std::vector<SelectItem> items;
        const auto add = [&](sql::WindowFunction function) {
            const auto item = make_window_select_item(function, mode, inputs, all_columns);
            aliases->push_back(item.alias);
            items.push_back(item);
        };

        if (mode == 7) {
            add(sql::WindowFunction::RowNumber);
            add(sql::WindowFunction::Sum);
            add(sql::WindowFunction::Count);
            return items;
        }

        switch (mode) {
        case 0:
            add(sql::WindowFunction::RowNumber);
            break;
        case 1:
            add(sql::WindowFunction::Rank);
            break;
        case 2:
            add(sql::WindowFunction::DenseRank);
            break;
        case 3:
            add(sql::WindowFunction::Count);
            break;
        case 4:
            add(sql::WindowFunction::Sum);
            break;
        case 5:
            add(sql::WindowFunction::Min);
            break;
        default:
            add(sql::WindowFunction::Max);
            break;
        }
        return items;
    }

    std::vector<WindowInput> window_inputs(bool aggregate_query,
                                           const std::vector<ColumnRef>& group_keys,
                                           const std::vector<AggregateExpr>& aggregate_exprs,
                                           const std::vector<ColumnRef>& all_columns) const {
        std::vector<WindowInput> inputs;
        if (aggregate_query) {
            for (const auto& key : group_keys) {
                inputs.push_back(WindowInput{sql_text(key), key.type, key.nullable, key.contains_null});
            }
            for (const auto& aggregate : aggregate_exprs) {
                inputs.push_back(
                    WindowInput{aggregate.expression, aggregate.type, aggregate.nullable, aggregate.contains_null});
            }
            if (std::none_of(inputs.begin(), inputs.end(), [](const auto& input) {
                    return input.type == catalog::ColumnType::Int64;
                })) {
                inputs.push_back(WindowInput{"COUNT(*)", catalog::ColumnType::Int64, false, false});
            }
        } else {
            for (const auto& column : all_columns) {
                inputs.push_back(WindowInput{sql_text(column), column.type, column.nullable, column.contains_null});
            }
        }
        if (inputs.empty()) {
            throw std::logic_error("generator has no window inputs");
        }
        return inputs;
    }

    SelectItem make_window_select_item(sql::WindowFunction function,
                                       std::size_t mode,
                                       const std::vector<WindowInput>& inputs,
                                       const std::vector<ColumnRef>& all_columns) {
        std::optional<WindowInput> argument;
        catalog::ColumnType output_type = catalog::ColumnType::Int64;
        bool nullable = false;
        bool count_star = false;
        switch (function) {
        case sql::WindowFunction::RowNumber:
        case sql::WindowFunction::Rank:
        case sql::WindowFunction::DenseRank:
            break;
        case sql::WindowFunction::Count:
            count_star = mode == 7 || chance(35);
            if (!count_star) {
                argument = pick_window_input(inputs, std::nullopt, mode == 3, false);
            }
            break;
        case sql::WindowFunction::Sum:
            argument = pick_window_input(inputs, catalog::ColumnType::Int64, false, false);
            nullable = true;
            break;
        case sql::WindowFunction::Min:
            argument = pick_window_input(inputs, std::nullopt, true, false);
            output_type = argument->type;
            nullable = true;
            break;
        case sql::WindowFunction::Max:
            argument = pick_window_input(inputs, std::nullopt, true, false);
            output_type = argument->type;
            nullable = true;
            break;
        }

        std::vector<WindowInput> partition_keys;
        const auto force_partition = mode == 0 || mode == 2 || mode == 3 || mode == 4 || mode == 5 || mode == 7;
        if (force_partition || chance(45)) {
            partition_keys.push_back(pick_window_input(inputs, std::nullopt, mode == 3 || mode == 5, mode == 0 || mode == 2));
        }

        std::vector<WindowInput> order_keys;
        if (function == sql::WindowFunction::RowNumber || function == sql::WindowFunction::Rank ||
            function == sql::WindowFunction::DenseRank) {
            const auto force_order = function != sql::WindowFunction::DenseRank || mode == 7;
            if (force_order || chance(55)) {
                order_keys.push_back(pick_window_input(inputs, std::nullopt, mode == 1, mode == 0));
            }
        }

        for (const auto& key : partition_keys) {
            has_null_window_key_ = has_null_window_key_ || key.contains_null;
            has_string_window_key_ = has_string_window_key_ || key.type == catalog::ColumnType::String;
        }
        for (const auto& key : order_keys) {
            has_null_window_key_ = has_null_window_key_ || key.contains_null;
            has_string_window_key_ = has_string_window_key_ || key.type == catalog::ColumnType::String;
        }
        if (argument.has_value() && argument->type == catalog::ColumnType::String) {
            has_string_window_argument_ = true;
        }

        ++window_definition_count_;
        partition_keys.empty() ? ++unpartitioned_window_count_ : ++partitioned_window_count_;
        switch (function) {
        case sql::WindowFunction::RowNumber:
            ++row_number_window_count_;
            break;
        case sql::WindowFunction::Rank:
            ++rank_window_count_;
            order_keys.empty() ? ++unordered_rank_window_count_ : ++ordered_rank_window_count_;
            break;
        case sql::WindowFunction::DenseRank:
            ++dense_rank_window_count_;
            order_keys.empty() ? ++unordered_rank_window_count_ : ++ordered_rank_window_count_;
            break;
        case sql::WindowFunction::Count:
            ++count_window_count_;
            break;
        case sql::WindowFunction::Sum:
            ++sum_window_count_;
            break;
        case sql::WindowFunction::Min:
            ++min_window_count_;
            break;
        case sql::WindowFunction::Max:
            ++max_window_count_;
            break;
        }

        const auto expression = window_expression_sql(function, count_star, argument, partition_keys, order_keys);
        return SelectItem{expression, next_alias(all_columns), std::nullopt, output_type, nullable};
    }

    WindowInput pick_window_input(const std::vector<WindowInput>& inputs,
                                  std::optional<catalog::ColumnType> required_type,
                                  bool prefer_string,
                                  bool prefer_null) {
        std::vector<WindowInput> candidates;
        for (const auto& input : inputs) {
            if (!required_type.has_value() || input.type == *required_type) {
                candidates.push_back(input);
            }
        }
        if (candidates.empty()) {
            throw std::logic_error("generator has no window input of requested type");
        }
        auto preferred = candidates;
        if (prefer_string) {
            preferred.erase(std::remove_if(preferred.begin(),
                                           preferred.end(),
                                           [](const auto& input) {
                                               return input.type != catalog::ColumnType::String;
                                           }),
                            preferred.end());
        }
        if (prefer_null && !preferred.empty()) {
            preferred.erase(std::remove_if(preferred.begin(),
                                           preferred.end(),
                                           [](const auto& input) {
                                               return !input.contains_null;
                                           }),
                            preferred.end());
        }
        if (!preferred.empty()) {
            return pick(preferred);
        }
        return pick(candidates);
    }

    std::string window_expression_sql(sql::WindowFunction function,
                                      bool count_star,
                                      const std::optional<WindowInput>& argument,
                                      const std::vector<WindowInput>& partition_keys,
                                      const std::vector<WindowInput>& order_keys) {
        std::ostringstream out;
        switch (function) {
        case sql::WindowFunction::RowNumber:
            out << "ROW_NUMBER()";
            break;
        case sql::WindowFunction::Rank:
            out << "RANK()";
            break;
        case sql::WindowFunction::DenseRank:
            out << "DENSE_RANK()";
            break;
        case sql::WindowFunction::Count:
            out << (count_star ? "COUNT(*)" : "COUNT(" + argument->expression + ")");
            break;
        case sql::WindowFunction::Sum:
            out << "SUM(" << argument->expression << ")";
            break;
        case sql::WindowFunction::Min:
            out << "MIN(" << argument->expression << ")";
            break;
        case sql::WindowFunction::Max:
            out << "MAX(" << argument->expression << ")";
            break;
        }
        out << " OVER (";
        bool wrote_clause = false;
        if (!partition_keys.empty()) {
            out << "PARTITION BY ";
            for (std::size_t i = 0; i < partition_keys.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << partition_keys[i].expression;
            }
            wrote_clause = true;
        }
        if (!order_keys.empty()) {
            if (wrote_clause) {
                out << " ";
            }
            out << "ORDER BY ";
            for (std::size_t i = 0; i < order_keys.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << order_keys[i].expression << (chance(50) ? " ASC" : " DESC");
            }
        }
        out << ")";
        return out.str();
    }

    std::vector<std::string> order_by_candidates(bool distinct,
                                                 bool aggregate_query,
                                                 const std::vector<ColumnRef>& group_keys,
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

    bool any_string_order_key(const std::vector<std::string>& keys,
                              const std::vector<SelectItem>& select_items,
                              const std::vector<ColumnRef>& all_columns) const {
        for (const auto& key : keys) {
            for (const auto& item : select_items) {
                if (item.alias == key && item.type == catalog::ColumnType::String) {
                    return true;
                }
            }
            for (const auto& column : all_columns) {
                if (sql_text(column) == key && column.type == catalog::ColumnType::String) {
                    return true;
                }
            }
        }
        return false;
    }

    bool any_string_group_key(const std::vector<ColumnRef>& group_keys) const {
        for (const auto& key : group_keys) {
            if (key.type == catalog::ColumnType::String) {
                return true;
            }
        }
        return false;
    }

    bool any_string_select_item(const std::vector<SelectItem>& select_items) const {
        for (const auto& item : select_items) {
            if (item.type == catalog::ColumnType::String) {
                return true;
            }
        }
        return false;
    }

    std::size_t string_column_count(const std::vector<GeneratedTable>& tables) const {
        std::size_t count = 0;
        for (const auto& table : tables) {
            for (const auto type : table.types) {
                if (type == catalog::ColumnType::String) {
                    ++count;
                }
            }
        }
        return count;
    }

    std::string join_predicate(const std::vector<ColumnRef>& left_columns, const std::vector<ColumnRef>& right_columns) {
        const auto common_types = common_column_types(left_columns, right_columns);
        std::vector<catalog::ColumnType> null_key_types;
        for (const auto type : common_types) {
            const auto left_typed = columns_of_type(left_columns, type);
            const auto right_typed = columns_of_type(right_columns, type);
            const auto side_has_null = [&](const std::vector<ColumnRef>& columns) {
                return std::any_of(columns.begin(), columns.end(), [](const auto& column) {
                    return column.contains_null;
                });
            };
            if (side_has_null(left_typed) || side_has_null(right_typed)) {
                null_key_types.push_back(type);
            }
        }
        const auto type = !null_key_types.empty() && chance(70)
                              ? pick(null_key_types)
                              : contains_type(common_types, catalog::ColumnType::String) && chance(45)
                                    ? catalog::ColumnType::String
                                    : pick(common_types);
        saw_string_join_key_ = saw_string_join_key_ || type == catalog::ColumnType::String;
        const auto left_typed = columns_of_type(left_columns, type);
        const auto right_typed = columns_of_type(right_columns, type);
        auto left = pick(left_typed);
        auto right = pick(right_typed);
        std::vector<ColumnRef> null_left;
        std::vector<ColumnRef> null_right;
        std::copy_if(left_typed.begin(), left_typed.end(), std::back_inserter(null_left), [](const auto& column) {
            return column.contains_null;
        });
        std::copy_if(right_typed.begin(), right_typed.end(), std::back_inserter(null_right), [](const auto& column) {
            return column.contains_null;
        });
        if ((!null_left.empty() || !null_right.empty()) && chance(70)) {
            if (!null_left.empty() && (null_right.empty() || chance(50))) {
                left = pick(null_left);
            } else {
                right = pick(null_right);
            }
        }
        null_key_join_count_ += left.contains_null || right.contains_null ? 1 : 0;
        const auto left_text = sql_text(left);
        const auto right_text = sql_text(right);
        auto predicate = left_text + " = " + right_text;
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
        if (chance(18)) {
            const auto expression = chance(85) ? sql_text(pick(columns)) : std::string{"NULL"};
            return expression + (chance(55) ? " IS NULL" : " IS NOT NULL");
        }

        const auto op = comparison_op_text(random_op());
        const auto column_first = chance(70);
        const auto use_two_columns = chance(24);
        if (use_two_columns) {
            const auto type = pick(column_types(columns));
            const auto typed_columns = columns_of_type(columns, type);
            return sql_text(pick(typed_columns)) + " " + op + " " + sql_text(pick(typed_columns));
        }
        const auto column = pick(columns);
        if (column_first) {
            return sql_text(column) + " " + op + " " + literal(column.type);
        }
        return literal(column.type) + " " + op + " " + sql_text(column);
    }

    const std::string& table_column_of_type(const GeneratedTable& table, catalog::ColumnType type) const {
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            if (table.types[i] == type) {
                return table.columns[i];
            }
        }
        throw std::logic_error("generated table has no column of required subquery type");
    }

    void reset_subquery_coverage() {
        has_scalar_subquery_ = false;
        has_in_subquery_ = false;
        has_not_in_subquery_ = false;
        has_exists_subquery_ = false;
        has_not_exists_subquery_ = false;
        has_nested_subquery_ = false;
        has_subquery_join_ = false;
        has_subquery_aggregate_ = false;
        has_null_bearing_subquery_ = false;
        has_empty_subquery_ = false;
        has_or_embedded_subquery_ = false;
    }

    void reset_window_coverage() {
        window_definition_count_ = 0;
        row_number_window_count_ = 0;
        rank_window_count_ = 0;
        dense_rank_window_count_ = 0;
        count_window_count_ = 0;
        sum_window_count_ = 0;
        min_window_count_ = 0;
        max_window_count_ = 0;
        partitioned_window_count_ = 0;
        unpartitioned_window_count_ = 0;
        ordered_rank_window_count_ = 0;
        unordered_rank_window_count_ = 0;
        has_null_window_key_ = false;
        has_string_window_key_ = false;
        has_string_window_argument_ = false;
        has_window_outer_order_ = false;
    }

    std::string subquery_predicate(const std::vector<ColumnRef>& outer_columns,
                                   const std::vector<GeneratedTable>& tables) {
        const auto mode = static_cast<std::size_t>((seed_ - 1) % 10);
        const auto outer = pick(outer_columns);
        const auto& first = pick(tables);
        const auto& second = pick(tables);
        const auto& first_typed_column = table_column_of_type(first, outer.type);
        const auto& second_int_column = table_column_of_type(second, catalog::ColumnType::Int64);
        const auto outer_text = sql_text(outer);
        const auto ordinary = outer_text + " = " + literal(outer.type);

        switch (mode) {
        case 0:
            has_scalar_subquery_ = true;
            has_subquery_aggregate_ = true;
            return outer_text + " " + comparison_op_text(random_op()) + " (SELECT MAX(sq0." +
                   first_typed_column + ") FROM " + first.name + " AS sq0)";
        case 1:
            has_in_subquery_ = true;
            return outer_text + " IN (SELECT sq0." + first_typed_column + " FROM " + first.name +
                   " AS sq0)";
        case 2: {
            has_not_in_subquery_ = true;
            has_subquery_aggregate_ = true;
            has_null_bearing_subquery_ = true;
            const auto outer_int = pick(columns_of_type(outer_columns, catalog::ColumnType::Int64));
            const auto& inner_int = table_column_of_type(first, catalog::ColumnType::Int64);
            return sql_text(outer_int) + " NOT IN (SELECT MAX(sq0." + inner_int + ") FROM " + first.name +
                   " AS sq0 WHERE 1 = 0)";
        }
        case 3:
            has_exists_subquery_ = true;
            has_empty_subquery_ = true;
            return "EXISTS (SELECT sq0." + first_typed_column + " FROM " + first.name +
                   " AS sq0 WHERE 1 = 0)";
        case 4:
            has_not_exists_subquery_ = true;
            has_subquery_join_ = true;
            return "NOT EXISTS (SELECT sq0." + table_column_of_type(first, catalog::ColumnType::Int64) +
                   " FROM " + first.name + " AS sq0 JOIN " + second.name + " AS sq1 ON sq0." +
                   table_column_of_type(first, catalog::ColumnType::Int64) + " = sq1." + second_int_column + ")";
        case 5:
            has_in_subquery_ = true;
            has_exists_subquery_ = true;
            has_nested_subquery_ = true;
            return outer_text + " IN (SELECT sq0." + first_typed_column + " FROM " + first.name +
                   " AS sq0 WHERE EXISTS (SELECT sq1." + second_int_column + " FROM " + second.name +
                   " AS sq1 WHERE 1 = 1))";
        case 6:
            has_in_subquery_ = true;
            has_subquery_join_ = true;
            return outer_text + " IN (SELECT sq0." + first_typed_column + " FROM " + first.name +
                   " AS sq0 JOIN " + second.name + " AS sq1 ON sq0." +
                   table_column_of_type(first, catalog::ColumnType::Int64) + " = sq1." + second_int_column + ")";
        case 7:
            has_scalar_subquery_ = true;
            has_subquery_join_ = true;
            has_subquery_aggregate_ = true;
            return outer_text + " = (SELECT MAX(sq0." + first_typed_column + ") FROM " + first.name +
                   " AS sq0 JOIN " + second.name + " AS sq1 ON sq0." +
                   table_column_of_type(first, catalog::ColumnType::Int64) + " = sq1." + second_int_column + ")";
        case 8:
            has_in_subquery_ = true;
            has_or_embedded_subquery_ = true;
            return "(" + ordinary + " OR " + outer_text + " IN (SELECT sq0." + first_typed_column +
                   " FROM " + first.name + " AS sq0))";
        default:
            has_exists_subquery_ = true;
            has_or_embedded_subquery_ = true;
            return "(" + ordinary + " OR EXISTS (SELECT sq0." + first_typed_column + " FROM " + first.name +
                   " AS sq0))";
        }
    }

    std::string having_tree(const std::vector<ColumnRef>& group_keys,
                            const std::vector<AggregateExpr>& aggregate_exprs,
                            int depth) {
        if (depth == 0 || chance(45)) {
            return having_leaf(group_keys, aggregate_exprs);
        }
        return "(" + having_tree(group_keys, aggregate_exprs, depth - 1) + " " + bool_op() + " " +
               having_tree(group_keys, aggregate_exprs, depth - 1) + ")";
    }

    std::string having_leaf(const std::vector<ColumnRef>& group_keys,
                            const std::vector<AggregateExpr>& aggregate_exprs) {
        const auto expressions = having_expressions(group_keys, aggregate_exprs);
        const auto type = pick(expression_types(expressions));
        const auto typed_expressions = expressions_of_type(expressions, type);
        return pick(typed_expressions).expression + " " + comparison_op_text(random_op()) + " " +
               (chance(70) ? literal(type) : pick(typed_expressions).expression);
    }

    AggregateExpr random_aggregate(const std::vector<ColumnRef>& columns) {
        const auto choice = between(0, 4);
        if (choice == 0) {
            return AggregateExpr{"COUNT(*)", catalog::ColumnType::Int64, false, false};
        }
        if (choice == 1) {
            return AggregateExpr{"COUNT(" + sql_text(pick(columns)) + ")", catalog::ColumnType::Int64, false, false};
        }
        if (choice == 2) {
            const auto argument = sql_text(pick(columns_of_type(columns, catalog::ColumnType::Int64)));
            return AggregateExpr{"SUM(" + argument + ")", catalog::ColumnType::Int64, true, true};
        }
        const auto argument_column = pick(columns);
        const auto argument = sql_text(argument_column);
        if (choice == 3) {
            return AggregateExpr{"MIN(" + argument + ")", argument_column.type, true, true};
        }
        return AggregateExpr{"MAX(" + argument + ")", argument_column.type, true, true};
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
            for (std::size_t column_index = 0; column_index < range.columns.size(); ++column_index) {
                refs.push_back(ColumnRef{range.alias,
                                         range.columns[column_index],
                                         range.types[column_index],
                                         range.nullable[column_index],
                                         range.contains_null[column_index]});
            }
        }
        return refs;
    }

    bool same_column_identity(const ColumnRef& left, const ColumnRef& right) const {
        return left.alias == right.alias && left.column == right.column;
    }

    std::vector<ColumnRef> sample_unique_columns(std::vector<ColumnRef> values, std::size_t count) {
        std::sort(values.begin(), values.end(), [](const ColumnRef& left, const ColumnRef& right) {
            if (left.alias != right.alias) {
                return left.alias < right.alias;
            }
            return left.column < right.column;
        });
        values.erase(std::unique(values.begin(),
                                 values.end(),
                                 [&](const ColumnRef& left, const ColumnRef& right) {
                                     return same_column_identity(left, right);
                                 }),
                     values.end());
        std::shuffle(values.begin(), values.end(), rng_);
        if (values.size() > count) {
            values.resize(count);
        }
        return values;
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

    template <typename T>
    std::vector<T> sample_with_replacement(const std::vector<T>& values, std::size_t count) {
        std::vector<T> sample;
        sample.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            sample.push_back(pick(values));
        }
        return sample;
    }

    bool contains_type(const std::vector<catalog::ColumnType>& types, catalog::ColumnType type) const {
        return std::find(types.begin(), types.end(), type) != types.end();
    }

    std::vector<catalog::ColumnType> column_types(const std::vector<ColumnRef>& columns) const {
        std::vector<catalog::ColumnType> types;
        for (const auto& column : columns) {
            if (!contains_type(types, column.type)) {
                types.push_back(column.type);
            }
        }
        return types;
    }

    std::vector<catalog::ColumnType> common_column_types(const std::vector<ColumnRef>& left,
                                                        const std::vector<ColumnRef>& right) const {
        std::vector<catalog::ColumnType> common;
        for (const auto type : column_types(left)) {
            if (contains_type(column_types(right), type)) {
                common.push_back(type);
            }
        }
        return common;
    }

    std::vector<ColumnRef> columns_of_type(const std::vector<ColumnRef>& columns, catalog::ColumnType type) const {
        std::vector<ColumnRef> typed;
        for (const auto& column : columns) {
            if (column.type == type) {
                typed.push_back(column);
            }
        }
        if (typed.empty()) {
            throw std::logic_error("generator has no columns of requested type");
        }
        return typed;
    }

    std::vector<AggregateExpr> having_expressions(const std::vector<ColumnRef>& group_keys,
                                                  const std::vector<AggregateExpr>& aggregate_exprs) const {
        std::vector<AggregateExpr> expressions;
        expressions.reserve(group_keys.size() + aggregate_exprs.size());
        for (const auto& key : group_keys) {
            expressions.push_back(AggregateExpr{sql_text(key), key.type, key.nullable, key.contains_null});
        }
        expressions.insert(expressions.end(), aggregate_exprs.begin(), aggregate_exprs.end());
        return expressions;
    }

    std::vector<catalog::ColumnType> expression_types(const std::vector<AggregateExpr>& expressions) const {
        std::vector<catalog::ColumnType> types;
        for (const auto& expression : expressions) {
            if (!contains_type(types, expression.type)) {
                types.push_back(expression.type);
            }
        }
        return types;
    }

    std::vector<AggregateExpr> expressions_of_type(const std::vector<AggregateExpr>& expressions,
                                                   catalog::ColumnType type) const {
        std::vector<AggregateExpr> typed;
        for (const auto& expression : expressions) {
            if (expression.type == type) {
                typed.push_back(expression);
            }
        }
        if (typed.empty()) {
            throw std::logic_error("generator has no expressions of requested type");
        }
        return typed;
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

    std::string random_string_value() {
        static const std::vector<std::string> pool{
            "",
            "",
            "a",
            "a",
            "b",
            "b",
            "aa",
            "z",
            "key0",
            "key1",
            "key1",
            "left",
            "right",
        };
        if (chance(88)) {
            return pick(pool);
        }
        return "s" + std::to_string(between(0, 9));
    }

    Cell int_cell(std::int64_t value) const {
        Cell cell;
        cell.type = catalog::ColumnType::Int64;
        cell.is_null = false;
        cell.int_value = value;
        return cell;
    }

    Cell string_cell(std::string value) const {
        Cell cell;
        cell.type = catalog::ColumnType::String;
        cell.is_null = false;
        cell.string_value = std::move(value);
        return cell;
    }

    Cell null_cell(catalog::ColumnType type) const {
        Cell cell;
        cell.type = type;
        cell.is_null = true;
        return cell;
    }

    Cell random_cell(catalog::ColumnType type, bool nullable) {
        if (nullable && chance(22)) {
            return null_cell(type);
        }
        return type == catalog::ColumnType::Int64 ? int_cell(random_value()) : string_cell(random_string_value());
    }

    catalog::ColumnType random_literal_type() {
        return chance(45) ? catalog::ColumnType::String : catalog::ColumnType::Int64;
    }

    std::string literal(catalog::ColumnType type) {
        if (chance(14)) {
            return "NULL";
        }
        if (type == catalog::ColumnType::Int64) {
            return std::to_string(random_value());
        }
        return sql::quote_string_literal(random_string_value());
    }

    bool chance(int percent) { return between(1, 100) <= static_cast<std::size_t>(percent); }

    std::size_t between(std::size_t low, std::size_t high) {
        std::uniform_int_distribution<std::size_t> dist(low, high);
        return dist(rng_);
    }

    template <typename T>
    const T& pick(const std::vector<T>& values) {
        return values[between(0, values.size() - 1)];
    }

    std::uint64_t seed_{0};
    std::mt19937_64 rng_;
    std::size_t next_output_alias_{0};
    std::vector<std::string> used_output_aliases_;
    bool saw_string_join_key_{false};
    bool saw_string_order_key_{false};
    std::size_t inner_join_count_{0};
    std::size_t left_join_count_{0};
    std::size_t right_join_count_{0};
    std::size_t null_key_join_count_{0};
    bool has_scalar_subquery_{false};
    bool has_in_subquery_{false};
    bool has_not_in_subquery_{false};
    bool has_exists_subquery_{false};
    bool has_not_exists_subquery_{false};
    bool has_nested_subquery_{false};
    bool has_subquery_join_{false};
    bool has_subquery_aggregate_{false};
    bool has_null_bearing_subquery_{false};
    bool has_empty_subquery_{false};
    bool has_or_embedded_subquery_{false};
    std::size_t window_definition_count_{0};
    std::size_t row_number_window_count_{0};
    std::size_t rank_window_count_{0};
    std::size_t dense_rank_window_count_{0};
    std::size_t count_window_count_{0};
    std::size_t sum_window_count_{0};
    std::size_t min_window_count_{0};
    std::size_t max_window_count_{0};
    std::size_t partitioned_window_count_{0};
    std::size_t unpartitioned_window_count_{0};
    std::size_t ordered_rank_window_count_{0};
    std::size_t unordered_rank_window_count_{0};
    bool has_null_window_key_{false};
    bool has_string_window_key_{false};
    bool has_string_window_argument_{false};
    bool has_window_outer_order_{false};
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

std::string correlated_sql_for_seed(std::uint64_t seed) {
    switch ((seed - 1) % 5) {
    case 0:
        return "SELECT o.c0 FROM t0 AS o WHERE EXISTS "
               "(SELECT i.c0 FROM t1 AS i WHERE i.c0 = o.c0)";
    case 1:
        return "SELECT o.c0 FROM t0 AS o WHERE NOT EXISTS "
               "(SELECT i.c0 FROM t1 AS i WHERE i.c0 = o.c0)";
    case 2:
        return "SELECT o.c0 FROM t0 AS o WHERE o.c0 IN "
               "(SELECT i.c0 FROM t1 AS i WHERE i.c0 = o.c0)";
    case 3:
        return "SELECT o.c0 FROM t0 AS o WHERE o.c0 = "
               "(SELECT MAX(i.c0) FROM t1 AS i WHERE i.c0 = o.c0)";
    default:
        return "SELECT o.c0 FROM t0 AS o WHERE EXISTS "
               "(SELECT i.c0 FROM t1 AS i WHERE i.c0 = o.c0 OR i.c0 = 0)";
    }
}

bool is_residual_correlation_guard(const plan::LogicalPlan& logical) {
    try {
        (void)plan::lower_to_physical(logical);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()) ==
               "vectorized execution does not support residual correlated subqueries";
    }
    return false;
}

bool verify_correlated_case(std::uint64_t seed,
                            const GeneratedCase& generated,
                            differential::ComparisonStats* stats) {
    const auto sql_text = correlated_sql_for_seed(seed);
    const auto mode = static_cast<std::size_t>((seed - 1) % 5);
    const auto expects_decorrelation = mode < 3;
    try {
        const auto logical = sql::bind_select(sql::parse_select(sql_text), generated.catalog);
        const auto oracle = execution::execute_interpreted(logical, generated.catalog);
        ++stats->execution_path_count;

        const auto rewritten = optimizer::rewrite_to_fixpoint(logical, optimizer::default_rules());
        const auto rewritten_oracle = execution::execute_interpreted(rewritten.plan, generated.catalog);
        ++stats->execution_path_count;
        if (!differential::same_batch(oracle, rewritten_oracle) ||
            !is_residual_correlation_guard(rewritten.plan)) {
            std::cerr << "correlated standalone oracle/guard divergence\nseed: " << seed
                      << "\nsql: " << sql_text << "\n" << generated.catalog_dump;
            return false;
        }
        ++stats->execution_path_count;
        ++stats->residual_correlated_guard_paths;

        optimizer::Memo memo;
        const auto root = memo.insert(logical);
        const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        const auto alternatives =
            memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{256, 4096});
        stats->alternative_count = alternatives.plans.size();
        stats->max_group_expression_count = alternatives.max_group_expression_count;
        stats->hit_expression_bound = alternatives.hit_expression_bound;
        stats->hit_plan_bound = alternatives.hit_plan_bound;
        stats->correlated_exists_to_semi_firings = std::count(
            explored.fired_rules.begin(), explored.fired_rules.end(), "CorrelatedExistsToSemiJoinRule");
        stats->correlated_not_exists_to_anti_firings = std::count(
            explored.fired_rules.begin(), explored.fired_rules.end(), "CorrelatedNotExistsToAntiJoinRule");
        stats->correlated_in_to_semi_firings = std::count(
            explored.fired_rules.begin(), explored.fired_rules.end(), "CorrelatedInToSemiJoinRule");
        const auto new_firings = stats->correlated_exists_to_semi_firings +
                                 stats->correlated_not_exists_to_anti_firings +
                                 stats->correlated_in_to_semi_firings;
        if ((expects_decorrelation && new_firings == 0) || (!expects_decorrelation && new_firings != 0)) {
            std::cerr << "correlated fuzzer rule guard mismatch\nseed: " << seed << "\nsql: " << sql_text
                      << "\ntrace: " << differential::format_trace(explored.fired_rules)
                      << "\nmemo:\n" << memo.dump();
            return false;
        }

        std::size_t native_vectorized_paths = 0;
        for (const auto& alternative : alternatives.plans) {
            const auto alternative_oracle = execution::execute_interpreted(alternative, generated.catalog);
            ++stats->execution_path_count;
            if (!differential::same_batch(oracle, alternative_oracle)) {
                std::cerr << "correlated memo oracle divergence\nseed: " << seed << "\nsql: " << sql_text
                          << "\nalternative:\n" << plan::to_string(alternative) << "\n";
                return false;
            }
            if (is_residual_correlation_guard(alternative)) {
                ++stats->execution_path_count;
                ++stats->residual_correlated_guard_paths;
                continue;
            }
            const auto vectorized = execution::execute_vectorized(alternative, generated.catalog);
            ++stats->execution_path_count;
            if (!differential::same_batch(alternative_oracle, vectorized)) {
                std::cerr << "correlated native vectorized divergence\nseed: " << seed << "\nsql: " << sql_text
                          << "\nalternative:\n" << plan::to_string(alternative) << "\n";
                return false;
            }
            ++native_vectorized_paths;
        }
        if ((expects_decorrelation && native_vectorized_paths == 0) ||
            (!expects_decorrelation && native_vectorized_paths != 0)) {
            std::cerr << "correlated fuzzer physical-path guard mismatch\nseed: " << seed
                      << "\nsql: " << sql_text << "\nnative_paths: " << native_vectorized_paths << "\n";
            return false;
        }

        const auto best = memo.extract_best(root, generated.catalog);
        const auto best_oracle = execution::execute_interpreted(best, generated.catalog);
        ++stats->execution_path_count;
        if (!differential::same_batch(oracle, best_oracle)) {
            std::cerr << "correlated extract_best oracle divergence\nseed: " << seed << "\nsql: " << sql_text
                      << "\nbest:\n" << plan::to_string(best) << "\n";
            return false;
        }
        if (is_residual_correlation_guard(best)) {
            ++stats->execution_path_count;
            ++stats->residual_correlated_guard_paths;
        } else {
            const auto best_vectorized = execution::execute_vectorized(best, generated.catalog);
            ++stats->execution_path_count;
            if (!differential::same_batch(best_oracle, best_vectorized)) {
                return false;
            }
        }
        return true;
    } catch (const std::exception& error) {
        std::cerr << "correlated verification setup failed\nseed: " << seed << "\nsql: " << sql_text << "\n"
                  << generated.catalog_dump << "exception: " << error.what() << "\n";
        return false;
    }
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
    std::size_t string_columns = 0;
    std::size_t string_literal_queries = 0;
    std::size_t string_join_key_queries = 0;
    std::size_t string_group_key_queries = 0;
    std::size_t string_distinct_output_queries = 0;
    std::size_t string_order_key_queries = 0;
    std::size_t inner_joins = 0;
    std::size_t left_joins = 0;
    std::size_t right_joins = 0;
    std::size_t null_key_joins = 0;
    std::size_t mixed_inner_outer_chain_queries = 0;
    std::size_t scalar_subquery_queries = 0;
    std::size_t in_subquery_queries = 0;
    std::size_t not_in_subquery_queries = 0;
    std::size_t exists_subquery_queries = 0;
    std::size_t not_exists_subquery_queries = 0;
    std::size_t nested_subquery_queries = 0;
    std::size_t subquery_join_queries = 0;
    std::size_t subquery_aggregate_queries = 0;
    std::size_t null_bearing_subquery_queries = 0;
    std::size_t empty_subquery_queries = 0;
    std::size_t or_embedded_subquery_queries = 0;
    std::size_t window_queries = 0;
    std::size_t window_definitions = 0;
    std::size_t row_number_windows = 0;
    std::size_t rank_windows = 0;
    std::size_t dense_rank_windows = 0;
    std::size_t count_windows = 0;
    std::size_t sum_windows = 0;
    std::size_t min_windows = 0;
    std::size_t max_windows = 0;
    std::size_t partitioned_windows = 0;
    std::size_t unpartitioned_windows = 0;
    std::size_t ordered_rank_windows = 0;
    std::size_t unordered_rank_windows = 0;
    std::size_t null_window_key_queries = 0;
    std::size_t string_window_key_queries = 0;
    std::size_t string_window_argument_queries = 0;
    std::size_t joined_window_queries = 0;
    std::size_t grouped_window_queries = 0;
    std::size_t having_window_queries = 0;
    std::size_t multi_window_queries = 0;
    std::size_t window_outer_order_queries = 0;
    std::size_t exists_to_semi_firings = 0;
    std::size_t not_exists_to_anti_firings = 0;
    std::size_t in_to_semi_firings = 0;
    std::size_t correlated_exists_to_semi_firings = 0;
    std::size_t correlated_not_exists_to_anti_firings = 0;
    std::size_t correlated_in_to_semi_firings = 0;
    std::size_t residual_correlated_guard_paths = 0;
    std::vector<std::size_t> correlated_mode_queries(5, 0);
    std::size_t left_join_to_inner_firings = 0;
    std::size_t join_commute_firings = 0;
    std::size_t join_associate_firings = 0;
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
        string_columns += generated.string_column_count;
        string_literal_queries += generated.has_string_literal ? 1 : 0;
        string_join_key_queries += generated.has_string_join_key ? 1 : 0;
        string_group_key_queries += generated.has_string_group_key ? 1 : 0;
        string_distinct_output_queries += generated.has_string_distinct_output ? 1 : 0;
        string_order_key_queries += generated.has_string_order_key ? 1 : 0;
        inner_joins += generated.inner_join_count;
        left_joins += generated.left_join_count;
        right_joins += generated.right_join_count;
        null_key_joins += generated.null_key_join_count;
        mixed_inner_outer_chain_queries += generated.has_mixed_inner_outer_chain ? 1 : 0;
        scalar_subquery_queries += generated.has_scalar_subquery ? 1 : 0;
        in_subquery_queries += generated.has_in_subquery ? 1 : 0;
        not_in_subquery_queries += generated.has_not_in_subquery ? 1 : 0;
        exists_subquery_queries += generated.has_exists_subquery ? 1 : 0;
        not_exists_subquery_queries += generated.has_not_exists_subquery ? 1 : 0;
        nested_subquery_queries += generated.has_nested_subquery ? 1 : 0;
        subquery_join_queries += generated.has_subquery_join ? 1 : 0;
        subquery_aggregate_queries += generated.has_subquery_aggregate ? 1 : 0;
        null_bearing_subquery_queries += generated.has_null_bearing_subquery ? 1 : 0;
        empty_subquery_queries += generated.has_empty_subquery ? 1 : 0;
        or_embedded_subquery_queries += generated.has_or_embedded_subquery ? 1 : 0;
        window_queries += generated.has_window ? 1 : 0;
        window_definitions += generated.window_definition_count;
        row_number_windows += generated.row_number_window_count;
        rank_windows += generated.rank_window_count;
        dense_rank_windows += generated.dense_rank_window_count;
        count_windows += generated.count_window_count;
        sum_windows += generated.sum_window_count;
        min_windows += generated.min_window_count;
        max_windows += generated.max_window_count;
        partitioned_windows += generated.partitioned_window_count;
        unpartitioned_windows += generated.unpartitioned_window_count;
        ordered_rank_windows += generated.ordered_rank_window_count;
        unordered_rank_windows += generated.unordered_rank_window_count;
        null_window_key_queries += generated.has_null_window_key ? 1 : 0;
        string_window_key_queries += generated.has_string_window_key ? 1 : 0;
        string_window_argument_queries += generated.has_string_window_argument ? 1 : 0;
        joined_window_queries += generated.has_joined_window_input ? 1 : 0;
        grouped_window_queries += generated.has_grouped_window_input ? 1 : 0;
        having_window_queries += generated.has_having_window_input ? 1 : 0;
        multi_window_queries += generated.has_multiple_windows ? 1 : 0;
        window_outer_order_queries += generated.has_window_outer_order ? 1 : 0;
        exists_to_semi_firings += stats.exists_to_semi_firings;
        not_exists_to_anti_firings += stats.not_exists_to_anti_firings;
        in_to_semi_firings += stats.in_to_semi_firings;
        left_join_to_inner_firings += stats.left_join_to_inner_firings;
        join_commute_firings += stats.join_commute_firings;
        join_associate_firings += stats.join_associate_firings;
        hit_expression_bound = hit_expression_bound || stats.hit_expression_bound;
        hit_plan_bound = hit_plan_bound || stats.hit_plan_bound;

        differential::ComparisonStats correlated_stats;
        if (!verify_correlated_case(seed, generated, &correlated_stats)) {
            return 1;
        }
        ++queries;
        ++correlated_mode_queries.at(static_cast<std::size_t>((seed - 1) % 5));
        alternatives += correlated_stats.alternative_count;
        execution_paths += correlated_stats.execution_path_count;
        accepted_error_paths += correlated_stats.accepted_error_path_count;
        max_group_expression_count =
            std::max(max_group_expression_count, correlated_stats.max_group_expression_count);
        correlated_exists_to_semi_firings += correlated_stats.correlated_exists_to_semi_firings;
        correlated_not_exists_to_anti_firings +=
            correlated_stats.correlated_not_exists_to_anti_firings;
        correlated_in_to_semi_firings += correlated_stats.correlated_in_to_semi_firings;
        residual_correlated_guard_paths += correlated_stats.residual_correlated_guard_paths;
        hit_expression_bound = hit_expression_bound || correlated_stats.hit_expression_bound;
        hit_plan_bound = hit_plan_bound || correlated_stats.hit_plan_bound;
    }

    if (seeds.size() == kDefaultSeedCount &&
        (left_joins == 0 || right_joins == 0 || null_key_joins == 0 || mixed_inner_outer_chain_queries == 0 ||
         left_join_to_inner_firings == 0 || join_associate_firings == 0 || scalar_subquery_queries == 0 ||
         in_subquery_queries == 0 || not_in_subquery_queries == 0 || exists_subquery_queries == 0 ||
         not_exists_subquery_queries == 0 || nested_subquery_queries == 0 || subquery_join_queries == 0 ||
         subquery_aggregate_queries == 0 || null_bearing_subquery_queries == 0 || empty_subquery_queries == 0 ||
         or_embedded_subquery_queries == 0 || exists_to_semi_firings == 0 || not_exists_to_anti_firings == 0 ||
         in_to_semi_firings == 0 || correlated_exists_to_semi_firings == 0 ||
         correlated_not_exists_to_anti_firings == 0 || correlated_in_to_semi_firings == 0 ||
         residual_correlated_guard_paths == 0 || window_queries == 0 || window_definitions == 0 ||
         row_number_windows == 0 || rank_windows == 0 || dense_rank_windows == 0 || count_windows == 0 ||
         sum_windows == 0 || min_windows == 0 || max_windows == 0 || partitioned_windows == 0 ||
         unpartitioned_windows == 0 || ordered_rank_windows == 0 || unordered_rank_windows == 0 ||
         null_window_key_queries == 0 || string_window_key_queries == 0 ||
         string_window_argument_queries == 0 || joined_window_queries == 0 || grouped_window_queries == 0 ||
         having_window_queries == 0 || multi_window_queries == 0 || window_outer_order_queries == 0 ||
         std::any_of(correlated_mode_queries.begin(), correlated_mode_queries.end(), [](auto count) {
             return count == 0;
         }))) {
        std::cerr << "default fuzz corpus missed required outer-join or subquery coverage\n"
                  << "left_joins=" << left_joins << " right_joins=" << right_joins
                  << " null_key_joins=" << null_key_joins
                  << " mixed_inner_outer_chain_queries=" << mixed_inner_outer_chain_queries
                  << " left_join_to_inner_firings=" << left_join_to_inner_firings
                  << " join_associate_firings=" << join_associate_firings
                  << " scalar_subquery_queries=" << scalar_subquery_queries
                  << " in_subquery_queries=" << in_subquery_queries
                  << " not_in_subquery_queries=" << not_in_subquery_queries
                  << " exists_subquery_queries=" << exists_subquery_queries
                  << " not_exists_subquery_queries=" << not_exists_subquery_queries
                  << " nested_subquery_queries=" << nested_subquery_queries
                  << " subquery_join_queries=" << subquery_join_queries
                  << " subquery_aggregate_queries=" << subquery_aggregate_queries
                  << " null_bearing_subquery_queries=" << null_bearing_subquery_queries
                  << " empty_subquery_queries=" << empty_subquery_queries
                  << " or_embedded_subquery_queries=" << or_embedded_subquery_queries
                  << " exists_to_semi_firings=" << exists_to_semi_firings
                  << " not_exists_to_anti_firings=" << not_exists_to_anti_firings
                  << " in_to_semi_firings=" << in_to_semi_firings << "\n";
        std::cerr << " correlated_exists_to_semi_firings=" << correlated_exists_to_semi_firings
                  << " correlated_not_exists_to_anti_firings=" << correlated_not_exists_to_anti_firings
                  << " correlated_in_to_semi_firings=" << correlated_in_to_semi_firings
                  << " residual_correlated_guard_paths=" << residual_correlated_guard_paths << "\n";
        std::cerr << " window_queries=" << window_queries
                  << " window_definitions=" << window_definitions
                  << " row_number_windows=" << row_number_windows
                  << " rank_windows=" << rank_windows
                  << " dense_rank_windows=" << dense_rank_windows
                  << " count_windows=" << count_windows
                  << " sum_windows=" << sum_windows
                  << " min_windows=" << min_windows
                  << " max_windows=" << max_windows
                  << " partitioned_windows=" << partitioned_windows
                  << " unpartitioned_windows=" << unpartitioned_windows
                  << " ordered_rank_windows=" << ordered_rank_windows
                  << " unordered_rank_windows=" << unordered_rank_windows
                  << " null_window_key_queries=" << null_window_key_queries
                  << " string_window_key_queries=" << string_window_key_queries
                  << " string_window_argument_queries=" << string_window_argument_queries
                  << " joined_window_queries=" << joined_window_queries
                  << " grouped_window_queries=" << grouped_window_queries
                  << " having_window_queries=" << having_window_queries
                  << " multi_window_queries=" << multi_window_queries
                  << " window_outer_order_queries=" << window_outer_order_queries << "\n";
        return 1;
    }

    std::cout << "sql_fuzz_differential coverage: seeds=" << seeds.size() << " queries=" << queries
              << " alternatives=" << alternatives << " execution_paths=" << execution_paths
              << " accepted_error_paths=" << accepted_error_paths
              << " max_group_expressions=" << max_group_expression_count
              << " string_columns=" << string_columns
              << " string_literal_queries=" << string_literal_queries
              << " string_join_key_queries=" << string_join_key_queries
              << " string_group_key_queries=" << string_group_key_queries
              << " string_distinct_output_queries=" << string_distinct_output_queries
              << " string_order_key_queries=" << string_order_key_queries
              << " inner_joins=" << inner_joins
              << " left_joins=" << left_joins
              << " right_joins=" << right_joins
              << " null_key_joins=" << null_key_joins
              << " mixed_inner_outer_chain_queries=" << mixed_inner_outer_chain_queries
              << " scalar_subquery_queries=" << scalar_subquery_queries
              << " in_subquery_queries=" << in_subquery_queries
              << " not_in_subquery_queries=" << not_in_subquery_queries
              << " exists_subquery_queries=" << exists_subquery_queries
              << " not_exists_subquery_queries=" << not_exists_subquery_queries
              << " nested_subquery_queries=" << nested_subquery_queries
              << " subquery_join_queries=" << subquery_join_queries
              << " subquery_aggregate_queries=" << subquery_aggregate_queries
              << " null_bearing_subquery_queries=" << null_bearing_subquery_queries
              << " empty_subquery_queries=" << empty_subquery_queries
              << " or_embedded_subquery_queries=" << or_embedded_subquery_queries
              << " window_queries=" << window_queries
              << " window_definitions=" << window_definitions
              << " row_number_windows=" << row_number_windows
              << " rank_windows=" << rank_windows
              << " dense_rank_windows=" << dense_rank_windows
              << " count_windows=" << count_windows
              << " sum_windows=" << sum_windows
              << " min_windows=" << min_windows
              << " max_windows=" << max_windows
              << " partitioned_windows=" << partitioned_windows
              << " unpartitioned_windows=" << unpartitioned_windows
              << " ordered_rank_windows=" << ordered_rank_windows
              << " unordered_rank_windows=" << unordered_rank_windows
              << " null_window_key_queries=" << null_window_key_queries
              << " string_window_key_queries=" << string_window_key_queries
              << " string_window_argument_queries=" << string_window_argument_queries
              << " joined_window_queries=" << joined_window_queries
              << " grouped_window_queries=" << grouped_window_queries
              << " having_window_queries=" << having_window_queries
              << " multi_window_queries=" << multi_window_queries
              << " window_outer_order_queries=" << window_outer_order_queries
              << " exists_to_semi_firings=" << exists_to_semi_firings
              << " not_exists_to_anti_firings=" << not_exists_to_anti_firings
              << " in_to_semi_firings=" << in_to_semi_firings
              << " correlated_exists_queries=" << correlated_mode_queries[0]
              << " correlated_not_exists_queries=" << correlated_mode_queries[1]
              << " correlated_in_queries=" << correlated_mode_queries[2]
              << " correlated_scalar_queries=" << correlated_mode_queries[3]
              << " correlated_blocked_or_queries=" << correlated_mode_queries[4]
              << " correlated_exists_to_semi_firings=" << correlated_exists_to_semi_firings
              << " correlated_not_exists_to_anti_firings=" << correlated_not_exists_to_anti_firings
              << " correlated_in_to_semi_firings=" << correlated_in_to_semi_firings
              << " residual_correlated_guard_paths=" << residual_correlated_guard_paths
              << " left_join_to_inner_firings=" << left_join_to_inner_firings
              << " join_commute_firings=" << join_commute_firings
              << " join_associate_firings=" << join_associate_firings
              << " hit_expression_bound=" << (hit_expression_bound ? "yes" : "no")
              << " hit_plan_bound=" << (hit_plan_bound ? "yes" : "no") << "\n";
    return 0;
}
