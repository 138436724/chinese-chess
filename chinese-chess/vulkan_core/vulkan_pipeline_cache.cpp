#include "vulkan_pipeline_cache.h"

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

constexpr std::string_view pipeline_cache_path = "resources\\cache\\pipeline_cache.cache";

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

[[nodiscard]] bool is_cache_valid(const vk::raii::PhysicalDevice& _physical_device, std::span<const uint8_t> _data) noexcept
{
    if (_data.size() < sizeof(vk::PipelineCacheHeaderVersionOne))
    {
        return false;
    }

    const auto* const header = reinterpret_cast<const vk::PipelineCacheHeaderVersionOne*>(_data.data());
    if (header->headerVersion != vk::PipelineCacheHeaderVersion::eOne || header->headerSize < sizeof(vk::PipelineCacheHeaderVersionOne))
    {
        return false;
    }

    const auto properties = _physical_device.getProperties();
    return header->vendorID == properties.vendorID && header->deviceID == properties.deviceID
           && header->pipelineCacheUUID == properties.pipelineCacheUUID;
}

}  // namespace

void vulkan_pipeline_cache::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device) noexcept
{
    const std::vector<uint8_t> data = read_file(pipeline_cache_path);
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

const vk::raii::PipelineCache& vulkan_pipeline_cache::operator*() const noexcept
{
    return pipeline_cache;
}
