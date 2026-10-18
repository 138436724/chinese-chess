/**
 * @file vulkan_pipeline_cache.cpp
 * @brief vulkan_pipeline_cache 的实现：缓存文件读写、头部校验与加载失败回退。
 * @details 固定路径 resources/cache/pipeline_cache.cache；打不开或长度非正时 read_file 返回空 vector，
 *          create 因此转入空缓存分支。写盘前先 create_directories，写完再查流状态。
 *          校验只认 PipelineCacheHeaderVersion::eOne，且 vendor / device / UUID 三者全等才算命中。
 */

#include "vulkan_pipeline_cache.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

/** @brief 缓存文件路径：相对运行目录（chinese-chess/），对应 .gitignore 里的 resources/cache/。 */
constexpr std::string_view pipeline_cache_path = "resources\\cache\\pipeline_cache.cache";

/**
 * @brief 整块读入文件；不存在、打不开或长度为 0 时返回空 vector。
 * @note 分配异常未捕获，read 后也未检查是否读满；此处不提供完整的读取失败回退保证。
 */
[[nodiscard]] std::vector<uint8_t> read_file(const std::filesystem::path& _path)
{
    std::ifstream file(_path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0)
    {
        return {};
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/**
 * @brief 建目录后整块写出；打开或写入失败抛 std::runtime_error（由 save 捕获后吞掉）。
 * @note 写之前 create_directories(parent_path)，所以 resources/cache/ 不存在也能落盘。
 */
void write_file(const std::filesystem::path& _path, const std::span<const uint8_t> _data)
{
    std::filesystem::create_directories(_path.parent_path());
    std::ofstream file(_path, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error(std::format("Failed to open {} for writing.", _path.generic_string()));
    }

    file.write(reinterpret_cast<const char*>(_data.data()), static_cast<std::streamsize>(_data.size()));
    if (!file)
    {
        throw std::runtime_error(std::format("Failed to write pipeline cache to {}.", _path.generic_string()));
    }
}

/**
 * @brief 校验缓存头：版本为 eOne、头部长度够，且 vendorID / deviceID / pipelineCacheUUID 与当前设备全等。
 * @note 由设备提供的 UUID 判断缓存兼容性，并不直接比较驱动版本号。属性按值取 getProperties2().properties，
 *       避免把引用绑到临时对象上（本文件用不到 *2 链式查询，只取核心属性）。
 */
[[nodiscard]] bool is_cache_valid(const vk::raii::PhysicalDevice& _physical_device, std::span<const uint8_t> _data)
{
    if (_data.size() < sizeof(vk::PipelineCacheHeaderVersionOne))
    {
        return false;
    }

    vk::PipelineCacheHeaderVersionOne header{};
    std::memcpy(&header, _data.data(), sizeof(header));

    if (header.headerVersion != vk::PipelineCacheHeaderVersion::eOne || header.headerSize < sizeof(vk::PipelineCacheHeaderVersionOne))
    {
        return false;
    }

    const auto properties = _physical_device.getProperties2().properties;
    return header.vendorID == properties.vendorID && header.deviceID == properties.deviceID
           && header.pipelineCacheUUID == properties.pipelineCacheUUID;
}

}  // namespace

/**
 * @brief 数据为空或头部校验失败就建空缓存，否则用读到的字节建缓存；构造抛 vk::SystemError 时也退到空缓存。
 * @note 仅带初始数据创建失败时打印（Debug）或静默（Release）回退；读取、查询及创建空缓存仍可能抛异常。
 */
void vulkan_pipeline_cache::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device)
{
    const auto data = read_file(pipeline_cache_path);
    if (data.empty() || !is_cache_valid(_physical_device, data))
    {
        pipeline_cache = vk::raii::PipelineCache(_device, vk::PipelineCacheCreateInfo());
        return;
    }

    try
    {
        pipeline_cache = vk::raii::PipelineCache(_device, vk::PipelineCacheCreateInfo({}, data.size(), data.data()));
    }
#ifndef NDEBUG
    catch (const vk::SystemError& _error)
    {
        std::println(std::cerr, "Failed to load pipeline cache: {}", _error.what());
        pipeline_cache = vk::raii::PipelineCache(_device, vk::PipelineCacheCreateInfo());
    }
#else
    catch (const vk::SystemError&)
    {
        pipeline_cache = vk::raii::PipelineCache(_device, vk::PipelineCacheCreateInfo());
    }
#endif  // !NDEBUG
}

/**
 * @brief 取 getData() 写盘；空句柄直接返回，取数据或写盘失败按 Debug / Release 分支处理。
 * @note 只捕获实现中列出的运行时/Vulkan 错误；noexcept 不等于捕获所有异常，未捕获异常会 terminate。
 */
void vulkan_pipeline_cache::save() const noexcept
{
    if (!(*pipeline_cache))
    {
        return;
    }

    try
    {
        std::vector<uint8_t> data = pipeline_cache.getData();
        write_file(std::filesystem::path(pipeline_cache_path), data);
    }
#ifndef NDEBUG
    catch (const vk::SystemError& _error)
    {
        std::println(std::cerr, "Cannot get pipeline cache data: {}", _error.what());
    }
    catch (const std::runtime_error& _error)
    {
        std::println(std::cerr, "Failed to save pipeline cache: {}", _error.what());
    }
#else
    catch (const std::runtime_error&)
    {
    }
#endif  // !NDEBUG
}

/** @brief 直接返回缓存句柄，不做惰性创建；未 create 时是空句柄。 */
const vk::raii::PipelineCache& vulkan_pipeline_cache::operator*() const noexcept
{
    return pipeline_cache;
}
