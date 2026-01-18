#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdlib>

#ifdef __INTELLISENSE__
#include <chrono>
#endif // __INTELLISENSE__

import std;
import vulkan_application;
import scene_manager;
import record_loader;

struct window_info
{
	bool need_resize = false;
	bool mouse_right_down = false;
	double last_mouse_x = 0.0;
	double last_mouse_y = 0.0;
	float all_mouse_x = 0.f;
	float all_mouse_y = 0.f;
	bool parse_before = false;
	bool parse_next = false;
	bool need_save = false;
};

static void resize_callback(GLFWwindow* window, int width, int height)
{
	if (width > 0 && height > 0)
	{
		static_cast<window_info*>(glfwGetWindowUserPointer(window))->need_resize = true;
	}
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	if (action == GLFW_PRESS || action == GLFW_REPEAT)
	{
		// WASD处理
		switch (key)
		{
		case GLFW_KEY_W:
		case GLFW_KEY_A:
			static_cast<window_info*>(glfwGetWindowUserPointer(window))->parse_before = true;
			break;
		case GLFW_KEY_S:
		case GLFW_KEY_D:
			static_cast<window_info*>(glfwGetWindowUserPointer(window))->parse_next = true;
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
	window_info info;

	constexpr uint32_t WIDTH = 800;
	constexpr uint32_t HEIGHT = 600;

	// init window
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	auto* window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);

	std::unique_ptr<vulkan_application> app = std::make_unique<vulkan_application>();

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
	scene->create(window, app.get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));

	chess_pieces* piece_manager = scene->get_piece_manager();
	// piece_manager->load_record(std::string(RECORDS_PATH) + "棋谱1.txt");

	// render loop
	while (!glfwWindowShouldClose(window))
	{
		while (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
			glfwWaitEvents();

		glfwPollEvents();

		if (info.parse_before)
		{
			piece_manager->parse_back();
			info.parse_before = false;
		}
		else if (info.parse_next)
		{
			piece_manager->parse_next();
			info.parse_next = false;
		}


		// write here is keep resize before render
		if (info.need_resize)
		{
			glfwGetFramebufferSize(window, &width, &height);
			scene->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			info.need_resize = false;
		}

		auto start = std::chrono::high_resolution_clock::now();

		scene->update();

		scene->render(info.need_save);

		if (info.need_save)
		{
			info.need_save = false;
		}

		auto end = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration<float>(end - start).count();
		glfwSetWindowTitle(window, std::format("Chinese Chess {} fps", 1.f / duration).c_str());
	}

	// wait and destroy
	app->wait_idle();

	scene->destroy();

	glfwDestroyWindow(window);
	glfwTerminate();

	return EXIT_SUCCESS;
}