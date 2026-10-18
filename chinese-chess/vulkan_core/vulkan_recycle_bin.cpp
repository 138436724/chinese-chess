/**
 * @file vulkan_recycle_bin.cpp
 * @brief 回收站的队列注册、析构排空与按 token 判定释放。
 * @details release() 为每个条目查询各时间线的 GPU 计数，同一时间线在一次调用内只查询一次。
 *          析构不丢弃条目：逐条等待 token 完成后再 pop，保证依赖资源的销毁顺序。
 *          资源所有权挂在条目里的 move_only_function 捕获上，erase 销毁条目时一并释放，顺序与插入顺序一致。
 */
#include "vulkan_recycle_bin.h"

#include "vulkan_semaphore.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

/**
 * @brief 登记队列，retire_after_submitted 会遍历它读取已提交前沿。
 * @note 只保存指针，队列的生命周期必须覆盖回收站。
 */
void vulkan_recycle_bin::add_queue(const vulkan_queue& _queue)
{
    queues.push_back(&_queue);
}

/**
 * @brief 按插入顺序排空所有条目：等 token 完成后再 pop，资源随条目析构。
 * @note 异常展开路径同样执行；token 等待走 wait_for_cleanup，不会因设备丢失卡死。
 */
vulkan_recycle_bin::~vulkan_recycle_bin()
{
    // Preserve dependent-resource destruction order even during unwinding.
    while (!resources.empty())
    {
        for (const auto& token : resources.front().last_use) token.wait_for_cleanup();
        resources.pop_front();
    }
}

/**
 * @brief 释放所有 last-use 已完成的条目，未完成的保留到下次调用。
 * @note 按插入顺序 erase 以保持配对资源（descriptor set 与 pool）的销毁顺序。
 * @note 同一时间线在一次调用内只查询一次 GPU 计数，避免逐条目重复查询。
 */
void vulkan_recycle_bin::release()
{
    std::vector<std::pair<const vulkan_semaphore*, uint64_t>> gpu_values;

    const auto is_complete = [&gpu_values](const vulkan_submission_token& _token) {
        if (!_token.is_valid())
        {
            return true;
        }

        const auto iter = std::ranges::find_if(gpu_values, [&_token](const auto& _pair) {
            return _pair.first == _token.semaphore;
        });
        if (iter != gpu_values.end())
        {
            return iter->second >= _token.value;
        }

        gpu_values.emplace_back(_token.semaphore, _token.semaphore->get_gpu_value());
        return gpu_values.back().second >= _token.value;
    };

    // Destroy in insertion order. Paired resources rely on it: descriptor sets
    // must be freed before the pool they were allocated from, and a vector of
    // raii descriptor sets frees them in its destructor.
    for (auto iter = resources.begin(); iter != resources.end();)
    {
        if (std::ranges::all_of(iter->last_use, is_complete))
        {
            if (output)
            {
                std::println("Retire resource: {}", iter->message);
            }
            iter = resources.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}
