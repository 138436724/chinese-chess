#include "vulkan_recycle_bin.h"

void vulkan_recycle_bin::create(vulkan_semaphore* _semaphore) noexcept
{
	semaphore = _semaphore;
}

void vulkan_recycle_bin::release() noexcept
{
	uint64_t gpu_val = semaphore->get_gpu_value();
	while (!resources.empty() && resources.front().first <= gpu_val)
	{
#ifndef NDEBUG
		resources.front().second();
#endif // !NDEBUG
		resources.pop_front();
	}
}
