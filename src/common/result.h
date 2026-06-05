#pragma once

#include <optional>
#include <string>
#include <utility>

namespace chimera::common {

enum class ErrorCode {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kIoError,
    kConfigError,
    kUnsupported,
    kDeviceError,
    kTimeout,
    kPolicyRefused,
    kUnknown,
};

struct Status final {
    ErrorCode code{ErrorCode::kOk};
    std::string message{};

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }

    static Status Ok() { return {}; }
    static Status Error(ErrorCode error_code, std::string error_message) {
        return Status{error_code, std::move(error_message)};
    }
};

template <typename T>
class [[nodiscard]] Result final {
public:
    Result(T value) : status_(Status::Ok()), value_(std::move(value)) {}
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] const T& value() const& { return value_.value(); }
    [[nodiscard]] T& value() & { return value_.value(); }
    [[nodiscard]] T&& value() && { return std::move(value_.value()); }

private:
    Status status_{Status::Ok()};
    std::optional<T> value_{};
};

template <>
class [[nodiscard]] Result<void> final {
public:
    Result() = default;
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    Status status_{Status::Ok()};
};

}  // namespace chimera::common

#define CHIMERA_RETURN_IF_ERROR(expr)        \
    do {                                     \
        const auto chimera_status = (expr);  \
        if (!chimera_status.ok()) {          \
            return chimera_status.status();  \
        }                                    \
    } while (false)
