#include "bench_common.hpp"

#include <iostream>

namespace {

using namespace bench;

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
    {
        const std::string sql =
            "SELECT l.payload AS payload FROM join_left AS l WHERE l.k1 NOT IN ("
            "SELECT r.k1 FROM join_right AS r "
            "WHERE r.k2 = l.k2 AND r.payload < 10016)";
        workloads.push_back(Workload{"selective_not_in_null_aware_anti",
                                     "outer=100000,right_source=256,right_rows=16,survivors=93744",
                                     sql,
                                     bind_best_null_aware_anti_query(catalog, sql)});
    }
    add("aggregate_few_groups",
        "fact=200000,groups=8",
        "SELECT group_few, SUM(value) AS total, COUNT(*) AS n "
        "FROM fact GROUP BY group_few ORDER BY group_few");
    add("aggregate_many_groups",
        "fact=200000,groups=50000",
        "SELECT group_many, SUM(value) AS total "
        "FROM fact GROUP BY group_many ORDER BY group_many");
    add("window_row_number_sum",
        "fact=200000,partitions=100",
        "SELECT bucket, value, "
        "ROW_NUMBER() OVER (PARTITION BY bucket ORDER BY sort_key) AS rn, "
        "SUM(value) OVER (PARTITION BY bucket) AS bucket_sum "
        "FROM fact");
    add("window_running_sum",
        "fact=200000,partitions=100",
        "SELECT bucket, sort_key, value, "
        "SUM(value) OVER (PARTITION BY bucket ORDER BY sort_key) AS running_sum "
        "FROM fact");
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
    add("three_way_route",
        "left=120000,right=256,class=128",
        "SELECT l.group_id AS group_id, COUNT(*) AS n, MAX(r.measure) AS top "
        "FROM e2e_left AS l JOIN e2e_right AS r ON l.k = r.k "
        "JOIN e2e_class AS c ON l.group_id = c.group_id "
        "WHERE c.tier = 1 "
        "GROUP BY l.group_id ORDER BY n DESC, group_id ASC LIMIT 20");
    add("string_group_by",
        "string_fact=120000,groups=16",
        "SELECT k, COUNT(*) AS n, MIN(label) AS first_label, MAX(label) AS last_label "
        "FROM string_fact GROUP BY k ORDER BY k ASC");

    return workloads;
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
