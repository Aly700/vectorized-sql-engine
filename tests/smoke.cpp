#include "execution/interpreter.hpp"
#include "optimizer/memo.hpp"
#include "plan/logical_plan.hpp"
#include "sql/ast.hpp"
#include "sql/binder.hpp"

#include <cassert>

namespace {

void assert_column_copy_reuses_storage_until_mutation() {
    storage::Int64Column original;
    original.append(10);
    original.append(20);
    original.append(30);

    storage::ColumnarBatch batch;
    batch.add_column("qualified.a", original);

    const auto& shared = batch.column("qualified.a");
    assert(&shared.values() == &original.values());

    auto mutated = shared;
    mutated.append(40);

    assert(&mutated.values() != &shared.values());
    assert(shared.size() == 3);
    assert(mutated.size() == 4);
    assert(mutated.at(3) == 40);
}

} // namespace

int main() {
    assert_column_copy_reuses_storage_until_mutation();

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
    auto group = memo.insert(logical);
    assert(memo.group(group).expressions.size() == 1);
    assert(plan::to_string(memo.extract(group)) == plan::to_string(logical));
    return 0;
}
