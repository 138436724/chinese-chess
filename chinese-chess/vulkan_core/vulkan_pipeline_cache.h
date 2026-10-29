/**
 * @file vulkan_pipeline_cache.h
 * @brief 磁盘持久化的 VkPipelineCache 包装：启动时校验并加载，退出前写回一次。
 * @details 缓存文件固定为 resources/cache/pipeline_cache.cache（相对运行目录 chinese-chess/）；
 *          加载前用 vk::PipelineCacheHeaderVersionOne 校验 vendorID / deviceID / pipelineCacheUUID，
 *          空数据或头部校验失败时创建空缓存，带初始数据创建失败时也尝试回退；读取分配、属性查询或
 *          创建空缓存本身的异常仍可能传播，不能视为完整的容错边界。
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
    vulkan_pipeline_cache& operator=(const vulkan_pipeline_cache&)     = delete;  ///< @brief 禁用拷贝赋值：同上
    /** @brief 禁用移动：单实例且被 vulkan_application 长期持有，地址必须稳定。 */
    vulkan_pipeline_cache(vulkan_pipeline_cache&&) noexcept            = delete;
    vulkan_pipeline_cache& operator=(vulkan_pipeline_cache&&) noexcept = delete;  ///< @brief 禁用移动赋值：同上

    /**
     * @brief 读取磁盘缓存（若有效）或建一份空缓存。
     * @param _physical_device 用于校验缓存头的物理设备（vendor / device / pipelineCacheUUID 三者全等才算命中）。
     * @param _device 创建 VkPipelineCache 的逻辑设备。
     * @throws vk::SystemError 建缓存本身失败时（加载失败已在内部捕获并回退）。
     * @note 读取与校验同步完成；仅带初始数据创建缓存时的 vk::SystemError 被捕获并回退，其他失败可能传播。
     */
    void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device);
    /**
     * @brief 把当前缓存数据写到 resources/cache/pipeline_cache.cache。
     * @note 已捕获的运行时/Vulkan 错误在 Debug 打印、Release 静默；未捕获异常（如 bad_alloc）会因 noexcept
     *       触发 terminate。调用方需同步缓存访问；当前调用点是 ~glfw_window()，位于 wait_idle() 之后。
     */
    void save() const noexcept;

    /**
     * @brief 取出底层 vk::raii::PipelineCache，供 vk::raii::Pipeline 构造函数使用。
     * @return 缓存句柄的引用；create 之前为空句柄（Vulkan 允许传空缓存，只是不复用）。
     */
    [[nodiscard]] const vk::raii::PipelineCache& operator*() const noexcept;

private:
    vk::raii::PipelineCache pipeline_cache = nullptr;  ///< @brief 底层 VkPipelineCache；create 之前为空，加载失败回退时也是新建的空缓存
};
