/**
 * @file vulkan_commandbuffer.h
 * @brief 绑定到 vulkan_queue 的命令缓冲包装：录制、提交并给出 timeline 完成凭据。
 * @details 每个实例记住 owner queue 和上一次成功提交的 submission_token：begin_record 先等
 *          这个 token 再 reset，避免 GPU 仍在执行时复用命令缓冲；submit 只有提交成功后才
 *          发布 token，失败会把已累积的等待项、信号项与依赖清空再重抛。
 *          等待项分两类：二进制 semaphore（swapchain acquire/present）与其它提交的 timeline
 *          token（提交时合并进 vkQueueSubmit2 的等待项）。
 */

#pragma once

#include "vulkan_submission_token.h"

#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

class vulkan_queue;

/**
 * @brief 一条命令缓冲的生命周期管理（分配 → 录制 → 提交 → 完成）与提交依赖累积。
 * @details create 校验命令池必须与 owner queue 匹配；同一命令缓冲的复用靠 begin_record
 *          的等待串行化 —— 它先等上一次提交的 token 再 reset。析构走 wait_for_cleanup，
 *          覆盖「局部命令缓冲已成功提交、但后续分配或提交抛异常」的清理路径。等待/信号/
 *          依赖列表在每次成功提交后清空，同一条命令缓冲不能靠一次添加就反复提交。
 */
class vulkan_commandbuffer
{
public:
    /** @brief 构造未分配的包装：句柄为 nullptr、未绑定 queue、token 无效。 */
    vulkan_commandbuffer()                                       = default;
    /**
     * @brief 析构前等自己的 token 完成，避免命令缓冲在 GPU 执行中被销毁。
     * @note 正常路径里回收站已经确认过完成，这里兜底的是异常路径。
     */
    ~vulkan_commandbuffer();
    /** @brief 禁止拷贝：命令缓冲句柄与提交状态必须有唯一持有者。 */
    vulkan_commandbuffer(const vulkan_commandbuffer&)            = delete;
    /** @brief 禁止拷贝赋值：提交状态只能随对象一起移动。 */
    vulkan_commandbuffer& operator=(const vulkan_commandbuffer&) = delete;
    /**
     * @brief 移动构造：接管句柄、owner queue、token 与待提交列表，源对象留下空状态。
     * @param _other 被移动的源包装，构造后其句柄为 nullptr、owner queue 为空指针。
     */
    vulkan_commandbuffer(vulkan_commandbuffer&& _other) noexcept;
    /**
     * @brief 移动赋值：自赋值直接返回，否则逐成员 std::ranges::swap。
     * @param _other 被交换的源包装，交换后两边的句柄与待提交列表互换。
     * @return *this。
     */
    vulkan_commandbuffer& operator=(vulkan_commandbuffer&& _other) noexcept;

    /**
     * @brief 从一个命令池批量分配命令缓冲，并把每个句柄移进独立的包装对象。
     * @param _device 分配命令缓冲的设备。
     * @param _allocate_info 分配信息；commandPool 必须与 _queue 的命令池一致。
     * @param _queue 非拥有的 owner queue，用于提交与命令池校验。
     * @return 与分配数量等长的包装数组，每个元素携带自己的 owner queue 指针。
     * @throws std::runtime_error _queue 为空，或 commandPool 与该 queue 的命令池不一致。
     * @throws vk::SystemError 分配失败时由 Vulkan-Hpp 抛出。
     * @note _queue 只保存指针，调用方必须保证它活得比返回的命令缓冲久。
     */
    [[nodiscard]] static std::vector<vulkan_commandbuffer> create(const vk::raii::Device&              _device,
                                                                  const vk::CommandBufferAllocateInfo& _allocate_info,
                                                                  const vulkan_queue*                  _queue);

    /**
     * @brief 等上一次提交完成后 reset 并 begin 一条新的录制。
     * @param _usage 录制用途标志，原样交给 VkCommandBufferBeginInfo。
     * @note 这里是命令缓冲自身的复用保护，不依赖上层是否已等待对应 frame slot。
     * @note 会清掉上一次录制的内容，提交前必须再走一次 end_record。
     */
    void begin_record(vk::CommandBufferUsageFlags _usage);
    /** @brief 结束录制（vkEndCommandBuffer）；之后才能 submit。 */
    void end_record() const;

    // Publishes a per-queue completion token only after a successful submit.
    /**
     * @brief 把当前录制的命令缓冲连同等待项、信号项、timeline 依赖提交给 owner queue。
     * @return 本次提交的完成凭据；只有提交成功后才发布。
     * @throws std::runtime_error 命令缓冲未分配或已被移走，或没有绑定 owner queue。
     * @note 提交失败会先清空 waited/signal/dependency 再重抛，避免重试时带上陈旧等待。
     * @note 成功提交后这三张表也会清空，下一次提交要重新添加。
     */
    [[nodiscard]] vulkan_submission_token submit();
    /**
     * @brief 阻塞到本命令缓冲最近一次提交完成。
     * @note 从未成功提交过时直接返回，不做任何等待。
     */
    void                           wait() const;

    // Binary semaphore waits/signals (swapchain acquire/present).
    /**
     * @brief 追加一个二进制 semaphore 等待项（如 swapchain acquire 的 image-ready）。
     * @param _submit_info semaphore 与阶段掩码；semaphore 的生命周期由调用方保证。
     */
    void add_waited_info(const vk::SemaphoreSubmitInfo& _submit_info);
    /**
     * @brief 批量追加二进制 semaphore 等待项。
     * @param _submit_infos 等待项区间，按顺序追加到内部列表。
     */
    void add_waited_info(std::span<const vk::SemaphoreSubmitInfo> _submit_infos);
    /**
     * @brief 追加一个二进制 semaphore 信号项（如 render-finished）。
     * @param _submit_info 要信号的 semaphore 与阶段掩码。
     */
    void add_signal_info(const vk::SemaphoreSubmitInfo& _submit_info);

    // Timeline dependencies from other submissions.
    /**
     * @brief 追加一个 timeline 依赖，提交时转成 vkQueueSubmit2 的等待项。
     * @param _dependency 其它提交返回的完成凭据。
     * @note 使用已成功提交的 token（或无依赖/零值前沿）；若等待的值始终没有后续 signal 达到，提交将无法完成。
     */
    void add_dependency(vulkan_submission_token _dependency);
    /**
     * @brief 批量追加 timeline 依赖。
     * @param _dependencies 依赖 token 区间。
     */
    void add_dependency(std::span<const vulkan_submission_token> _dependencies);

    /**
     * @brief 取最近一次成功提交的完成凭据。
     * @return 提交 token；从未成功提交时 token 的 is_valid() 为 false。
     */
    [[nodiscard]] vulkan_submission_token               get_submission_token() const noexcept;
    /**
     * @brief 取底层命令缓冲句柄。
     * @return vk::raii::CommandBuffer 的常量引用，用于直接录制或传给 Vulkan 调用。
     * @note 调用方自己保证不在 GPU 执行期间往里录命令。
     */
    [[nodiscard]] const vk::raii::CommandBuffer& operator*() const noexcept;

private:
    /**
     * @brief 接管已分配的句柄并绑定 owner queue，不做任何校验。
     * @param _commandbuffer 已经分配好的命令缓冲，移动进来。
     * @param _queue 非拥有的 owner queue。
     */
    void create(vk::raii::CommandBuffer&& _commandbuffer, const vulkan_queue* _queue) noexcept;

private:
    /** @brief 底层命令缓冲句柄；未分配或已被移走时为 nullptr。 */
    vk::raii::CommandBuffer commandbuffer = nullptr;
    /** @brief 非拥有的 owner queue，submit 与命令池校验都靠它。 */
    const vulkan_queue*     queue         = nullptr;
    /** @brief 最近一次成功提交的完成凭据，begin_record 与 wait 用它判断是否可复用。 */
    vulkan_submission_token        token;

    /** @brief 待提交的二进制 semaphore 等待项，成功提交后清空。 */
    std::vector<vk::SemaphoreSubmitInfo> waited_info;
    /** @brief 待提交的二进制 semaphore 信号项，成功提交后清空。 */
    std::vector<vk::SemaphoreSubmitInfo> signal_info;
    /** @brief 待提交的 timeline 依赖 token，submit 时合并进等待项后清空。 */
    std::vector<vulkan_submission_token>        dependencies;
};
