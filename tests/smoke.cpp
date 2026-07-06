#include "execution/interpreter.hpp"
#include "optimizer/memo.hpp"
#include "plan/logical_plan.hpp"
#include "sql/ast.hpp"
#include "sql/binder.hpp"

#include <cassert>

int main() {
    auto query = sql::parse_select("SELECT a FROM t WHERE a = 2");
    assert(query.projection.size() == 1);
    assert(sql::output_name(query.projection[0].expression) == "a");
    assert(query.table == "t");
    assert(query.predicate.has_value());

    storage::Int64Column a;
    a.append(1);
    a.append(2);
    a.append(2);
    storage::ColumnarBatch batch;
    batch.add_column("a", a);

    execution::Catalog catalog;
    catalog.add_table("t", batch);

    auto logical = sql::bind_select(query, catalog);
    auto out = execution::execute_interpreted(logical, catalog);
    assert(out.row_count() == 2);
    assert(out.column("a").at(0) == 2);

    optimizer::Memo memo;
    auto group = memo.insert_group(logical);
    assert(memo.group(group).size() == 1);
    return 0;
}
