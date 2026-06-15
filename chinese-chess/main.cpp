#define GLFW_INCLUDE_VULKAN

#include "scene/scene_manager.h"
#include "ui/ui_record.h"
#include "vulkan_core/vulkan_application.h"
#include <GLFW/glfw3.h>

struct window_info
{
	bool need_resize = false;
	bool mouse_right_down = false;
	double last_mouse_x = 0.0;
	double last_mouse_y = 0.0;
	float all_mouse_x = 0.f;
	float all_mouse_y = 0.f;
	bool load_previous_record = false;
	bool load_next_record = false;
	bool need_save = false;
};

static void resize_callback(GLFWwindow* window, int width, int height)
{
	if (width > 0 && height > 0)
	{
		static_cast<window_info*>(glfwGetWindowUserPointer(window))->need_resize = true;
	}
}

static void key_callback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/)
{
	if (action == GLFW_PRESS || action == GLFW_REPEAT)
	{
		// WASD处理
		switch (key)
		{
		case GLFW_KEY_W:
		case GLFW_KEY_A:
			static_cast<window_info*>(glfwGetWindowUserPointer(window))->load_previous_record = true;
			break;
		case GLFW_KEY_S:
		case GLFW_KEY_D:
			static_cast<window_info*>(glfwGetWindowUserPointer(window))->load_next_record = true;
			break;
		case GLFW_KEY_C:
			static_cast<window_info*>(glfwGetWindowUserPointer(window))->need_save = true;
			break;
		default:
			break;
		}
	}
}

int main()
{
	constexpr uint32_t WIDTH = 800;
	constexpr uint32_t HEIGHT = 600;

	// init window
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	auto* window = glfwCreateWindow(WIDTH, HEIGHT, "Chinese Chess", nullptr, nullptr);

	std::unique_ptr<vulkan_application> app = std::make_unique<vulkan_application>();

	window_info info;
	glfwSetWindowUserPointer(window, &info);

	glfwSetFramebufferSizeCallback(window, resize_callback);

	//glfwSetCursorPosCallback(window, cursorPositionCallback);
	//glfwSetMouseButtonCallback(window, mouseButtonCallback);

	glfwSetKeyCallback(window, key_callback);


	// init application
	uint32_t count = 0;
	auto glfw_extensions = glfwGetRequiredInstanceExtensions(&count);
	std::vector<const char*> extensions(glfw_extensions, glfw_extensions + count);
	app->init({}, extensions);

	VkSurfaceKHR _surface;
	if (glfwCreateWindowSurface(*(app->get_instance()), window, nullptr, &_surface))
	{
		throw std::runtime_error("failed to create window surface!");
	}

	int width, height;
	glfwGetFramebufferSize(window, &width, &height);
	app->create(_surface, true, true, static_cast<uint32_t>(width), static_cast<uint32_t>(height));


	// create scene manager
	std::unique_ptr<scene_manager> scene = std::make_unique<scene_manager>();
	scene->create(app.get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));


	// create ui
	std::unique_ptr<ui_record> UI = std::make_unique<ui_record>();
	UI->create(window, app.get(), scene.get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));


	// bind
	app->bind_image(&scene->get_render_image(), &UI->get_render_image());


	// render loop
	while (!glfwWindowShouldClose(window))
	{
		while (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
		{
			glfwWaitEvents();
		}

		glfwPollEvents();

		if (info.load_previous_record)
		{
			UI->load_previous();
			info.load_previous_record = false;
		}
		else if (info.load_next_record)
		{
			UI->load_next();
			info.load_next_record = false;
		}


		// write here is keep resize before render
		if (info.need_resize)
		{
			glfwGetFramebufferSize(window, &width, &height);

			app->wait();
			app->resize(width, height);

			scene->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			UI->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

			app->bind_image(&scene->get_render_image(), &UI->get_render_image());

			info.need_resize = false;
		}

		auto start = std::chrono::high_resolution_clock::now();

		app->begin();

		UI->update();
		scene->update();

		const auto& cb1 = UI->render();
		const auto& cb2 = scene->render();

		std::vector<vk::CommandBuffer> cbs = { *cb1,*cb2 };
		app->render(cbs);

		app->end(info.need_save);

		if (info.need_save)
		{
			app->save_image(scene->get_render_image());
			info.need_save = false;
		}

		auto end = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration<float>(end - start).count();
		glfwSetWindowTitle(window, std::format("Chinese Chess {} fps", 1.f / duration).c_str());
	}


	// wait and destroy
	app->wait();

	scene->destroy();
	UI->destroy();

	glfwDestroyWindow(window);
	glfwTerminate();

	return EXIT_SUCCESS;
}