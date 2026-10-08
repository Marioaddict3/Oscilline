// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// A value or an error string. Parsers do not throw on bad input.

#pragma once

#include <optional>
#include <string>
#include <utility>

namespace oscilline {

// Success-or-message result. Parsers return this on malformed input; they do not throw.
template <typename T> class Result {
  public:
    static Result success(T value) {
        Result result;
        result.value_ = std::move(value);
        return result;
    }

    static Result failure(std::string error) {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool ok() const { return value_.has_value(); }

    [[nodiscard]] explicit operator bool() const { return ok(); }

    // optional::value() throws if the result is a failure. Callers check ok() first.
    [[nodiscard]] const T& value() const { return value_.value(); }

    [[nodiscard]] T& value() { return value_.value(); }

    [[nodiscard]] const std::string& error() const { return error_; }

  private:
    std::optional<T> value_;
    std::string error_;
};

} // namespace oscilline
