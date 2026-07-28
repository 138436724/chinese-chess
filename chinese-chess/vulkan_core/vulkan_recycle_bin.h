#pragma once

#include "vulkan_semaphore.h"

#include <deque>
#include <functional>
#include <print>
#include <utility>

class vulkan_recycle_bin
{
public:
    vulkan_recycle_bin()                                     = default;
    ~vulkan_recycle_bin()                                    = default;
    vulkan_recycle_bin(const vulkan_recycle_bin&)            = delete;
    vulkan_recycle_bin& operator=(const vulkan_recycle_bin&) = delete;
    vulkan_recycle_bin(vulkan_recycle_bin&&)                 = delete;
    vulkan_recycle_bin& operator=(vulkan_recycle_bin&&)      = delete;

    void create(const vulkan_semaphore* _semaphore);

    template <typename T>
    void retire(T&& _resource, std::string&& _message);

    void release() noexcept;

private:
    const vulkan_semaphore*                                          semaphore = nullptr;
    std::deque<std::pair<uint64_t, std::move_only_function<void()>>> resources;
};

template <typename T>
inline void vulkan_recycle_bin::retire(T&& _resource, std::string&& _message)
{
    uint64_t cpu_value = semaphore->get_cpu_value();
    resources.emplace_back(cpu_value, [resource = std::move(_resource), message = std::move(_message)]() mutable {
#ifndef NDEBUG
        std::println("{}", message);
#endif  // !NDEBUG
    });
}
