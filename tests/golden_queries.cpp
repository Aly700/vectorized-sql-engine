#include "execution/interpreter.hpp"
#include "sql/ast.hpp"
#include "sql/binder.hpp"
#include "sql/errors.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using ExpectedCell = std::optional<std::int64_t>;

struct ExpectedResult {
    std::vector<std::string> columns;
    std::vector<std::vector<ExpectedCell>> rows;
};

enum class ErrorKind { Parse, Bind, Runtime };

struct ExpectedError {
    ErrorKind kind;
    std::size_t position;
    std::string message;
};

struct GoldenQuery {
    std::string name;
    std::string sql;
    ExpectedResult result;
    bool expects_error{false};
    ExpectedError error{ErrorKind::Parse, 0, ""};
};

execution::Catalog make_catalog() {
    storage::Int64Column a;
    for (auto value : {1, 2, 3, 4}) {
        a.append(value);
    }

    storage::Int64Column b;
    for (auto value : {10, 20, 20, 40}) {
        b.append(value);
    }

    storage::Int64Column c;
    for (auto value : {5, 6, 7, 8}) {
        c.append(value);
    }

    storage::ColumnarBatch batch;
    batch.add_column("a", a);
    batch.add_column("b", b);
    batch.add_column("c", c);

    execution::Catalog catalog;
    catalog.add_table("t", batch);

    storage::Int64Column nullable_k;
    nullable_k.append(1);
    nullable_k.append_null();
    nullable_k.append(2);
    nullable_k.append_null();
    storage::Int64Column nullable_v;
    nullable_v.append(10);
    nullable_v.append(20);
    nullable_v.append_null();
    nullable_v.append(30);
    storage::ColumnarBatch nullable;
    nullable.add_column("k", std::move(nullable_k));
    nullable.add_column("v", std::move(nullable_v));
    catalog.add_table("nullable", std::move(nullable));

    storage::Int64Column agg_g;
    agg_g.append_null();
    agg_g.append_null();
    agg_g.append(1);
    agg_g.append(1);
    agg_g.append(2);
    agg_g.append(2);
    agg_g.append_null();
    storage::Int64Column agg_h;
    for (auto value : {1, 1, 1, 2, 1, 1, 2}) {
        agg_h.append(value);
    }
    storage::Int64Column agg_x;
    agg_x.append_null();
    agg_x.append(10);
    agg_x.append_null();
    agg_x.append(5);
    agg_x.append_null();
    agg_x.append_null();
    agg_x.append(20);
    storage::ColumnarBatch agg_null;
    agg_null.add_column("g", std::move(agg_g));
    agg_null.add_column("h", std::move(agg_h));
    agg_null.add_column("x", std::move(agg_x));
    catalog.add_table("agg_null", std::move(agg_null));

    storage::Int64Column j1_k;
    j1_k.append(1);
    j1_k.append_null();
    j1_k.append(2);
    j1_k.append_null();
    storage::Int64Column j1_v;
    for (auto value : {10, 20, 30, 40}) {
        j1_v.append(value);
    }
    storage::ColumnarBatch j1;
    j1.add_column("k", std::move(j1_k));
    j1.add_column("v", std::move(j1_v));
    catalog.add_table("j1", std::move(j1));

    storage::Int64Column j2_k;
    j2_k.append_null();
    j2_k.append(1);
    j2_k.append(2);
    j2_k.append_null();
    storage::Int64Column j2_w;
    for (auto value : {100, 101, 102, 103}) {
        j2_w.append(value);
    }
    storage::ColumnarBatch j2;
    j2.add_column("k", std::move(j2_k));
    j2.add_column("w", std::move(j2_w));
    catalog.add_table("j2", std::move(j2));

    storage::Int64Column dup_k;
    for (auto value : {1, 1, 2, 1, 2}) {
        dup_k.append(value);
    }
    storage::Int64Column dup_v;
    for (auto value : {10, 10, 20, 11, 20}) {
        dup_v.append(value);
    }
    storage::ColumnarBatch dup;
    dup.add_column("k", std::move(dup_k));
    dup.add_column("v", std::move(dup_v));
    catalog.add_table("dup", std::move(dup));

    storage::Int64Column t1_a;
    for (auto value : {1, 2, 2}) {
        t1_a.append(value);
    }
    storage::Int64Column t1_b;
    for (auto value : {10, 20, 30}) {
        t1_b.append(value);
    }
    storage::ColumnarBatch t1;
    t1.add_column("a", std::move(t1_a));
    t1.add_column("b", std::move(t1_b));
    catalog.add_table("t1", std::move(t1));

    storage::Int64Column t2_a;
    for (auto value : {2, 2, 3}) {
        t2_a.append(value);
    }
    storage::Int64Column t2_c;
    for (auto value : {200, 201, 300}) {
        t2_c.append(value);
    }
    storage::ColumnarBatch t2;
    t2.add_column("a", std::move(t2_a));
    t2.add_column("c", std::move(t2_c));
    catalog.add_table("t2", std::move(t2));

    storage::Int64Column t3_c;
    for (auto value : {200, 201, 201}) {
        t3_c.append(value);
    }
    storage::Int64Column t3_d;
    for (auto value : {1, 2, 3}) {
        t3_d.append(value);
    }
    storage::ColumnarBatch t3;
    t3.add_column("c", std::move(t3_c));
    t3.add_column("d", std::move(t3_d));
    catalog.add_table("t3", std::move(t3));

    storage::ColumnarBatch empty;
    empty.add_column("a", storage::Int64Column{});
    catalog.add_table("empty", std::move(empty));

    storage::Int64Column overflow_value;
    overflow_value.append(std::numeric_limits<std::int64_t>::max());
    overflow_value.append(1);
    storage::ColumnarBatch overflow;
    overflow.add_column("v", std::move(overflow_value));
    catalog.add_table("overflow", std::move(overflow));
    return catalog;
}

std::string format_result(const ExpectedResult& result) {
    std::ostringstream out;
    out << "columns=[";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << result.columns[i];
    }
    out << "]; rows=[";
    for (std::size_t row = 0; row < result.rows.size(); ++row) {
        if (row != 0) {
            out << ",";
        }
        out << "[";
        for (std::size_t col = 0; col < result.rows[row].size(); ++col) {
            if (col != 0) {
                out << ",";
            }
            if (result.rows[row][col].has_value()) {
                out << *result.rows[row][col];
            } else {
                out << "NULL";
            }
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

std::string format_result(const storage::ColumnarBatch& batch) {
    std::ostringstream out;
    const auto& column_order = batch.column_names();
    out << "columns=[";
    for (std::size_t i = 0; i < column_order.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << column_order[i];
    }
    out << "]; rows=[";
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        if (row != 0) {
            out << ",";
        }
        out << "[";
        for (std::size_t col = 0; col < column_order.size(); ++col) {
            if (col != 0) {
                out << ",";
            }
            if (!batch.has_column(column_order[col])) {
                out << "<missing:" << column_order[col] << ">";
            } else {
                const auto& column = batch.column(column_order[col]);
                if (column.is_null(row)) {
                    out << "NULL";
                } else {
                    out << column.at(row);
                }
            }
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

std::string format_error(const ExpectedError& error) {
    std::ostringstream out;
    const auto kind = error.kind == ErrorKind::Parse ? "parse" : error.kind == ErrorKind::Bind ? "bind" : "runtime";
    out << kind << "@" << error.position << ": " << error.message;
    return out.str();
}

std::string run_query(const GoldenQuery& test, const execution::Catalog& catalog) {
    try {
        auto parsed = sql::parse_select(test.sql);
        auto logical = sql::bind_select(parsed, catalog);
        auto actual = execution::execute_interpreted(logical, catalog);
        return format_result(actual);
    } catch (const sql::ParseError& error) {
        return format_error(ExpectedError{ErrorKind::Parse, error.position(), error.message()});
    } catch (const sql::BindError& error) {
        return format_error(ExpectedError{ErrorKind::Bind, error.position(), error.message()});
    } catch (const std::exception& error) {
        return format_error(ExpectedError{ErrorKind::Runtime, 0, error.what()});
    }
}

bool run_case(const GoldenQuery& test, const execution::Catalog& catalog) {
    const auto actual = run_query(test, catalog);
    const auto expected = test.expects_error ? format_error(test.error) : format_result(test.result);
    if (actual == expected) {
        return true;
    }

    std::cerr << "golden query failed: " << test.name << "\n"
              << "query: " << test.sql << "\n"
              << "expected: " << expected << "\n"
              << "actual:   " << actual << "\n";
    return false;
}

} // namespace

int main() {
    const auto catalog = make_catalog();
    const std::vector<GoldenQuery> tests{
        GoldenQuery{
            "multi projection literal and AND comparisons",
            "SELECT a, b, 99 FROM t WHERE a >= 2 AND b <> 40",
            ExpectedResult{{"a", "b", "99"}, {{2, 20, 99}, {3, 20, 99}}},
        },
        GoldenQuery{
            "projected NULL literal uses literal output name",
            "SELECT NULL FROM t LIMIT 2",
            ExpectedResult{{"NULL"}, {{std::nullopt}, {std::nullopt}}},
        },
        GoldenQuery{
            "nullable column projection preserves validity",
            "SELECT k, v FROM nullable",
            ExpectedResult{{"k", "v"}, {{1, 10}, {std::nullopt, 20}, {2, std::nullopt}, {std::nullopt, 30}}},
        },
        GoldenQuery{
            "IS NULL keeps exactly null rows",
            "SELECT k, v FROM nullable WHERE k IS NULL",
            ExpectedResult{{"k", "v"}, {{std::nullopt, 20}, {std::nullopt, 30}}},
        },
        GoldenQuery{
            "IS NOT NULL keeps exactly present rows",
            "SELECT k, v FROM nullable WHERE k IS NOT NULL",
            ExpectedResult{{"k", "v"}, {{1, 10}, {2, std::nullopt}}},
        },
        GoldenQuery{
            "comparison with NULL literal is UNKNOWN and rejected by WHERE",
            "SELECT k FROM nullable WHERE k = NULL",
            ExpectedResult{{"k"}, {}},
        },
        GoldenQuery{
            "UNKNOWN OR TRUE keeps TRUE rows",
            "SELECT k FROM nullable WHERE k = NULL OR 1 = 1",
            ExpectedResult{{"k"}, {{1}, {std::nullopt}, {2}, {std::nullopt}}},
        },
        GoldenQuery{
            "UNKNOWN OR data TRUE keeps TRUE rows",
            "SELECT v FROM nullable WHERE k = NULL OR v = 30",
            ExpectedResult{{"v"}, {{30}}},
        },
        GoldenQuery{
            "UNKNOWN AND FALSE rejects rows",
            "SELECT k FROM nullable WHERE k = NULL AND 1 = 0",
            ExpectedResult{{"k"}, {}},
        },
        GoldenQuery{
            "UNKNOWN AND data TRUE still rejects UNKNOWN",
            "SELECT v FROM nullable WHERE k = NULL AND v = 30",
            ExpectedResult{{"v"}, {}},
        },
        GoldenQuery{
            "nullable aggregates ignore NULL inputs",
            "SELECT COUNT(*), COUNT(k), COUNT(v), SUM(k), MIN(v), MAX(v) FROM nullable",
            ExpectedResult{{"COUNT(*)", "COUNT(k)", "COUNT(v)", "SUM(k)", "MIN(v)", "MAX(v)"},
                           {{4, 2, 3, 3, 10, 30}}},
        },
        GoldenQuery{
            "HAVING UNKNOWN rejects grouped rows",
            "SELECT a, COUNT(*) FROM t GROUP BY a HAVING COUNT(*) = NULL",
            ExpectedResult{{"a", "COUNT(*)"}, {}},
        },
        GoldenQuery{
            "hash join null keys never match",
            "SELECT l.v, r.w FROM j1 AS l JOIN j2 AS r ON l.k = r.k",
            ExpectedResult{{"l.v", "r.w"}, {{10, 101}, {30, 102}}},
        },
        GoldenQuery{
            "self join null keys never match themselves",
            "SELECT x.v, y.v FROM j1 AS x JOIN j1 AS y ON x.k = y.k",
            ExpectedResult{{"x.v", "y.v"}, {{10, 10}, {30, 30}}},
        },
        GoldenQuery{
            "less than comparison",
            "SELECT a FROM t WHERE a < 3",
            ExpectedResult{{"a"}, {{1}, {2}}},
        },
        GoldenQuery{
            "projection order follows SELECT list",
            "SELECT b, a FROM t WHERE a = 2",
            ExpectedResult{{"b", "a"}, {{20, 2}}},
        },
        GoldenQuery{
            "select item aliases become output names",
            "SELECT a AS x, b AS y FROM t WHERE a = 2",
            ExpectedResult{{"x", "y"}, {{2, 20}}},
        },
        GoldenQuery{
            "less equal and literal left comparison",
            "SELECT b FROM t WHERE a <= 3 AND 10 < b",
            ExpectedResult{{"b"}, {{20}, {20}}},
        },
        GoldenQuery{
            "greater than comparison",
            "SELECT a FROM t WHERE a > 2",
            ExpectedResult{{"a"}, {{3}, {4}}},
        },
        GoldenQuery{
            "inner join duplicates multiply in left row major order",
            "SELECT t1.a, t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{{"t1.a", "t1.b", "t2.c"}, {{2, 20, 200}, {2, 20, 201}, {2, 30, 200}, {2, 30, 201}}},
        },
        GoldenQuery{
            "explicit inner join and where predicate",
            "SELECT t1.b, t2.c FROM t1 INNER JOIN t2 ON t1.a = t2.a WHERE t2.c > 200 AND t1.b = 20",
            ExpectedResult{{"t1.b", "t2.c"}, {{20, 201}}},
        },
        GoldenQuery{
            "join with empty side returns no rows",
            "SELECT t1.a FROM t1 JOIN empty ON t1.a = empty.a",
            ExpectedResult{{"t1.a"}, {}},
        },
        GoldenQuery{
            "left deep multi join preserves deterministic order",
            "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c WHERE t3.d >= 2",
            ExpectedResult{{"t1.b", "t2.c", "t3.d"}, {{20, 201, 2}, {20, 201, 3}, {30, 201, 2}, {30, 201, 3}}},
        },
        GoldenQuery{
            "qualified duplicate column names use qualified output names",
            "SELECT t1.a, t2.a FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{{"t1.a", "t2.a"}, {{2, 2}, {2, 2}, {2, 2}, {2, 2}}},
        },
        GoldenQuery{
            "order by single key descending keeps equal-key ties stable",
            "SELECT a, b FROM t ORDER BY b DESC",
            ExpectedResult{{"a", "b"}, {{4, 40}, {2, 20}, {3, 20}, {1, 10}}},
        },
        GoldenQuery{
            "order by multiple keys with mixed directions",
            "SELECT a, b FROM t ORDER BY b ASC, a DESC",
            ExpectedResult{{"a", "b"}, {{1, 10}, {3, 20}, {2, 20}, {4, 40}}},
        },
        GoldenQuery{
            "order by output alias in ungrouped query",
            "SELECT a AS x, b FROM t ORDER BY x DESC",
            ExpectedResult{{"x", "b"}, {{4, 40}, {3, 20}, {2, 20}, {1, 10}}},
        },
        GoldenQuery{
            "order by output name wins over FROM scope",
            "SELECT b AS a, a AS original FROM t ORDER BY a DESC",
            ExpectedResult{{"a", "original"}, {{40, 4}, {20, 2}, {20, 3}, {10, 1}}},
        },
        GoldenQuery{
            "order by ascending places NULLs last",
            "SELECT k, v FROM nullable ORDER BY k ASC",
            ExpectedResult{{"k", "v"}, {{1, 10}, {2, std::nullopt}, {std::nullopt, 20}, {std::nullopt, 30}}},
        },
        GoldenQuery{
            "order by descending places NULLs first",
            "SELECT k, v FROM nullable ORDER BY k DESC",
            ExpectedResult{{"k", "v"}, {{std::nullopt, 20}, {std::nullopt, 30}, {2, std::nullopt}, {1, 10}}},
        },
        GoldenQuery{
            "order by binds unprojected qualified join column from FROM scope",
            "SELECT t1.b FROM t1 JOIN t2 ON t1.a = t2.a ORDER BY t2.c DESC, t1.b ASC",
            ExpectedResult{{"t1.b"}, {{20}, {30}, {20}, {30}}},
        },
        GoldenQuery{
            "implicit table alias qualifies projection and predicate",
            "SELECT x.a FROM t x WHERE x.a = 2",
            ExpectedResult{{"x.a"}, {{2}}},
        },
        GoldenQuery{
            "aliased join orders by aliased keys",
            "SELECT x.b, y.c FROM t1 AS x JOIN t2 AS y ON x.a = y.a ORDER BY y.c DESC, x.b ASC",
            ExpectedResult{{"x.b", "y.c"}, {{20, 201}, {30, 201}, {20, 200}, {30, 200}}},
        },
        GoldenQuery{
            "self join equality keeps alias identities separate",
            "SELECT x.a, y.a FROM t AS x JOIN t AS y ON x.a = y.a ORDER BY x.a ASC, y.a ASC",
            ExpectedResult{{"x.a", "y.a"}, {{1, 1}, {2, 2}, {3, 3}, {4, 4}}},
        },
        GoldenQuery{
            "self join memo-dedup regression result",
            "SELECT x.a, y.b FROM t1 AS x JOIN t1 AS y ON x.a = y.a WHERE x.b = 20 ORDER BY y.b ASC",
            ExpectedResult{{"x.a", "y.b"}, {{2, 20}, {2, 30}}},
        },
        GoldenQuery{
            "where OR has lower precedence than AND",
            "SELECT a, b FROM t WHERE a = 1 OR b = 20 AND c = 7",
            ExpectedResult{{"a", "b"}, {{1, 10}, {3, 20}}},
        },
        GoldenQuery{
            "parenthesized boolean group in WHERE",
            "SELECT a FROM t WHERE (a = 1 OR b = 20) AND c >= 6",
            ExpectedResult{{"a"}, {{2}, {3}}},
        },
        GoldenQuery{
            "join ON accepts parenthesized OR tree",
            "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a AND (t2.c = 200 OR t1.b = 30)",
            ExpectedResult{{"t1.b", "t2.c"}, {{20, 200}, {30, 200}, {30, 201}}},
        },
        GoldenQuery{
            "ungrouped aggregates use one global group",
            "SELECT COUNT(*), COUNT(b), SUM(a), MIN(b), MAX(b) FROM t",
            ExpectedResult{{"COUNT(*)", "COUNT(b)", "SUM(a)", "MIN(b)", "MAX(b)"}, {{4, 4, 10, 10, 40}}},
        },
        GoldenQuery{
            "group by preserves first appearance group order",
            "SELECT t1.a, COUNT(*), COUNT(t1.b), SUM(t1.b), MIN(t1.b), MAX(t1.b) FROM t1 GROUP BY t1.a",
            ExpectedResult{{"t1.a", "COUNT(*)", "COUNT(t1.b)", "SUM(t1.b)", "MIN(t1.b)", "MAX(t1.b)"},
                           {{1, 1, 1, 10, 10, 10}, {2, 2, 2, 50, 20, 30}}},
        },
        GoldenQuery{
            "join group by aggregates matched join rows",
            "SELECT t1.a, COUNT(*), SUM(t2.c), MIN(t2.c), MAX(t2.c) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a",
            ExpectedResult{{"t1.a", "COUNT(*)", "SUM(t2.c)", "MIN(t2.c)", "MAX(t2.c)"},
                           {{2, 4, 802, 200, 201}}},
        },
        GoldenQuery{
            "group by order by sorts grouping keys",
            "SELECT t1.a, COUNT(*) FROM t1 GROUP BY t1.a ORDER BY t1.a DESC",
            ExpectedResult{{"t1.a", "COUNT(*)"}, {{2, 2}, {1, 1}}},
        },
        GoldenQuery{
            "NULL grouping key forms one group and aggregates skip NULL inputs",
            "SELECT g, COUNT(*), COUNT(x), SUM(x), MIN(x), MAX(x) FROM agg_null GROUP BY g",
            ExpectedResult{{"g", "COUNT(*)", "COUNT(x)", "SUM(x)", "MIN(x)", "MAX(x)"},
                           {{std::nullopt, 3, 2, 30, 10, 20},
                            {1, 2, 1, 5, 5, 5},
                            {2, 2, 0, std::nullopt, std::nullopt, std::nullopt}}},
        },
        GoldenQuery{
            "multi key grouping treats matching NULL slots as equal",
            "SELECT g, h, COUNT(*), SUM(x) FROM agg_null GROUP BY g, h",
            ExpectedResult{{"g", "h", "COUNT(*)", "SUM(x)"},
                           {{std::nullopt, 1, 2, 10},
                            {1, 1, 1, std::nullopt},
                            {1, 2, 1, 5},
                            {2, 1, 2, std::nullopt},
                            {std::nullopt, 2, 1, 20}}},
        },
        GoldenQuery{
            "having filters grouped rows using projected aggregate",
            "SELECT a, SUM(b) FROM t GROUP BY a HAVING SUM(b) > 20 ORDER BY a ASC",
            ExpectedResult{{"a", "SUM(b)"}, {{4, 40}}},
        },
        GoldenQuery{
            "HAVING UNKNOWN from NULL aggregate rejects group",
            "SELECT g, SUM(x) FROM agg_null GROUP BY g HAVING SUM(x) > 5",
            ExpectedResult{{"g", "SUM(x)"}, {{std::nullopt, 30}}},
        },
        GoldenQuery{
            "HAVING IS NULL sees NULL aggregate results",
            "SELECT g, SUM(x) FROM agg_null GROUP BY g HAVING SUM(x) IS NULL",
            ExpectedResult{{"g", "SUM(x)"}, {{2, std::nullopt}}},
        },
        GoldenQuery{
            "having computes unprojected aggregate then final project drops it",
            "SELECT a FROM t GROUP BY a HAVING SUM(b) > 20",
            ExpectedResult{{"a"}, {{4}}},
        },
        GoldenQuery{
            "having can eliminate every group",
            "SELECT a, COUNT(*) FROM t GROUP BY a HAVING SUM(b) > 1000",
            ExpectedResult{{"a", "COUNT(*)"}, {}},
        },
        GoldenQuery{
            "grouped order by aggregate output alias after having",
            "SELECT a, SUM(b) AS total FROM t GROUP BY a HAVING SUM(b) >= 20 ORDER BY total DESC",
            ExpectedResult{{"a", "total"}, {{4, 40}, {2, 20}, {3, 20}}},
        },
        GoldenQuery{
            "grouped order by canonical aggregate output name",
            "SELECT a, SUM(b) FROM t GROUP BY a ORDER BY SUM(b) DESC",
            ExpectedResult{{"a", "SUM(b)"}, {{4, 40}, {2, 20}, {3, 20}, {1, 10}}},
        },
        GoldenQuery{
            "having OR may mix grouping key and aggregate output",
            "SELECT b, COUNT(*) FROM t GROUP BY b HAVING b = 10 OR COUNT(*) > 1",
            ExpectedResult{{"b", "COUNT(*)"}, {{10, 1}, {20, 2}}},
        },
        GoldenQuery{
            "distinct deduplicates projected rows in first appearance order",
            "SELECT DISTINCT k, v FROM dup",
            ExpectedResult{{"k", "v"}, {{1, 10}, {2, 20}, {1, 11}}},
        },
        GoldenQuery{
            "distinct applies to complete output row not individual columns",
            "SELECT DISTINCT k FROM dup",
            ExpectedResult{{"k"}, {{1}, {2}}},
        },
        GoldenQuery{
            "distinct join output collapses multiplied matches",
            "SELECT DISTINCT t1.a FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{{"t1.a"}, {{2}}},
        },
        GoldenQuery{
            "distinct may deduplicate aggregate output rows after group by",
            "SELECT DISTINCT COUNT(*) FROM t GROUP BY b",
            ExpectedResult{{"COUNT(*)"}, {{1}, {2}}},
        },
        GoldenQuery{
            "distinct over global aggregate is legal and single row",
            "SELECT DISTINCT COUNT(*) FROM t",
            ExpectedResult{{"COUNT(*)"}, {{4}}},
        },
        GoldenQuery{
            "distinct treats NULLs as equal for deduplication",
            "SELECT DISTINCT g FROM agg_null",
            ExpectedResult{{"g"}, {{std::nullopt}, {1}, {2}}},
        },
        GoldenQuery{
            "distinct complete rows compare NULL slots for equality",
            "SELECT DISTINCT g, h FROM agg_null",
            ExpectedResult{{"g", "h"}, {{std::nullopt, 1}, {1, 1}, {1, 2}, {2, 1}, {std::nullopt, 2}}},
        },
        GoldenQuery{
            "distinct sorts after dedup and keeps tie order stable",
            "SELECT DISTINCT k, v FROM dup ORDER BY k DESC",
            ExpectedResult{{"k", "v"}, {{2, 20}, {1, 10}, {1, 11}}},
        },
        GoldenQuery{
            "limit zero returns schema with no rows",
            "SELECT a FROM t LIMIT 0",
            ExpectedResult{{"a"}, {}},
        },
        GoldenQuery{
            "limit one takes first row",
            "SELECT a FROM t LIMIT 1",
            ExpectedResult{{"a"}, {{1}}},
        },
        GoldenQuery{
            "limit equal to count returns every row",
            "SELECT a FROM t LIMIT 4",
            ExpectedResult{{"a"}, {{1}, {2}, {3}, {4}}},
        },
        GoldenQuery{
            "limit greater than count returns every row",
            "SELECT a FROM t LIMIT 99",
            ExpectedResult{{"a"}, {{1}, {2}, {3}, {4}}},
        },
        GoldenQuery{
            "limit applies after order by",
            "SELECT a FROM t ORDER BY b DESC LIMIT 2",
            ExpectedResult{{"a"}, {{4}, {2}}},
        },
        GoldenQuery{
            "limit applies after join row order",
            "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a LIMIT 3",
            ExpectedResult{{"t1.b", "t2.c"}, {{20, 200}, {20, 201}, {30, 200}}},
        },
        GoldenQuery{
            "distinct limit combination shapes after dedup and sort",
            "SELECT DISTINCT b FROM t ORDER BY b ASC LIMIT 2",
            ExpectedResult{{"b"}, {{10}, {20}}},
        },
        GoldenQuery{
            "global count over empty input returns zero",
            "SELECT COUNT(*) FROM empty",
            ExpectedResult{{"COUNT(*)"}, {{0}}},
        },
        GoldenQuery{
            "grouped empty input returns no groups",
            "SELECT a, COUNT(*) FROM empty GROUP BY a",
            ExpectedResult{{"a", "COUNT(*)"}, {}},
        },
        GoldenQuery{
            "SUM MIN MAX over empty global group return NULL",
            "SELECT SUM(empty.a), MIN(empty.a), MAX(empty.a), COUNT(empty.a), COUNT(*) FROM empty",
            ExpectedResult{{"SUM(empty.a)", "MIN(empty.a)", "MAX(empty.a)", "COUNT(empty.a)", "COUNT(*)"},
                           {{std::nullopt, std::nullopt, std::nullopt, 0, 0}}},
        },
        GoldenQuery{
            "SUM MIN MAX over all NULL global group return NULL",
            "SELECT SUM(x), MIN(x), MAX(x), COUNT(x), COUNT(*) FROM agg_null WHERE g = 2",
            ExpectedResult{{"SUM(x)", "MIN(x)", "MAX(x)", "COUNT(x)", "COUNT(*)"},
                           {{std::nullopt, std::nullopt, std::nullopt, 0, 2}}},
        },
        GoldenQuery{
            "sum overflow fails loudly",
            "SELECT SUM(v) FROM overflow",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Runtime, 0, "SUM(v) overflowed int64"},
        },
        GoldenQuery{
            "non grouped projected column is a bind error",
            "SELECT a, b, COUNT(*) FROM t GROUP BY a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 10, "non-grouped column 'b' must appear in GROUP BY or be aggregated"},
        },
        GoldenQuery{
            "nested aggregate is a bind error",
            "SELECT SUM(COUNT(*)) FROM t",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 11, "nested aggregate 'COUNT' is not allowed"},
        },
        GoldenQuery{
            "grouped ORDER BY unknown name must use grouping columns or output names",
            "SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY b",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 46, "ORDER BY column 'b' must be a GROUP BY column or SELECT output name in aggregate queries"},
        },
        GoldenQuery{
            "having without group by is a bind error",
            "SELECT COUNT(*) FROM t HAVING COUNT(*) > 0",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 23, "HAVING requires GROUP BY in this SQL slice"},
        },
        GoldenQuery{
            "non grouped having column is a bind error",
            "SELECT a, COUNT(*) FROM t GROUP BY a HAVING b = 20",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 44, "HAVING column 'b' must be a GROUP BY column or aggregate expression"},
        },
        GoldenQuery{
            "nested aggregate in having is a bind error",
            "SELECT a FROM t GROUP BY a HAVING SUM(COUNT(*)) > 0",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 38, "nested aggregate 'COUNT' is not allowed"},
        },
        GoldenQuery{
            "unterminated boolean parenthesis is rejected",
            "SELECT a FROM t WHERE (a = 1 OR b = 20",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 38, "expected ')' after boolean expression"},
        },
        GoldenQuery{
            "OR without right predicate is rejected",
            "SELECT a FROM t WHERE a = 1 OR",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 30, "expected expression in comparison"},
        },
        GoldenQuery{
            "parenthesized scalar expression is not in scope",
            "SELECT a FROM t WHERE (a)",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 24, "expected comparison operator"},
        },
        GoldenQuery{
            "trailing projection comma is rejected",
            "SELECT a, FROM t",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 10, "expected projection expression"},
        },
        GoldenQuery{
            "unknown WHERE column is a bind error",
            "SELECT a FROM t WHERE missing = 1",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 22, "unknown column 'missing' in table 't'"},
        },
        GoldenQuery{
            "duplicate output names are bind errors",
            "SELECT a, a FROM t",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 10, "duplicate output name 'a'"},
        },
        GoldenQuery{
            "duplicate qualified output names are bind errors",
            "SELECT t1.a, t1.a FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 13, "duplicate output name 't1.a'"},
        },
        GoldenQuery{
            "ambiguous unqualified joined column is a bind error",
            "SELECT a FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 7, "ambiguous column 'a' matches tables 't1', 't2'"},
        },
        GoldenQuery{
            "unknown qualifier is a bind error",
            "SELECT missing.a FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 7, "unknown table qualifier 'missing'"},
        },
        GoldenQuery{
            "physical table name is not a qualifier when alias is present",
            "SELECT t.a FROM t AS x",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 7, "unknown table qualifier 't'"},
        },
        GoldenQuery{
            "duplicate unaliased self join binding is a bind error",
            "SELECT t.a FROM t JOIN t ON t.a = t.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 23, "duplicate table binding 't' requires a unique alias"},
        },
        GoldenQuery{
            "duplicate alias binding is a bind error",
            "SELECT x.a FROM t AS x JOIN t1 AS x ON x.a = x.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 34, "duplicate table binding 'x' requires a unique alias"},
        },
        GoldenQuery{
            "unqualified column ambiguity reports alias scopes",
            "SELECT a FROM t AS x JOIN t2 AS y ON x.a = y.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 7, "ambiguous column 'a' matches tables 'x', 'y'"},
        },
        GoldenQuery{
            "unknown qualified column is a bind error",
            "SELECT t1.missing FROM t1 JOIN t2 ON t1.a = t2.a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 7, "unknown column 'missing' in table 't1'"},
        },
        GoldenQuery{
            "duplicate output aliases are bind errors",
            "SELECT a AS x, b AS x FROM t",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 20, "duplicate output name 'x'"},
        },
        GoldenQuery{
            "reserved keyword after AS is rejected as select item alias",
            "SELECT a AS FROM t",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 12, "expected select item alias after AS"},
        },
        GoldenQuery{
            "reserved keyword after AS is rejected as table alias",
            "SELECT a FROM t AS JOIN",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 19, "expected alias after AS"},
        },
        GoldenQuery{
            "ORDER must be followed by BY",
            "SELECT a FROM t ORDER a",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 22, "expected BY after ORDER"},
        },
        GoldenQuery{
            "ORDER BY sort keys must be column references",
            "SELECT a FROM t ORDER BY 1",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 25, "expected ORDER BY column name"},
        },
        GoldenQuery{
            "ORDER BY unknown FROM-scope column is a bind error",
            "SELECT a FROM t ORDER BY missing",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 25, "unknown column 'missing' in table 't'"},
        },
        GoldenQuery{
            "negative LIMIT is rejected at literal position",
            "SELECT a FROM t LIMIT -1",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 22, "LIMIT must be a non-negative integer"},
        },
        GoldenQuery{
            "non-integer LIMIT is rejected at token position",
            "SELECT a FROM t LIMIT nope",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 22, "expected non-negative integer after LIMIT"},
        },
    };

    bool ok = true;
    for (const auto& test : tests) {
        ok = run_case(test, catalog) && ok;
    }
    return ok ? 0 : 1;
}
