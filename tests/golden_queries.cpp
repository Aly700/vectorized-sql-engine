#include "execution/interpreter.hpp"
#include "sql/ast.hpp"
#include "sql/binder.hpp"
#include "sql/errors.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ExpectedResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::int64_t>> rows;
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
            out << result.rows[row][col];
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
                out << batch.column(column_order[col]).at(row);
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
            "sum over empty global group fails loudly",
            "SELECT SUM(empty.a) FROM empty",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Runtime, 0, "SUM(empty.a) over empty input has no NULL-free result"},
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
            "grouped ORDER BY must use grouping columns",
            "SELECT a, COUNT(*) FROM t GROUP BY a ORDER BY b",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Bind, 46, "ORDER BY column 'b' must be a GROUP BY column in aggregate queries"},
        },
        GoldenQuery{
            "unsupported OR is rejected at token position",
            "SELECT a FROM t WHERE a = 2 OR b = 20",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 28, "expected end of input after query"},
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
            "projection aliases are outside the slice",
            "SELECT a AS x FROM t",
            ExpectedResult{},
            true,
            ExpectedError{ErrorKind::Parse, 9, "expected ',' or FROM after projection expression"},
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
    };

    bool ok = true;
    for (const auto& test : tests) {
        ok = run_case(test, catalog) && ok;
    }
    return ok ? 0 : 1;
}
