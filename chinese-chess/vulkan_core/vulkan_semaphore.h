/**
 * @file vulkan_semaphore.h
 * @brief 作为每队列完成凭据的时间线信号量包装。
 * @details 时间线值由 vulkan_queue 在提交锁内用 reserve() 分配，信号量本身不发布完成值，
 *          只提供等待与查询 GPU 计数的能力。
 *          与二进制信号量不同，同一个句柄可跨多次提交复用，因此适合做延迟回收的判定依据。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

// Timeline semaphore used as the per-queue completion credential.
// Values are allocated with reserve() by vulkan_queue while its submission
// mutex is held; vulkan_semaphore itself does not publish completion values.
/**
 * @brief 时间线信号量的 RAII 包装，禁止拷贝与移动。
 * @details create() 后计数从 0 开始；reserve() 原子地取下一个待信号值，提交侧必须在同一次提交里 signal 它。
 *          wait() 阻塞到 GPU 计数达到指定值，wait_for_cleanup() 是析构与异常路径的容错版本。
 */
class vulkan_semaphore
{
public:
    /** @brief 默认构造：尚未创建底层 VkSemaphore。 */
    vulkan_semaphore()                                     = default;
    /** @brief 默认析构：由 vk::raii::Semaphore 负责销毁句柄。 */
    ~vulkan_semaphore()                                    = default;
    /** @brief 禁止拷贝构造：信号量句柄不可复制。 */
    vulkan_semaphore(const vulkan_semaphore&)              = delete;
    /** @brief 禁止拷贝赋值：信号量句柄不可复制。 */
    vulkan_semaphore& operator=(const vulkan_semaphore&)   = delete;
    /**
     * @brief 禁止移动构造：token 保存本包装的裸指针，须保持其地址稳定。
     * @param _other 被移动的源对象；该重载被删除，仅用于阻止移动。
     */
    vulkan_semaphore(vulkan_semaphore&& _other)            = delete;
    /**
     * @brief 禁止移动赋值：避免已有 token 指向另一条时间线。
     * @param _other 被移动的源对象；该重载被删除，仅用于阻止移动。
     */
    vulkan_semaphore& operator=(vulkan_semaphore&& _other) = delete;

    /**
     * @brief 创建时间线信号量，初始计数为 0。
     * @param _device 逻辑设备，必须已启用 timelineSemaphore feature。
     * @throws vk::SystemError 当信号量创建失败。
     * @note 重复调用会替换旧句柄；通常只在队列初始化时调用一次。
     * @note 底层计数重新从 0 开始，但 CPU 的 value 分配计数不会被 create 复位。
     * @note 已有 submission_token 引用本对象时不要重新创建，否则旧 token 的完成判定会落到新时间线上。
     */
    void create(const vk::raii::Device& _device);

    // Allocates the next timeline value. The caller must submit a signal for
    // this value before exposing it as a completion credential.
    /**
     * @brief 原子地分配下一个时间线值。
     * @return 新的时间线值，从 1 开始递增。
     * @note 调用方必须在同一提交锁内把它作为 signal value 提交；本函数只分配、不提交。
     */
    [[nodiscard]] uint64_t reserve() noexcept;

    /**
     * @brief 阻塞等待时间线计数达到 _value。
     * @param _value 要等待的时间线值。
     * @throws vk::SystemError 当等待失败，例如设备丢失。
     * @note 无限超时，只有非 eTimeout 的错误才会返回或抛出。
     */
    void wait(uint64_t _value) const;
    // Destruction/unwinding only: a lost device is safe to tear down. Other
    // errors must not silently turn an in-flight resource into a free resource.
    /**
     * @brief 析构与异常路径专用的等待：设备丢失不抛异常。
     * @param _value 要等待的时间线值。
     * @note wait 报 device lost 时打印后继续；其他标准异常退回 vkDeviceWaitIdle，回退成功或 device lost 均继续，其余错误 terminate。
     * @note 声明为 noexcept，绝不能把异常带出析构链。
     */
    void wait_for_cleanup(uint64_t _value) const noexcept;

    /**
     * @brief 查询当前已完成的时间线计数。
     * @return GPU 已 signal 到的计数。
     * @throws vk::SystemError 当查询失败。
     */
    [[nodiscard]] uint64_t                   get_gpu_value() const;
    /**
     * @brief 访问底层信号量句柄。
     * @return 对 vk::raii::Semaphore 的常量引用，所有权仍归本对象。
     */
    [[nodiscard]] const vk::raii::Semaphore& get_semaphore() const noexcept;

private:
    /** @brief 底层时间线信号量句柄。 */
    vk::raii::Semaphore   semaphore = nullptr;
    /** @brief CPU 侧已分配到的最大时间线值；原子递增，初值 0。 */
    std::atomic<uint64_t> value     = 0;
};
