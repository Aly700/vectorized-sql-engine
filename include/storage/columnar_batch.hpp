#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace storage {

class Int64Column {
public:
    void append(std::int64_t value) { values_.push_back(value); }
    [[nodiscard]] std::size_t size() const { return values_.size(); }
    [[nodiscard]] std::int64_t at(std::size_t row) const { return values_.at(row); }
    [[nodiscard]] const std::vector<std::int64_t>& values() const { return values_; }

private:
    std::vector<std::int64_t> values_;
};

struct RowMask {
    std::vector<std::uint8_t> keep;
};

class ColumnarBatch {
public:
    void add_column(std::string name, Int64Column column);
    [[nodiscard]] std::size_t row_count() const;
    [[nodiscard]] bool has_column(const std::string& name) const;
    [[nodiscard]] const Int64Column& column(const std::string& name) const;
    [[nodiscard]] ColumnarBatch filter(const RowMask& mask) const;
    [[nodiscard]] const std::map<std::string, Int64Column>& columns() const { return columns_; }

private:
    std::map<std::string, Int64Column> columns_;
};

RowMask equals_i64(const ColumnarBatch& batch, const std::string& column, std::int64_t value);

} // namespace storage
