#pragma once

#include "vulkan_semaphore.h"

#include <deque>
#include <functional>
#include <print>
#include <utility>

class vulkan_recycle_bin
{
public:
    vulkan_recycle_bin()                                = default;
    ~vulkan_recycle_bin()                               = default;
    vulkan_recycle_bin(vulkan_recycle_bin&)             = delete;
    vulkan_recycle_bin& operator=(vulkan_recycle_bin&)  = delete;
    vulkan_recycle_bin(vulkan_recycle_bin&&)            = delete;
    vulkan_recycle_bin& operator=(vulkan_recycle_bin&&) = delete;

    void create(vulkan_semaphore* _semaphore);

    template <typename T>
    void retire(T&& _resource) noexcept;

    void release() noexcept;

private:
    vulkan_semaphore*                                                        semaphore = nullptr;
    std::deque<std::pair<uint64_t, std::move_only_function<void(uint64_t)>>> resources;
};

template <typename T>
inline void vulkan_recycle_bin::retire(T&& _resource) noexcept
{
    uint64_t cpu_value = semaphore->get_cpu_value();
    resources.push_back({cpu_value, [res = std::move(_resource)](uint64_t _value) mutable {
#ifndef NDEBUG
                             std::println("Resource retire value is {}.", _value);
#endif  // !NDEBUG
                         }});
}
