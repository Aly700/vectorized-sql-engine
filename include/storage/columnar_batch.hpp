#pragma once

#include "catalog/catalog.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace storage {

class Int64Column {
public:
    Int64Column() : values_(std::make_shared<std::vector<std::int64_t>>()) {}

    void append(std::int64_t value) {
        detach_for_append();
        values_->push_back(value);
        if (validity_ != nullptr) {
            validity_->push_back(1);
        }
    }

    void append_null() {
        detach_for_append();
        ensure_validity_for_null();
        values_->push_back(0);
        validity_->push_back(0);
    }

    void reserve(std::size_t count) {
        detach_for_append();
        values_->reserve(count);
        if (validity_ != nullptr) {
            validity_->reserve(count);
        }
    }

    [[nodiscard]] std::size_t size() const { return values().size(); }
    [[nodiscard]] std::int64_t at(std::size_t row) const { return values().at(row); }
    [[nodiscard]] bool is_null(std::size_t row) const {
        (void)values().at(row);
        return validity_ != nullptr && validity_->at(row) == 0;
    }
    [[nodiscard]] bool has_nulls() const { return validity_ != nullptr; }
    [[nodiscard]] const std::vector<std::int64_t>& values() const {
        static const std::vector<std::int64_t> empty;
        return values_ == nullptr ? empty : *values_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& validity() const {
        static const std::vector<std::uint8_t> all_valid;
        return validity_ == nullptr ? all_valid : *validity_;
    }

private:
    void detach_for_append() {
        if (values_ == nullptr) {
            values_ = std::make_shared<std::vector<std::int64_t>>();
        } else if (values_.use_count() != 1) {
            values_ = std::make_shared<std::vector<std::int64_t>>(*values_);
        }
        if (validity_ != nullptr && validity_.use_count() != 1) {
            validity_ = std::make_shared<std::vector<std::uint8_t>>(*validity_);
        }
    }

    void ensure_validity_for_null() {
        if (validity_ != nullptr) {
            return;
        }
        validity_ = std::make_shared<std::vector<std::uint8_t>>(values().size(), 1);
    }

    std::shared_ptr<std::vector<std::int64_t>> values_;
    std::shared_ptr<std::vector<std::uint8_t>> validity_;
};

class StringColumn {
public:
    StringColumn() : values_(std::make_shared<std::vector<std::string>>()) {}

    void append(std::string value) {
        detach_for_append();
        values_->push_back(std::move(value));
        if (validity_ != nullptr) {
            validity_->push_back(1);
        }
    }

    void append(const char* value) {
        append(std::string(value));
    }

    void append_null() {
        detach_for_append();
        ensure_validity_for_null();
        values_->push_back("");
        validity_->push_back(0);
    }

    void reserve(std::size_t count) {
        detach_for_append();
        values_->reserve(count);
        if (validity_ != nullptr) {
            validity_->reserve(count);
        }
    }

    [[nodiscard]] std::size_t size() const { return values().size(); }
    [[nodiscard]] const std::string& at(std::size_t row) const { return values().at(row); }
    [[nodiscard]] bool is_null(std::size_t row) const {
        (void)values().at(row);
        return validity_ != nullptr && validity_->at(row) == 0;
    }
    [[nodiscard]] bool has_nulls() const { return validity_ != nullptr; }
    [[nodiscard]] const std::vector<std::string>& values() const {
        static const std::vector<std::string> empty;
        return values_ == nullptr ? empty : *values_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& validity() const {
        static const std::vector<std::uint8_t> all_valid;
        return validity_ == nullptr ? all_valid : *validity_;
    }

private:
    void detach_for_append() {
        if (values_ == nullptr) {
            values_ = std::make_shared<std::vector<std::string>>();
        } else if (values_.use_count() != 1) {
            values_ = std::make_shared<std::vector<std::string>>(*values_);
        }
        if (validity_ != nullptr && validity_.use_count() != 1) {
            validity_ = std::make_shared<std::vector<std::uint8_t>>(*validity_);
        }
    }

    void ensure_validity_for_null() {
        if (validity_ != nullptr) {
            return;
        }
        validity_ = std::make_shared<std::vector<std::uint8_t>>(values().size(), 1);
    }

    std::shared_ptr<std::vector<std::string>> values_;
    std::shared_ptr<std::vector<std::uint8_t>> validity_;
};

struct RowMask {
    std::vector<std::uint8_t> keep;
};

class ColumnarBatch {
public:
    void add_column(std::string name, Int64Column column);
    void add_column(std::string name, StringColumn column);
    [[nodiscard]] std::size_t row_count() const;
    [[nodiscard]] bool has_column(const std::string& name) const;
    [[nodiscard]] catalog::ColumnType column_type(const std::string& name) const;
    [[nodiscard]] const Int64Column& column(const std::string& name) const;
    [[nodiscard]] const StringColumn& string_column(const std::string& name) const;
    [[nodiscard]] ColumnarBatch filter(const RowMask& mask) const;
    [[nodiscard]] const std::vector<std::string>& column_names() const { return column_names_; }

private:
    using Column = std::variant<Int64Column, StringColumn>;

    [[nodiscard]] static std::size_t column_size(const Column& column);
    void add_typed_column(std::string name, Column column);

    std::map<std::string, Column> columns_;
    std::vector<std::string> column_names_;
};

RowMask equals_i64(const ColumnarBatch& batch, const std::string& column, std::int64_t value);

} // namespace storage
