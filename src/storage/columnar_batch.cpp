#include "storage/columnar_batch.hpp"

#include <utility>
#include <variant>

namespace storage {

std::size_t ColumnarBatch::column_size(const Column& column) {
    if (const auto* int64_column = std::get_if<Int64Column>(&column)) {
        return int64_column->size();
    }
    return std::get<StringColumn>(column).size();
}

void ColumnarBatch::add_typed_column(std::string name, Column column) {
    if (!columns_.empty() && column_size(column) != row_count()) {
        throw std::invalid_argument("all columns in a batch must have the same row count");
    }
    auto [inserted_it, inserted] = columns_.emplace(std::move(name), std::move(column));
    if (!inserted) {
        throw std::invalid_argument("duplicate column name");
    }
    column_names_.push_back(inserted_it->first);
}

void ColumnarBatch::add_column(std::string name, Int64Column column) {
    add_typed_column(std::move(name), std::move(column));
}

void ColumnarBatch::add_column(std::string name, StringColumn column) {
    add_typed_column(std::move(name), std::move(column));
}

std::size_t ColumnarBatch::row_count() const {
    if (column_names_.empty()) {
        return 0;
    }
    return column_size(columns_.at(column_names_.front()));
}

bool ColumnarBatch::has_column(const std::string& name) const {
    return columns_.contains(name);
}

const Int64Column& ColumnarBatch::column(const std::string& name) const {
    auto it = columns_.find(name);
    if (it == columns_.end()) {
        throw std::out_of_range("unknown column: " + name);
    }
    const auto* column = std::get_if<Int64Column>(&it->second);
    if (column == nullptr) {
        throw std::invalid_argument("column is not int64: " + name);
    }
    return *column;
}

const StringColumn& ColumnarBatch::string_column(const std::string& name) const {
    auto it = columns_.find(name);
    if (it == columns_.end()) {
        throw std::out_of_range("unknown column: " + name);
    }
    const auto* column = std::get_if<StringColumn>(&it->second);
    if (column == nullptr) {
        throw std::invalid_argument("column is not string: " + name);
    }
    return *column;
}

catalog::ColumnType ColumnarBatch::column_type(const std::string& name) const {
    auto it = columns_.find(name);
    if (it == columns_.end()) {
        throw std::out_of_range("unknown column: " + name);
    }
    return std::holds_alternative<Int64Column>(it->second) ? catalog::ColumnType::Int64 : catalog::ColumnType::String;
}

ColumnarBatch ColumnarBatch::filter(const RowMask& mask) const {
    if (mask.keep.size() != row_count()) {
        throw std::invalid_argument("row mask size must equal batch row count");
    }
    ColumnarBatch out;
    for (const auto& name : column_names_) {
        const auto& col = columns_.at(name);
        if (const auto* int64_column = std::get_if<Int64Column>(&col)) {
            Int64Column filtered;
            for (std::size_t i = 0; i < mask.keep.size(); ++i) {
                if (mask.keep[i]) {
                    if (int64_column->is_null(i)) {
                        filtered.append_null();
                    } else {
                        filtered.append(int64_column->at(i));
                    }
                }
            }
            out.add_column(name, filtered);
        } else {
            const auto& string_column = std::get<StringColumn>(col);
            StringColumn filtered;
            for (std::size_t i = 0; i < mask.keep.size(); ++i) {
                if (mask.keep[i]) {
                    if (string_column.is_null(i)) {
                        filtered.append_null();
                    } else {
                        filtered.append(string_column.at(i));
                    }
                }
            }
            out.add_column(name, filtered);
        }
    }
    return out;
}

RowMask equals_i64(const ColumnarBatch& batch, const std::string& column, std::int64_t value) {
    const auto& col = batch.column(column);
    RowMask mask;
    mask.keep.reserve(col.size());
    for (std::size_t row = 0; row < col.size(); ++row) {
        mask.keep.push_back(!col.is_null(row) && col.at(row) == value ? 1 : 0);
    }
    return mask;
}

} // namespace storage
