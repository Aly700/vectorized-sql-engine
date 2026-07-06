# Benchmarks

This page records the Phase 14 measurement arc. `sql_bench` is a standalone
benchmark executable and is intentionally not registered with CTest; the test
gate remains timing-free.

## Build And Run

Benchmarks must be built optimized. The numbers below used this exact
invocation:

```bash
cmake -S . -B build-bench-o2 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build build-bench-o2 --target sql_bench
./build-bench-o2/sql_bench
```

The default correctness build,
`cmake -S . -B build && cmake --build build`, leaves `CMAKE_BUILD_TYPE` empty
with this generator. Use it for the smoke/CTest gate, not for performance
claims.

## Methodology

- Machine: MacBook Pro Mac14,7, Apple M2, 8 cores, 16 GB memory.
- OS/toolchain: macOS 26.5.1 build 25F80, AppleClang 17.0.0, CMake 3.31.6.
- Timer: `std::chrono::steady_clock` inside the benchmark binary only.
- Repetitions: 5 per engine per workload, reporting min and median ms.
- Plans: each SQL query is parsed and bound once; interpreted and vectorized
  execution run the same bound logical plan.
- Correctness: each workload runs both engines once before timing and prints a
  row-count plus checksum signature. Timing aborts on mismatch.
- Data generation: deterministic fixed data. The `fact` table uses seed
  `0x5eed000000000001`; the join dimension tables use arithmetic generation.
  Large scan/aggregation/sort tables are 200,000 rows. Join workloads use a
  100,000-row or 120,000-row left table with a 256-row dimension table so the
  interpreted nested-loop oracle remains runnable.

## Baseline Before No-Copy Scan

Baseline source was a temporary archive of `HEAD` before the shared-column
storage change, with the benchmark harness copied in.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0xbed96a798e222bcd | 12.142 | 12.241 | 15.798 | 15.974 | 0.766x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x45a16c604fed9716 | 13.132 | 13.264 | 17.190 | 17.870 | 0.742x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xcebbf422ce3b4b37 | 17.559 | 17.938 | 24.017 | 24.200 | 0.741x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0x8c117d14db53b4f5 | 4173.485 | 4324.153 | 26.794 | 27.310 | 158.338x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xbca846a1d8d0e49c | 22.375 | 22.786 | 26.466 | 26.680 | 0.854x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xede4362884d0d436 | 65.250 | 66.093 | 38.590 | 39.080 | 1.691x |
| sort | fact=200000 | match rows=1000 checksum=0x3fffff63abea6d61 | 145.270 | 146.227 | 156.855 | 160.828 | 0.909x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0xace3431b834cd54f | 3070.797 | 3072.025 | 45.634 | 45.851 | 67.000x |

## After Shared Column Storage

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0xbed96a798e222bcd | 12.035 | 12.308 | 15.168 | 15.287 | 0.805x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x45a16c604fed9716 | 13.870 | 14.074 | 16.440 | 16.923 | 0.832x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xcebbf422ce3b4b37 | 18.441 | 18.506 | 22.936 | 23.490 | 0.788x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0x8c117d14db53b4f5 | 4275.677 | 4302.640 | 29.756 | 30.435 | 141.370x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xbca846a1d8d0e49c | 23.253 | 23.447 | 25.936 | 26.331 | 0.890x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xede4362884d0d436 | 68.606 | 71.053 | 41.912 | 47.996 | 1.480x |
| sort | fact=200000 | match rows=1000 checksum=0x3fffff63abea6d61 | 149.087 | 151.055 | 158.457 | 159.234 | 0.949x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0xace3431b834cd54f | 3091.061 | 3093.569 | 44.194 | 44.370 | 69.722x |

## Readout

The scan-copy fix directly helped scan/filter median vectorized timings:
1 percent selectivity improved from 15.974 ms to 15.287 ms, 10 percent from
17.870 ms to 16.923 ms, and 50 percent from 24.200 ms to 23.490 ms. The
vectorized scan/filter path is still slower than the interpreted oracle because
it still pays selection-vector traversal plus final materialization overhead,
while the oracle's simple row loop is compact for this narrow projection.

The vectorized engine earns its name on join-heavy workloads: the multi-key
join is 141.370x faster on median after the change, and the joined/grouped/
ordered query is 69.722x faster. Those wins come from the vectorized hash join
versus the oracle's nested-loop join, not from scan qualification.

Aggregation is mixed. Many-group aggregation still favors the vectorized path,
but the after run was slower than the baseline run for that workload
(39.080 ms to 47.996 ms vectorized median). Few-group aggregation and sort
remain slightly slower than the oracle in these measurements. No benchmark
story is massaged: scan qualification improved the targeted scan/filter wart,
but not every vectorized operator wins yet.

## Phase 15 Batch Kernels Before/After

The before table below is a fresh Release run on branch `phase15-kernels`
before expression pre-resolution and tight vectorized kernels were implemented.

### Before Expression Pre-Resolution

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0xbed96a798e222bcd | 11.868 | 11.876 | 15.038 | 15.242 | 0.779x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x45a16c604fed9716 | 13.127 | 13.160 | 16.388 | 16.570 | 0.794x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xcebbf422ce3b4b37 | 18.182 | 18.402 | 22.600 | 22.659 | 0.812x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0x8c117d14db53b4f5 | 4250.389 | 4272.688 | 28.805 | 29.016 | 147.253x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xbca846a1d8d0e49c | 22.565 | 22.718 | 24.674 | 24.801 | 0.916x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xede4362884d0d436 | 64.072 | 64.749 | 38.739 | 39.354 | 1.645x |
| sort | fact=200000 | match rows=1000 checksum=0x3fffff63abea6d61 | 148.910 | 150.770 | 159.343 | 159.526 | 0.945x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0xace3431b834cd54f | 3071.000 | 3097.848 | 43.980 | 44.778 | 69.183x |

### After Expression Pre-Resolution

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0xbed96a798e222bcd | 11.966 | 12.153 | 8.324 | 8.525 | 1.426x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x45a16c604fed9716 | 13.258 | 13.470 | 9.462 | 9.671 | 1.393x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xcebbf422ce3b4b37 | 18.241 | 18.375 | 14.138 | 14.233 | 1.291x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0x8c117d14db53b4f5 | 4281.207 | 4301.099 | 10.848 | 11.477 | 374.769x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xbca846a1d8d0e49c | 22.243 | 22.622 | 11.857 | 11.873 | 1.905x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xede4362884d0d436 | 65.198 | 65.713 | 20.292 | 21.126 | 3.111x |
| sort | fact=200000 | match rows=1000 checksum=0x3fffff63abea6d61 | 149.147 | 151.055 | 37.289 | 37.380 | 4.041x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0xace3431b834cd54f | 3107.523 | 3129.509 | 23.770 | 23.878 | 131.061x |

### Phase 15 Readout

Expression pre-resolution and exact output reservation moved the scan/filter
path from slower-than-oracle to faster-than-oracle: 1 percent selectivity
improved from 0.779x to 1.426x, 10 percent from 0.794x to 1.393x, and 50
percent from 0.812x to 1.291x median speedup. This directly addresses the
Phase 14 finding that vectorized scan/filter lost because scalar evaluation and
materialization were doing per-row column-name map lookups.

The same pre-resolution benefited the other measured vectorized paths. Hash
join median speedup rose from 147.253x to 374.769x; few-group aggregation moved
from 0.916x to 1.905x; many-group aggregation from 1.645x to 3.111x; sort from
0.945x to 4.041x; and the joined/grouped/sorted/limited workload from 69.183x
to 131.061x. No measured workload regressed materially in this run.
