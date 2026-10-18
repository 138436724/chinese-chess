/**
 * @file vulkan_common.h
 * @brief Vulkan 公共常量与传输辅助接口，供 vulkan_core 与 scene 层共用。
 * @details 只包含无状态自由函数与进程级常量，不持有任何 Vulkan 对象，资源生命周期由调用方管理。
 *          upload_buffer / upload_image / download_image 负责 staging 缓冲、一次性命令缓冲与跨队列族的 release/acquire，
 *          并把中间资源交给 vulkan_recycle_bin 按完成 token 延迟销毁。
 *          返回的 submission_token 是资源可被消费的唯一凭据，调用方必须把它纳入后续提交的依赖。
 */
#pragma once

#include "vulkan_submission_token.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ranges>
#include <span>
#include <string>
#include <vector>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

class vulkan_recycle_bin;
class vulkan_queue;
class vulkan_buffer;
class vulkan_image;

namespace vulkan_common {

/** @brief 全局 MSAA 采样数，默认 e1；application 初始化时按设备支持改写。 */
inline constinit vk::SampleCountFlagBits MSAA_SAMPLE_COUNT     = vk::SampleCountFlagBits::e1;
/** @brief 全局深度缓冲格式，默认 eUndefined；application 查到受支持格式后改写。 */
inline constinit vk::Format              DEPTH_FORMAT          = vk::Format::eUndefined;
/** @brief CPU 侧帧槽数量：每槽一套 UBO/描述符/同步对象；swapchain 申请该值加一张 image。 */
inline constexpr uint32_t                MAX_FRAMES_IN_FLIGHT  = 3;
/** @brief 是否启用 OCIO：为 true 时交换链取 UNORM，合成阶段经 LUT 做 ACES 2.0 输出变换。 */
inline constexpr bool                    USE_OCIO              = true;
/** @brief bindless 采样器数组容量，与 texture manager 越界检查、描述符池容量共用。 */
inline constexpr uint32_t                MAX_BINDLESS_TEXTURES = 1024;

/**
 * @brief 按候选顺序挑出第一个在指定 tiling 下支持全部所需特性的格式。
 * @param _physical_device 用于查询 getFormatProperties2 的物理设备。
 * @param _candidates 候选格式，顺序即优先级。
 * @param _tiling 线性或最优 tiling，决定检查哪一组 feature 位。
 * @param _features 必须全部置位的格式特性。
 * @return 命中的格式；全部不满足时返回 std::unexpected 并附带候选列表。
 * @note 未找到格式不抛异常，而是返回 std::unexpected；Vulkan 调用自身的失败仍可能以 vk::SystemError 传播。
 */
[[nodiscard]] inline std::expected<vk::Format, std::string> find_supported_format(const vk::raii::PhysicalDevice& _physical_device,
                                                                                  const std::vector<vk::Format>& _candidates,
                                                                                  vk::ImageTiling        _tiling,
                                                                                  vk::FormatFeatureFlags _features)
{
    const auto format_iter = std::ranges::find_if(_candidates, [&](const auto& format) {
        vk::FormatProperties props = _physical_device.getFormatProperties2(format).formatProperties;
        return (_tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & _features) == _features)
               || (_tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & _features) == _features);
    });

    if (format_iter == _candidates.end()) [[unlikely]]
    {
        const std::string result = _candidates
                                   | std::views::transform([](const auto& _format) { return vk::to_string(_format); })
                                   | std::views::join_with(',') | std::ranges::to<std::string>();
        return std::unexpected(std::format("No supported format among candidates: {}", result));
    }
    return *format_iter;
}

/**
 * @brief 把 value 向上对齐到 alignment 的整数倍。
 * @param value 待对齐的字节数或偏移。
 * @param alignment 对齐粒度，必须是 2 的幂（实现用位掩码取整）。
 * @return 不小于 value 的最小对齐值。
 */
[[nodiscard]] inline constexpr auto align_up(const auto value, const size_t alignment) noexcept
{
    return ((value + alignment - 1) & ~(alignment - 1));
}

/**
 * @brief 归一化向量，长度平方非有限或过小时返回兜底向量。
 * @param _value 待归一化的向量。
 * @param _default 退化时返回的备用方向。
 * @return _value 的单位向量，或 _default。
 * @note 防止把 NaN 或零向量写进 UBO；1e-12 是长度平方的判定阈值。
 */
[[nodiscard]] inline glm::vec3 safe_normalize(const glm::vec3& _value, const glm::vec3& _default) noexcept
{
    const float length_squared = glm::dot(_value, _value);
    return std::isfinite(length_squared) && length_squared > 1e-12f ? _value / std::sqrt(length_squared) : _default;
}

// VkTransformMatrixKHR is row-major 3x4, glm::mat4 is column-major; transpose before std::memcpy.
/**
 * @brief 把 glm 列主序 4x4 矩阵转成 Vulkan 行主序 3x4 变换矩阵。
 * @param m 待转换的 glm 矩阵。
 * @return 可直接写入加速结构实例的 TransformMatrixKHR。
 * @note 实现先转置再 memcpy，因为两种 API 主序相反；只复制左上 3x4 部分。
 */
[[nodiscard]] inline auto glm_matrix_to_vulkan(const glm::mat4& m) noexcept
{
    vk::TransformMatrixKHR t;
    std::memcpy(&t, glm::value_ptr(glm::transpose(m)), sizeof(t));
    return t;
}

/**
 * @brief 把 CPU 数据上传到设备缓冲，必要时重建缓冲并处理跨队列族所有权。
 * @param _allocator VMA 分配器。
 * @param _device 逻辑设备，用于创建缓冲与一次性命令缓冲。
 * @param _recycle_bin 延迟销毁中间资源与命令缓冲的回收站。
 * @param _owner_queue 缓冲的消费者队列，上传完成后由它拥有缓冲。
 * @param _transfer_queue 执行复制的队列；内联更新时改用 _owner_queue。可与 _owner_queue 是同一个 VkQueue。
 * @param _buffer 目标缓冲；容量不足时先按新尺寸重建并退休旧缓冲。
 * @param _usage 缓冲用途，内部会并上 eTransferDst。
 * @param _consumer_stages 消费者访问缓冲的阶段（barrier 的 dst stage）。
 * @param _consumer_access 消费者使用的访问类型（barrier 的 dst access）。
 * @param _data 源数据，按字节解释；为空时直接返回无效 token。
 * @param _buffer_name 调试名，用于缓冲与回收站日志。
 * @return 上传完成点的 token；无效 token 表示没有执行任何工作。
 * @throws std::runtime_error 当旧缓冲的最后一个消费者不是 _owner_queue，或缓冲创建/命令提交失败。
 * @note 数据不超过 64 KiB 且 4 字节对齐时走 vkCmdUpdateBuffer 内联路径，不建 staging、不碰 transfer 队列。
 * @note 跨队列族时旧缓冲先由 owner release、transfer acquire，传输完成后再由 owner acquire 到消费者状态；新建缓冲在承担上传的队列族创建（内联路径取 owner，否则取 transfer）。
 */
[[nodiscard]] vulkan_submission_token upload_buffer(const vma::raii::Allocator&    _allocator,
                                             const vk::raii::Device&        _device,
                                             vulkan_recycle_bin&            _recycle_bin,
                                             const vulkan_queue&            _owner_queue,
                                             const vulkan_queue&            _transfer_queue,
                                             vulkan_buffer&                 _buffer,
                                             vk::BufferUsageFlags           _usage,
                                             vk::PipelineStageFlags2        _consumer_stages,
                                             vk::AccessFlags2               _consumer_access,
                                             const std::span<const uint8_t> _data,
                                             const std::string&             _buffer_name = "");

/**
 * @brief 按指定维度创建或替换图像，并把 CPU 数据上传为着色器可读布局。
 * @param _allocator VMA 分配器。
 * @param _device 逻辑设备，用于创建图像、staging 与命令缓冲。
 * @param _recycle_bin 延迟销毁 staging、命令缓冲与旧图像的回收站。
 * @param _owner_queue 上传完成后拥有图像的队列。
 * @param _transfer_queue 执行复制的队列，可与 _owner_queue 是同一个 VkQueue。
 * @param _image_type 图像维度类型。
 * @param _image_view_type 图像视图类型，需与 _image_type 匹配。
 * @param _image_format 图像格式，同时用于 view 与缓冲到图像的复制。
 * @param _image_extent 图像的宽高与深度；调用点通常传深度 1。
 * @param _image 输出图像；成功后整体替换，旧图像进入回收站。
 * @param _consumer_stages 消费者首次采样图像的阶段（barrier 的 dst stage）。
 * @param _consumer_access 消费者首次采样图像的访问类型（barrier 的 dst access）。
 * @param _data 源像素数据，按图像格式紧密排列；为空时抛异常。
 * @param _image_name 调试名，用于图像、staging 与回收站日志。
 * @return 图像进入 eShaderReadOnlyOptimal 并可被采样的完成点 token。
 * @throws std::invalid_argument 当 _data 为空。
 * @throws std::runtime_error 当图像/缓冲创建、命令缓冲分配或队列提交失败。
 * @note 固定 1 级 mip、1 层数组、eOptimal tiling；清除值 (0,0,0,1) 只记录在图像上供后续 render pass 使用，本函数不执行 clearImage。
 * @note 旧图像通过 retire_after_submitted 延迟销毁，新图像只在 token 完成后才能采样。
 */
[[nodiscard]] vulkan_submission_token upload_image(const vma::raii::Allocator&     _allocator,
                                            const vk::raii::Device&         _device,
                                            vulkan_recycle_bin&             _recycle_bin,
                                            const vulkan_queue&             _owner_queue,
                                            const vulkan_queue&             _transfer_queue,
                                            vk::ImageType                   _image_type,
                                            vk::ImageViewType               _image_view_type,
                                            vk::Format                      _image_format,
                                            const vk::Extent3D&             _image_extent,
                                            vulkan_image&                   _image,
                                            vk::PipelineStageFlags2         _consumer_stages,
                                            vk::AccessFlags2                _consumer_access,
                                            const std::span<const uint8_t>  _data,
                                            const std::string&              _image_name = "");

/**
 * @brief 把图像内容回读到 host 可见缓冲，供 CPU 侧读取像素。
 * @param _allocator VMA 分配器。
 * @param _device 逻辑设备，用于创建回读缓冲与一次性命令缓冲。
 * @param _recycle_bin 延迟销毁命令缓冲与旧回读缓冲的回收站。
 * @param _owner_queue 图像所属队列，也是恢复原状态后重新拥有图像的队列。
 * @param _transfer_queue 执行复制的队列，可与 _owner_queue 是同一个 VkQueue。
 * @param _waited_info 可选的前置 token（如产生该图像内容的提交）；无效 token 表示无需额外等待。
 * @param _image 被回读的图像，函数结束时恢复到调用前的 stage/access/layout。
 * @param _buffer 输出回读缓冲，整体替换，旧缓冲进入回收站。
 * @param _buffer_name 调试名，用于回读缓冲与回收站日志。
 * @return 图像恢复原状态且回读完成的 token。
 * @throws std::runtime_error 当回读缓冲创建、命令缓冲分配或队列提交失败。
 * @note 三段式：owner release 到 TransferSrcOptimal → transfer copy 并释放回原布局 → owner acquire 恢复原阶段/访问。
 * @note token 完成后 CPU 才能安全读取 _buffer；非一致内存还需先 invalidate。
 */
[[nodiscard]] vulkan_submission_token download_image(const vma::raii::Allocator&  _allocator,
                                              const vk::raii::Device&      _device,
                                              vulkan_recycle_bin&          _recycle_bin,
                                              const vulkan_queue&          _owner_queue,
                                              const vulkan_queue&          _transfer_queue,
                                              vulkan_submission_token             _waited_info,
                                              vulkan_image&                _image,
                                              vulkan_buffer&               _buffer,
                                              const std::string&           _buffer_name = "");
}  // namespace vulkan_common
