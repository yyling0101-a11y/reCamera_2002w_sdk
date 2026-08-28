#include <recamera/status.hpp>

namespace recamera
{

const char *toString(RecoveryLevel level) noexcept
{
    switch (level)
    {
    case RecoveryLevel::Streaming:
        return "streaming";
    case RecoveryLevel::RequestingIdr:
        return "requesting-idr";
    case RecoveryLevel::Restarting:
        return "restarting";
    }
    return "unknown";
}

} // namespace recamera
