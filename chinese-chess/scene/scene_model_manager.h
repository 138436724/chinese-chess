/**
 * @file scene_model_manager.h
 * @brief 模型实例与网格槽管理器：网格去重、共享顶点/索引缓冲、BLAS/TLAS、间接绘制命令与模型 SSBO。
 * @details 对外只发 model_handle；scene_model_data（网格 / 材质句柄、可见性、模型矩阵）按槽存放，
 *          同一路径的网格只占一个 mesh 槽并带一条 BLAS。顶点与索引按网格槽序紧凑拼进共享缓冲，
 *          TLAS 实例与 drawIndexedIndirect 命令按模型槽序生成。网格按存活模型的引用数回收，
 *          模型本身只由 destroy 移除。所有 GPU 数据都在 update() 里上传，令牌追加到调用方
 *          给出的 waited_infos，等待与释放由 scene_manager 统一负责。
 */

#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "vulkan_core/vulkan_acceleration_structure.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;
class scene_material_manager;

/**
 * @brief 模型层管理器：持有模型与网格两张槽表，以及由它们派生出的全部 GPU 缓冲与加速结构。
 * @details 两张表的生命周期互相独立：模型槽可随时增删，网格槽只在引用数归零时回收。
 *          网格数据（顶点 / 索引 / BLAS）随 meshes_dirty 整批重建，TLAS、绘制命令与 SSBO
 *          随 is_dirty 每次更新重建。所有引用（分配器 / 设备 / 队列 / 材质管理器）都不拥有，
 *          必须比本对象活得久；析构不等待 GPU，调用方须先让设备空闲。
 */
class scene_model_manager
{
public:
    /**
     * @brief 记录分配器、设备、队列与材质管理器的引用，不分配任何 GPU 资源。
     * @param _allocator VMA 分配器，后续网格缓冲、TLAS 与 SSBO 都由它分配。
     * @param _physical_device 物理设备，BLAS/TLAS 构建时查询属性与 scratch 对齐。
     * @param _device 逻辑设备，创建缓冲、加速结构与一次性命令缓冲。
     * @param _recycle_bin 延迟回收站，旧缓冲、旧 BLAS 与一次性命令缓冲在这里按提交令牌销毁。
     * @param _graphic_queue 图形队列，网格上传以及 BLAS/TLAS 构建都提交到它。
     * @param _transfer_queue 传输队列，upload_buffer 的 staging 拷贝与队列族转移走它。
     * @param _material_manager 材质管理器，update_ssbo 从中取材质的紧凑绑定下标。
     * @note 全部参数都按引用保存，调用方必须保证它们比本对象活得久；构造 noexcept，不触碰 GPU。
     */
    scene_model_manager(const vma::raii::Allocator&     _allocator,
                        const vk::raii::PhysicalDevice& _physical_device,
                        const vk::raii::Device&         _device,
                        vulkan_recycle_bin&             _recycle_bin,
                        const vulkan_queue&             _graphic_queue,
                        const vulkan_queue&             _transfer_queue,
                        scene_material_manager&         _material_manager) noexcept;
    /**
     * @brief 默认析构：缓冲与加速结构按成员声明逆序直接归还，不等待 GPU。
     * @note 调用方必须保证没有在飞帧仍在读这些资源（scene_manager 析构前先等设备空闲）。
     */
    ~scene_model_manager() = default;

    /**
     * @brief 按路径加载网格并新建一个模型实例；同一路径的网格复用已有 mesh 槽。
     * @param _model_path glTF 模型路径，相对运行目录 chinese-chess/。
     * @return 新模型句柄；加载失败时打印错误并返回默认构造（无效）的 model_handle。
     * @note 网格首次加载时只把顶点 / 索引留在 CPU 槽里，真正上传与 BLAS 构建推迟到下一次 update()。
     * @note 创建实例会置 is_dirty，首次加载网格还会置 meshes_dirty；模型矩阵、材质与可见性由调用方随后经 get() 写入。
     */
    [[nodiscard]] model_handle      create(const std::filesystem::path& _model_path);
    /**
     * @brief 取模型槽的可写数据。
     * @param _handle 模型句柄。
     * @return 指向 scene_model_data 的指针；句柄失效或越界时返回 nullptr。
     * @note 指针在下一次模型槽插入 / 擦除之前有效，不要跨帧保存。
     */
    [[nodiscard]] scene_model_data* get(model_handle _handle) noexcept;
    /**
     * @brief 立即移除一个模型槽。
     * @param _handle 模型句柄。
     * @return 擦除成功返回 true；句柄已失效或不存在返回 false。
     * @note 只删模型，不直接删网格：网格的 BLAS 要等 update() 重算引用数归零后才回收。
     */
    [[nodiscard]] bool              destroy(model_handle _handle) noexcept;

    /**
     * @brief 按需重建网格缓冲、BLAS、TLAS、间接绘制命令与模型 SSBO，并回收零引用网格。
     * @param _waited_infos 上传与构建产生的提交令牌追加到这里，由 scene_manager 在录制渲染前统一等待。
     * @return 本次是否真正改动了 GPU 数据；is_dirty 为 false 时返回 false。
     * @note 每次调用都先重算网格引用数并销毁零引用网格，即使 is_dirty 为 false 也会做这一步。
     * @note 脏标志只在整个更新成功后清除；中途抛异常时 is_dirty 保持 true，下次会重做。
     * @note 必须在主线程、渲染命令录制之前调用；内部会提交一次性命令缓冲，但不等 GPU。
     */
    [[nodiscard]] bool update(std::vector<vulkan_submission_token>& _waited_infos);

    /**
     * @brief 标记实例数据为脏，更新 TLAS、间接绘制与模型 SSBO，不单独触发顶点/索引与 BLAS 重建。
     * @note 用于模型矩阵、可见性或材质绑定变化；网格增删走 create / destroy 自带的脏标志。
     */
    void need_update() noexcept;

    /**
     * @brief 取存活模型数（不含网格数）。
     * @return 模型槽的 live_size()，同时也是 drawIndexedIndirect 的命令条数与 shader 的 model_count。
     */
    [[nodiscard]] size_t                               get_models_size() const noexcept;
    /**
     * @brief 取所有网格顶点拼成的共享顶点缓冲。
     * @return 缓冲引用；首次 update() 之前是空缓冲。
     * @note 引用随 update_meshes 重建失效，不要跨帧保存。
     */
    [[nodiscard]] const vulkan_buffer&                 get_vertices_buffer() const noexcept;
    /**
     * @brief 取所有网格索引拼成的共享索引缓冲。
     * @return 缓冲引用；首次 update() 之前是空缓冲。
     * @note 引用随 update_meshes 重建失效，不要跨帧保存。
     */
    [[nodiscard]] const vulkan_buffer&                 get_indices_buffer() const noexcept;
    /**
     * @brief 取覆盖全部网格有效模型的顶层加速结构。
     * @return TLAS 引用；空场景时是一条 0 实例的 TLAS。
     * @note 光追与 Ray Query 渲染器把它的句柄写进描述符，重建后必须重写描述符。
     */
    [[nodiscard]] const vulkan_acceleration_structure& get_tlas() const noexcept;
    /**
     * @brief 取按模型槽序排列的间接绘制命令缓冲。
     * @return eIndirectBuffer 缓冲引用；is_show 为 false 的模型 instanceCount 写 0。
     */
    [[nodiscard]] const vulkan_buffer&                 get_draw_commands() const noexcept;
    /**
     * @brief 取模型 SSBO（vulkan_model_data 数组），着色器经 scene data 的 models 指针访问。
     * @return 缓冲引用；其设备地址每帧写进渲染器的 UBO。
     */
    [[nodiscard]] const vulkan_buffer&                 get_ssbo_buffer() const noexcept;

private:
    /**
     * @brief 网格槽内容：一份唯一的 CPU 顶点 / 索引，加共享缓冲里的字节偏移与一条 BLAS。
     * @details 同一路径的模型共用一个网格槽；reference_count 由 update() 每轮按存活模型重算，
     *          归零即回收。vertex_offset / index_offset 是相对共享缓冲起始的字节偏移，
     *          缓冲重建后按槽序重排仍然有效，BLAS 也随缓冲重建一起重建。
     */
    struct scene_mesh_data
    {
        /** @brief CPU 侧顶点数据，元素是 vulkan_model_vertex（位置 / 法线 / uv）。 */
        std::vector<vulkan_model_vertex> vertices;
        /** @brief CPU 侧 32 位索引，指向本网格的 vertices。 */
        std::vector<uint32_t>            indices;
        /** @brief 本网格顶点在共享顶点缓冲中的字节偏移。 */
        size_t                           vertex_offset   = 0;
        /** @brief 本网格索引在共享索引缓冲中的字节偏移。 */
        size_t                           index_offset    = 0;
        /** @brief 引用本网格的存活模型数，update() 每轮重算，0 表示可回收。 */
        uint32_t                         reference_count = 0;
        /** @brief 本网格的 BLAS，顶点 / 索引缓冲重建时整条重建。 */
        vulkan_acceleration_structure    blas;
    };

    /**
     * @brief 按网格槽序重建共享顶点 / 索引缓冲，并重建所有网格的 BLAS。
     * @param _waited_infos 两条上传与一次 BLAS 构建提交的令牌追加到这里。
     * @note 旧缓冲与旧 BLAS 先搬进回收站再建新的，新 BLAS 使用新缓冲的设备地址。
     * @note scratch 与一次性命令缓冲按 BLAS 构建提交的令牌退休，不能提前释放。
     */
    void update_meshes(std::vector<vulkan_submission_token>& _waited_infos);
    /**
     * @brief 重建或 refit TLAS，使其覆盖当前所有网格有效的存活模型。
     * @param _waited_infos 实例上传与构建提交的令牌追加到这里。
     * @note 上次实例数非零且本次不变时走 refit（复用 scratch），否则整条重建；空场景建 0 实例 TLAS。
     */
    void update_tlas(std::vector<vulkan_submission_token>& _waited_infos);
    /**
     * @brief 按模型槽序重建间接绘制命令。
     * @param _waited_infos 命令上传的令牌追加到这里。
     * @note 网格无效时命令写 0 索引 / 0 实例；is_show 为 false 时 instanceCount 写 0。
     */
    void update_draw_commands(std::vector<vulkan_submission_token>& _waited_infos);
    /**
     * @brief 按模型槽序重建 vulkan_model_data 数组并上传到 SSBO。
     * @param _waited_infos 上传令牌追加到这里。
     * @note material_index 来自材质管理器的紧凑绑定；网格无效时两个设备地址都写 0。
     */
    void update_ssbo(std::vector<vulkan_submission_token>& _waited_infos);

    /** @brief VMA 分配器，网格缓冲、TLAS 与 SSBO 都由它分配。 */
    const vma::raii::Allocator&     allocator;
    /** @brief 物理设备，BLAS/TLAS 构建时查询属性与 scratch 对齐。 */
    const vk::raii::PhysicalDevice& physical_device;
    /** @brief 逻辑设备，创建缓冲、加速结构与一次性命令缓冲。 */
    const vk::raii::Device&         device;
    /** @brief 延迟回收站，所有被替换的 GPU 资源都推迟到对应提交完成之后销毁。 */
    vulkan_recycle_bin&             recycle_bin;
    /** @brief 图形队列，网格上传与加速结构构建提交到它。 */
    const vulkan_queue&             graphic_queue;
    /** @brief 传输队列，upload_buffer 的 staging 拷贝与队列族转移走它。 */
    const vulkan_queue&             transfer_queue;
    /** @brief 材质管理器，update_ssbo 用它把 material_handle 翻成紧凑绑定下标。 */
    scene_material_manager&         material_manager;

    /** @brief 模型 / 网格数据需要重传 GPU（模型增删、矩阵或可见性变化）。 */
    bool is_dirty     = true;
    /** @brief 网格集合发生变化，需要重建顶点 / 索引缓冲与全部 BLAS。 */
    bool meshes_dirty = true;

    /** @brief 网格槽表，按槽序拼进共享顶点 / 索引缓冲。 */
    scene_handle_map<scene_mesh_data, mesh_tag>            meshes;  // submit to gpu in slot order
    /** @brief 模型槽表，槽序决定实例下标、draw 命令与 SSBO 元素的顺序。 */
    scene_handle_map<scene_model_data, model_tag>          models;
    /** @brief 路径 → 网格槽的去重缓存；网格回收时同步清掉对应项。 */
    std::unordered_map<std::filesystem::path, mesh_handle> mesh_cache;

    /** @brief 所有网格顶点拼成的共享顶点缓冲，同时用作 BLAS 顶点输入与光追的 BDA 顶点地址。 */
    vulkan_buffer vertices_buffer;
    /** @brief 所有网格索引拼成的共享索引缓冲，同时用作 BLAS 索引输入与光追的 BDA 索引地址。 */
    vulkan_buffer indices_buffer;

    /** @brief 上一次 TLAS 构建 / refit 使用的实例数，用于判断能否就地 refit。 */
    size_t                        tlas_instance_count = 0;
    /** @brief TLAS 构建 / refit 复用的 scratch 缓冲，尺寸按 build 与 update 取较大者。 */
    vulkan_buffer                 tlas_scratch_buffer;
    /** @brief 顶层加速结构，覆盖所有网格有效的模型实例。 */
    vulkan_acceleration_structure tlas;

    /** @brief 按模型槽序排列的 vk::DrawIndexedIndirectCommand 缓冲。 */
    vulkan_buffer draw_commands;

    /** @brief 模型 SSBO，元素是 vulkan_model_data（模型矩阵、材质绑定、顶点 / 索引设备地址）。 */
    vulkan_buffer ssbo;
};
