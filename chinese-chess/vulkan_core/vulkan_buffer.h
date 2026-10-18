/**
 * @file vulkan_buffer.h
 * @brief VMA 分配的 buffer 的 RAII 封装。一次 create 同时定下独占 queue family、同步状态与设备/主机地址。
 * @details create 只接受 queueFamilyIndexCount == 1 的 Exclusive 缓冲，并把 queue family 与字节数记录下来
 *          host 可见性只看 AllocationCreateFlags 里的 HOST_ACCESS 位（eAuto* 由 VMA 选内存类型，并不单独保证 HOST_VISIBLE），
 *          命中时补 eMapped 并记录映射指针，否则记录 vkGetBufferAddress 的设备地址。
 *          transition_state 是唯一的 barrier 生成点，返回前把追踪状态改成目标值，调用方必须把返回值交给 pipelineBarrier2 录制。
 */

#pragma once

// need replace "vk_mem_alloc.h" by <vma/vk_mem_alloc.h> if use vcpkg install
#include <cstdint>
#include <string>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief VMA 分配的 buffer 以及 stage/access/queue 状态跟踪。
 * @details 状态跟踪仅跟踪**最近一次过渡**：buffer 没有 layout，queue 只在跨 queue family 转移时更新，否则保持不变。
 *          地址在创建时确定，host 可见的分配需要人为调用 flush()/invalidate()，容器重建后地址随新缓冲重新查询。
 */
class vulkan_buffer
{
public:
    vulkan_buffer()                                = default;   ///< @brief 默认构造函数，实际数据需要使用 create 创建。
    ~vulkan_buffer()                               = default;   ///< @brief 默认析构函数，不等待GPU，立刻销毁数据。
    vulkan_buffer(const vulkan_buffer&)            = delete;    ///< @brief 禁止拷贝构造。
    vulkan_buffer& operator=(const vulkan_buffer&) = delete;    ///< @brief 禁止拷贝赋值。
    vulkan_buffer(vulkan_buffer&& _other) noexcept;             ///< @brief 移动构造函数。
    vulkan_buffer& operator=(vulkan_buffer&& _other) noexcept;  ///< @brief 移动赋值函数。

    /**
     * @brief 按 _buffer_info 分配 VMA 缓冲，记录 queue family、大小与地址。
     * @param _allocator VMA 分配器。
     * @param _device 用于查询设备地址。
     * @param _buffer_info 创建信息。仅接受 queueFamilyIndexCount == 1 且 eExclusive。
     * @param _usage VMA 内存选择策略。eAuto* 不单独保证 host 可见或持久映射。
     * @param _flags 分配标志。含 HOST_ACCESS 位时自动补 eMapped。
     * @param _name Debug 名字后缀，仅 DEBUG 下用于设置调试名称。
     * @throws std::runtime_error 共享模式不是 Exclusive 或 queueFamilyIndexCount 不为 1 时抛出。
     * @throws vk::SystemError VMA 分配失败时由分配器抛出。
     * @note 按 HOST_ACCESS 标志选择记录 pMappedData 或 device address，不查询实际内存类型。
     *       eShaderDeviceAddress 不会进行持久化映射。CPU/GPU 访问同步与 flush()/invalidate() 由调用方负责。
     * @note 调用即刻覆盖旧句柄，立刻析构原来的对象，不做任何等待。
     * @warning create 不重置 stage/access；重新使用已有对象时不能假定追踪状态已回到 eNone。
     */
    void create(const vma::raii::Allocator& _allocator,
                const vk::raii::Device&     _device,
                const vk::BufferCreateInfo& _buffer_info,
                vma::MemoryUsage            _usage,
                vma::AllocationCreateFlags  _flags,
                const std::string&          _name = "");

    /**
     * @brief 将整块映射内存的 CPU 写刷到设备可见范围（offset 0、vk::WholeSize）。
     * @note 只对 host 可见且已映射的分配有意义，其它分配不应调用。
     */
    void flush() const;

    /**
     * @brief 丢弃整块映射内存的 CPU 缓存，使后续读取能看到 GPU 或其它写者的结果。
     * @note 与 flush() 对称，同样只适用于已映射的 host 可见分配。
     * @note 必须先等待 GPU 写入完成，invalidate 本身不会等待 GPU。
     */
    void invalidate() const;

    /**
     * @brief 生成一条 buffer barrier，并把追踪的 stage/access/queue 更新为目标值。
     * @param _stage 目标管线阶段，同时作为 barrier 的 dstStageMask。
     * @param _access 目标访问掩码，同时作为 barrier 的 dstAccessMask。
     * @param _queue 目标 queue family；默认使用 QueueFamilyIgnored 表示不转移所有权。
     * @return src使用当前追踪状态、dst使用传入参数的 barrier，必须交给 commandbuffer 的 pipelineBarrier2 录制。
     * @note 调用即完成状态更新。一旦返回值没被录制，追踪状态会与 GPU 实际状态不一致。
     * @note 不转移所有权 srcQueueFamilyIndex/dstQueueFamilyIndex 均为 QueueFamilyIgnored。
     * @note 跨 queue family 时只生成一条屏障，调用方仍须自行配对 release/acquire 并建立提交间依赖。
     */
    [[nodiscard("Barrier need record by commandbuffer!")]]
    vk::BufferMemoryBarrier2 transition_state(vk::PipelineStageFlags2 _stage, vk::AccessFlags2 _access, uint32_t _queue = vk::QueueFamilyIgnored);

    /**
     * @brief 获取最近一次过渡记录的目标阶段。
     * @return 追踪中的 vk::PipelineStageFlags2，未过渡时是 eNone。
     */
    [[nodiscard]] vk::PipelineStageFlags2 get_stage() const noexcept;

    /**
     * @brief 获取最近一次过渡记录的目标访问掩码。
     * @return 追踪中的 vk::AccessFlags2，未过渡时是 eNone。
     */
    [[nodiscard]] vk::AccessFlags2 get_access() const noexcept;

    /**
     * @brief 获取当前追踪到的 queue family。
     * @return queue family 下标；create 之前是 QueueFamilyIgnored。
     */
    [[nodiscard]] uint32_t get_queue() const noexcept;

    /**
     * @brief 获取 vulkan buffer 句柄。实际从 VMA 对象中获取。
     * @return 常量引用；未 create 时内部持有 nullptr，可用它判空。
     */
    [[nodiscard]] const vk::raii::Buffer& get_buffer() const noexcept;

    /**
     * @brief 获取创建时记录的映射指针或设备地址。
     * @return 创建时含 HOST_ACCESS 标志则记录 pMappedData，否则记录 vkGetBufferAddress 的地址。。
     */
    [[nodiscard]] vk::DeviceOrHostAddressKHR get_buffer_address() const noexcept;

    /**
     * @brief 获取创建时记录的缓冲字节数。
     * @return 字节数，未 create 时为 0。
     */
    [[nodiscard]] vk::DeviceSize get_size() const noexcept;

    /**
     * @brief 录制一条 vkCmdCopyBuffer2 命令，信息全由外部传入 _copy_info 决定。
     * @param _commandbuffer 录制目标。
     * @param _src_buffer 源缓冲，必须处于可读的转移状态。
     * @param _dst_buffer 目标缓冲，必须处于可写的转移状态。
     * @param _copy_info 拷贝信息。
     * @note 仅录制命令，不生成也不插入 barrier，同步由调用方负责。
     */
    static void copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                      const vk::Buffer&              _src_buffer,
                                      const vk::Buffer&              _dst_buffer,
                                      const vk::BufferCopy2&         _copy_info) noexcept;
    /**
     * @brief 录制一条 vkCmdCopyBufferToImage2 命令，调用方必须先把目标图像转到 eTransferDstOptimal，信息全由外部传入 _copy_info 决定。
     * @param _commandbuffer 录制目标。
     * @param _buffer 源缓冲，需带 eTransferSrc usage，并由调用方建立传输读取所需的同步。
     * @param _image 目标图像，必须已转到 eTransferDstOptimal。
     * @param _copy_info 拷贝信息。
     * @note 仅录制命令，不生成也不插入 barrier，前置布局转换及同步由调用方负责。
     */
    static void copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                     const vk::Buffer&              _buffer,
                                     const vk::Image&               _image,
                                     const vk::BufferImageCopy2&    _copy_info) noexcept;

private:
    vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eNone;  ///<@brief 下一条 barrier 的 srcStageMask，即最近一次过渡的目标阶段。
    vk::AccessFlags2 access = vk::AccessFlagBits2::eNone;  ///<@brief 下一条 barrier 的 srcAccessMask，即最近一次过渡的目标访问。
    uint32_t queue = vk::QueueFamilyIgnored;  ///<@brief 当前归属的 queue family，跨 family 转移时作为 barrier 的 srcQueueFamilyIndex。

    vma::raii::Buffer          buffer         = nullptr;  ///<@brief VMA 缓冲句柄，未 create 时为 nullptr。
    vk::DeviceSize             buffer_size    = 0;        ///<@brief 创建时记录的字节数。
    vk::DeviceOrHostAddressKHR buffer_address = nullptr;  ///<@brief 创建记录的映射指针或设备地址。
};
