#include "vulkan_recycle_bin.h"

#include <stdexcept>

void vulkan_recycle_bin::create(const vulkan_semaphore* _semaphore)
{
    if (!_semaphore)
    {
        throw std::runtime_error("Recycle cannot use nullptr as semaphore!");
    }
    semaphore = _semaphore;
}

void vulkan_recycle_bin::release() noexcept
{
    const uint64_t gpu_val = semaphore->get_gpu_value();
    while (!resources.empty() && resources.front().first < gpu_val)
    {
#ifndef NDEBUG
        if constexpr (output)
        {
            auto& [_value, _print_message] = resources.front();
            std::print("Semaphore value {}. Retire resource value {}. ", gpu_val, _value);
            _print_message();
        }
#endif  // !NDEBUG
        resources.pop_front();
    }
}
