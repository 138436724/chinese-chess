/**
 * @file scene_texture_manager.h
 * @brief 贴图槽管理器：按用途去重、压缩上传（BC6H/BC7）、字形图集合成与 bindless 紧凑绑定维护。
 * @details 贴图没有对外的 destroy：槽的 reference_count 由持有者经 retain / release 维护，
 *          update() 回收零引用槽，再按槽序把 binding 重排成 0..N-1（N = texture_binding_count）。
 *          每种 texture_usage 最多一个采样器，参数由实现文件里的 sampler_info_for 定死。
 *          压缩格式在构造时探测：环境图优先 BC6H（UASTC HDR 转码），LDR 图优先 BC7；
 *          不支持时回退未压缩 RGBA16F / RGBA8。上传令牌追加到调用方给的 waited_infos。
 */

#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "tools/image_helper.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_sampler.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class vulkan_recycle_bin;
class vulkan_queue;

/**
 * @brief 贴图资源的唯一持有者：槽表 + 路径缓存 + 每用途采样器 + 压缩格式选择。
 * @details scene_texture_data 只描述 source + usage（当前 get 返回非 const 指针），Vulkan 图像留在 texture_slot 里，
 *          get() 经槽表校验后只返回描述，不返回图像。引用边只能经 scene_material_manager::set_texture 与
 *          scene_manager::set_environment_image 写入；绕过它们直接写句柄会让贴图被提前回收。
 *          类不提交渲染命令；上传走 vulkan_common::upload_image 并只返回令牌。
 */
class scene_texture_manager
{
public:
    /**
     * @brief 探测设备支持的压缩格式并保存各管理器与队列的引用。
     * @param _allocator VMA 分配器，所有贴图图像都由它分配。
     * @param _physical_device 物理设备，构造时探测 BC6H / BC7 的采样与传输目标支持。
     * @param _device 逻辑设备，创建图像、视图与采样器。
     * @param _recycle_bin 延迟回收站，被替换或回收的贴图图像按提交令牌销毁。
     * @param _graphic_queue 图形队列，上传的 acquire 与 queue family 取自它。
     * @param _transfer_queue 传输队列，staging 拷贝与跨队列族转移走它。
     * @note 两种压缩格式探测失败时记为 eUndefined，后续上传自动回退未压缩格式。
     */
    scene_texture_manager(const vma::raii::Allocator&     _allocator,
                          const vk::raii::PhysicalDevice& _physical_device,
                          const vk::raii::Device&         _device,
                          vulkan_recycle_bin&             _recycle_bin,
                          const vulkan_queue&             _graphic_queue,
                          const vulkan_queue&             _transfer_queue);
    /**
     * @brief 默认析构：槽内图像与采样器按成员声明逆序直接归还，不等待 GPU。
     * @note 调用方必须保证没有在飞帧仍在使用这些图像。
     */
    ~scene_texture_manager() = default;

    /**
     * @brief 按路径与用途创建贴图，命中缓存且槽仍活时直接复用。
     * @param _image_path 源图片路径；usage 为 environment 时按 HDR（half）读取，其余按 LDR 读取。
     * @param _usage 用途，决定采样器参数、HDR 判定与压缩目标格式。
     * @param _waited_infos 上传令牌追加到这里，由调用方纳入后续 GPU 提交依赖。
     * @return 贴图句柄；解码等 expected 错误打印后返回无效句柄，压缩失败会先尝试未压缩回退。
     * @throws std::exception 资源分配、Vulkan 创建或提交等异常直接传播。
     * @note 去重键是 (usage, source)：同一张图按不同用途各占一个槽与一个 bindless 槽位。
     * @note 新建槽引用数为 0，复用缓存也不自动 retain；必须在下一次 update 前由持有者建立引用。
     * @note 异常展开时由 vulkan_submission_scope 先停提交再排空已发布令牌。
     */
    [[nodiscard]] texture_handle      create(const std::filesystem::path&          _image_path,
                                             texture_usage                         _usage,
                                             std::vector<vulkan_submission_token>& _waited_infos);
    /**
     * @brief 在 CPU 侧合成彩色字形图集并创建 glyph 用途贴图，同一组参数复用缓存。
     * @param _font_path 字体文件路径。
     * @param _font_size 字号（像素）。
     * @param _characters 要烘进图集的字符集合，顺序即图集从左到右的顺序。
     * @param _padding 每个字形 advance 左右与图集字形区域上下各加入的空白像素数。
     * @param _foreground_color 字形前景色（线性 RGB）。
     * @param _background_color 背景色（线性 RGB），字形覆盖度为 0 处即此色。
     * @param _waited_infos 上传令牌追加到这里。
     * @return 贴图句柄；字体加载或粘贴等 expected 错误打印后返回无效句柄，压缩失败会尝试未压缩回退。
     * @throws std::exception 资源分配、Vulkan 创建或提交等异常直接传播。
     * @note 去重键是拼出的虚拟路径：字体 / 字号 / 字符 / padding / 前后景色；颜色按四位小数格式化，细微差异可能复用同一槽。
     * @note 创建或复用不自动 retain，调用方须在下一次 update 前建立引用。
     * @note 图集四周额外留 4 texel 纯背景色，配合 clamp-to-edge 采样兜住超出 [0,1] 的模型 UV。
     */
    [[nodiscard]] texture_handle      create(const std::filesystem::path&          _font_path,
                                             uint32_t                              _font_size,
                                             std::wstring_view                     _characters,
                                             uint32_t                              _padding,
                                             const glm::vec3&                      _foreground_color,
                                             const glm::vec3&                      _background_color,
                                             std::vector<vulkan_submission_token>& _waited_infos);
    /**
     * @brief 取贴图的源路径与用途描述。
     * @param _handle 贴图句柄。
     * @return 指向 scene_texture_data 的指针；句柄失效或越界时返回 nullptr。
     * @note 指针在下一次槽表插入 / 擦除之前有效；data 不含 Vulkan 对象；虽然指针可写，直接修改 source / usage 不会同步缓存键、采样器或图像。
     */
    [[nodiscard]] scene_texture_data* get(texture_handle _handle) noexcept;

    /**
     * @brief 重新读取源文件并在原槽里换图，句柄与紧凑绑定保持不变。
     * @param _handle 贴图句柄。
     * @param _waited_infos 新图上传令牌追加到这里。
     * @return 换图成功返回 true；句柄无效、glyph 图集或上传失败返回 false（旧图保留）。
     * @note 旧图进回收站按已提交前沿销毁；只置 is_dirty，不置 layout_dirty（槽序与下标都没变）。
     */
    [[nodiscard]] bool reload_texture(texture_handle _handle, std::vector<vulkan_submission_token>& _waited_infos);
    /**
     * @brief F6 热重载：遍历所有非 glyph 贴图，源文件存在且哈希变化时才重载。
     * @param _waited_infos 所有重载上传的令牌追加到这里。
     * @return 至少有一张贴图被重载返回 true。
     * @note 字形图集的 source 是虚拟路径，直接跳过；单线程遍历，不加锁。
     */
    [[nodiscard]] bool reload_all_textures(std::vector<vulkan_submission_token>& _waited_infos);

    /**
     * @brief 引用计数加一；句柄无效时无操作。
     * @param _handle 被持有的贴图句柄。
     * @note 只由持有者（材质通道、环境图）在建立引用边时调用，必须与 release 成对。
     */
    void retain(texture_handle _handle) noexcept;
    /**
     * @brief 引用计数减一；句柄无效时无操作。
     * @param _handle 被释放的贴图句柄。
     * @throws std::runtime_error 仅 Debug：计数已是 0 仍继续 release（retain / release 不配对）。
     * @note 计数归零不会立刻删槽，真正的回收发生在下一次 update()。
     */
    void release(texture_handle _handle);

    /**
     * @brief update() 的结果：是否需要重画 / 重写描述符，以及绑定下标是否移动。
     * @details needs_redraw 为 true 时渲染器要 update()（重建描述符）；layout_moved 为 true 时
     *          材质 SSBO 必须重传，因为贴图的紧凑绑定可能改变。
     */
    struct update_result
    {
        /** @brief 贴图集合或绑定发生变化，需要重写描述符。 */
        bool needs_redraw = false;
        /** @brief 绑定下标可能移动，材质 SSBO 里的 texture_index 需要重算。 */
        bool layout_moved = false;
    };

    /**
     * @brief 回收零引用贴图并重排 bindless 紧凑绑定。
     * @return 重画标志与布局移动标志。
     * @throws std::runtime_error 仅 Debug：存活贴图数超过 MAX_BINDLESS_TEXTURES。
     * @note 无参：贴图没有对外的 destroy，零引用就是垃圾。
     * @note 即使没有死槽也会清 layout_dirty；is_dirty 为 false 时直接返回。
     */
    [[nodiscard]] update_result update();

    /**
     * @brief 只把贴图集合标记为脏，不改变引用计数与槽序。
     * @note 环境强度变化等只需要重画的场合用它；它不会自动置 layout_dirty。
     */
    void need_update() noexcept;

    /**
     * @brief 取当前 bindless 绑定数（也是 shader 的 texture_count）。
     * @return 上一次 update() 排出的 binding 数，上限 MAX_BINDLESS_TEXTURES。
     */
    [[nodiscard]] size_t                               get_textures_size() const noexcept;
    /**
     * @brief 取贴图的紧凑绑定下标。
     * @param _handle 贴图句柄。
     * @return 0..texture_binding_count-1；句柄无效或绑定越界时为 scene_handle<texture_tag>::npos。
     */
    [[nodiscard]] uint32_t                             get_texture_binding(texture_handle _handle) const noexcept;
    /**
     * @brief 按绑定序拼出 bindless 数组的 DescriptorImageInfo。
     * @return 每个已绑定贴图一项：采样器按 usage 选、view 来自槽内图像、布局 eShaderReadOnlyOptimal，
     *         返回值下标即紧凑绑定。
     * @throws std::runtime_error 某个 usage 还没有采样器（正常路径不会发生，create 前必 ensure_sampler）。
     */
    [[nodiscard]] std::vector<vk::DescriptorImageInfo> get_descriptor_info() const;

private:
    /**
     * @brief 一个贴图槽：图像 + 源路径/用途描述 + 紧凑绑定 + 引用计数。
     * @details binding 由 update() 按槽序重排；reference_count 由 retain / release 维护，
     *          归零的槽在下一次 update() 被销毁。
     */
    struct texture_slot
    {
        /** @brief 上传后的 Vulkan 图像与视图。 */
        vulkan_image       image;
        /** @brief 对外暴露的描述（源路径 + 用途），调用方不应直接改写去重键。 */
        scene_texture_data data;
        /** @brief bindless 数组下标；npos 表示尚未绑定或超出上限。 */
        uint32_t           binding         = scene_handle<texture_tag>::npos;
        /** @brief 引用计数：材质通道与环境图各算一次。 */
        uint32_t           reference_count = 0;
    };

    /** @brief 去重键：用途 + 源路径。 */
    using texture_key = std::pair<texture_usage, std::filesystem::path>;

    /**
     * @brief 销毁一个槽并把它的图像进回收站。
     * @param _handle 贴图句柄。
     * @return 当前实现恒为 true（槽不存在时 erase 的失败结果被忽略）。
     * @note 会同步清掉 texture_cache 中所有指向该句柄的项，并置 is_dirty 与 layout_dirty。
     */
    [[nodiscard]] bool destroy(texture_handle _handle);

    /**
     * @brief 上传一条贴图数据：优先复用 / 生成 .ktx2 侧车并转码，失败再回退未压缩格式。
     * @param _source 源文件路径（HDR 按 half 读，LDR 按 uint8 读）。
     * @param _usage 用途，只用来判定是否按 HDR 处理。
     * @param _image 输出参数：成功后持有新图像。
     * @param _waited_infos 上传令牌追加到这里。
     * @return 成功为空 expected；失败为带原因的错误串。
     * @note environment 用 BC6H / RGBA16F，其余用 BC7 / RGBA8；两种压缩格式在构造时已探测。
     * @note 侧车存在且源文件未修改时直接读 .ktx2，否则重新压缩并同步写侧车。
     */
    [[nodiscard]] std::expected<void, std::string> upload_texture_data(const std::filesystem::path& _source,
                                                                       texture_usage                _usage,
                                                                       vulkan_image&                _image,
                                                                       std::vector<vulkan_submission_token>& _waited_infos) const;
    /**
     * @brief 把 KTX2 转码成目标压缩格式并上传 mip0。
     * @param _ktx2 已解码的 KTX2 纹理，函数内可能原地转码。
     * @param _target_format 目标 VkFormat，决定转码目标（BC6H / BC7 / BC4 / EAC_R11）。
     * @param _image 输出图像。
     * @param _waited_infos 上传令牌追加到这里。
     * @param _name Debug 对象名后缀。
     * @return 成功为空 expected；失败为错误串（不支持的格式、转码失败、取 mip 偏移失败、无 mip0）。
     * @note 只上传 mip0；KTX2 的 vkFormat 已等于目标格式时跳过转码。
     */
    [[nodiscard]] std::expected<void, std::string> upload_ktx2(const ktx2_texture_ptr&               _ktx2,
                                                               vk::Format                            _target_format,
                                                               vulkan_image&                         _image,
                                                               std::vector<vulkan_submission_token>& _waited_infos,
                                                               const std::string&                    _name) const;

    /**
     * @brief 确保该用途已有采样器；没有则按 sampler_info_for 创建并存入表。
     * @param _usage 贴图用途。
     * @throws vk::SystemError 采样器创建失败。
     * @note 每种用途只建一次，后续调用直接返回。
     */
    void                      ensure_sampler(texture_usage _usage);
    /**
     * @brief 取该用途的采样器句柄。
     * @param _usage 贴图用途。
     * @return 裸 vk::Sampler；表里没有时抛异常。
     * @throws std::runtime_error 该用途还没有采样器。
     */
    [[nodiscard]] vk::Sampler get_sampler(texture_usage _usage) const;

    /** @brief VMA 分配器，所有贴图图像都由它分配。 */
    const vma::raii::Allocator&     allocator;
    /** @brief 物理设备，创建采样器时读取各向异性上限。 */
    const vk::raii::PhysicalDevice& physical_device;
    /** @brief 逻辑设备，创建图像、视图与采样器。 */
    const vk::raii::Device&         device;
    /** @brief 延迟回收站，被替换或回收的图像按提交令牌销毁。 */
    vulkan_recycle_bin&             recycle_bin;
    /** @brief 图形队列，上传的 acquire 与 queue family 取自它。 */
    const vulkan_queue&             graphic_queue;
    /** @brief 传输队列，staging 拷贝与跨队列族转移走它。 */
    const vulkan_queue&             transfer_queue;

    /** @brief 贴图集合 / 绑定需要重排并重写描述符。 */
    bool is_dirty     = true;
    /** @brief 槽被插入或擦除，紧凑绑定可能改变；用于判定材质 SSBO 是否必须重传。 */
    bool layout_dirty = false;

    /** @brief 上一次 update() 排出的绑定数，也是 shader 的 texture_count。 */
    size_t                                            texture_binding_count = 0;
    /** @brief 贴图槽表，槽序就是绑定顺序。 */
    scene_handle_map<texture_slot, texture_tag>       textures;
    /** @brief (usage, source) → 句柄的去重表；槽回收时同步清理。 */
    std::map<texture_key, texture_handle>             texture_cache;
    /** @brief 每种用途一个采样器，最多三个。 */
    std::unordered_map<texture_usage, vulkan_sampler> samplers;

    /** @brief 环境图压缩格式，构造时探测 BC6H；不支持时为 eUndefined。 */
    vk::Format compressed_hdr_format = vk::Format::eBc6HUfloatBlock;
    /** @brief LDR 图压缩格式，构造时探测 BC7；不支持时为 eUndefined。 */
    vk::Format compressed_ldr_format = vk::Format::eBc7UnormBlock;
};
