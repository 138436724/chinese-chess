/**
 * @file vulkan_image.h
 * @brief VMA 图像 + ImageView 的 RAII 包装，附带 layout/stage/access/queue 状态跟踪。
 * @details create 要求 image 与 imageview 的格式、层数一致，且只接受单 queue family 的
 *          Exclusive 图像；成功后缓存格式、extent、clear value 与归属 queue family。
 *          transition_state 用追踪的旧状态作 src、入参作 dst 生成 barrier，并在返回前把
 *          layout/stage/access 更新为目标值。类不等待 GPU，也不替调用方做首次布局转换。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 图像与视图的成对 RAII 持有者，外加最近一次布局/阶段/访问的追踪。
 * @details imageview 用同一个 allocator 的 callbacks 创建，析构按声明逆序先释放 view 再
 *          释放 image。追踪的 layout 决定下一条 barrier 的 oldLayout，任何绕过
 *          transition_state 的布局变化都会让后续 barrier 的 src 与实际不符。
 *          clear_value 只是随图像存下来的元数据，本类不做 vkCmdClearColorImage。
 */
class vulkan_image
{
public:
    /** @brief 构造空对象：格式 eUndefined、extent 为 0、布局 eUndefined。 */
    vulkan_image()                               = default;
    /** @brief 默认析构：imageview 先于 image 释放，不等待 GPU。 */
    ~vulkan_image()                              = default;
    /** @brief 禁止拷贝：图像与视图句柄必须有唯一持有者。 */
    vulkan_image(const vulkan_image&)            = delete;
    /** @brief 禁止拷贝赋值：所有权只能经移动转移。 */
    vulkan_image& operator=(const vulkan_image&) = delete;
    /**
     * @brief 移动构造：逐成员 std::exchange 接管，源对象回到未创建状态。
     * @param _other 被移动的源图像，构造后其句柄与追踪状态都为空。
     */
    vulkan_image(vulkan_image&& _other) noexcept;
    /**
     * @brief 移动赋值：自赋值直接返回，否则逐成员 std::ranges::swap。
     * @param _other 被交换的源图像，交换后两边各自持有对方原来的图像与追踪状态。
     * @return *this。
     */
    vulkan_image& operator=(vulkan_image&& _other) noexcept;

    /**
     * @brief 按 _image_info 分配图像与视图，并缓存格式、extent、clear value 与 queue family。
     * @param _allocator VMA 分配器，图像与视图都通过它（含 callbacks）创建。
     * @param _device 创建 imageview，Debug 下还用来设置对象名。
     * @param _image_info 图像创建信息；只接受 queueFamilyIndexCount == 1 且 eExclusive。
     * @param _imageview_info 视图创建信息；format 与 layerCount 必须与 _image_info 一致。
     * @param _usage VMA 内存用途。
     * @param _flags 分配标志，原样透传给 VMA，不做 host 可见性处理。
     * @param _clear_value 随图像保存的清除值，不参与创建。
     * @param _name Debug 名字后缀，image 与 imageview 各用一次。
     * @throws std::runtime_error 格式或层数不一致，或共享模式不是 Exclusive。
     * @throws vk::SystemError 图像或视图创建失败时由 Vulkan-Hpp 抛出。
     * @note _imageview_info 按值传递，函数内把它的 image 成员改为新句柄，调用方无需预先填。
     * @note 初始布局取自 _image_info.initialLayout，本函数不做任何布局转换。
     */
    void create(const vma::raii::Allocator& _allocator,
                const vk::raii::Device&     _device,
                const vk::ImageCreateInfo&  _image_info,
                vk::ImageViewCreateInfo     _imageview_info,
                vma::MemoryUsage            _usage,
                vma::AllocationCreateFlags  _flags,
                const vk::ClearValue&       _clear_value,
                const std::string&          _name = "");

    /**
     * @brief 生成 image barrier，并把追踪的 layout/stage/access/queue 更新为目标值。
     * @param _stage 目标管线阶段，同时作为 barrier 的 dstStageMask。
     * @param _access 目标访问掩码，同时作为 barrier 的 dstAccessMask。
     * @param _layout 目标布局，同时作为 barrier 的 newLayout。
     * @param _queue 目标 queue family；缺省 QueueFamilyIgnored 表示不转移所有权。
     * @param _range 作用的子资源范围，缺省只覆盖 1 个 mip、1 个 array 层与 eColor 方面。
     * @return 源取追踪状态、目标取入参的 barrier，必须交给 commandbuffer 的 pipelineBarrier2 录制。
     * @note 默认范围对深度图与多 mip / 多层图不够，必须显式传完整的 _range。
     * @note 调用即完成状态更新；返回值没被录制时，追踪布局会领先于 GPU 实际布局。
     */
    [[nodiscard("Barrier need record by commandbuffer!")]]
    vk::ImageMemoryBarrier2 transition_state(
        vk::PipelineStageFlags2   _stage,
        vk::AccessFlags2          _access,
        vk::ImageLayout           _layout,
        uint32_t                  _queue = vk::QueueFamilyIgnored,
        vk::ImageSubresourceRange _range = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    /**
     * @brief 取图像格式。
     * @return 创建时缓存的 vk::Format；未创建时是 eUndefined。
     */
    [[nodiscard]] vk::Format                 get_format() const noexcept;
    /**
     * @brief 取图像宽高。
     * @return 创建时缓存的 extent；未创建时是 0 x 0。
     */
    [[nodiscard]] vk::Extent2D               get_extent() const noexcept;
    /**
     * @brief 取最近一次过渡记录的目标阶段。
     * @return 追踪中的 vk::PipelineStageFlags2，未过渡时是 eNone。
     */
    [[nodiscard]] vk::PipelineStageFlags2    get_stage() const noexcept;
    /**
     * @brief 取最近一次过渡记录的目标访问掩码。
     * @return 追踪中的 vk::AccessFlags2，未过渡时是 eNone。
     */
    [[nodiscard]] vk::AccessFlags2           get_access() const noexcept;
    /**
     * @brief 取当前追踪到的图像布局。
     * @return 下一条 barrier 的 oldLayout；未创建时是 eUndefined。
     */
    [[nodiscard]] vk::ImageLayout            get_layout() const noexcept;
    /**
     * @brief 取当前归属的 queue family。
     * @return queue family 下标；create 之前是 QueueFamilyIgnored。
     */
    [[nodiscard]] uint32_t                   get_queue() const noexcept;
    /**
     * @brief 取创建时保存的清除值。
     * @return 传给 create 的 vk::ClearValue，只作元数据，本类不会用它清图像。
     */
    [[nodiscard]] vk::ClearValue             get_clear_value() const noexcept;
    /**
     * @brief 取 VMA 图像句柄。
     * @return 常量引用；未 create 时内部持有 nullptr。
     */
    [[nodiscard]] const vk::raii::Image&     get_image() const noexcept;
    /**
     * @brief 取图像视图句柄。
     * @return 与图像成对创建的 vk::raii::ImageView 常量引用；未 create 时持有 nullptr。
     */
    [[nodiscard]] const vk::raii::ImageView& get_imageview() const noexcept;

    /**
     * @brief 录一条 vkCmdCopyImageToBuffer2，源布局由参数显式给出。
     * @param _commandbuffer 录制目标。
     * @param _image 源图像，必须已处于 _layout 指定的布局。
     * @param _buffer 目标缓冲，必须已处于 eTransferDst 可写状态。
     * @param _layout 源图像的当前布局，截图回读时通常是 eTransferSrcOptimal。
     * @param _copy_info 拷贝区域与 mip/层。
     * @note 不生成也不插入 barrier，同步与布局转换由调用方负责。
     */
    static void copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                     const vk::Image&               _image,
                                     const vk::Buffer&              _buffer,
                                     vk::ImageLayout                _layout,
                                     const vk::BufferImageCopy2&    _copy_info) noexcept;
    /**
     * @brief 录一条 vkCmdCopyImage2，两端布局都由参数显式给出。
     * @param _commandbuffer 录制目标。
     * @param _image_src 源图像，必须已处于 _src_layout。
     * @param _image_dst 目标图像，必须已处于 _dst_layout。
     * @param _src_layout 源图像的当前布局。
     * @param _dst_layout 目标图像的当前布局。
     * @param _copy_info 拷贝区域、mip 与层。
     * @note 不生成也不插入 barrier；调用前两端都必须已转到入参给出的布局。
     */
    static void copy_image_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                    const vk::Image&               _image_src,
                                    const vk::Image&               _image_dst,
                                    vk::ImageLayout                _src_layout,
                                    vk::ImageLayout                _dst_layout,
                                    const vk::ImageCopy2&          _copy_info) noexcept;

private:
    /** @brief 创建时缓存的图像格式，get_format() 直接返回它。 */
    vk::Format     format      = vk::Format::eUndefined;
    /** @brief 创建时缓存的宽高，get_extent() 直接返回它。 */
    vk::Extent2D   extent      = vk::Extent2D{0, 0};
    /** @brief 创建时保存的清除值，只作为元数据保留。 */
    vk::ClearValue clear_value = vk::ClearColorValue(0.f, 0.f, 0.f, 0.f);

    /** @brief 下一条 barrier 的 srcStageMask，最近一次过渡的目标阶段。 */
    vk::PipelineStageFlags2 stage  = vk::PipelineStageFlagBits2::eNone;
    /** @brief 下一条 barrier 的 srcAccessMask，最近一次过渡的目标访问。 */
    vk::AccessFlags2        access = vk::AccessFlagBits2::eNone;
    /** @brief 下一条 barrier 的 oldLayout，最近一次过渡的目标布局。 */
    vk::ImageLayout         layout = vk::ImageLayout::eUndefined;
    /** @brief 当前归属的 queue family，跨 family 转移时作为 barrier 的 srcQueueFamilyIndex。 */
    uint32_t                queue  = vk::QueueFamilyIgnored;

    /** @brief VMA 图像句柄，析构即归还内存；未 create 时为 nullptr。 */
    vma::raii::Image    image     = nullptr;
    /** @brief 与图像成对创建的视图句柄，按声明序先于 image 销毁；未 create 时为 nullptr。 */
    vk::raii::ImageView imageview = nullptr;
};
