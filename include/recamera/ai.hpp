#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <recamera/error.hpp>

namespace recamera
{

enum class TensorDataType
{
    Float32,
    Int32,
    UInt32,
    BFloat16,
    Int16,
    UInt16,
    Int8,
    UInt8,
    Unknown
};

struct TensorInfo
{
    std::string name;
    std::vector<int> shape;
    TensorDataType dataType = TensorDataType::Unknown;
    std::size_t elementCount = 0;
    std::size_t byteSize = 0;
    float quantizationScale = 1.0F;
    int quantizationZeroPoint = 0;
};

struct Tensor
{
    TensorInfo info;
    std::vector<std::uint8_t> data;
};

// Generic CVIMODEL runner for the SG2002 TPU. Model-specific resize,
// normalization and post-processing intentionally remain application policy.
class Model
{
  public:
    Model();
    ~Model();
    Model(Model &&) noexcept;
    Model &operator=(Model &&) noexcept;
    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;

    bool load(const std::string &path);
    void close() noexcept;
    bool isLoaded() const noexcept;

    const std::vector<TensorInfo> &inputs() const noexcept;
    const std::vector<TensorInfo> &outputs() const noexcept;

    // Copies exactly one complete tensor into runtime-owned input memory.
    bool setInput(std::size_t index, const void *data, std::size_t bytes);
    bool setInput(std::size_t index, const std::vector<std::uint8_t> &data);
    bool run();
    Tensor output(std::size_t index) const;

    Error lastError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

const char *toString(TensorDataType type) noexcept;

} // namespace recamera
