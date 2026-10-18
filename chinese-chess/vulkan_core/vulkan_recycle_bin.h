/**
 * @file vulkan_recycle_bin.h
 * @brief 按提交完成 token 延迟销毁 Vulkan 资源的回收站。
 * @details 资源登记时带上最后使用它的 token（或 token 集），release() 只销毁全部 token 都已完成的条目。
 *          不同队列的 token 独立查询，不因队首未完成而阻塞其余条目。
 *          容器与销毁回调必须在接管资源所有权之前分配好，分配失败时调用方仍拥有资源。
 */
#pragma once

#include "vulkan_queue.h"
#include "vulkan_submission_token.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Delayed destruction keyed by explicit completion tokens. A resource is only
// destroyed once every recorded last-use token has completed; entries from
// different queues are checked independently instead of blocking the queue
// head on one global counter.
/**
 * @brief 以提交 token 为凭据的延迟销毁队列，禁止拷贝与移动。
 * @details 每个条目记录一组 last_use token，只有它们全部完成才销毁资源；release() 按插入顺序清理。
 *          retire_after_submitted 用已注册队列的已提交前沿作为 last use，适用于“替换发生在下一次引用之前”的资源。
 *          析构会等待所有条目完成，保证 descriptor set 先于 pool 这类依赖顺序在异常展开时也不乱。
 */
class vulkan_recycle_bin
{
public:
    /** @brief 默认构造：空回收站，尚未注册任何队列。 */
    vulkan_recycle_bin()                                     = default;
    /**
     * @brief 析构：按插入顺序等待所有 token 完成并销毁剩余资源。
     * @note 会阻塞到 GPU 完成；每个 token 都走 wait_for_cleanup，设备丢失时降级为打印后继续。
     */
    ~vulkan_recycle_bin();
    /** @brief 禁止拷贝构造：回收站独占条目与其中资源的所有权。 */
    vulkan_recycle_bin(const vulkan_recycle_bin&)            = delete;
    /** @brief 禁止拷贝赋值：回收站独占条目与其中资源的所有权。 */
    vulkan_recycle_bin& operator=(const vulkan_recycle_bin&) = delete;
    /** @brief 禁止移动构造：条目里的队列指针与资源句柄不能随对象搬移而悬空。 */
    vulkan_recycle_bin(vulkan_recycle_bin&&)                 = delete;
    /** @brief 禁止移动赋值：条目里的队列指针与资源句柄不能随对象搬移而悬空。 */
    vulkan_recycle_bin& operator=(vulkan_recycle_bin&&)      = delete;

    /**
     * @brief 登记一个队列，供 retire_after_submitted 读取已提交前沿。
     * @param _queue 被登记的队列；回收站只保存指针，不拥有它。
     * @note 队列必须比回收站活得久；重复登记同一条队列没有意义，只会让 last_use 里多一个 token。
     */
    void add_queue(const vulkan_queue& _queue);

    /**
     * @brief 登记一个资源，等单个 last-use token 完成后再销毁。
     * @param _resource 右值资源，所有权移入回收站。
     * @param _last_use 最后使用该资源的提交 token。
     * @param _message 销毁时的日志标识。
     * @throws std::bad_alloc 登记所需的容器或回调分配失败（先排空该 token 再重抛）。
     * @note 资源必须可无异常移动构造；左值参数被 requires 约束拒绝。
     */
    template <typename T>
        requires(!std::is_lvalue_reference_v<T>) && std::movable<std::remove_cvref_t<T>>
    void retire(T&& _resource, vulkan_submission_token _last_use, std::string_view _message);

    /**
     * @brief 登记一个资源，等一组 last-use token 全部完成后再销毁。
     * @param _resource 右值资源，所有权移入回收站。
     * @param _last_use 最后使用该资源的 token 集合，全部完成才可销毁；空集合表示下一次 release() 即可销毁。
     * @param _message 销毁时的日志标识。
     * @throws std::bad_alloc 登记所需的容器或回调分配失败（先排空这些 token 再重抛）。
     * @note 资源必须可无异常移动构造；左值参数被 requires 约束拒绝。
     */
    template <typename T>
        requires(!std::is_lvalue_reference_v<T>) && std::movable<std::remove_cvref_t<T>>
    void retire(T&& _resource, std::span<const vulkan_submission_token> _last_use, std::string_view _message);

    // Last use is "everything already submitted on the queues registered with
    // this bin". Safe for resources whose replacement happens before the next
    // submission that could reference them.
    /**
     * @brief 以所有已注册队列的已提交前沿作为 last use 登记资源。
     * @param _resource 右值资源，所有权移入回收站。
     * @param _message 销毁时的日志标识。
     * @throws std::logic_error 当回收站还没有注册任何队列。
     * @throws std::bad_alloc 登记所需的容器或回调分配失败。
     * @note 只快照调用时刻的 get_latest_token()；调用方必须保证之后不再有引用该资源的提交。
     */
    template <typename T>
        requires(!std::is_lvalue_reference_v<T>) && std::movable<std::remove_cvref_t<T>>
    void retire_after_submitted(T&& _resource, std::string_view _message);

    /**
     * @brief 销毁所有 last-use 已完成的条目，未完成的留到下次调用。
     * @throws vk::SystemError 查询时间线计数失败（如设备丢失）时抛出；已完成的条目仍保持原顺序。
     * @note 按插入顺序销毁，满足 descriptor set 先于 pool 这类配对顺序；不同队列独立判定，不阻塞队首。
     */
    void release();

private:
    /**
     * @brief 一条待销毁记录：last-use token 集、日志标识与持有资源的销毁回调。
     * @details destroy 的捕获里持有 optional 资源，调用体为空；条目被 erase 时回调析构，资源随之析构。
     *          因此真正的销毁发生在 release() 擦除条目的瞬间，而不是登记资源的时候。
     */
    struct entry
    {
        /** @brief 最后使用该资源的 token 集合，全部完成才可销毁。 */
        std::vector<vulkan_submission_token>   last_use;
        /** @brief 调试与日志标识，output 打开时由 release() 打印。 */
        std::string                     message;
        /** @brief 持有资源所有权的回调；调用体为空，析构时释放资源。 */
        std::move_only_function<void()> destroy;
    };

    /** @brief 调试输出开关：为 true 时 release() 打印每条被销毁资源的 message。 */
    constexpr inline static bool output = false;

    /** @brief 已注册队列的裸指针，只用于 retire_after_submitted 取已提交前沿，不拥有队列。 */
    std::vector<const vulkan_queue*> queues;
    /** @brief 按插入顺序保存的待销毁条目。 */
    std::deque<entry>                resources;
};

template <typename T>
    requires(!std::is_lvalue_reference_v<T>) && std::movable<std::remove_cvref_t<T>>
inline void vulkan_recycle_bin::retire(T&& _resource, vulkan_submission_token _last_use, std::string_view _message)
{
    const std::array<vulkan_submission_token, 1> last_use = {_last_use};
    retire(std::forward<T>(_resource), std::span<const vulkan_submission_token>(last_use), std::move(_message));
}

template <typename T>
    requires(!std::is_lvalue_reference_v<T>) && std::movable<std::remove_cvref_t<T>>
inline void vulkan_recycle_bin::retire(T&& _resource, std::span<const vulkan_submission_token> _last_use, std::string_view _message)
{
    using resource_type = std::remove_cvref_t<T>;
    static_assert(std::is_nothrow_move_constructible_v<resource_type>);
    try
    {
        // Allocate every container/callback BEFORE taking ownership. If any
        // allocation fails, the caller still owns the resource and can unwind
        // safely after the catch below drains its submitted last users.
        auto holder = std::make_unique<std::optional<resource_type>>();
        auto* slot = holder.get();
        resources.push_back(entry{.last_use = std::vector<vulkan_submission_token>(_last_use.begin(), _last_use.end()),
                                  .message = std::string(_message),
                                  .destroy = [holder = std::move(holder)]() {}});
        slot->emplace(std::move(_resource));
    }
    catch (...)
    {
        for (const auto& token : _last_use) token.wait_for_cleanup();
        throw;
    }
}

template <typename T>
    requires(!std::is_lvalue_reference_v<T>) && std::movable<std::remove_cvref_t<T>>
inline void vulkan_recycle_bin::retire_after_submitted(T&& _resource, std::string_view _message)
{
    if (queues.empty())
    {
        // Without a registered queue there is no submitted frontier; retiring
        // immediately would silently reintroduce the old CPU-counter bug.
        throw std::logic_error("retire_after_submitted requires at least one registered queue.");
    }

    try
    {
        std::vector<vulkan_submission_token> last_use;
        last_use.reserve(queues.size());
        for (const auto* queue : queues)
        {
            last_use.push_back(queue->get_latest_token());
        }
        retire(std::forward<T>(_resource), std::span<const vulkan_submission_token>(last_use), std::move(_message));
    }
    catch (...)
    {
        for (const auto* queue : queues) queue->get_latest_token().wait_for_cleanup();
        throw;
    }
}
