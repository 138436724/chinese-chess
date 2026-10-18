/**
 * @file vulkan_pipeline_cache.h
 * @brief 磁盘持久化的 VkPipelineCache 包装：启动时校验并加载，退出前写回一次。
 * @details 缓存文件固定为 resources/cache/pipeline_cache.cache（相对运行目录 chinese-chess/）；
 *          加载前用 vk::PipelineCacheHeaderVersionOne 校验 vendorID / deviceID / pipelineCacheUUID，
 *          不匹配、截断或构造抛 vk::SystemError 都回退到空缓存，只损失首次编译时间，不影响正确性。
 *          进程内单实例（vulkan_application 持有），只在 ~glfw_window() 里 wait_idle 之后 save 一次，F5 不写盘。
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 管线缓存的 RAII 持有者：可解引用成 vk::raii::PipelineCache，供创建管线时传入。
 * @details 不可拷贝也不可移动（单实例、地址稳定），生命周期由 vulkan_application 的成员声明顺序保证；
 *          析构只释放 VkPipelineCache，不会自动写盘 —— 写盘必须显式调用 save()。
 */
class vulkan_pipeline_cache
{
public:
    /** @brief 空对象：operator* 返回空句柄，直到 create 成功。 */
    vulkan_pipeline_cache()                                            = default;
    /** @brief 析构：由 vk::raii 释放 VkPipelineCache，不写盘。 */
    ~vulkan_pipeline_cache()                                           = default;
    /** @brief 禁用拷贝：缓存句柄是独占的。 */
    vulkan_pipeline_cache(const vulkan_pipeline_cache&)                = delete;
    /** @brief 禁用拷贝赋值：同上。 */
    vulkan_pipeline_cache& operator=(const vulkan_pipeline_cache&)     = delete;
    /** @brief 禁用移动：单实例且被 vulkan_application 长期持有，地址必须稳定。 */
    vulkan_pipeline_cache(vulkan_pipeline_cache&&) noexcept            = delete;
    /** @brief 禁用移动赋值：同上。 */
    vulkan_pipeline_cache& operator=(vulkan_pipeline_cache&&) noexcept = delete;

    /**
     * @brief 读取磁盘缓存（若有效）或建一份空缓存。
     * @param _physical_device 用于校验缓存头的物理设备（vendor / device / pipelineCacheUUID 三者全等才算命中）。
     * @param _device 创建 VkPipelineCache 的逻辑设备。
     * @throws vk::SystemError 建缓存本身失败时（加载失败已在内部捕获并回退）。
     * @note 读取与校验都在这里同步完成；失败只打印（Debug）或静默（Release）回退，不阻止启动。
     */
    void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device);
    /**
     * @brief 把当前缓存数据写到 resources/cache/pipeline_cache.cache。
     * @note noexcept 且内部吞掉所有失败（Debug 打印，Release 静默），写盘失败不影响退出流程。
     *       必须在设备空闲（wait_idle）之后调用，避免把不完整的数据固化到磁盘；当前唯一调用点是
     *       ~glfw_window()，紧跟 wait_idle_for_cleanup()。
     */
    void save() const noexcept;

    /**
     * @brief 取出底层 vk::raii::PipelineCache，供 vk::raii::Pipeline 构造函数使用。
     * @return 缓存句柄的引用；create 之前为空句柄（Vulkan 允许传空缓存，只是不复用）。
     */
    [[nodiscard]] const vk::raii::PipelineCache& operator*() const noexcept;

private:
    /** @brief 底层 VkPipelineCache；create 之前为空，加载失败回退时也是新建的空缓存。 */
    vk::raii::PipelineCache pipeline_cache = nullptr;
};
