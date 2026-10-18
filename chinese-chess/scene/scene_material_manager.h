/**
 * @file scene_material_manager.h
 * @brief 材质实体的句柄表、贴图引用边与紧凑 binding 上传。
 * @details 每个槽是 material_entry{data, binding}：binding 是 update_ssbo 时按 live() 顺序重新编号的
 *          紧凑下标，模型的 material_index 用它索引 GPU 材质数组。set_texture 是唯一修改 textures 的入口，
 *          负责对旧句柄 release、对新句柄 retain；destroy 对全部通道各 release 一次。
 *          live_texture_references() 把「所有材质 × 所有通道」展开成视图，供 Debug 校验悬空句柄。
 * @note 增删材质可能改变后续 binding，scene_manager 因此在 create_material / destroy(material) 时
 *       同时置模型脏；贴图布局变化则由 texture manager 的 layout_moved 触发材质重传。
 */
#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;
class scene_texture_manager;

/**
 * @brief 管理场景材质：维护贴图引用计数、紧凑 binding 与共享 SSBO。
 * @details create / destroy / set_texture 会置 is_dirty；通过 get 改参数后须显式调用 need_update。Vulkan 工作集中在 update() -> update_ssbo()：它按 live() 顺序重新编号
 *          binding，并把每个材质的 PBR 参数与 albedo 通道的贴图绑定打成 vulkan_material_data 数组上传。
 *          正常上传不在 CPU 上等待 GPU，完成凭据经 _waited_infos 交回调用方；异常清理可能等待。
 * @note 引用边只能经 set_texture 改写：绕开它直接写 textures 会让贴图被提前回收或漏加引用。
 */
class scene_material_manager
{
public:
    /**
     * @brief 保存注入的依赖与贴图管理器引用，置脏等待第一次 update。
     * @param _allocator VMA 分配器，用于创建与重建 SSBO。
     * @param _device 逻辑设备，用于创建缓冲。
     * @param _recycle_bin 延迟销毁旧缓冲与中间资源的回收站。
     * @param _graphic_queue SSBO 的所有者队列，也是着色器读取的消费者队列。
     * @param _transfer_queue 执行上传复制的队列；可与 _graphic_queue 是同一个 VkQueue。
     * @param _texture_manager 贴图引用计数与紧凑 binding 的来源，必须活得比本 manager 久。
     * @note 构造函数不做任何 Vulkan 调用，因此标 noexcept。
     */
    scene_material_manager(const vma::raii::Allocator& _allocator,
                           const vk::raii::Device&     _device,
                           vulkan_recycle_bin&         _recycle_bin,
                           const vulkan_queue&         _graphic_queue,
                           const vulkan_queue&         _transfer_queue,
                           scene_texture_manager&      _texture_manager) noexcept;
    /**
     * @brief 默认析构：槽表与 ssbo 随之销毁，不等待 GPU。
     * @note 默认析构不会 release 贴图；销毁单个材质必须走 destroy()，scene_manager 整体销毁时贴图随后销毁；若单独销毁本 manager 而保留贴图管理器，必须先释放材质引用。
     */
    ~scene_material_manager() = default;

    /**
     * @brief 插入一个默认材质并置脏。
     * @return 新材质句柄；默认 albedo 白、roughness 0.5、ior 1.5、opacity 1，全部贴图通道无效。
     * @note binding 先留 npos，直到 update_ssbo 按存活顺序分配。
     * @throws std::bad_alloc 槽表扩容失败。
     */
    [[nodiscard]] material_handle      create();
    /**
     * @brief 按句柄取可写材质数据。
     * @param _handle 材质句柄。
     * @return 存活槽内 data 的指针；无效、越界或世代不符时返回 nullptr。
     * @note 指针有效期到下一次 insert / erase；修改后需调 need_update() 才会重传 SSBO。
     */
    [[nodiscard]] scene_material_data* get(material_handle _handle) noexcept;
    /**
     * @brief 释放全部通道的贴图引用，然后擦除材质并置脏。
     * @param _handle 材质句柄。
     * @return 是否真的擦除了一个存活槽；句柄已死时返回 false。
     * @note 标 noexcept；每个通道各 release 一次，无效句柄对 release 是 no-op；Debug 下计数错误会抛出并因 noexcept 终止进程。
     * @note 擦除后，槽序位于其后的存活材质 binding 会在下次更新时前移，调用方应同时置模型脏。
     */
    [[nodiscard]] bool                 destroy(material_handle _handle) noexcept;

    /**
     * @brief 脏时才重新编号 binding 并重传材质 SSBO。
     * @param _waited_infos 追加本次上传的完成 token，调用方须把它们纳入后续提交的依赖。
     * @return 本次是否处理了脏状态；空表也返回 true，但不上传缓冲。
     * @throws vk::SystemError 底层 Vulkan 创建或提交失败。
     * @throws std::exception 内存分配或上传状态检查失败时传播对应异常。
     * @note 处理成功后清 is_dirty；异常时保持脏状态，但上传守卫可能已停止队列提交，不能据此保证可重试。
     */
    [[nodiscard]] bool update(std::vector<vulkan_submission_token>& _waited_infos);

    /**
     * @brief 置脏，让下一次 update 重新编号 binding 并重传 SSBO。
     */
    void need_update() noexcept;

    /**
     * @brief 把某个贴图通道换成新句柄，并维护新旧句柄的引用计数。
     * @param _handle 材质句柄；无效时直接返回。
     * @param _channel 目标通道，合法值必须小于 MAX_CHANNEL_COUNT。
     * @param _texture 新贴图句柄；无效句柄表示清空该通道。
     * @throws std::runtime_error 仅 Debug：_channel 越界，或释放旧贴图时发现引用计数已为 0。
     * @note 先 release 槽内旧句柄、再写入新句柄并 retain；只有下标合法才置 is_dirty。
     * @note 引用边必须经本函数写入：直接改 textures 会破坏引用计数；Debug 下 scene_manager::update 只能检测非空悬空句柄，不能验证计数是否配对。
     */
    void set_texture(material_handle _handle, texture_channel _channel, texture_handle _texture);

    /**
     * @brief 取存活材质数量。
     * @return 槽表中的 live_count，不含空闲槽。
     */
    [[nodiscard]] size_t               get_materials_size() const noexcept;
    /**
     * @brief 取材质 SSBO。
     * @return 常量引用；材质从未非空过时内部缓冲尚未创建。
     */
    [[nodiscard]] const vulkan_buffer& get_ssbo_buffer() const noexcept;
    /**
     * @brief 取材质的紧凑 binding。
     * @param _handle 材质句柄。
     * @return 该材质在 GPU 数组中的下标；句柄无效或已死时返回 scene_handle<material_tag>::npos。
     * @note binding 只在 update_ssbo 时刷新，刚 create 的材质在 update 之前仍是 npos。
     */
    [[nodiscard]] uint32_t             get_material_binding(material_handle _handle) const noexcept;

    /**
     * @brief 展开所有存活材质的所有贴图通道句柄。
     * @return 惰性视图，元素是 const texture_handle&；空通道也会出现在结果里。
     * @note 与 live() 同一有效期：下一次 insert / erase 之后不要再用。
     * @note 当前用于 Debug 校验贴图句柄存活性（scene_manager::update），不参与渲染路径。
     */
    [[nodiscard]] auto live_texture_references() const noexcept
    {
        return materials.live()
               | std::views::transform([](const auto& _entry) -> const auto& { return _entry.value.data.textures; })
               | std::views::join;
    }

private:
    /**
     * @brief 材质槽的内容：CPU 侧参数与更新时重新分配的紧凑 binding。
     * @details binding 的初值 npos 表示尚未参与过 SSBO 打包；update_ssbo 会按遍历顺序覆盖它。
     */
    struct material_entry
    {
        /** @brief 材质的 CPU 侧参数，字段语义见 scene_material_data。 */
        scene_material_data data;
        /** @brief GPU 材质数组下标；update_ssbo 按 live() 顺序重新赋值。 */
        uint32_t            binding = scene_handle<material_tag>::npos;
    };

    /**
     * @brief 按 live() 顺序重新编号 binding，并把材质参数与 albedo 贴图绑定上传到 ssbo。
     * @param _waited_infos 追加本次上传的完成 token。
     * @throws std::exception 缓冲分配、创建、上传或提交失败时传播对应异常。
     * @note 空表直接返回，不重建也不清空 SSBO；其余四个通道目前不进入 GPU ABI。
     */
    void update_ssbo(std::vector<vulkan_submission_token>& _waited_infos);

    /** @brief VMA 分配器引用，创建与重建 SSBO 时使用。 */
    const vma::raii::Allocator& allocator;
    /** @brief 逻辑设备引用，创建缓冲与一次性命令缓冲时使用。 */
    const vk::raii::Device&     device;
    /** @brief 回收站引用，旧 SSBO 与中间资源按 token 延迟销毁。 */
    vulkan_recycle_bin&         recycle_bin;
    /** @brief SSBO 的所有者队列，也是着色器读取的消费者队列。 */
    const vulkan_queue&         graphic_queue;
    /** @brief 上传复制使用的队列，可与 graphic_queue 是同一个 VkQueue。 */
    const vulkan_queue&         transfer_queue;
    /** @brief 贴图管理器引用：set_texture / destroy 在此 retain/release，SSBO 在此查紧凑 binding。 */
    scene_texture_manager&      texture_manager;

    /** @brief 脏标志：材质增删或贴图通道改动后置位，update 成功后清除；初值 true 保证首次处理（空表不上传）。 */
    bool is_dirty = true;

    /** @brief 材质槽表，遍历顺序决定 GPU 数组下标（binding）。 */
    scene_handle_map<material_entry, material_tag> materials;
    /** @brief 存放 vulkan_material_data 数组的共享 SSBO，同时提供设备地址给渲染器。 */
    vulkan_buffer                                  ssbo;
};
