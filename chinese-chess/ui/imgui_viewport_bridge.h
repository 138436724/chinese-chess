/**
 * @file imgui_viewport_bridge.h
 * @brief 只暴露上游 Vulkan backend 的副窗口渲染缓冲所有者初始化入口。
 * @details 上游 backend 的 ImGui_ImplVulkan_CreateWindow 同时建 surface/swapchain 与渲染缓冲；
 *          本仓库要自己接管 WSI，因此直接从 backend 源码导出这个最小入口：它分配
 *          ImGui_ImplVulkan_ViewportData（RendererUserData），并按副窗口颜色格式建共享管线。
 *          顶点/索引缓冲仍由上游 ImGui_ImplVulkan_RenderDrawData 在首次绘制时惰性创建。
 */
#pragma once

#include <imgui.h>
#include <vulkan/vulkan.h>

// Initializes only upstream's private VBO/IBO owner. The application owns WSI.
/**
 * @brief 为指定平台视口分配上游的 ViewportData，并创建/复用一个副窗口绘制管线。
 * @param viewport ImGui 平台视口；RendererUserData 必须为空，且 Vulkan backend 已初始化。
 * @param format   首次创建共享管线时使用的颜色格式；后续调用必须与该管线兼容。
 * @return 上游 backend 持有的共享管线句柄；创建失败且错误回调返回时可能为空。
 * @note 管线创建成功后，同一 backend 生命周期内的后续调用复用它，不根据 format 重建。
 * @note 调用方负责 WSI；本函数不创建 surface、swapchain 或顶点/索引缓冲。
 * @note 与上游 backend 的 ImGui 版本绑定，升级 Dear ImGui 时必须复核该私有入口。
 */
VkPipeline ImGui_ImplVulkan_CreateViewportRenderBuffers(ImGuiViewport* viewport, VkFormat format);
