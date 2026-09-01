// Fuzz target for the query front end and planner. Arbitrary bytes are treated
// as SQL text and driven through parsing, catalog binding, rule rewriting, memo
// exploration, cost-based selection, and plan extraction against a fixed
// schema-only catalog. The contract under test is that no input can crash that
// pipeline or trigger undefined behavior: malformed or unsupported queries must
// surface as ParseError, BindError, or another std::exception, never a memory
// error. No plan is executed, so no user data is read or written.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "catalog/catalog.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "sql/binder.hpp"
#include "sql/ast.hpp"

namespace {

class FixedCatalog final : public catalog::Catalog {
public:
    FixedCatalog() {
        using catalog::ColumnSchema;
        using catalog::ColumnType;
        using catalog::TableSchema;
        // Row counts are required by the cost model; without them extraction
        // throws on every plan and the fuzzer never reaches costing.
        tables_.push_back(TableSchema{"t", {ColumnSchema{"a", ColumnType::Int64},
                                            ColumnSchema{"b", ColumnType::Int64}}, 1000});
        tables_.push_back(TableSchema{"t1", {ColumnSchema{"a", ColumnType::Int64},
                                             ColumnSchema{"b", ColumnType::Int64}}, 100});
        tables_.push_back(TableSchema{"t2", {ColumnSchema{"a", ColumnType::Int64},
                                             ColumnSchema{"c", ColumnType::Int64}}, 10});
        tables_.push_back(TableSchema{"strings", {ColumnSchema{"s", ColumnType::String},
                                                  ColumnSchema{"i", ColumnType::Int64}}, 50});
    }

    [[nodiscard]] std::optional<catalog::TableSchema>
    find_table_schema(const std::string& name) const override {
        for (const auto& table : tables_) {
            if (table.name == name) {
                return table;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<catalog::TableSchema> tables_;
};

const FixedCatalog& fixed_catalog() {
    static const FixedCatalog instance;
    return instance;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string sql(reinterpret_cast<const char*>(data), size);
    try {
        const sql::SelectQuery query = sql::parse_select(sql);
        const plan::LogicalPlan logical = sql::bind_select(query, fixed_catalog());
        optimizer::Memo memo;
        const optimizer::GroupId root = memo.insert(logical);
        (void)optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        (void)memo.extract_best(root, fixed_catalog());
    } catch (const std::exception&) {
        // Parse, bind, and planning errors are the designed rejection path for
        // malformed or unsupported input; the sanitizers still watch for memory
        // and undefined-behavior defects on every input regardless of outcome.
    }
    return 0;
}
