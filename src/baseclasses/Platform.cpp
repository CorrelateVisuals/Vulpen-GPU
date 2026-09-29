#include "baseclasses/Platform.h"

#include <dlfcn.h>

#include <stdexcept>
#include <utility>

namespace VP {

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

} // namespace VP
