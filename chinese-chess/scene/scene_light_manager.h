/**
 * @file scene_light_manager.h
 * @brief 灯光实体的句柄表与共享 SSBO 上传。
 * @details 实体是纯数据 scene_light_data，按槽位顺序打包成 vulkan_light_data 数组，经 upload_buffer
 *          写进一块带 eStorageBuffer / eShaderDeviceAddress 的共享 SSBO；渲染器不绑定它，而是取设备地址
 *          写进每帧 UBO。is_dirty 门控上传，成功后清除。
 * @note 灯的 GPU 下标就是 live() 的遍历顺序，没有独立的紧凑 binding；因此增删灯只需重传本 SSBO，
 *       不牵连模型或材质。
 */
#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;

/**
 * @brief 管理场景灯光：创建/销毁句柄、维护脏标志并把存活灯打包上传到共享 SSBO。
 * @details 所有修改都只置 is_dirty，真正的 Vulkan 工作集中在 update()：它把 live() 视图映射成
 *          vulkan_light_data（方向先 safe_normalize），再交给 upload_buffer 重建或复用 ssbo。
 *          类不等待 GPU，上传的完成凭据经 _waited_infos 交回调用方。
 * @note 槽表与 ssbo 的销毁不等待 GPU：回收时序由 recycle_bin 的 token 与调用方的 waited_infos 保证。
 */
class scene_light_manager
{
public:
    /**
     * @brief 保存注入的依赖并置脏，等待第一次 update 建 SSBO。
     * @param _allocator VMA 分配器，用于创建与重建 SSBO。
     * @param _device 逻辑设备，用于创建缓冲。
     * @param _recycle_bin 延迟销毁旧缓冲与中间资源的回收站。
     * @param _graphic_queue SSBO 的所有者队列，也是着色器读取的消费者队列。
     * @param _transfer_queue 执行上传复制的队列；可与 _graphic_queue 是同一个 VkQueue。
     * @note 只保存引用，五者的生命周期必须覆盖本 manager；构造函数不做任何 Vulkan 调用，因此标 noexcept。
     */
    scene_light_manager(const vma::raii::Allocator& _allocator,
                        const vk::raii::Device&     _device,
                        vulkan_recycle_bin&         _recycle_bin,
                        const vulkan_queue&         _graphic_queue,
                        const vulkan_queue&         _transfer_queue) noexcept;
    /**
     * @brief 默认析构：槽表与 ssbo 随之销毁，不等待 GPU。
     */
    ~scene_light_manager() = default;

    /**
     * @brief 插入一盏默认参数的灯并置脏。
     * @return 新灯句柄；参数取 scene_light_data 的默认值（方向光、白色、强度 1）。
     * @throws 槽表扩容或实体移动构造失败时传播异常，此时不留下半插入的槽。
     */
    [[nodiscard]] light_handle      create();
    /**
     * @brief 按句柄取可写灯数据。
     * @param _handle 灯句柄。
     * @return 存活槽的指针；无效、越界或世代不符时返回 nullptr。
     * @note 指针有效期到下一次 insert / erase；修改后需调 need_update() 才会重传 SSBO。
     */
    [[nodiscard]] scene_light_data* get(light_handle _handle) noexcept;
    /**
     * @brief 擦除一盏灯并置脏。
     * @param _handle 灯句柄。
     * @return 是否真的擦除了一个存活槽；句柄已死时返回 false 且不置脏。
     * @note 标 noexcept；旧句柄在擦除后立即失效，SSBO 在下一次 update 时按新的存活顺序重传。
     */
    [[nodiscard]] bool              destroy(light_handle _handle) noexcept;

    /**
     * @brief 脏时才把存活灯重新打包上传到共享 SSBO。
     * @param _waited_infos 追加本次上传的完成 token，调用方须把它们纳入后续提交的依赖。
     * @return 本次是否真的执行了上传（is_dirty 为 true 时才可能）。
     * @throws std::runtime_error upload_buffer 在缓冲创建或命令提交失败时抛出。
     * @note 上传成功后立即清 is_dirty；成功前失败则标志保持，下一次 update 会重试。
     */
    [[nodiscard]] bool update(std::vector<vulkan_submission_token>& _waited_infos);

    /**
     * @brief 置脏，让下一次 update 重新上传 SSBO。
     */
    void need_update() noexcept;

    /**
     * @brief 取存活灯数量。
     * @return 槽表中的 live_count，不含空闲槽。
     */
    [[nodiscard]] size_t               get_lights_size() const noexcept;
    /**
     * @brief 取灯光 SSBO。
     * @return 常量引用；灯从未非空过时内部缓冲尚未创建，可用 get_buffer() 判空。
     * @note 渲染器每帧从这里取设备地址，所以缓冲重建后无需另行通知它们更新地址。
     */
    [[nodiscard]] const vulkan_buffer& get_ssbo_buffer() const noexcept;


private:
    /**
     * @brief 把 live() 按槽序映射成 vulkan_light_data 并上传到 ssbo。
     * @param _waited_infos 追加本次上传的完成 token。
     * @throws std::runtime_error upload_buffer 的创建或提交失败。
     * @note 空表直接返回，不重建也不清空 SSBO，保留上一份内容；方向在映射时统一归一化。
     */
    void update_ssbo(std::vector<vulkan_submission_token>& _waited_infos);

    /** @brief VMA 分配器引用，创建与重建 SSBO 时使用。 */
    const vma::raii::Allocator& allocator;
    /** @brief 逻辑设备引用，创建缓冲与一次性命令缓冲时使用。 */
    const vk::raii::Device&     device;
    /** @brief 回收站引用，旧 SSBO 与中间资源按 token 延迟销毁。 */
    vulkan_recycle_bin&         recycle_bin;
    /** @brief SSBO 的所有者队列，消费者阶段覆盖光追/计算/片元。 */
    const vulkan_queue&         graphic_queue;
    /** @brief 上传复制使用的队列，可与 graphic_queue 是同一个 VkQueue。 */
    const vulkan_queue&         transfer_queue;

    /** @brief 脏标志：灯增删或外部修改数据后置位，update 成功后清除；初值 true 保证首帧上传。 */
    bool is_dirty = true;

    /** @brief 灯光槽表，句柄由 create 返回，遍历顺序即 GPU 数组下标。 */
    scene_handle_map<scene_light_data, light_tag> lights;

    /** @brief 存放 vulkan_light_data 数组的共享 SSBO，同时提供设备地址给渲染器。 */
    vulkan_buffer ssbo;
};
