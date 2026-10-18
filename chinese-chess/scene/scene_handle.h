/**
 * @file scene_handle.h
 * @brief 场景实体的 POD 句柄与贴图用途枚举，是 scene 层对外的唯一对象标识。
 * @details scene_handle<Tag> 只保存槽位下标与世代：下标等于 npos 表示无效，世代与槽表当前值一致才算存活，
 *          所以槽被回收后旧句柄不会静默指向新对象。五个 tag 都是不完全类型，句柄可以出现在只前置声明实体的头里，
 *          判活必须回到持有槽表的 manager（get() 对死句柄返回 nullptr）。
 * @note 句柄可平凡拷贝、可比较，不含任何资源或所有权；贴图的引用计数由持有者经 retain/release 维护。
 */
#pragma once

#include <compare>
#include <cstdint>

/**
 * @brief 贴图用途，决定采样器参数与 KTX2 压缩格式，也是贴图去重键的一半。
 * @details 每种用途在 scene_texture_manager 里对应一个长期存活的采样器：color 走默认采样 LDR，
 *          environment 走 clamp-to-edge 各向异性 HDR，glyph 走 clamp-to-edge 且 max_lod 为 0。
 *          同一路径配不同用途会建出两张独立贴图。
 */
enum class texture_usage : uint32_t
{
    color,       ///< 普通 LDR 颜色贴图：默认采样器与 BC7（无 BC7 时回退未压缩 RGBA8）。
    environment, ///< 环境/天空盒 HDR：clamp-to-edge + 各向异性，优先压 BC6H。
    glyph        ///< 字体图集：clamp-to-edge、max_lod 0、mipmap 最近邻，优先压 BC7。
};

/**
 * @brief 场景实体的泛型句柄：槽位下标 + 世代，npos 表示无效。
 * @details 世代在槽被擦除时自增，所以「旧句柄」与「复用该槽的新句柄」不相等；
 *          比较运算符由编译器默认生成，先比 index 再比 generation。
 * @tparam T tag 类型，只用于区分句柄类别（light / texture / material / mesh / model），不需要完整定义。
 */
template <typename T>
struct scene_handle
{
    /** @brief 无效槽位下标，默认构造的 index 就是它。 */
    static constexpr uint32_t npos = 0xFFFFFFFFu;

    /** @brief 槽位下标；等于 npos 时句柄无效。 */
    uint32_t index      = npos;
    /** @brief 槽位世代；必须与槽表当前世代相同，句柄才指向原来的那个对象。 */
    uint32_t generation = 0;

    /**
     * @brief 判断句柄是否非空（只看下标，不校验槽表）。
     * @return index 不等于 npos 时为 true；槽已回收但下标尚未复用时仍返回 true，需配合 manager 的 get() 判活。
     */
    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != npos; }

    /**
     * @brief 默认三路比较：依次比较 index 与 generation。
     * @return 强序结果；调用方据此把句柄放进有序容器或去重。
     */
    auto operator<=>(const scene_handle&) const noexcept = default;
};

/**
 * @brief 灯光句柄的 tag。
 * @details 空结构，只作 scene_handle 的模板参数；对应实体是 scene_light_data。
 */
struct light_tag;
/**
 * @brief 贴图句柄的 tag。
 * @details 空结构，只作 scene_handle 的模板参数；对应句柄指向 scene_texture_manager 的贴图槽，
 *          而 get() 返回的是只读描述 scene_texture_data。
 */
struct texture_tag;
/**
 * @brief 材质句柄的 tag。
 * @details 空结构，只作 scene_handle 的模板参数；对应实体是 scene_material_data。
 */
struct material_tag;
/**
 * @brief 网格句柄的 tag。
 * @details 空结构，只作 scene_handle 的模板参数；网格只存在于 scene_model_manager 的槽里，
 *          CPU 侧没有对应的 POD 结构。
 */
struct mesh_tag;
/**
 * @brief 模型句柄的 tag。
 * @details 空结构，只作 scene_handle 的模板参数；对应实体是 scene_model_data。
 */
struct model_tag;

/**
 * @brief 五类实体的句柄别名，句柄之间不能隐式互转。
 * @details 别名只固定 tag，不改变布局；拷贝、比较与默认构造行为都来自 scene_handle。
 */
using light_handle    = scene_handle<light_tag>;
using texture_handle  = scene_handle<texture_tag>;
using material_handle = scene_handle<material_tag>;
using mesh_handle     = scene_handle<mesh_tag>;
using model_handle    = scene_handle<model_tag>;
