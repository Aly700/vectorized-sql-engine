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

void assert_column_reserve_detaches_shared_storage_without_changing_values() {
    storage::Int64Column column;
    column.reserve(3);
    assert(column.size() == 0);
    assert(column.values().capacity() >= 3);
    column.append(10);
    column.append(20);
    column.append(30);

    const auto& shared_values = column.values();
    auto reserved = column;
    reserved.reserve(8);

    assert(&reserved.values() != &shared_values);
    assert(reserved.size() == 3);
    assert(reserved.values().capacity() >= 8);
    assert(reserved.at(0) == 10);
    assert(reserved.at(1) == 20);
    assert(reserved.at(2) == 30);
    assert(column.size() == 3);
}

void assert_column_validity_copy_reuses_storage_until_mutation() {
    storage::Int64Column original;
    original.append(10);
    original.append_null();
    original.append(30);

    storage::ColumnarBatch batch;
    batch.add_column("qualified.a", original);

    const auto& shared = batch.column("qualified.a");
    assert(shared.has_nulls());
    assert(&shared.values() == &original.values());
    assert(&shared.validity() == &original.validity());
    assert(!shared.is_null(0));
    assert(shared.is_null(1));
    assert(!shared.is_null(2));

    auto mutated = shared;
    mutated.append_null();

    assert(&mutated.values() != &shared.values());
    assert(&mutated.validity() != &shared.validity());
    assert(shared.size() == 3);
    assert(mutated.size() == 4);
    assert(mutated.is_null(3));
}

void assert_string_column_copy_reuses_storage_and_catalog_reports_type() {
    storage::StringColumn original;
    original.append("alpha");
    original.append_null();
    original.append("");

    storage::ColumnarBatch batch;
    batch.add_column("qualified.s", original);

    const auto& shared = batch.string_column("qualified.s");
    assert(batch.column_type("qualified.s") == catalog::ColumnType::String);
    assert(shared.has_nulls());
    assert(&shared.values() == &original.values());
    assert(&shared.validity() == &original.validity());
    assert(shared.at(0) == "alpha");
    assert(shared.is_null(1));
    assert(shared.at(2).empty());

    auto mutated = shared;
    mutated.append("omega");

    assert(&mutated.values() != &shared.values());
    assert(&mutated.validity() != &shared.validity());
    assert(shared.size() == 3);
    assert(mutated.size() == 4);
    assert(mutated.at(3) == "omega");

    execution::Catalog catalog;
    catalog.add_table("strings", std::move(batch));
    const auto schema = catalog.find_table_schema("strings");
    assert(schema.has_value());
    assert(schema->columns.size() == 1);
    assert(schema->columns[0].type == catalog::ColumnType::String);
}

} // namespace

int main() {
    assert_column_copy_reuses_storage_until_mutation();
    assert_column_reserve_detaches_shared_storage_without_changing_values();
    assert_column_validity_copy_reuses_storage_until_mutation();
    assert_string_column_copy_reuses_storage_and_catalog_reports_type();

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
