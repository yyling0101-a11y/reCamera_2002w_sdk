#pragma once

#include <string>

namespace recamera
{

enum class ErrorCode
{
    Ok,
    InvalidArgument,
    NotInitialized,
    AlreadyRunning,
    BackendError,
    Timeout,
    Unsupported,
};

struct Error
{
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    explicit operator bool() const noexcept
    {
        return code != ErrorCode::Ok;
    }
};

} // namespace recamera
