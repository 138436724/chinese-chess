/**
 * @file scene_manager.h
 * @brief 场景层编排器：拥有相机、环境、四个 manager、三套渲染器与截图线程。
 * @details 对外只暴露句柄与少量引用（相机、环境、输出图像）；实体的增删查转交对应 manager。
 *          update() 是唯一的重活入口：先按脏标志更新贴图/材质/模型/灯光，四个 manager 都报告没变时
 *          只清累积，否则重建活跃渲染器的描述符与场景资源；render() 负责录制、提交并可选回读截图。
 *          三套渲染模式各建一个 manager_render proxy，活跃者用 proxy_view 指向 map 内部，不转移所有权。
 * @note 成员声明顺序即析构逆序：texture_manager 必须早于 material_manager、material 早于 model；
 *       save_thread 声明在最后，析构时先 join 再释放 GPU 资源。
 */
#pragma once

#include "scene_base.h"
#include "scene_camera.h"
#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "scene_texture_manager.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_data_type.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <cstdint>
#include <exception>
#include <mutex>
#include <filesystem>
#include <glm/glm.hpp>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

class vulkan_application;

/**
 * @brief 渲染模式，同时用作 scene_renders 的键与 UI 下拉框的取值。
 * @details 三种模式共享同一份场景数据与输出图像，切换时只换活跃 proxy 并重建描述符，
 *          不重建场景资源。
 */
enum class render_mode : uint32_t
{
    rasterization, ///< 光栅化前向 PBR：有天空盒，但没有可见性项，因此无阴影、无 AO。
    ray_tracing,   ///< 硬件光追管线：参考实现，无偏蒙特卡洛路径追踪。
    ray_query,     ///< 计算管线 + Ray Query：与光追管线逐像素同分布，只换求交 API。
};

/**
 * @brief 场景层唯一入口：持有并编排全部场景状态、资源与渲染后端。
 * @details 构造时按依赖顺序初始化四个 manager、注册回收站队列、建三套渲染器并建输出图像；
 *          析构先 wait_idle_for_cleanup 再按成员逆序释放，后台截图线程在此隐式 join。
 *          实体访问返回裸指针，有效期到下一次 insert / erase；生命周期由本类持有的槽表决定。
 * @note 非线程安全：除截图线程只读自己的 staging 缓冲并写 save_error 外，其余成员只在主线程（窗口帧循环与 UI）访问。
 */
class scene_manager
{
public:
    /**
     * @brief 组装四个 manager 与三套渲染器，注册回收站队列，最后按初始尺寸建输出图像。
     * @param _app 提供 device / allocator / queue / pipeline cache 的应用对象。
     * @param _width 初始输出宽度，不能为 0。
     * @param _height 初始输出高度，不能为 0。
     * @throws vk::SystemError 图像、管线或描述符创建失败时由 Vulkan-Hpp 抛出。
     * @throws std::runtime_error 渲染器或命令缓冲初始化失败时抛出。
     * @note 构造末尾调用 resize()，因此 render_output 与活跃渲染器在构造完成时已可用。
     * @note recycle_bin 在此登记 graphic / transfer 两条队列，使两侧 token 可直接比较。
     */
    scene_manager(vulkan_application& _app, uint32_t _width, uint32_t _height);
    /**
     * @brief 先 wait_idle_for_cleanup，再按成员逆序释放；后台截图线程在此隐式 join。
     * @note 不能捕获 wait_idle_for_cleanup 的失败：该函数对设备丢失只打印，其余失败直接 terminate。
     */
    ~scene_manager();

    /**
     * @brief 按新尺寸重建场景输出图像，并调整当前活跃渲染器。
     * @param _width 新宽度。
     * @param _height 新高度。
     * @throws vk::SystemError 图像或视图创建失败时抛出。
     * @note 旧输出图像交给 retire_after_submitted 延迟销毁；另外两个非活跃渲染器不在此 resize，
     *       切过去时靠 need_descriptor_update 重写描述符补上。
     */
    void                                  resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 脏门控更新：先重抛后台截图异常，再依次更新贴图/材质/模型/灯光并按需更新渲染器。
     * @throws std::exception 后台截图线程保存的异常（std::exception_ptr 原样重抛）。
     * @throws std::runtime_error Debug 下发现贴图句柄悬空（持有者绕过 retain/release）时抛出。
     * @throws vk::SystemError 各 manager 的上传或渲染器 update 失败时抛出。
     * @note 无脏时直接返回；相机-only 变化只走 reset_accumulation，不重写描述符。
     * @note 贴图布局移动会连带置材质脏；成功后清 is_dirty 与 need_descriptor_update。
     * @note save_error 保存后不会被清除，因此失败一旦发生，后续每次 update 都会重抛同一个异常。
     */
    void                                  update();
    /**
     * @brief 录制、提交本帧场景命令，可选回读输出图像做截图。
     * @param frame_slot 帧槽下标，用于取命令缓冲与渲染器 UBO 槽，必须小于 MAX_FRAMES_IN_FLIGHT。
     * @param _need_capture 是否在提交后回读 render_output 并异步写盘。
     * @return 提交完成 token；带截图时是回读末条命令的 token，合成必须等它。
     * @throws std::out_of_range frame_slot 超出已分配的命令缓冲数量时抛出。
     * @throws std::runtime_error 命令缓冲未分配、提交失败或截图格式不支持时抛出。
     * @note 内部会再调用一次 update()，以覆盖本帧稍后切换的渲染模式。
     * @note 之前累积的 waited_infos 在这里转成命令缓冲依赖并清空。
     */
    [[nodiscard]] vulkan_submission_token        render(uint32_t frame_slot, bool _need_capture);
    /**
     * @brief 处理窗口按键：F5 重建活跃管线，F6 重载全部贴图，其余键忽略。
     * @param _key GLFW 键码。
     * @param _scancode 平台扫描码，当前未使用。
     * @param _action GLFW 动作；只处理 GLFW_PRESS，重复与释放都被忽略。
     * @param _mods 修饰键位掩码，当前未使用。
     * @note F5 的 recreate 失败只打印并保留旧管线，不向调用方抛出。
     * @note F6 成功后会同时置材质与模型脏，因为材质 SSBO 的贴图绑定需要重算。
     */
    void                                  handle(int _key, int _scancode, int _action, int _mods);

    /**
     * @brief 置全脏：场景标志与四个 manager 的脏标志一起置位。
     * @note 用于整体刷新（如初次显示），代价是下一帧重传全部 SSBO 并重建描述符。
     */
    void need_update() noexcept;
    /**
     * @brief 只置场景脏，不碰任何 manager。
     * @note 相机矩阵每帧直接写 UBO，因此相机变化不需要重传场景数据或重写描述符。
     */
    void need_camera_update() noexcept;
    /**
     * @brief 置场景脏与灯光脏，让下一帧重传灯光 SSBO。
     */
    void need_light_update() noexcept;
    /**
     * @brief 置场景脏与材质脏，让下一帧重排 binding 并重传材质 SSBO。
     */
    void need_material_update() noexcept;
    /**
     * @brief 置场景脏与模型脏，让下一帧重建实例缓冲与 TLAS。
     */
    void need_model_update() noexcept;
    /**
     * @brief 置场景脏与贴图脏，用于环境贴图或强度的变化。
     * @note 环境在每帧 render 时才翻译成 GPU 数据，所以这里只让贴图管理器重算紧凑 binding；
     *       不会连带重建材质 SSBO。
     */
    void need_environment_update() noexcept;

    /**
     * @brief 新建一盏默认参数的灯。
     * @return 新灯句柄；insert 只会返回有效句柄或抛出异常。
     * @throws 槽表扩容或实体移动构造失败时传播异常。
     */
    [[nodiscard]] light_handle    create_light();
    /**
     * @brief 从图片文件创建或复用一张贴图。
     * @param _image_path 源图路径，同时是去重键。
     * @param _usage 贴图用途，决定采样器与压缩格式。
     * @return 贴图句柄；解码或上传失败时返回无效句柄并打印错误。
     * @note 句柄由材质或环境持有前必须先经 set_material_texture / set_environment_image 建立引用。
     * @throws vk::SystemError 底层图像创建或上传提交失败时抛出。
     */
    [[nodiscard]] texture_handle  create_texture(const std::filesystem::path& _image_path, texture_usage _usage);
    /**
     * @brief 用字体与前景/背景色合成彩色字形图集贴图。
     * @param _font_path 字体文件路径，同时参与去重键。
     * @param _font_size 字号（像素），同时决定字形位图大小。
     * @param _characters 要合成的字符集合。
     * @param _padding 每个字形四周的留白，单位 texel。
     * @param _foreground_color 字形前景色，线性 RGB。
     * @param _background_color 图集背景色，线性 RGB；clamp-to-edge 采样时露出的就是它。
     * @return 贴图句柄；字体加载或上传失败时返回无效句柄并打印错误。
     * @throws vk::SystemError 底层图像创建或上传提交失败时抛出。
     * @note 图集先按覆盖度合成单通道，再逐像素在背景色与前景色之间插值展开成 RGBA。
     * @note 去重键包含路径、字号、字符集、留白与前景/背景色，任一不同都会建出独立图集。
     */
    [[nodiscard]] texture_handle  create_texture(const std::filesystem::path& _font_path,
                                                 uint32_t                     _font_size,
                                                 std::wstring_view            _characters,
                                                 uint32_t                     _padding,
                                                 const glm::vec3&             _foreground_color,
                                                 const glm::vec3&             _background_color);
    /**
     * @brief 新建一个默认材质。
     * @return 新材质句柄。
     * @note 同时置模型脏：新增材质可能占用更小的空闲槽，让已有材质的紧凑 binding 前移，
     *       模型 SSBO 的 material_index 需要重写。
     * @throws 槽表扩容或实体移动构造失败时传播异常。
     */
    [[nodiscard]] material_handle create_material();
    /**
     * @brief 从模型文件创建模型实例（同路径网格会被复用）。
     * @param _model_path glTF / GLB 路径。
     * @return 新模型句柄；模型加载失败时返回无效句柄并打印错误。
     * @throws 槽表扩容或网格插入失败时传播异常。
     * @note 模型实例不拥有几何：网格槽由 scene_model_manager 按路径去重并维护引用计数。
     */
    [[nodiscard]] model_handle    create_model(const std::filesystem::path& _model_path);

    /**
     * @brief 按句柄取可写灯数据。
     * @param _handle 灯句柄。
     * @return 存活指针；无效、越界或世代不符时返回 nullptr。
     * @note 指针有效期到下一次 insert / erase。
     */
    [[nodiscard]] scene_light_data*    get(light_handle _handle) noexcept;
    /**
     * @brief 按句柄取贴图描述。
     * @param _handle 贴图句柄。
     * @return 存活槽的只读描述指针；无效、越界或世代不符时返回 nullptr。
     * @note 返回值不含 Vulkan 对象，Vulkan 图像与采样器一直留在 scene_texture_manager 内部。
     */
    [[nodiscard]] scene_texture_data*  get(texture_handle _handle) noexcept;
    /**
     * @brief 按句柄取可写材质数据。
     * @param _handle 材质句柄。
     * @return 存活指针；无效、越界或世代不符时返回 nullptr。
     */
    [[nodiscard]] scene_material_data* get(material_handle _handle) noexcept;
    /**
     * @brief 按句柄取可写模型数据。
     * @param _handle 模型句柄。
     * @return 存活指针；无效、越界或世代不符时返回 nullptr。
     */
    [[nodiscard]] scene_model_data*    get(model_handle _handle) noexcept;

    /**
     * @brief 销毁一盏灯。
     * @param _handle 灯句柄；已死时什么也不做。
     * @note 标 noexcept；成功时置场景脏，灯光 SSBO 在下一帧重传。
     */
    void destroy(light_handle _handle) noexcept;
    /**
     * @brief 销毁一个材质，先释放它的全部贴图引用。
     * @param _handle 材质句柄；已死时什么也不做。
     * @note 标 noexcept；成功时同时置场景与模型脏，因为后续材质的紧凑 binding 可能前移。
     */
    void destroy(material_handle _handle) noexcept;
    /**
     * @brief 销毁一个模型实例（网格按引用计数决定是否回收）。
     * @param _handle 模型句柄；已死时什么也不做。
     * @note 标 noexcept；成功时置场景脏，实例缓冲与 TLAS 在下一帧重建。
     */
    void destroy(model_handle _handle) noexcept;

    /**
     * @brief 转发到 material_manager::set_texture，并置场景脏。
     * @param _handle 材质句柄。
     * @param _channel 贴图通道。
     * @param _texture 新贴图句柄，无效句柄表示清空该通道。
     * @note 标 noexcept；Debug 下非法通道会让 material_manager 抛 runtime_error，穿过 noexcept 会 terminate。
     */
    void set_material_texture(material_handle _handle, texture_channel _channel, texture_handle _texture) noexcept;
    /**
     * @brief 更换环境贴图：释放旧图引用、建立新图引用并置环境脏。
     * @param _image 新环境贴图句柄；无效句柄表示没有天空盒，着色器退回程序化天空。
     * @note 必须经本函数写入：直接改 get_environment().image 会破坏引用计数。
     * @note 强度变化不经过本函数，直接改 environment.intensity 后调用 need_environment_update()。
     */
    void set_environment_image(texture_handle _image) noexcept;
    /**
     * @brief 切换活跃渲染器，并按当前尺寸 resize 新后端。
     * @param _mode 目标渲染模式。
     * @throws std::out_of_range _mode 不在已创建的三套渲染器里时抛出。
     * @note 置 need_descriptor_update：新后端的描述符可能指向旧输出图像，必须重写。
     * @note 只重建描述符，不重传场景数据。
     */
    void set_render_mode(render_mode _mode);

    /**
     * @brief 重新加载单张贴图。
     * @param _handle 贴图句柄。
     * @return 是否重载成功；失败时保留旧图并返回 false。
     * @throws vk::SystemError 重载过程中的图像创建或上传提交失败时抛出。
     * @note 成功后会置模型脏，因为模型 SSBO 里的贴图绑定可能需要更新。
     */
    [[nodiscard]] bool reload_texture(texture_handle _handle);

    /**
     * @brief 取环境状态引用。
     * @return scene_environment_data 的引用，供 UI 读 image 判空、改 intensity。
     * @note image 是引用边，只能经 set_environment_image 写。
     */
    [[nodiscard]] scene_environment_data& get_environment() noexcept;
    /**
     * @brief 查询场景是否有待处理的更新。
     * @return is_dirty；true 表示下一次 update 会真正工作。
     */
    [[nodiscard]] bool                    get_need_update() const noexcept;
    /**
     * @brief 取当前相机。
     * @return 相机引用，UI 可直接修改参数，改完应调用 need_camera_update()。
     */
    [[nodiscard]] scene_camera&           get_active_camera() noexcept;
    /**
     * @brief 取场景渲染输出图像。
     * @return 图像引用，供合成阶段采样；resize 时整体替换。
     */
    [[nodiscard]] vulkan_image&           get_render_image() noexcept;


private:
    /**
     * @brief 三段式回读 render_output，并启动后台线程做 OCIO 变换与写盘。
     * @param _waited_info 产生当前图像内容的提交 token，回读链要先等它。
     * @return 回读末条命令的 token；合成必须把它纳入依赖。
     * @throws std::runtime_error 输出格式既不是 16 位浮点也不是 8 位无符号整数时抛出。
     * @throws std::runtime_error download_image 的缓冲创建或命令提交失败时抛出。
     * @note 后台线程只读 staging 缓冲；失败时把异常存进 save_error，由下一次 update 在主线程重抛。
     * @note 新线程先构造，随后 jthread 移动赋值 join 旧作业；旧任务不响应 stop_token，主线程可能等待其全部完成。
     */
    [[nodiscard]] vulkan_submission_token capture_frame(vulkan_submission_token _waited_info);

private:
    /** @brief 场景脏标志：任一 manager 可能变化或需要重建渲染器描述符时置位，update 成功后清除。 */
    bool is_dirty               = true;
    /** @brief 渲染器自身图像被换掉或切换后端时置位：即使四个 manager 都没变也必须重写描述符。 */
    bool need_descriptor_update = true;

    /** @brief 应用对象引用，提供 Vulkan 对象、队列、分配器与 wait_idle_for_cleanup。 */
    vulkan_application& app;

    /** @brief 场景环境状态；image 是引用边，强度每帧直接进入 render 的 GPU 数据。 */
    scene_environment_data environment;

    /** @brief 场景命令与资源的所有者队列，命令池与 SSBO 都挂在它上面。 */
    vulkan_queue& graphic_queue;
    /** @brief 上传专用队列；与 graphic_queue 可能是同一个 VkQueue 的别名。 */
    vulkan_queue& transfer_queue;

    /** @brief 场景侧延迟回收站，已登记两条队列，按 token 判定释放。 */
    vulkan_recycle_bin                recycle_bin;
    /** @brief 每帧槽一条主命令缓冲，构造时按 MAX_FRAMES_IN_FLIGHT 分配。 */
    std::vector<vulkan_commandbuffer> commandbuffers;

    /** @brief 当前输出宽度，resize 与切渲染模式时传给渲染器。 */
    uint32_t                             width         = 0;
    /** @brief 当前输出高度，resize 与切渲染模式时传给渲染器。 */
    uint32_t                             height        = 0;
    /** @brief 本帧待等待的提交 token（各 manager 的上传），render 时转成命令缓冲依赖并清空。 */
    std::vector<vulkan_submission_token>        waited_infos;

    /** @brief 灯光管理器：句柄表 + 灯光 SSBO。 */
    scene_light_manager    light_manager;
    /** @brief 贴图管理器；必须先于材质与模型析构，因为后两者的引用边指向它。 */
    scene_texture_manager  texture_manager;   // must before material
    /** @brief 材质管理器；必须先于模型管理器析构，因为模型 SSBO 引用材质 binding。 */
    scene_material_manager material_manager;  // must before model
    /** @brief 模型管理器：网格/模型句柄表、实例缓冲与 TLAS。 */
    scene_model_manager    model_manager;

    /** @brief 当前相机，渲染时整体交给活跃渲染器。 */
    scene_camera active_camera;

    /** @brief 场景输出格式：固定 RGBA16F，截图据此选择 EXR；改成 8 位无符号整数格式则走 PNG。 */
    static constexpr vk::Format color_format = vk::Format::eR16G16B16A16Sfloat;
    /** @brief 场景渲染输出图像，同时是 UI 合成与截图回读的来源。 */
    vulkan_image                render_output;

    /** @brief 三种渲染模式各一个 proxy，按值持有；F5 只重建活跃者，其余保持原状。 */
    std::unordered_map<render_mode, pro::proxy<manager_render>> scene_renders;
    /** @brief 指向 scene_renders 中当前模式的视图；切模式只改指向并重建描述符。 */
    pro::proxy_view<manager_render>                             active_render = nullptr;

    /** @brief 保护 save_error 的互斥量：后台线程写、主线程在 update 开头读。 */
    std::mutex save_error_mutex;
    /** @brief 后台截图失败时保存的异常，由下一次 update 在主线程重抛。 */
    std::exception_ptr save_error;
    /** @brief 截图保存线程；移动赋值时 join 旧作业，成员析构时 join 当前作业，新线程可能已提前开始。 */
    std::jthread save_thread; // joins before error state and GPU resources die
};
