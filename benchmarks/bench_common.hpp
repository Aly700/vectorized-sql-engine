#pragma once

#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "sql/ast.hpp"
#include "sql/binder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bench {

constexpr std::size_t kFactRows = 200'000;
constexpr std::size_t kJoinLeftRows = 100'000;
constexpr std::size_t kJoinRightRows = 256;
constexpr std::size_t kEndToEndLeftRows = 120'000;
constexpr std::size_t kEndToEndRightRows = 256;
constexpr std::size_t kClassRows = 128;
constexpr std::size_t kStringRows = 120'000;
constexpr std::size_t kRepetitions = 5;

inline std::uint64_t benchmark_sink = 0;

struct SplitMix64 {
    std::uint64_t state;

    explicit SplitMix64(std::uint64_t seed) : state(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
};

struct ResultSignature {
    std::size_t row_count{0};
    std::size_t column_count{0};
    std::uint64_t checksum{0};
};

struct Timings {
    double min_ms{0.0};
    double median_ms{0.0};
};

inline void mix_byte(std::uint64_t& hash, std::uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ULL;
}

inline void mix_u64(std::uint64_t& hash, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        mix_byte(hash, static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }
}

inline void mix_string(std::uint64_t& hash, const std::string& value) {
    mix_u64(hash, value.size());
    for (unsigned char ch : value) {
        mix_byte(hash, ch);
    }
}

inline ResultSignature signature_for(const storage::ColumnarBatch& batch) {
    std::uint64_t hash = 1469598103934665603ULL;
    mix_u64(hash, batch.row_count());
    mix_u64(hash, batch.column_names().size());
    for (const auto& name : batch.column_names()) {
        mix_string(hash, name);
    }
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        for (const auto& name : batch.column_names()) {
            if (batch.column_type(name) == catalog::ColumnType::Int64) {
                const auto& column = batch.column(name);
                mix_byte(hash, column.is_null(row) ? 0 : 1);
                if (!column.is_null(row)) {
                    mix_u64(hash, static_cast<std::uint64_t>(column.at(row)));
                }
            } else {
                const auto& column = batch.string_column(name);
                mix_byte(hash, column.is_null(row) ? 0 : 1);
                if (!column.is_null(row)) {
                    mix_string(hash, column.at(row));
                }
            }
        }
    }
    return ResultSignature{batch.row_count(), batch.column_names().size(), hash};
}

inline bool operator==(const ResultSignature& left, const ResultSignature& right) {
    return left.row_count == right.row_count && left.column_count == right.column_count &&
           left.checksum == right.checksum;
}

inline std::string hex_checksum(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

inline void add_column(storage::ColumnarBatch& batch,
                       std::string name,
                       std::vector<std::int64_t> values) {
    storage::Int64Column column;
    for (auto value : values) {
        column.append(value);
    }
    batch.add_column(std::move(name), std::move(column));
}

inline void add_string_column(storage::ColumnarBatch& batch,
                              std::string name,
                              std::vector<std::string> values) {
    storage::StringColumn column;
    for (auto& value : values) {
        column.append(std::move(value));
    }
    batch.add_column(std::move(name), std::move(column));
}

inline storage::ColumnarBatch make_fact_table() {
    SplitMix64 rng(0x5eed000000000001ULL);
    std::vector<std::int64_t> a;
    std::vector<std::int64_t> bucket;
    std::vector<std::int64_t> decile;
    std::vector<std::int64_t> half;
    std::vector<std::int64_t> group_few;
    std::vector<std::int64_t> group_many;
    std::vector<std::int64_t> sort_key;
    std::vector<std::int64_t> value;
    a.reserve(kFactRows);
    bucket.reserve(kFactRows);
    decile.reserve(kFactRows);
    half.reserve(kFactRows);
    group_few.reserve(kFactRows);
    group_many.reserve(kFactRows);
    sort_key.reserve(kFactRows);
    value.reserve(kFactRows);

    for (std::size_t row = 0; row < kFactRows; ++row) {
        a.push_back(static_cast<std::int64_t>(row));
        bucket.push_back(static_cast<std::int64_t>(row % 100));
        decile.push_back(static_cast<std::int64_t>(row % 10));
        half.push_back(static_cast<std::int64_t>(row % 2));
        group_few.push_back(static_cast<std::int64_t>(row % 8));
        group_many.push_back(static_cast<std::int64_t>(row % 50'000));
        sort_key.push_back(static_cast<std::int64_t>(rng.next() % kFactRows));
        value.push_back(static_cast<std::int64_t>(rng.next() % 1'000));
    }

    storage::ColumnarBatch batch;
    add_column(batch, "a", std::move(a));
    add_column(batch, "bucket", std::move(bucket));
    add_column(batch, "decile", std::move(decile));
    add_column(batch, "half", std::move(half));
    add_column(batch, "group_few", std::move(group_few));
    add_column(batch, "group_many", std::move(group_many));
    add_column(batch, "sort_key", std::move(sort_key));
    add_column(batch, "value", std::move(value));
    return batch;
}

inline storage::ColumnarBatch make_join_left_table() {
    std::vector<std::int64_t> k1;
    std::vector<std::int64_t> k2;
    std::vector<std::int64_t> payload;
    k1.reserve(kJoinLeftRows);
    k2.reserve(kJoinLeftRows);
    payload.reserve(kJoinLeftRows);
    for (std::size_t row = 0; row < kJoinLeftRows; ++row) {
        k1.push_back(static_cast<std::int64_t>(row % 16));
        k2.push_back(static_cast<std::int64_t>((row / 16) % 16));
        payload.push_back(static_cast<std::int64_t>((row * 17) % 1'000'003));
    }

    storage::ColumnarBatch batch;
    add_column(batch, "k1", std::move(k1));
    add_column(batch, "k2", std::move(k2));
    add_column(batch, "payload", std::move(payload));
    return batch;
}

inline storage::ColumnarBatch make_join_right_table() {
    std::vector<std::int64_t> k1;
    std::vector<std::int64_t> k2;
    std::vector<std::int64_t> payload;
    k1.reserve(kJoinRightRows);
    k2.reserve(kJoinRightRows);
    payload.reserve(kJoinRightRows);
    for (std::size_t row = 0; row < kJoinRightRows; ++row) {
        k1.push_back(static_cast<std::int64_t>(row % 16));
        k2.push_back(static_cast<std::int64_t>(row / 16));
        payload.push_back(static_cast<std::int64_t>(10'000 + row));
    }

    storage::ColumnarBatch batch;
    add_column(batch, "k1", std::move(k1));
    add_column(batch, "k2", std::move(k2));
    add_column(batch, "payload", std::move(payload));
    return batch;
}

inline storage::ColumnarBatch make_e2e_left_table() {
    std::vector<std::int64_t> k;
    std::vector<std::int64_t> group_id;
    std::vector<std::int64_t> filter_key;
    k.reserve(kEndToEndLeftRows);
    group_id.reserve(kEndToEndLeftRows);
    filter_key.reserve(kEndToEndLeftRows);
    for (std::size_t row = 0; row < kEndToEndLeftRows; ++row) {
        k.push_back(static_cast<std::int64_t>(row % kEndToEndRightRows));
        group_id.push_back(static_cast<std::int64_t>((row / 3) % 128));
        filter_key.push_back(static_cast<std::int64_t>(row % 100));
    }

    storage::ColumnarBatch batch;
    add_column(batch, "k", std::move(k));
    add_column(batch, "group_id", std::move(group_id));
    add_column(batch, "filter_key", std::move(filter_key));
    return batch;
}

inline storage::ColumnarBatch make_e2e_right_table() {
    std::vector<std::int64_t> k;
    std::vector<std::int64_t> measure;
    k.reserve(kEndToEndRightRows);
    measure.reserve(kEndToEndRightRows);
    for (std::size_t row = 0; row < kEndToEndRightRows; ++row) {
        k.push_back(static_cast<std::int64_t>(row));
        measure.push_back(static_cast<std::int64_t>(1 + (row % 97)));
    }

    storage::ColumnarBatch batch;
    add_column(batch, "k", std::move(k));
    add_column(batch, "measure", std::move(measure));
    return batch;
}

inline storage::ColumnarBatch make_e2e_class_table() {
    std::vector<std::int64_t> group_id;
    std::vector<std::int64_t> tier;
    group_id.reserve(kClassRows);
    tier.reserve(kClassRows);
    for (std::size_t row = 0; row < kClassRows; ++row) {
        group_id.push_back(static_cast<std::int64_t>(row));
        tier.push_back(static_cast<std::int64_t>(row % 4));
    }
    storage::ColumnarBatch batch;
    add_column(batch, "group_id", std::move(group_id));
    add_column(batch, "tier", std::move(tier));
    return batch;
}

inline storage::ColumnarBatch make_string_fact_table() {
    static const std::vector<std::string> key_pool{
        "",
        "alpha",
        "beta",
        "gamma",
        "delta",
        "key000",
        "key001",
        "key002",
        "key003",
        "key004",
        "key005",
        "key006",
        "key007",
        "key008",
        "key009",
        "omega",
    };

    std::vector<std::string> k;
    std::vector<std::string> label;
    k.reserve(kStringRows);
    label.reserve(kStringRows);
    for (std::size_t row = 0; row < kStringRows; ++row) {
        k.push_back(key_pool[row % key_pool.size()]);
        label.push_back("label" + std::to_string((row * 17) % 4096));
    }

    storage::ColumnarBatch batch;
    add_string_column(batch, "k", std::move(k));
    add_string_column(batch, "label", std::move(label));
    return batch;
}

inline execution::Catalog make_catalog() {
    execution::Catalog catalog;
    catalog.add_table("fact", make_fact_table());
    catalog.add_table("join_left", make_join_left_table());
    catalog.add_table("join_right", make_join_right_table());
    catalog.add_table("e2e_left", make_e2e_left_table());
    catalog.add_table("e2e_right", make_e2e_right_table());
    catalog.add_table("e2e_class", make_e2e_class_table());
    catalog.add_table("string_fact", make_string_fact_table());
    return catalog;
}

inline plan::LogicalPlan bind_query(const execution::Catalog& catalog, const std::string& sql) {
    return sql::bind_select(sql::parse_select(sql), catalog);
}

inline plan::LogicalPlan bind_decorrelated_semi_query(const execution::Catalog& catalog,
                                                       const std::string& sql) {
    const auto logical = bind_query(catalog, sql);
    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    if (!explored.reached_fixpoint) {
        throw std::logic_error("benchmark semi-join memo exploration did not reach fixpoint");
    }
    const auto alternatives =
        memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 1024});
    for (const auto& alternative : alternatives.plans) {
        if (plan::to_string(alternative).find("SemiJoin[") != std::string::npos) {
            return alternative;
        }
    }
    throw std::logic_error("benchmark IN query did not produce a SemiJoin alternative");
}

inline plan::LogicalPlan bind_best_null_aware_anti_query(const execution::Catalog& catalog,
                                                          const std::string& sql) {
    const auto logical = bind_query(catalog, sql);
    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    if (!explored.reached_fixpoint) {
        throw std::logic_error("benchmark NULL-aware anti memo exploration did not reach fixpoint");
    }
    const auto best = memo.extract_best(root, catalog);
    if (plan::to_string(best).find("NullAwareAntiJoin[") == std::string::npos) {
        throw std::logic_error("benchmark NOT IN query did not choose its NullAwareAnti alternative");
    }
    return best;
}

using ExecuteFn = storage::ColumnarBatch (*)(const plan::LogicalPlan&, const execution::Catalog&);

inline Timings measure(const plan::LogicalPlan& plan,
                       const execution::Catalog& catalog,
                       const std::string& engine_name,
                       ExecuteFn execute) {
    std::vector<double> durations;
    durations.reserve(kRepetitions);
    for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        auto result = execute(plan, catalog);
        const auto end = std::chrono::steady_clock::now();
        const auto signature = signature_for(result);
        benchmark_sink ^= signature.checksum + signature.row_count + signature.column_count + repetition;
        durations.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }

    if (durations.size() != kRepetitions) {
        throw std::logic_error(engine_name + " timing loop did not run all repetitions");
    }
    std::sort(durations.begin(), durations.end());
    return Timings{durations.front(), durations.at(durations.size() / 2)};
}

} // namespace bench
