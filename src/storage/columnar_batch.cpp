#include "storage/columnar_batch.hpp"

#include <utility>

namespace storage {

void ColumnarBatch::add_column(std::string name, Int64Column column) {
    if (!columns_.empty() && column.size() != row_count()) {
        throw std::invalid_argument("all columns in a batch must have the same row count");
    }
    auto [inserted_it, inserted] = columns_.emplace(std::move(name), std::move(column));
    if (!inserted) {
        throw std::invalid_argument("duplicate column name");
    }
    column_names_.push_back(inserted_it->first);
}

std::size_t ColumnarBatch::row_count() const {
    if (column_names_.empty()) {
        return 0;
    }
    return columns_.at(column_names_.front()).size();
}

bool ColumnarBatch::has_column(const std::string& name) const {
    return columns_.contains(name);
}

const Int64Column& ColumnarBatch::column(const std::string& name) const {
    auto it = columns_.find(name);
    if (it == columns_.end()) {
        throw std::out_of_range("unknown column: " + name);
    }
    return it->second;
}

ColumnarBatch ColumnarBatch::filter(const RowMask& mask) const {
    if (mask.keep.size() != row_count()) {
        throw std::invalid_argument("row mask size must equal batch row count");
    }
    ColumnarBatch out;
    for (const auto& name : column_names_) {
        const auto& col = columns_.at(name);
        Int64Column filtered;
        for (std::size_t i = 0; i < mask.keep.size(); ++i) {
            if (mask.keep[i]) {
                filtered.append(col.at(i));
            }
        }
        out.add_column(name, filtered);
    }
    return out;
}

RowMask equals_i64(const ColumnarBatch& batch, const std::string& column, std::int64_t value) {
    const auto& col = batch.column(column);
    RowMask mask;
    mask.keep.reserve(col.size());
    for (auto v : col.values()) {
        mask.keep.push_back(v == value ? 1 : 0);
    }
    return mask;
}

} // namespace storage
