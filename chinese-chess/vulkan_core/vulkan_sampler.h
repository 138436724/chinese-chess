/**
 * @file vulkan_sampler.h
 * @brief 采样器参数聚合（sampler_create_info）与 vk::raii::Sampler 的创建包装。
 * @details sampler_create_info 用默认的 operator== / operator<=> 做逐字段比较，便于按参数
 *          去重（场景层每种 texture_usage 只留一个采样器）。vulkan_sampler::create 把聚合体
 *          翻成 vk::SamplerCreateInfo，各向异性开启时取设备上限 maxSamplerAnisotropy，关闭时写 1；
 *          borderColor 恒为不透明黑、compareOp 恒为 eAlways，比较采样不在本类范围内。
 */
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <utility>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 采样器创建参数的纯数据聚合，默认值是三线性过滤 + Repeat 寻址。
 * @details 只暴露 vk::SamplerCreateInfo 里实际用到的字段，其余（compareOp、borderColor、
 *          unnormalizedCoordinates）在 vulkan_sampler::create 里写死。默认的 operator== /
 *          operator<=> 让它能直接做去重比较。
 */
struct sampler_create_info
{
    vk::Filter            mag_filter  = vk::Filter::eLinear;  ///< @brief 放大过滤，默认 eLinear
    vk::Filter            min_filter  = vk::Filter::eLinear;  ///< @brief 缩小过滤，默认 eLinear
    vk::SamplerMipmapMode mipmap_mode = vk::SamplerMipmapMode::eLinear;  ///< @brief mip 之间过滤方式，默认 eLinear；字形图集用 eNearest 配 max_lod = 0

    vk::SamplerAddressMode address_mode_u = vk::SamplerAddressMode::eRepeat;  ///< @brief U 方向寻址模式，默认 eRepeat；eClampToEdge 将越界采样钳制到边缘 texel
    vk::SamplerAddressMode address_mode_v = vk::SamplerAddressMode::eRepeat;  ///< @brief V 方向寻址模式，默认 eRepeat
    vk::SamplerAddressMode address_mode_w = vk::SamplerAddressMode::eRepeat;  ///< @brief W 方向寻址模式，默认 eRepeat（2D 采样用不到）

    bool  enable_anisotropy = false;  ///< @brief 是否开启各向异性过滤；开启时 create 取设备上限 maxSamplerAnisotropy
    float min_lod           = 0.f;  ///< @brief 最小 LOD 钳制，默认 0
    float max_lod           = vk::LodClampNone;  ///< @brief 最大 LOD 钳制，默认 vk::LodClampNone 表示不限制

    bool operator==(const sampler_create_info&) const  = default;  ///< @brief 默认逐字段相等比较，用于去重与容器查找
    auto operator<=>(const sampler_create_info&) const = default;  ///< @brief 默认三路比较，字段声明序即比较顺序
};

/**
 * @brief vk::raii::Sampler 的移动-only 包装，创建参数来自 sampler_create_info。
 * @details 只持有一个句柄，没有按参数缓存或去重的逻辑 —— 去重由调用方（场景层按
 *          texture_usage 建表）负责。create 可重复调用，新句柄赋值时旧采样器立即销毁；
 *          构造不做任何设备查询。
 */
class vulkan_sampler
{
public:
    /** @brief 构造空对象：内部持有 nullptr，尚未创建采样器。 */
    vulkan_sampler()                                 = default;
    /** @brief 默认析构：立即销毁采样器，不等待 GPU。 */
    ~vulkan_sampler()                                = default;
    /** @brief 禁止拷贝：采样器句柄必须有唯一持有者。 */
    vulkan_sampler(const vulkan_sampler&)            = delete;
    vulkan_sampler& operator=(const vulkan_sampler&) = delete;  ///< @brief 禁止拷贝赋值：所有权只能经移动转移
    /**
     * @brief 移动构造：std::exchange 接管句柄，源对象回到 nullptr。
     * @param _other 被移动的源包装，构造后其句柄为 nullptr。
     */
    vulkan_sampler(vulkan_sampler&& _other) noexcept;
    vulkan_sampler& operator=(vulkan_sampler&& _other) noexcept;  ///< @brief 移动赋值：自赋值直接返回，否则 std::ranges::swap 两个句柄

    /**
     * @brief 按 _info 组装 vk::SamplerCreateInfo 并创建采样器句柄。
     * @param _physical_device 只在开启各向异性时用来读 limits.maxSamplerAnisotropy。
     * @param _device 创建采样器，Debug 下还用来设置对象名。
     * @param _info 过滤、寻址与 LOD 参数。
     * @param _name Debug 名字后缀，仅 NDEBUG 未定义时生效。
     * @throws vk::SystemError 创建失败时由 Vulkan-Hpp 抛出。
     * @note borderColor 恒为 eFloatOpaqueBlack，仅在相应方向使用 eClampToBorder 时参与越界采样；
     *       eClampToEdge 采样边缘 texel，不使用 borderColor。
     * @note 重复调用会直接替换旧句柄，旧采样器立即销毁，不做任何等待。
     */
    void create(const vk::raii::PhysicalDevice& _physical_device,
                const vk::raii::Device&         _device,
                const sampler_create_info&      _info,
                const std::string&              _name);

    /**
     * @brief 取底层采样器句柄。
     * @return vk::raii::Sampler 常量引用；未 create 时持有 nullptr。
     */
    [[nodiscard]] const vk::raii::Sampler& operator*() const noexcept;

private:
    vk::raii::Sampler sampler = nullptr;  ///< @brief 采样器句柄；未 create 时为 nullptr
};
