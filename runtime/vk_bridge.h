// Shared by runtime/vulkan.c and the generated runtime/gen/vulkan_thunks.c.
#pragma once
#include "win32.h"

extern unsigned vk_generation;               // bumped when the instance changes: thunks re-resolve
void (*vk_host_proc(const char *name))(void); // host function pointer for a Vulkan command (or NULL)
void vk_missing(const char *name);
