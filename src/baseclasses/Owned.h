#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace VP {

// "And I give unto them eternal life; and they shall never perish, neither shall any man
// pluck them out of my hand." John 10:28

// A Vulkan object, destroyed by destroy, Vulkan's function for its kind, as
// vkDestroyQueryPool, with the parent that made it, a device or the instance; the
// instance and a device have none. Its one owner holds it as a member (A01), so it goes
// when the owner does, after the members declared after it, and a constructor that
// throws partway destroys what it made (CPP09). Moving hands it on: the one moved from
// then holds what the other held, and destroys that.
template <class Handle, auto destroy, class Parent = std::nullptr_t> class Owned {
public:
  Owned() = default;
  explicit Owned(Handle handle) : _handle(handle) {}
  Owned(Parent parent, Handle handle) : _parent(parent), _handle(handle) {}
  Owned(Owned &&other) noexcept
      : _parent(other._parent), _handle(std::exchange(other._handle, Handle{})) {}
  Owned &operator=(Owned &&other) noexcept {
    std::swap(_parent, other._parent);
    std::swap(_handle, other._handle);
    return *this;
  }
  Owned(const Owned &) = delete;
  Owned &operator=(const Owned &) = delete;
  ~Owned() {
    if (_handle == Handle{})
      return;
    if constexpr (std::is_same_v<Parent, std::nullptr_t>)
      destroy(_handle, nullptr);
    else
      destroy(_parent, _handle, nullptr);
  }

  operator Handle() const {
    return _handle;
  }

private:
  Parent _parent{};
  Handle _handle{};
};

} // namespace VP
