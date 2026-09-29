#include "baseclasses/Platform.h"

#include <GLFW/glfw3.h>
#include <dlfcn.h>

#include <format>
#include <stdexcept>
#include <utility>

namespace VP {

namespace {

[[noreturn]] void glfw_failed(const char *call) {
  const char *description = nullptr;
  glfwGetError(&description);
  throw std::runtime_error(
      std::format("{} failed: {}", call, description ? description : "no reason given"));
}

} // namespace

std::filesystem::path Files::executable() {
  return std::filesystem::read_symlink("/proc/self/exe");
}

Library::Library(const std::filesystem::path &file)
    : _handle(dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL)) {
  if (!_handle)
    throw std::runtime_error(dlerror());
}

Library::Library(Library &&other) noexcept
    : _handle(std::exchange(other._handle, nullptr)) {}

Library &Library::operator=(Library &&other) noexcept {
  std::swap(_handle, other._handle);
  return *this;
}

Library::~Library() {
  if (_handle)
    dlclose(_handle);
}

void *Library::symbol(const char *name) const {
  return dlsym(_handle, name);
}

bool Library::mapped(const std::filesystem::path &file) {
  void *const handle = dlopen(file.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (handle)
    dlclose(handle);
  return handle != nullptr;
}

// GLFW keeps process-wide state, so the one Window owns its init and termination.
Window::Window(const std::string &title, VkExtent2D size) {
  // GLFW then finds Vulkan through the loader vulpen links, not a second one.
  glfwInitVulkanLoader(vkGetInstanceProcAddr);
  if (!glfwInit())
    glfw_failed("glfwInit");
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  _window = glfwCreateWindow(static_cast<int>(size.width),
                             static_cast<int>(size.height),
                             title.c_str(),
                             nullptr,
                             nullptr);
  if (!_window) {
    glfwTerminate();
    glfw_failed("glfwCreateWindow");
  }
}

Window::~Window() {
  glfwDestroyWindow(_window);
  glfwTerminate();
}

bool Window::poll() const {
  glfwPollEvents();
  return !glfwWindowShouldClose(_window);
}

VkExtent2D Window::size() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(_window, &width, &height);
  return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

std::span<const char *const> Window::vulkan_extensions() const {
  std::uint32_t count = 0;
  const char **const names = glfwGetRequiredInstanceExtensions(&count);
  if (!names)
    glfw_failed("glfwGetRequiredInstanceExtensions");
  return {names, count};
}

VkSurfaceKHR Window::surface(VkInstance instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(instance, _window, nullptr, &surface) != VK_SUCCESS)
    glfw_failed("glfwCreateWindowSurface");
  return surface;
}

} // namespace VP
