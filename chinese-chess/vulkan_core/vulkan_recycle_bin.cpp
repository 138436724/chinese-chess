#include "vulkan_recycle_bin.h"

void vulkan_recycle_bin::create(vulkan_semaphore* _semaphore)
{
    if (_semaphore)
    {
        semaphore = _semaphore;
    }
    else
    {
        throw std::runtime_error("Recycle can not use nullpter as semaphore!");
    }
}

void vulkan_recycle_bin::release() noexcept
{
    uint64_t gpu_val = semaphore->get_gpu_value();
    while (!resources.empty() && resources.front().first < gpu_val)
    {
#ifndef NDEBUG
        auto& [_value, _func] = resources.front();
        _func(_value);
#endif  // !NDEBUG
        resources.pop_front();
    }
}
