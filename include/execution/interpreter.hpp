#pragma once

#include "plan/logical_plan.hpp"
#include "storage/columnar_batch.hpp"

#include <map>
#include <string>

namespace execution {

class Catalog {
public:
    void add_table(std::string name, storage::ColumnarBatch batch);
    [[nodiscard]] const storage::ColumnarBatch& table(const std::string& name) const;

private:
    std::map<std::string, storage::ColumnarBatch> tables_;
};

storage::ColumnarBatch execute_interpreted(const plan::LogicalPlan& plan, const Catalog& catalog);

} // namespace execution
