#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace wa::egl {

using Handle = uint32_t;
using Attributes = std::span<const int32_t>;

// EGL values used by the shim. The native build has no EGL SDK dependency.
inline constexpr int32_t none = 0x3038;
inline constexpr int32_t bad_parameter = 0x300c;
inline constexpr size_t max_attribute_pairs = 64;

Handle get_display(uint64_t native_display);
Handle get_current_display();
Handle get_current_context();
Handle get_current_surface(int32_t which);
bool initialize(Handle display, int32_t* major, int32_t* minor);
bool choose_config(Handle display, Attributes attributes, Handle& match);
Handle create_window_surface(Handle display, Handle config, uint64_t window, Attributes attributes);
Handle create_pbuffer_surface(Handle display, Handle config, Attributes attributes);
Handle create_context(Handle display, Handle config, Handle share, Attributes attributes);
bool make_current(Handle display, Handle draw, Handle read, Handle context);
bool swap_buffers(Handle display, Handle surface);
bool query_surface(Handle display, Handle surface, int32_t attribute, int32_t& value);
bool query_context(Handle display, Handle context, int32_t attribute, int32_t& value);
bool release_thread();
void unsupported_image(Handle display);
bool destroy_context(Handle display, Handle context);
bool destroy_surface(Handle display, Handle surface);
bool terminate(Handle display);
const char* query_string(Handle display, int32_t name);
int32_t get_error();
bool fail(int32_t error);

} // namespace wa::egl
