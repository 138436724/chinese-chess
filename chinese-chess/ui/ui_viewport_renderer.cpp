/**
 * @file ui_viewport_renderer.cpp
 * @brief ui_viewport_renderer 的实现：替换 ImGui 平台回调并接管副窗口的 WSI 与提交。
 * @details 构造保存上游五个 Renderer_* 回调，换成经 thread_local active 分发的 lambda；
 *          create 为每个 ImGuiID 建 surface、swapchain、管线与命令缓冲，render 录制并提交，
 *          present 呈现。回调不允许抛异常，invoke 把第一个异常存进 error 供 check_errors 重抛。
 */
#include "ui_viewport_renderer.h"
#include "imgui_viewport_bridge.h"

#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_common.h"

#include <GLFW/glfw3.h>
#include <array>
#include <exception>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

/**
 * @brief 渲染器的全部实现状态：应用引用、每窗口 WSI、原回调与被捕获的异常。
 * @note active 是 thread_local，保证同一线程只有一个实例接管平台回调。
 */
struct ui_viewport_renderer::implementation
{
    /**
     * @brief 一个副窗口的 WSI 状态：独立交换链、每帧槽命令缓冲与上游管线。
     * @note pipeline 由上游 backend 持有，这里只借句柄；析构先 shutdown 交换链等待 fence。
     */
    struct window_state
    {
        /** @brief 该副窗口独立的交换链；析构时 shutdown 会等待 present fence。 */
        vulkan_swapchain swapchain;
        /** @brief 每帧槽一条命令缓冲，下标来自 acquire 返回的 frame.slot。 */
        std::vector<vulkan_commandbuffer> commands;
        /** @brief 上游 backend 持有的副窗口管线，本类只借句柄。 */
        VkPipeline pipeline = VK_NULL_HANDLE; // owned by the upstream backend
        /** @brief 尺寸变化置位，下一次 render 先 recreate 交换链。 */
        bool resize = false;
        ~window_state() { swapchain.shutdown(); }
    };
    /** @brief 当前线程的活动实例，C 回调经它找到对象；同一线程只允许一个。 */
    inline static thread_local implementation* active = nullptr;
    /** @brief 提供实例、设备、队列与主交换链格式的应用对象。 */
    vulkan_application& app;
    /** @brief ImGuiID 到副窗口 WSI 状态的映射；窗口销毁时对应项被 erase。 */
    std::unordered_map<ImGuiID, std::unique_ptr<window_state>> windows;
    /** @brief 平台回调里捕获的第一个异常，由 check_errors 重抛。 */
    std::exception_ptr error;
    /** @brief 上游 Renderer_CreateWindow 的原实现，析构时还回。 */
    void (*original_create)(ImGuiViewport*);
    /** @brief 上游 Renderer_DestroyWindow 的原实现，析构时还回。 */
    void (*original_destroy)(ImGuiViewport*);
    /** @brief 上游 Renderer_SetWindowSize 的原实现，析构时还回。 */
    void (*original_resize)(ImGuiViewport*, ImVec2);
    /** @brief 上游 Renderer_RenderWindow 的原实现，析构时还回。 */
    void (*original_render)(ImGuiViewport*, void*);
    /** @brief 上游 Renderer_SwapBuffers 的原实现，析构时还回。 */
    void (*original_present)(ImGuiViewport*, void*);

    /** @brief 执行回调体并捕获第一个异常；C 回调不能向外抛，错误留给 check_errors。 */
    template<class F> void invoke(F&& operation) noexcept
    {
        try { operation(); }
        catch (...) { if (!error) error = std::current_exception(); }
    }

    /**
     * @brief 保存上游回调并安装五个经 active 分发的 lambda。
     * @throws std::logic_error 当前线程已有活动实例时抛出，避免互相覆盖回调。
     */
    explicit implementation(vulkan_application& application) : app(application)
    {
        if (active) throw std::logic_error("Only one ImGui renderer context may be active on this thread.");
        auto& io = ImGui::GetPlatformIO();
        original_create = io.Renderer_CreateWindow;
        original_destroy = io.Renderer_DestroyWindow;
        original_resize = io.Renderer_SetWindowSize;
        original_render = io.Renderer_RenderWindow;
        original_present = io.Renderer_SwapBuffers;
        active = this;
        io.Renderer_CreateWindow = [](ImGuiViewport* vp) { active->invoke([&] { active->create(vp); }); };
        io.Renderer_DestroyWindow = [](ImGuiViewport* vp) { active->invoke([&] { active->destroy(vp); }); };
        io.Renderer_SetWindowSize = [](ImGuiViewport* vp, ImVec2) {
            active->invoke([&] { if (auto found = active->windows.find(vp->ID); found != active->windows.end()) found->second->resize = true; });
        };
        io.Renderer_RenderWindow = [](ImGuiViewport* vp, void*) { active->invoke([&] { if (!active->error) active->render(vp); }); };
        io.Renderer_SwapBuffers = [](ImGuiViewport* vp, void*) { active->invoke([&] { if (!active->error) active->present(vp); }); };
    }

    /** @brief 恢复上游回调并清空 active；不负责销毁仍存在的平台窗口。 */
    ~implementation()
    {
        auto& io = ImGui::GetPlatformIO();
        io.Renderer_CreateWindow = original_create;
        io.Renderer_DestroyWindow = original_destroy;
        io.Renderer_SetWindowSize = original_resize;
        io.Renderer_RenderWindow = original_render;
        io.Renderer_SwapBuffers = original_present;
        active = nullptr;
    }

    /**
     * @brief 为一个副窗口建 surface、交换链、上游管线与每帧槽命令缓冲。
     * @throws std::runtime_error surface 创建失败、颜色格式与主 UI 不一致或管线创建失败时抛出。
     * @note 颜色格式必须与主交换链一致；局部 WSI 对象由 RAII 回收；桥接入口设置的 RendererUserData 仍须由上游 destroy 回调释放。
     */
    void create(ImGuiViewport* vp)
    {
        VkSurfaceKHR native_surface = VK_NULL_HANDLE;
        const auto result = glfwCreateWindowSurface(*app.get_instance(), static_cast<GLFWwindow*>(vp->PlatformHandle), nullptr, &native_surface);
        if (result != VK_SUCCESS) throw std::runtime_error("ImGui viewport surface creation failed.");
        vk::raii::SurfaceKHR surface(app.get_instance(), native_surface);
        auto state = std::make_unique<window_state>();
        const auto& physical = *app.get_physical_device();
        auto present_index = app.get_graphic_queue().get_index();
        if (!physical.getSurfaceSupportKHR(present_index, *surface))
            present_index = app.get_swapchain().get_present_queue().get_index();
        int width, height;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(vp->PlatformHandle), &width, &height);
        state->swapchain.create(app.get_instance(), physical, *app.get_device(), surface.release(),
            app.get_graphic_queue(), app.get_swapchain().get_present_queue(), present_index,
            static_cast<uint32_t>(width > 0 ? width : 1), static_cast<uint32_t>(height > 0 ? height : 1));
        if (state->swapchain.get_format() != app.get_swapchain().get_format())
            throw std::runtime_error("ImGui platform surface must support the main UI color format.");
        state->pipeline = ImGui_ImplVulkan_CreateViewportRenderBuffers(vp, static_cast<VkFormat>(state->swapchain.get_format()));
        if (!state->pipeline) throw std::runtime_error("ImGui viewport pipeline creation failed.");
        state->commands = vulkan_commandbuffer::create(*app.get_device(),
            vk::CommandBufferAllocateInfo(app.get_graphic_queue().get_command_pool(), vk::CommandBufferLevel::ePrimary,
                vulkan_common::MAX_FRAMES_IN_FLIGHT), &app.get_graphic_queue());
        windows.emplace(vp->ID, std::move(state));
    }

    /**
     * @brief 先 shutdown 本窗口交换链（等待 fence），再交给上游回调释放 VBO/IBO 与窗口数据。
     * @note 先 erase 再 original_destroy，保证上游释放缓冲前 GPU 已不再使用。
     */
    void destroy(ImGuiViewport* vp)
    {
        if (auto found = windows.find(vp->ID); found != windows.end())
        {
            found->second->swapchain.shutdown();
            windows.erase(found); // fences complete before upstream frees VBO/IBO
        }
        original_destroy(vp);
    }

    /**
     * @brief 重录并提交一个副窗口：按需 recreate、acquire、画 ImGui draw data、转 PRESENT_SRC。
     * @throws std::runtime_error acquire 返回 fatal 时抛出。
     * @note 命令录制或提交异常保留原类型；清理时若 abandon_frame 再失败，其异常被忽略。
     * @note 最小化或零尺寸直接跳过；acquire 成功后的异常路径尝试 abandon_frame，再重抛原异常，不保证取消必定成功。
     */
    void render(ImGuiViewport* vp)
    {
        auto found = windows.find(vp->ID);
        if (found == windows.end()) return;
        auto& state = *found->second;
        int width, height;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(vp->PlatformHandle), &width, &height);
        if (width <= 0 || height <= 0 || glfwGetWindowAttrib(static_cast<GLFWwindow*>(vp->PlatformHandle), GLFW_ICONIFIED)) return;
        if (state.resize)
        {
            if (!state.swapchain.recreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) return;
            state.resize = false;
        }
        const auto acquired = state.swapchain.acquire_image();
        if (acquired.status == vulkan_frame_status::recreate) { state.resize = true; return; }
        if (acquired.status == vulkan_frame_status::skip) return;
        if (acquired.status == vulkan_frame_status::fatal) throw std::runtime_error("ImGui viewport acquire failed.");
        try
        {
            auto& cb = state.commands.at(acquired.frame.slot);
            cb.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
            cb.add_waited_info(state.swapchain.get_waited_info());
            cb.add_signal_info(state.swapchain.get_signal_info());
            const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
            const vk::ImageMemoryBarrier2 before(vk::PipelineStageFlagBits2::eColorAttachmentOutput, {},
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, state.swapchain.get_current_image(), range);
            (*cb).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, before));
            const vk::RenderingAttachmentInfo attachment(state.swapchain.get_current_imageview(), vk::ImageLayout::eColorAttachmentOptimal,
                vk::ResolveModeFlagBits::eNone, {}, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));
            (*cb).beginRendering(vk::RenderingInfo({}, vk::Rect2D({}, state.swapchain.get_extent()), 1, {}, attachment));
            {
                const std::lock_guard lock(app.get_graphic_queue().get_submission_state()->mutex);
                ImGui_ImplVulkan_RenderDrawData(vp->DrawData, *(*cb), state.pipeline);
            }
            (*cb).endRendering();
            const vk::ImageMemoryBarrier2 after(vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                {}, {}, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, state.swapchain.get_current_image(), range);
            (*cb).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, after));
            cb.end_record();
            state.swapchain.submitted(cb.submit());
        }
        catch (...)
        {
            const auto failure = std::current_exception();
            try { state.swapchain.abandon_frame(); } catch (...) {}
            std::rethrow_exception(failure);
        }
    }

    /**
     * @brief 呈现当前副窗口 image；recreate 时只置 resize 标志，留到下次 render 处理。
     * @throws std::runtime_error present 返回 fatal 时抛出。
     */
    void present(ImGuiViewport* vp)
    {
        auto found = windows.find(vp->ID);
        if (found == windows.end() || !found->second->swapchain.frame_active()) return;
        const auto result = found->second->swapchain.present_image();
        if (result.status == vulkan_frame_status::recreate) found->second->resize = true;
        if (result.status == vulkan_frame_status::fatal) throw std::runtime_error("ImGui viewport present failed.");
    }
};

/** @brief 构造 pimpl；线程重入等逻辑错误由 implementation 的构造函数抛出。 */
ui_viewport_renderer::ui_viewport_renderer(vulkan_application& app) : impl(std::make_unique<implementation>(app)) {}
/** @brief 默认析构 pimpl，implementation 析构时把 ImGui 平台回调恢复成上游实现。 */
ui_viewport_renderer::~ui_viewport_renderer() = default;
/** @brief 有缓存异常就重抛；ui_manager 在 UpdatePlatformWindows 与平台窗口渲染之后各调一次。 */
void ui_viewport_renderer::check_errors() { if (impl->error) std::rethrow_exception(impl->error); }
