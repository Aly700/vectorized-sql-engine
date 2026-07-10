#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "sql/binder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kFactRows = 200'000;
constexpr std::size_t kJoinLeftRows = 100'000;
constexpr std::size_t kJoinRightRows = 256;
constexpr std::size_t kEndToEndLeftRows = 120'000;
constexpr std::size_t kEndToEndRightRows = 256;
constexpr std::size_t kStringRows = 120'000;
constexpr std::size_t kRepetitions = 5;

std::uint64_t benchmark_sink = 0;

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

struct Workload {
    std::string name;
    std::string rows;
    std::string sql;
    plan::LogicalPlan plan;
};

struct BenchmarkResult {
    std::string name;
    std::string rows;
    ResultSignature signature;
    Timings interpreted;
    Timings vectorized;
};

void mix_byte(std::uint64_t& hash, std::uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ULL;
}

void mix_u64(std::uint64_t& hash, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        mix_byte(hash, static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }
}

void mix_string(std::uint64_t& hash, const std::string& value) {
    mix_u64(hash, value.size());
    for (unsigned char ch : value) {
        mix_byte(hash, ch);
    }
}

ResultSignature signature_for(const storage::ColumnarBatch& batch) {
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

bool operator==(const ResultSignature& left, const ResultSignature& right) {
    return left.row_count == right.row_count && left.column_count == right.column_count &&
           left.checksum == right.checksum;
}

std::string hex_checksum(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

void add_column(storage::ColumnarBatch& batch, std::string name, std::vector<std::int64_t> values) {
    storage::Int64Column column;
    for (auto value : values) {
        column.append(value);
    }
    batch.add_column(std::move(name), std::move(column));
}

void add_string_column(storage::ColumnarBatch& batch, std::string name, std::vector<std::string> values) {
    storage::StringColumn column;
    for (auto& value : values) {
        column.append(std::move(value));
    }
    batch.add_column(std::move(name), std::move(column));
}

storage::ColumnarBatch make_fact_table() {
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

storage::ColumnarBatch make_join_left_table() {
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

storage::ColumnarBatch make_join_right_table() {
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

storage::ColumnarBatch make_e2e_left_table() {
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

storage::ColumnarBatch make_e2e_right_table() {
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

storage::ColumnarBatch make_string_fact_table() {
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

execution::Catalog make_catalog() {
    execution::Catalog catalog;
    catalog.add_table("fact", make_fact_table());
    catalog.add_table("join_left", make_join_left_table());
    catalog.add_table("join_right", make_join_right_table());
    catalog.add_table("e2e_left", make_e2e_left_table());
    catalog.add_table("e2e_right", make_e2e_right_table());
    catalog.add_table("string_fact", make_string_fact_table());
    return catalog;
}

plan::LogicalPlan bind_query(const execution::Catalog& catalog, const std::string& sql) {
    return sql::bind_select(sql::parse_select(sql), catalog);
}

plan::LogicalPlan bind_decorrelated_semi_query(const execution::Catalog& catalog, const std::string& sql) {
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

std::vector<Workload> make_workloads(const execution::Catalog& catalog) {
    std::vector<Workload> workloads;
    auto add = [&](std::string name, std::string rows, std::string sql) {
        workloads.push_back(Workload{std::move(name), std::move(rows), sql, bind_query(catalog, sql)});
    };

    add("scan_filter_1pct",
        "fact=200000",
        "SELECT a FROM fact WHERE bucket = 7");
    add("scan_filter_10pct",
        "fact=200000",
        "SELECT a FROM fact WHERE decile = 3");
    add("scan_filter_50pct",
        "fact=200000",
        "SELECT a FROM fact WHERE half = 1");
    add("multi_key_hash_join",
        "left=100000,right=256",
        "SELECT l.payload AS left_payload, r.payload AS right_payload "
        "FROM join_left AS l JOIN join_right AS r ON l.k1 = r.k1 AND l.k2 = r.k2");
    {
        const std::string sql =
            "SELECT l.payload AS payload FROM join_left AS l "
            "WHERE l.k1 IN (SELECT r.k1 FROM join_right AS r WHERE r.payload = 10000)";
        workloads.push_back(Workload{"selective_in_semi_join",
                                     "outer=100000,subquery_source=256,subquery_rows=1",
                                     sql,
                                     bind_decorrelated_semi_query(catalog, sql)});
    }
    add("aggregate_few_groups",
        "fact=200000,groups=8",
        "SELECT group_few, SUM(value) AS total, COUNT(*) AS n "
        "FROM fact GROUP BY group_few ORDER BY group_few");
    add("aggregate_many_groups",
        "fact=200000,groups=50000",
        "SELECT group_many, SUM(value) AS total "
        "FROM fact GROUP BY group_many ORDER BY group_many");
    add("sort",
        "fact=200000",
        "SELECT a, value FROM fact ORDER BY sort_key, a LIMIT 1000");
    add("join_group_sort_limit",
        "left=120000,right=256",
        "SELECT l.group_id AS group_id, SUM(r.measure) AS total "
        "FROM e2e_left AS l JOIN e2e_right AS r ON l.k = r.k "
        "WHERE l.filter_key < 80 "
        "GROUP BY l.group_id HAVING SUM(r.measure) > 0 "
        "ORDER BY total DESC LIMIT 20");
    add("string_group_by",
        "string_fact=120000,groups=16",
        "SELECT k, COUNT(*) AS n, MIN(label) AS first_label, MAX(label) AS last_label "
        "FROM string_fact GROUP BY k ORDER BY k ASC");

    return workloads;
}

Timings measure(const plan::LogicalPlan& plan,
                const execution::Catalog& catalog,
                const std::string& engine_name,
                storage::ColumnarBatch (*execute)(const plan::LogicalPlan&, const execution::Catalog&)) {
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

BenchmarkResult run_workload(const Workload& workload, const execution::Catalog& catalog) {
    const auto interpreted_result = execution::execute_interpreted(workload.plan, catalog);
    const auto vectorized_result = execution::execute_vectorized(workload.plan, catalog);
    const auto interpreted_signature = signature_for(interpreted_result);
    const auto vectorized_signature = signature_for(vectorized_result);
    if (!(interpreted_signature == vectorized_signature)) {
        std::cerr << "correctness mismatch in " << workload.name << "\n"
                  << "  interpreted rows=" << interpreted_signature.row_count
                  << " columns=" << interpreted_signature.column_count
                  << " checksum=" << hex_checksum(interpreted_signature.checksum) << "\n"
                  << "  vectorized rows=" << vectorized_signature.row_count
                  << " columns=" << vectorized_signature.column_count
                  << " checksum=" << hex_checksum(vectorized_signature.checksum) << "\n";
        throw std::runtime_error("benchmark correctness cross-check failed");
    }

    return BenchmarkResult{
        workload.name,
        workload.rows,
        interpreted_signature,
        measure(workload.plan, catalog, "interpreted", execution::execute_interpreted),
        measure(workload.plan, catalog, "vectorized", execution::execute_vectorized),
    };
}

void print_header() {
    std::cout << "sql_bench deterministic benchmark\n";
    std::cout << "seeds: fact=0x5eed000000000001; other tables arithmetic deterministic\n";
    std::cout << "repetitions: " << kRepetitions << "; timer: std::chrono::steady_clock; report: min/median ms\n";
    std::cout << "build: use Release/-O2, e.g. cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release "
                 "-DCMAKE_CXX_FLAGS_RELEASE=\"-O2 -DNDEBUG\" && "
                 "cmake --build build-release --target sql_bench\n\n";
    std::cout << "| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | "
                 "vectorized median ms | median speedup |\n";
    std::cout << "|---|---:|---|---:|---:|---:|---:|---:|\n";
}

void print_result(const BenchmarkResult& result) {
    const auto speedup = result.vectorized.median_ms == 0.0 ? 0.0 : result.interpreted.median_ms / result.vectorized.median_ms;
    std::cout << "| " << result.name
              << " | " << result.rows
              << " | match rows=" << result.signature.row_count
              << " checksum=" << hex_checksum(result.signature.checksum)
              << " | " << std::fixed << std::setprecision(3) << result.interpreted.min_ms
              << " | " << result.interpreted.median_ms
              << " | " << result.vectorized.min_ms
              << " | " << result.vectorized.median_ms
              << " | " << speedup << "x |\n";
}

} // namespace

int main() {
    try {
        const auto catalog = make_catalog();
        const auto workloads = make_workloads(catalog);
        print_header();
        for (const auto& workload : workloads) {
            print_result(run_workload(workload, catalog));
        }
        if (benchmark_sink == 0x0123456789abcdefULL) {
            std::cout << "sink=" << benchmark_sink << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "sql_bench failed: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
