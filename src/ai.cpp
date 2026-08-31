#include <recamera/ai.hpp>

#include <cstring>
#include <mutex>
#include <utility>

#include <cviruntime.h>

namespace recamera
{
namespace
{

TensorDataType convertType(CVI_FMT type) noexcept
{
    switch (type) {
    case CVI_FMT_FP32: return TensorDataType::Float32;
    case CVI_FMT_INT32: return TensorDataType::Int32;
    case CVI_FMT_UINT32: return TensorDataType::UInt32;
    case CVI_FMT_BF16: return TensorDataType::BFloat16;
    case CVI_FMT_INT16: return TensorDataType::Int16;
    case CVI_FMT_UINT16: return TensorDataType::UInt16;
    case CVI_FMT_INT8: return TensorDataType::Int8;
    case CVI_FMT_UINT8: return TensorDataType::UInt8;
    default: return TensorDataType::Unknown;
    }
}

TensorInfo describe(CVI_TENSOR &tensor)
{
    TensorInfo result;
    const char *name = CVI_NN_TensorName(&tensor);
    result.name = name ? name : "";
    const CVI_SHAPE shape = CVI_NN_TensorShape(&tensor);
    result.shape.assign(shape.dim, shape.dim + shape.dim_size);
    result.dataType = convertType(tensor.fmt);
    result.elementCount = CVI_NN_TensorCount(&tensor);
    result.byteSize = CVI_NN_TensorSize(&tensor);
    result.quantizationScale = CVI_NN_TensorQuantScale(&tensor);
    result.quantizationZeroPoint = CVI_NN_TensorQuantZeroPoint(&tensor);
    return result;
}

} // namespace

class Model::Impl
{
  public:
    ~Impl()
    {
        close();
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (handle) {
            CVI_NN_CleanupModel(handle);
        }
        handle = nullptr;
        inputTensors = nullptr;
        outputTensors = nullptr;
        inputCount = 0;
        outputCount = 0;
        inputInfo.clear();
        outputInfo.clear();
    }

    void fail(ErrorCode code, std::string message) const
    {
        error = {code, std::move(message)};
    }

    mutable std::mutex mutex;
    CVI_MODEL_HANDLE handle = nullptr;
    CVI_TENSOR *inputTensors = nullptr;
    CVI_TENSOR *outputTensors = nullptr;
    int32_t inputCount = 0;
    int32_t outputCount = 0;
    std::vector<TensorInfo> inputInfo;
    std::vector<TensorInfo> outputInfo;
    mutable Error error;
};

Model::Model() : impl_(std::make_unique<Impl>()) {}
Model::~Model() = default;
Model::Model(Model &&) noexcept = default;
Model &Model::operator=(Model &&) noexcept = default;

bool Model::load(const std::string &path)
{
    if (path.empty()) {
        impl_->fail(ErrorCode::InvalidArgument, "model path is empty");
        return false;
    }
    impl_->close();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CVI_MODEL_HANDLE handle = nullptr;
    int rc = CVI_NN_RegisterModel(path.c_str(), &handle);
    if (rc != 0 || !handle) {
        impl_->fail(ErrorCode::BackendError, "CVI_NN_RegisterModel failed (rc=" + std::to_string(rc) + ")");
        return false;
    }
    impl_->handle = handle;
    rc = CVI_NN_GetInputOutputTensors(handle, &impl_->inputTensors, &impl_->inputCount,
                                      &impl_->outputTensors, &impl_->outputCount);
    if (rc != 0 || impl_->inputCount <= 0 || impl_->outputCount <= 0) {
        CVI_NN_CleanupModel(handle);
        impl_->handle = nullptr;
        impl_->fail(ErrorCode::BackendError,
                    "CVI_NN_GetInputOutputTensors failed or returned no tensors (rc=" + std::to_string(rc) + ")");
        return false;
    }
    for (int32_t i = 0; i < impl_->inputCount; ++i) impl_->inputInfo.push_back(describe(impl_->inputTensors[i]));
    for (int32_t i = 0; i < impl_->outputCount; ++i) impl_->outputInfo.push_back(describe(impl_->outputTensors[i]));
    impl_->error = {};
    return true;
}

void Model::close() noexcept { impl_->close(); }
bool Model::isLoaded() const noexcept { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->handle != nullptr; }
const std::vector<TensorInfo> &Model::inputs() const noexcept { return impl_->inputInfo; }
const std::vector<TensorInfo> &Model::outputs() const noexcept { return impl_->outputInfo; }

bool Model::setInput(std::size_t index, const void *data, std::size_t bytes)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->handle) {
        impl_->fail(ErrorCode::NotInitialized, "no model is loaded");
        return false;
    }
    if (index >= impl_->inputInfo.size() || !data) {
        impl_->fail(ErrorCode::InvalidArgument, "invalid input tensor index or null data");
        return false;
    }
    if (bytes != impl_->inputInfo[index].byteSize) {
        impl_->fail(ErrorCode::InvalidArgument, "input byte count does not match tensor size");
        return false;
    }
    void *destination = CVI_NN_TensorPtr(&impl_->inputTensors[index]);
    if (!destination) {
        impl_->fail(ErrorCode::BackendError, "runtime returned a null input tensor buffer");
        return false;
    }
    std::memcpy(destination, data, bytes);
    impl_->error = {};
    return true;
}

bool Model::setInput(std::size_t index, const std::vector<std::uint8_t> &data)
{
    return setInput(index, data.data(), data.size());
}

bool Model::run()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->handle) {
        impl_->fail(ErrorCode::NotInitialized, "no model is loaded");
        return false;
    }
    const int rc = CVI_NN_Forward(impl_->handle, impl_->inputTensors, impl_->inputCount,
                                  impl_->outputTensors, impl_->outputCount);
    if (rc != 0) {
        impl_->fail(ErrorCode::BackendError, "CVI_NN_Forward failed (rc=" + std::to_string(rc) + ")");
        return false;
    }
    impl_->error = {};
    return true;
}

Tensor Model::output(std::size_t index) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Tensor result;
    if (!impl_->handle || index >= impl_->outputInfo.size()) {
        impl_->fail(ErrorCode::InvalidArgument, "invalid output tensor index or no model loaded");
        return result;
    }
    result.info = impl_->outputInfo[index];
    const void *source = CVI_NN_TensorPtr(&impl_->outputTensors[index]);
    if (!source) {
        impl_->fail(ErrorCode::BackendError, "runtime returned a null output tensor buffer");
        return {};
    }
    result.data.resize(result.info.byteSize);
    std::memcpy(result.data.data(), source, result.data.size());
    impl_->error = {};
    return result;
}

Error Model::lastError() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }

const char *toString(TensorDataType type) noexcept
{
    switch (type) {
    case TensorDataType::Float32: return "float32";
    case TensorDataType::Int32: return "int32";
    case TensorDataType::UInt32: return "uint32";
    case TensorDataType::BFloat16: return "bfloat16";
    case TensorDataType::Int16: return "int16";
    case TensorDataType::UInt16: return "uint16";
    case TensorDataType::Int8: return "int8";
    case TensorDataType::UInt8: return "uint8";
    default: return "unknown";
    }
}

} // namespace recamera
