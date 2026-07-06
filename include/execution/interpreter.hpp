#pragma once

#include "catalog/catalog.hpp"
#include "plan/logical_plan.hpp"
#include "storage/columnar_batch.hpp"

#include <map>
#include <optional>
#include <string>

namespace execution {

class Catalog final : public catalog::Catalog {
public:
    void add_table(std::string name, storage::ColumnarBatch batch);
    [[nodiscard]] std::optional<catalog::TableSchema> find_table_schema(const std::string& name) const override;
    [[nodiscard]] const storage::ColumnarBatch& table(const std::string& name) const;

private:
    std::map<std::string, storage::ColumnarBatch> tables_;
};

storage::ColumnarBatch execute_interpreted(const plan::LogicalPlan& plan, const Catalog& catalog);

} // namespace execution
