# Dear ImGui Vulkan backend

Source: Dear ImGui **v1.92.9b-docking**, `backends/imgui_impl_vulkan.{cpp,h}`,
MIT license retained in `LICENSE.txt`. ImGui core and GLFW backend remain supplied
by the pinned vcpkg dependency. These files are compiled into the executable;
the Vulkan backend object in the static vcpkg library is therefore not selected.

The only implementation addition is
`ImGui_ImplVulkan_CreateViewportRenderBuffers`, declared in
`chinese-chess/ui/imgui_viewport_bridge.h`. It initializes the private per-viewport
VBO/IBO owner with `WindowOwned=false` and creates the standard single-sample
viewport pipeline independently of the main UI's MSAA pipeline. The application then owns all secondary
WSI creation, acquire, submit, present, resize and fence-based destruction.
Upstream `Renderer_DestroyWindow` frees its buffers after our scheduler drains.

This avoids upstream's initial transition of every swapchain image before
acquire, as well as its unfenced presentation resource recycling. No upstream
WSI helper or device-idle wait runs in the application's viewport callbacks.
The draw-data, texture, font, pipeline and buffer implementations are unchanged.

When upgrading ImGui, update this source and header together, reapply the small
bridge, review the private owner and destructor, update the version assertion,
then check the ordinary application build and review viewport lifetime behavior.
Project tests and test hooks were removed at the user's request; do not restore
them until explicitly requested. Never silently remove the version assertion or
suppress WSI validation messages.
