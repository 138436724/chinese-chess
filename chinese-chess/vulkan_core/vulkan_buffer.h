/**
 * @file vulkan_buffer.h
 * @brief VMA 缓冲区包装：一次 create 同时定下独占 queue family、同步状态与设备/主机地址。
 * @details create 只接受 queueFamilyIndexCount == 1 的 Exclusive 缓冲，并把 queue family 与
 *          字节数缓存下来；host 可见性只看 AllocationCreateFlags 里的 HOST_ACCESS 位
 *          （eAuto* 由 VMA 选内存类型，并不单独保证 HOST_VISIBLE），命中时补 eMapped 并缓存映射指针，
 *          否则缓存 vkGetBufferAddress 的设备地址。transition_state 是唯一的 barrier 生成点，
 *          返回前就把追踪状态改成目标值，调用方必须把返回值交给 pipelineBarrier2 录制。
 */

#pragma once

// need replace "vk_mem_alloc.h" by <vma/vk_mem_alloc.h> if use vcpkg install
#include <cstdint>
#include <string>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief VMA 分配的 VkBuffer 及其 stage/access/queue 状态跟踪。
 * @details 追踪三件套只描述「最近一次过渡」：buffer 没有 layout，queue 只在跨 queue family
 *          转移时更新，否则保持不变。地址在创建时确定，所以 host 可见的分配要自己调
 *          flush()/invalidate()，容器重建后地址随新缓冲重新查询。类不等待 GPU：缓冲的
 *          销毁时机由持有者用 submission_token 推迟到对应提交完成之后。
 */
class vulkan_buffer
{
public:
    /**
     * @brief 构造空对象：大小为 0、地址为空，尚未分配 VMA 内存。
     */
    vulkan_buffer()                                = default;
    /**
     * @brief 默认析构：VMA 立即归还内存，不等待 GPU。
     */
    ~vulkan_buffer()                               = default;
    /**
     * @brief 禁止拷贝：VMA 分配必须有唯一持有者。
     */
    vulkan_buffer(const vulkan_buffer&)            = delete;
    /**
     * @brief 禁止拷贝赋值：所有权只能经移动转移。
     */
    vulkan_buffer& operator=(const vulkan_buffer&) = delete;
    /**
     * @brief 移动构造：std::exchange 接管全部字段，源对象回到未分配状态。
     * @param _other 被移动的源缓冲，构造后其句柄、大小与地址都为空。
     */
    vulkan_buffer(vulkan_buffer&& _other) noexcept;
    /**
     * @brief 移动赋值：自赋值直接返回，否则逐成员 std::ranges::swap。
     * @param _other 被交换的源缓冲，交换后两边各自持有对方原来的缓冲与状态。
     * @return *this。
     */
    vulkan_buffer& operator=(vulkan_buffer&& _other) noexcept;

    /**
     * @brief 按 _buffer_info 分配 VMA 缓冲，并缓存独占 queue family、大小与地址。
     * @param _allocator VMA 分配器，调用方保证它活得比本缓冲久。
     * @param _device 用于查询设备地址，Debug 下还用来设置对象名。
     * @param _buffer_info 创建信息；只接受 queueFamilyIndexCount == 1 且 eExclusive。
     * @param _usage VMA 内存选择策略；eAuto* 不单独保证 host 可见或持久映射。
     * @param _flags 分配标志；含 HOST_ACCESS 位时自动补 eMapped。
     * @param _name Debug 名字后缀，仅 NDEBUG 未定义时用于 setDebugUtilsObjectNameEXT。
     * @throws std::runtime_error 共享模式不是 Exclusive 或 queueFamilyIndexCount 不为 1。
     * @throws vk::SystemError VMA 分配失败时由分配器抛出。
     * @note host 可见分配缓存的是 pMappedData，之后必须自己 flush()/invalidate()；
     *       非 host 可见分配缓存的是 vkGetBufferAddress 的设备地址。
     * @note 重复调用会直接覆盖旧句柄，旧内存随之释放，不做任何等待。
     */
    void create(const vma::raii::Allocator& _allocator,
                const vk::raii::Device&     _device,
                const vk::BufferCreateInfo& _buffer_info,
                vma::MemoryUsage            _usage,
                vma::AllocationCreateFlags  _flags,
                const std::string&          _name = "");

    /**
     * @brief 把整块映射内存的 CPU 写刷到设备可见范围（offset 0、vk::WholeSize）。
     * @note 只对 host 可见且已映射的分配有意义，其它分配调用它属于误用。
     */
    void flush() const;
    /**
     * @brief 丢弃整块映射内存的 CPU 缓存，使后续读取能看到 GPU 或其它写者的结果。
     * @note 与 flush() 对称，同样只适用于已映射的 host 可见分配。
     */
    void invalidate() const;

    /**
     * @brief 生成一条 buffer barrier，并把追踪的 stage/access/queue 更新为目标值。
     * @param _stage 目标管线阶段，同时作为 barrier 的 dstStageMask。
     * @param _access 目标访问掩码，同时作为 barrier 的 dstAccessMask。
     * @param _queue 目标 queue family；缺省 QueueFamilyIgnored 表示不转移所有权。
     * @return 源取追踪状态、目标取入参的 barrier，必须交给 commandbuffer 的 pipelineBarrier2 录制。
     * @note 调用即完成状态更新：返回值没被录制时，追踪状态会与 GPU 实际状态不一致。
     * @note 只有 _queue 有效且与原 queue 不同才自动填 src/dstQueueFamilyIndex。
     */
    [[nodiscard("Barrier need record by commandbuffer!")]]
    vk::BufferMemoryBarrier2 transition_state(vk::PipelineStageFlags2 _stage, vk::AccessFlags2 _access, uint32_t _queue = vk::QueueFamilyIgnored);

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
     * @brief 取当前追踪到的 queue family。
     * @return queue family 下标；create 之前是 QueueFamilyIgnored。
     */
    [[nodiscard]] uint32_t                   get_queue() const noexcept;
    /**
     * @brief 取 VMA 缓冲句柄。
     * @return 常量引用；未 create 时内部持有 nullptr，可用它判空。
     */
    [[nodiscard]] const vk::raii::Buffer&    get_buffer() const noexcept;
    /**
     * @brief 取创建时缓存的映射指针或设备地址。
     * @return host 可见分配返回 pMappedData，其余返回 vkGetBufferAddress 的地址。
     * @note 判空看 get_buffer()，并按创建时的 flags 取联合体的对应成员。
     */
    [[nodiscard]] vk::DeviceOrHostAddressKHR get_buffer_address() const noexcept;
    /**
     * @brief 取创建时记录的缓冲字节数。
     * @return 字节数；未 create 时为 0。
     */
    [[nodiscard]] vk::DeviceSize             get_size() const noexcept;

    /**
     * @brief 录一条 vkCmdCopyBuffer2，区域与大小全由 _copy_info 决定。
     * @param _commandbuffer 录制目标。
     * @param _src_buffer 源缓冲，必须已处于可读的转移状态。
     * @param _dst_buffer 目标缓冲，必须已处于可写的转移状态。
     * @param _copy_info 拷贝区域。
     * @note 不生成也不插入 barrier，同步由调用方负责。
     */
    static void copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                      const vk::Buffer&              _src_buffer,
                                      const vk::Buffer&              _dst_buffer,
                                      const vk::BufferCopy2&         _copy_info) noexcept;
    /**
     * @brief 录一条 vkCmdCopyBufferToImage2；调用方必须先把目标图像转到 eTransferDstOptimal。
     * @param _commandbuffer 录制目标。
     * @param _buffer 源缓冲，需带 eTransferSrc usage，并由调用方建立传输读取所需的同步。
     * @param _image 目标图像，必须已转到 eTransferDstOptimal。
     * @param _copy_info 拷贝区域与目标 mip/层。
     * @note 不生成也不插入 barrier；transition_state 只返回屏障，调用方还必须将它录入命令缓冲。
     */
    static void copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                     const vk::Buffer&              _buffer,
                                     const vk::Image&               _image,
                                     const vk::BufferImageCopy2&    _copy_info) noexcept;

private:
    /** @brief 下一条 barrier 的 srcStageMask，即最近一次过渡的目标阶段。 */
    vk::PipelineStageFlags2 stage  = vk::PipelineStageFlagBits2::eNone;
    /** @brief 下一条 barrier 的 srcAccessMask，即最近一次过渡的目标访问。 */
    vk::AccessFlags2        access = vk::AccessFlagBits2::eNone;
    /** @brief 当前归属的 queue family，跨 family 转移时作为 barrier 的 srcQueueFamilyIndex。 */
    uint32_t                queue  = vk::QueueFamilyIgnored;

    /** @brief VMA 缓冲句柄，析构即归还内存；未 create 时为 nullptr。 */
    vma::raii::Buffer          buffer         = nullptr;
    /** @brief 创建时记录的字节数，get_size() 直接返回它。 */
    vk::DeviceSize             buffer_size    = 0;
    /** @brief 创建时缓存的映射指针或设备地址，get_buffer_address() 直接返回它。 */
    vk::DeviceOrHostAddressKHR buffer_address = nullptr;
};
