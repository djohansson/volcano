#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool verbose);
bool vkapp_is_initialized();
void vkapp_draw();
void vkapp_resize(int framebufferWidth, int framebufferHeight);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
