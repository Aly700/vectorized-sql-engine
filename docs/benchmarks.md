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

## Post-NULL re-run (phase 16, 2026-07-07)

Same methodology and machine as above. After the NULL arc (validity masks,
3VL predicates, null-aware compiled kernels, NULL-skipping aggregates), the
vectorized engine still wins every measured workload; correctness cross-checks
all match. Checksums differ from earlier tables because the workload
generator's data pool changed with nullable-column support.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0x95ba2cbd0a720591 | 14.693 | 15.226 | 8.968 | 9.410 | 1.618x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x8031b34dc1b05c00 | 15.690 | 16.686 | 13.097 | 13.732 | 1.215x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xd4bb87b7e1354cb7 | 21.613 | 22.297 | 16.044 | 16.845 | 1.324x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0xd6eff9218be6da07 | 5167.567 | 9372.338 | 14.520 | 16.350 | 573.226x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xd3f7b04105d9e1d2 | 23.970 | 29.962 | 12.783 | 14.442 | 2.075x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xf5d6639180dcf25a | 88.744 | 114.908 | 23.906 | 28.040 | 4.098x |
| sort | fact=200000 | match rows=1000 checksum=0x42e9485c298e1347 | 156.938 | 164.179 | 40.445 | 41.304 | 3.975x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0x76e0a28dbe61b35d | 3203.757 | 3230.453 | 25.906 | 26.636 | 121.281x |

Interpreted medians rose relative to phase 15 (machine load and null-aware
row paths); vectorized retains its advantage on all workloads, so the phase-15
conclusion stands after the NULL arc.

## Phase 17b String Baseline (2026-07-07)

Same optimized build command and correctness-checksum-before-timing discipline
as above. This phase added one string workload as a baseline for future string
performance work; no string optimization was attempted beyond the correctness
implementation.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| string_group_by | string_fact=120000,groups=16 | match rows=16 checksum=0x8ce7a59f7d8fbc52 | 24.914 | 25.003 | 9.233 | 9.402 | 2.659x |

## Phase 19 Boolean-Mask Filter Kernels (2026-07-07)

Same optimized build command and correctness-checksum-before-timing discipline
as above. The before table is a fresh Release run on branch
`phase19-mask-kernels` before replacing vectorized Filter's row-wise predicate
loop with paired `is_true`/`is_known` byte masks. The after table is the same
benchmark after the mask kernel change.

### Before Mask Kernels

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0x95ba2cbd0a720591 | 17.782 | 19.128 | 9.504 | 9.673 | 1.977x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x8031b34dc1b05c00 | 18.358 | 18.408 | 10.706 | 10.750 | 1.712x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xd4bb87b7e1354cb7 | 25.115 | 27.095 | 16.769 | 16.934 | 1.600x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0xd6eff9218be6da07 | 5750.826 | 5974.024 | 16.244 | 16.560 | 360.757x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xd3f7b04105d9e1d2 | 34.703 | 35.383 | 14.413 | 14.717 | 2.404x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xf5d6639180dcf25a | 85.207 | 87.990 | 25.445 | 26.379 | 3.336x |
| sort | fact=200000 | match rows=1000 checksum=0x42e9485c298e1347 | 379.770 | 400.650 | 61.695 | 65.693 | 6.099x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0x76e0a28dbe61b35d | 4191.192 | 4295.031 | 30.618 | 31.568 | 136.055x |
| string_group_by | string_fact=120000,groups=16 | match rows=16 checksum=0x8ce7a59f7d8fbc52 | 24.497 | 26.179 | 9.345 | 9.420 | 2.779x |

### After Mask Kernels

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | fact=200000 | match rows=2000 checksum=0x95ba2cbd0a720591 | 17.498 | 17.831 | 8.168 | 8.221 | 2.169x |
| scan_filter_10pct | fact=200000 | match rows=20000 checksum=0x8031b34dc1b05c00 | 19.853 | 21.639 | 9.627 | 9.821 | 2.203x |
| scan_filter_50pct | fact=200000 | match rows=100000 checksum=0xd4bb87b7e1354cb7 | 26.551 | 27.165 | 15.158 | 15.243 | 1.782x |
| multi_key_hash_join | left=100000,right=256 | match rows=100000 checksum=0xd6eff9218be6da07 | 5723.554 | 5891.121 | 15.159 | 15.459 | 381.077x |
| aggregate_few_groups | fact=200000,groups=8 | match rows=8 checksum=0xd3f7b04105d9e1d2 | 33.617 | 33.845 | 14.343 | 14.937 | 2.266x |
| aggregate_many_groups | fact=200000,groups=50000 | match rows=50000 checksum=0xf5d6639180dcf25a | 85.401 | 86.095 | 25.174 | 25.455 | 3.382x |
| sort | fact=200000 | match rows=1000 checksum=0x42e9485c298e1347 | 376.568 | 379.790 | 63.152 | 64.445 | 5.893x |
| join_group_sort_limit | left=120000,right=256 | match rows=20 checksum=0x76e0a28dbe61b35d | 4167.945 | 4406.484 | 28.676 | 29.492 | 149.414x |
| string_group_by | string_fact=120000,groups=16 | match rows=16 checksum=0x8ce7a59f7d8fbc52 | 24.609 | 24.827 | 9.478 | 9.578 | 2.592x |

### Phase 19 Readout

The targeted scan/filter vectorized medians improved at every measured
selectivity: 1 percent selectivity moved from 9.673 ms to 8.221 ms, 10 percent
from 10.750 ms to 9.821 ms, and 50 percent from 16.934 ms to 15.243 ms. The
median speedups versus the interpreted oracle rose from 1.977x to 2.169x,
1.712x to 2.203x, and 1.600x to 1.782x respectively.

No hybrid threshold was added because the simple mask path improved all three
scan/filter selectivities in this run. Non-target vectorized medians were flat
to favorable except for small movements in `aggregate_few_groups` (14.717 ms to
14.937 ms) and `string_group_by` (9.420 ms to 9.578 ms). Those workloads do not
exercise vectorized Filter in this benchmark set, so they are reported as
run-to-run noise rather than attributed wins. Correctness checksums stayed
byte-identical for every workload.

## Phase 21b Selective Semi Join (2026-07-10)

Same optimized build, five-repetition min/median, deterministic-data, and
checksum-before-timing methodology as above. The SQL source is an IN query over
the 100,000-row `join_left` table. The benchmark harness requires and times the
memo-produced equi-SemiJoin alternative, whose subquery scans 256 rows and
selects one key (`r.payload = 10000`). The interpreted and vectorized engines
execute the same decorrelated plan.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| selective_in_semi_join | outer=100000,subquery_source=256,subquery_rows=1 | match rows=6250 checksum=0x72a0d9bc69515c7e | 11.122 | 11.865 | 5.131 | 5.157 | 2.301x |

The checksum match pins left-only output and duplicate-preserving Semi
semantics before timing. The vectorized lookup-only hash implementation is
2.301x faster on median than the interpreted left-row-major nested-loop oracle
for this selective workload.

## Phase 22b Window Baseline (2026-07-10)

Same optimized build, five-repetition min/median, deterministic-data, and
checksum-before-timing methodology as above. This adds one correctness-first
Window workload over the 200,000-row `fact` table with 100 partitions:
`ROW_NUMBER()` ordered inside each partition plus `SUM(value)` replicated over
the partition. No Window-specific performance tuning was attempted in this
phase.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| window_row_number_sum | fact=200000,partitions=100 | match rows=200000 checksum=0x1f5b8dc9ce10a38c | 725.385 | 732.916 | 108.253 | 109.029 | 6.722x |

## Phase 23 Running Window Frame (2026-07-13)

Same optimized build, five-repetition min/median, deterministic-data, and
checksum-before-timing methodology as above. This workload measures the
default cumulative `RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW` frame
for `SUM(value) OVER (PARTITION BY bucket ORDER BY sort_key)` across the
200,000-row `fact` table and its 100 partitions.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| window_running_sum | fact=200000,partitions=100 | match rows=200000 checksum=0xefdfa729da68f560 | 704.578 | 711.366 | 102.685 | 105.883 | 6.718x |

## Phase 24 Selective NULL-Aware Anti Join (2026-07-16)

Same optimized build, five-repetition min/median, deterministic-data, and
checksum-before-timing methodology as above. The SQL is a correlated `NOT IN`
over the 100,000-row `join_left` table. Its equality correlation admits the
Phase 24 rewrite, and the benchmark harness fails loudly unless memo costing
chooses `NullAwareAntiJoin`. The right source has 256 rows; its local filter
retains 16 membership values in one correlation bucket. Because the left key
cycle ends partway through its final 256-row block, exactly 6,256 rows belong
to that bucket and 93,744 survive.

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| selective_not_in_null_aware_anti | outer=100000,right_source=256,right_rows=16,survivors=93744 | match rows=93744 checksum=0x3aa4b8448334dc4d | 193.038 | 196.903 | 11.762 | 12.043 | 16.350x |

The checksum match is performed before timing and pins the left-only output
and correlated candidate-set semantics. The vectorized grouped hash kernel is
16.350x faster on median than the interpreted left-row-major semantic oracle
for this selective workload.

## Yard Routes (2026-09-01)

`yard_export` (commit `1058974ea747`) enumerates every memo alternative for four
workings, prices each with `estimate_cost`, and times each through vectorized
execution with the same Release build, five-repetition min/median, deterministic
data, and checksum-before-timing methodology as above. The chosen row is the
`extract_best` winner; every alternative must reproduce its bag checksum. The
interpreted timing is for the chosen plan only.

### coupling

```sql
SELECT l.payload AS left_payload, r.payload AS right_payload FROM join_left AS l JOIN join_right AS r ON l.k1 = r.k1 AND l.k2 = r.k2
```

memo: 4 groups, 1 iterations, fired rules: JoinCommuteRule

| alternative | plan | total cost | vectorized min ms | vectorized median ms |
|---:|---|---:|---:|---:|
| 0 (chosen) | `Project[left_payload=col(l.payload), right_payload=col(r.payload)]   Join[col(l.k1) = col(r.k1) AND col(l.k2) = col(r.k2)]     Scan[join_left AS l]     Scan[join_right AS r]` | 200512.00 | 15.060 | 15.291 |
| 1 | `Project[left_payload=col(l.payload), right_payload=col(r.payload)]   Join[col(l.k1) = col(r.k1) AND col(l.k2) = col(r.k2)]     Scan[join_right AS r]     Scan[join_left AS l]` | 200512.00 | 15.361 | 15.564 |

interpreted (chosen plan): 5707.965 / 5737.780 ms; rows=100000 checksum=0xd6eff9218be6da07 bag checksum=0xe60844c222869ce8

### hump-and-bowl

```sql
SELECT l.group_id AS group_id, COUNT(*) AS n, MAX(r.measure) AS top FROM e2e_left AS l JOIN e2e_right AS r ON l.k = r.k WHERE l.filter_key < 80 GROUP BY l.group_id ORDER BY n DESC, group_id ASC LIMIT 20
```

memo: 11 groups, 2 iterations, fired rules: JoinCommuteRule, FilterIntoJoinRule, JoinCommuteRule

| alternative | plan | total cost | vectorized min ms | vectorized median ms |
|---:|---|---:|---:|---:|
| 0 (chosen) | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(l.filter_key) < lit(80)]           Join[col(l.k) = col(r.k)]             Scan[e2e_left AS l]             Scan[e2e_right AS r]` | 241486.08 | 27.881 | 27.934 |
| 1 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(l.filter_key) < lit(80)]           Join[col(l.k) = col(r.k)]             Scan[e2e_right AS r]             Scan[e2e_left AS l]` | 241486.08 | 25.992 | 26.419 |
| 2 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Filter[col(l.filter_key) < lit(80)]             Scan[e2e_left AS l]           Scan[e2e_right AS r]` | 283072.00 | 24.780 | 24.812 |
| 3 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Filter[col(l.filter_key) < lit(80)]             Scan[e2e_left AS l]` | 283072.00 | 23.322 | 23.467 |

interpreted (chosen plan): 4175.083 / 4200.672 ms; rows=20 checksum=0xae466c9b0f225c78 bag checksum=0x5ad6701e8dbf32d5

### three-trains

```sql
SELECT l.group_id AS group_id, COUNT(*) AS n, MAX(r.measure) AS top FROM e2e_left AS l JOIN e2e_right AS r ON l.k = r.k JOIN e2e_class AS c ON l.group_id = c.group_id WHERE c.tier = 1 GROUP BY l.group_id ORDER BY n DESC, group_id ASC LIMIT 20
```

memo: 22 groups, 3 iterations, fired rules: JoinCommuteRule, JoinCommuteRule, JoinAssociateRule, FilterIntoJoinRule, JoinAssociateRule, JoinCommuteRule, FilterIntoJoinRule, JoinCommuteRule, JoinCommuteRule, JoinAssociateRule, JoinAssociateRule, JoinCommuteRule, FilterIntoJoinRule, JoinCommuteRule, JoinCommuteRule, JoinCommuteRule

| alternative | plan | total cost | vectorized min ms | vectorized median ms |
|---:|---|---:|---:|---:|
| 0 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.group_id) = col(c.group_id)]             Join[col(l.k) = col(r.k)]               Scan[e2e_left AS l]               Scan[e2e_right AS r]             Scan[e2e_class AS c]` | 241224.68 | 39.776 | 39.897 |
| 1 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.group_id) = col(c.group_id)]             Join[col(l.k) = col(r.k)]               Scan[e2e_right AS r]               Scan[e2e_left AS l]             Scan[e2e_class AS c]` | 241224.68 | 34.654 | 34.667 |
| 2 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.group_id) = col(c.group_id)]             Scan[e2e_class AS c]             Join[col(l.k) = col(r.k)]               Scan[e2e_left AS l]               Scan[e2e_right AS r]` | 241224.68 | 32.942 | 32.990 |
| 3 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.group_id) = col(c.group_id)]             Scan[e2e_class AS c]             Join[col(l.k) = col(r.k)]               Scan[e2e_right AS r]               Scan[e2e_left AS l]` | 241224.68 | 30.710 | 30.725 |
| 4 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.k) = col(r.k)]             Scan[e2e_right AS r]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_left AS l]               Scan[e2e_class AS c]` | 241096.68 | 36.685 | 36.737 |
| 5 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.k) = col(r.k)]             Scan[e2e_right AS r]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_class AS c]               Scan[e2e_left AS l]` | 241096.68 | 32.799 | 32.900 |
| 6 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.k) = col(r.k)]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_left AS l]               Scan[e2e_class AS c]             Scan[e2e_right AS r]` | 241096.68 | 39.762 | 39.825 |
| 7 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Filter[col(c.tier) = lit(1)]           Join[col(l.k) = col(r.k)]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_class AS c]               Scan[e2e_left AS l]             Scan[e2e_right AS r]` | 241096.68 | 29.400 | 29.418 |
| 8 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.group_id) = col(c.group_id)]           Join[col(l.k) = col(r.k)]             Scan[e2e_left AS l]             Scan[e2e_right AS r]           Filter[col(c.tier) = lit(1)]             Scan[e2e_class AS c]` | 241109.48 | 27.592 | 27.625 |
| 9 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.group_id) = col(c.group_id)]           Join[col(l.k) = col(r.k)]             Scan[e2e_right AS r]             Scan[e2e_left AS l]           Filter[col(c.tier) = lit(1)]             Scan[e2e_class AS c]` | 241109.48 | 24.504 | 24.548 |
| 10 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.group_id) = col(c.group_id)]           Filter[col(c.tier) = lit(1)]             Scan[e2e_class AS c]           Join[col(l.k) = col(r.k)]             Scan[e2e_left AS l]             Scan[e2e_right AS r]` | 241109.48 | 25.881 | 25.902 |
| 11 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.group_id) = col(c.group_id)]           Filter[col(c.tier) = lit(1)]             Scan[e2e_class AS c]           Join[col(l.k) = col(r.k)]             Scan[e2e_right AS r]             Scan[e2e_left AS l]` | 241109.48 | 23.560 | 23.607 |
| 12 (chosen) | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Join[col(l.group_id) = col(c.group_id)]             Scan[e2e_left AS l]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]` | 240866.28 | 16.510 | 16.563 |
| 13 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Join[col(l.group_id) = col(c.group_id)]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]             Scan[e2e_left AS l]` | 240866.28 | 15.168 | 15.200 |
| 14 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Join[col(l.group_id) = col(c.group_id)]             Scan[e2e_left AS l]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]           Scan[e2e_right AS r]` | 240866.28 | 17.779 | 17.896 |
| 15 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Join[col(l.group_id) = col(c.group_id)]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]             Scan[e2e_left AS l]           Scan[e2e_right AS r]` | 240866.28 | 14.535 | 14.738 |
| 16 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Filter[col(c.tier) = lit(1)]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_left AS l]               Scan[e2e_class AS c]` | 240981.48 | 26.074 | 26.108 |
| 17 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Filter[col(c.tier) = lit(1)]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_class AS c]               Scan[e2e_left AS l]` | 240981.48 | 21.237 | 21.333 |
| 18 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Join[col(l.group_id) = col(c.group_id)]             Scan[e2e_left AS l]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]` | 240866.28 | 16.506 | 16.541 |
| 19 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Scan[e2e_right AS r]           Join[col(l.group_id) = col(c.group_id)]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]             Scan[e2e_left AS l]` | 240866.28 | 15.267 | 15.569 |
| 20 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Filter[col(c.tier) = lit(1)]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_left AS l]               Scan[e2e_class AS c]           Scan[e2e_right AS r]` | 240981.48 | 26.842 | 26.997 |
| 21 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Filter[col(c.tier) = lit(1)]             Join[col(l.group_id) = col(c.group_id)]               Scan[e2e_class AS c]               Scan[e2e_left AS l]           Scan[e2e_right AS r]` | 240981.48 | 20.516 | 20.581 |
| 22 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Join[col(l.group_id) = col(c.group_id)]             Scan[e2e_left AS l]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]           Scan[e2e_right AS r]` | 240866.28 | 17.074 | 17.142 |
| 23 | `Limit[20]   Sort[col(n) DESC, col(group_id) ASC]     Project[group_id=col(l.group_id), n=col(COUNT(*)), top=col(MAX(r.measure))]       Aggregate[group_keys=[col(l.group_id)], aggregates=[COUNT(*), MAX(r.measure)=col(r.measure)]]         Join[col(l.k) = col(r.k)]           Join[col(l.group_id) = col(c.group_id)]             Filter[col(c.tier) = lit(1)]               Scan[e2e_class AS c]             Scan[e2e_left AS l]           Scan[e2e_right AS r]` | 240866.28 | 14.410 | 14.423 |

interpreted (chosen plan): 1591.984 / 1601.294 ms; rows=20 checksum=0x7e53943623dc6b01 bag checksum=0x8f1c966523a83a50

### special

```sql
SELECT l.payload AS payload FROM join_left AS l WHERE l.k1 IN (SELECT r.k1 FROM join_right AS r WHERE r.payload = 10000)
```

memo: 7 groups, 1 iterations, fired rules: InToSemiJoinRule

| alternative | plan | total cost | vectorized min ms | vectorized median ms |
|---:|---|---:|---:|---:|
| 0 (chosen) | `Project[payload=col(l.payload)]   Filter[col(l.k1) IN subquery(IN subquery at position 59)]     Subquery[IN subquery at position 59]       Project[r.k1=col(r.k1)]         Filter[col(r.payload) = lit(10000)]           Scan[join_right AS r]     Scan[join_left AS l]` | 200512.00 | 3.063 | 3.151 |
| 1 | `Project[payload=col(l.payload)]   SemiJoin[col(l.k1) = col(r.k1)]     Scan[join_left AS l]     Project[r.k1=col(r.k1)]       Filter[col(r.payload) = lit(10000)]         Scan[join_right AS r]` | 200537.60 | 5.250 | 5.272 |

interpreted (chosen plan): 8.902 / 9.068 ms; rows=6250 checksum=0x72a0d9bc69515c7e bag checksum=0x5b144f1cb20c62ff

### hump workloads

| workload | predicate | kept | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms |
|---|---|---:|---:|---:|---:|---:|
| scan_filter_1pct | `bucket = 7` | 2000 | 17.409 | 17.525 | 7.904 | 7.949 |
| scan_filter_10pct | `decile = 3` | 20000 | 18.751 | 18.816 | 9.067 | 9.089 |
| scan_filter_50pct | `half = 1` | 100000 | 25.338 | 25.427 | 14.235 | 14.249 |

### three_way_route in sql_bench

The three-trains working is also registered in `sql_bench` as `three_way_route`, timed on the
query as bound (the first alternative above, before the memo reorders it). Same Release build,
five repetitions, checksum before timing:

| workload | rows | correctness | interpreted min ms | interpreted median ms | vectorized min ms | vectorized median ms | median speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| three_way_route | left=120000,right=256,class=128 | match rows=20 checksum=0x7e53943623dc6b01 | 5829.809 | 5843.328 | 39.476 | 39.516 | 147.871x |
