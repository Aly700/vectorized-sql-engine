#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace sql {

class ParseError : public std::runtime_error {
public:
    ParseError(std::size_t position, std::string message)
        : std::runtime_error(format("parse", position, message)),
          position_(position),
          message_(std::move(message)) {}

    [[nodiscard]] std::size_t position() const { return position_; }
    [[nodiscard]] const std::string& message() const { return message_; }

private:
    static std::string format(const char* kind, std::size_t position, const std::string& message) {
        return std::string(kind) + " error at position " + std::to_string(position) + ": " + message;
    }

    std::size_t position_;
    std::string message_;
};

class BindError : public std::runtime_error {
public:
    BindError(std::size_t position, std::string message)
        : std::runtime_error(format("bind", position, message)),
          position_(position),
          message_(std::move(message)) {}

    [[nodiscard]] std::size_t position() const { return position_; }
    [[nodiscard]] const std::string& message() const { return message_; }

private:
    static std::string format(const char* kind, std::size_t position, const std::string& message) {
        return std::string(kind) + " error at position " + std::to_string(position) + ": " + message;
    }

    std::size_t position_;
    std::string message_;
};

} // namespace sql
