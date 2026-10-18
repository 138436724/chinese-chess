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

struct ui_viewport_renderer::implementation
{
    struct window_state
    {
        vulkan_swapchain swapchain;
        std::vector<vulkan_commandbuffer> commands;
        VkPipeline pipeline = VK_NULL_HANDLE; // owned by the upstream backend
        bool resize = false;
        ~window_state() { swapchain.shutdown(); }
    };
    inline static thread_local implementation* active = nullptr;
    vulkan_application& app;
    std::unordered_map<ImGuiID, std::unique_ptr<window_state>> windows;
    std::exception_ptr error;
    void (*original_create)(ImGuiViewport*);
    void (*original_destroy)(ImGuiViewport*);
    void (*original_resize)(ImGuiViewport*, ImVec2);
    void (*original_render)(ImGuiViewport*, void*);
    void (*original_present)(ImGuiViewport*, void*);

    template<class F> void invoke(F&& operation) noexcept
    {
        try { operation(); }
        catch (...) { if (!error) error = std::current_exception(); }
    }

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

    void destroy(ImGuiViewport* vp)
    {
        if (auto found = windows.find(vp->ID); found != windows.end())
        {
            found->second->swapchain.shutdown();
            windows.erase(found); // fences complete before upstream frees VBO/IBO
        }
        original_destroy(vp);
    }

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

    void present(ImGuiViewport* vp)
    {
        auto found = windows.find(vp->ID);
        if (found == windows.end() || !found->second->swapchain.frame_active()) return;
        const auto result = found->second->swapchain.present_image();
        if (result.status == vulkan_frame_status::recreate) found->second->resize = true;
        if (result.status == vulkan_frame_status::fatal) throw std::runtime_error("ImGui viewport present failed.");
    }
};

ui_viewport_renderer::ui_viewport_renderer(vulkan_application& app) : impl(std::make_unique<implementation>(app)) {}
ui_viewport_renderer::~ui_viewport_renderer() = default;
void ui_viewport_renderer::check_errors() { if (impl->error) std::rethrow_exception(impl->error); }
