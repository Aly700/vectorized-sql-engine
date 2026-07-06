#include "execution/interpreter.hpp"
#include "sql/ast.hpp"
#include "sql/binder.hpp"
#include "sql/errors.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ExpectedResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::int64_t>> rows;
};

enum class ErrorKind { Parse, Bind };

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
    out << (error.kind == ErrorKind::Parse ? "parse" : "bind") << "@" << error.position << ": " << error.message;
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
        return std::string("unexpected exception: ") + error.what();
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
    };

    bool ok = true;
    for (const auto& test : tests) {
        ok = run_case(test, catalog) && ok;
    }
    return ok ? 0 : 1;
}
