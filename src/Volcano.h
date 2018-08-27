#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

int vkapp_create(void* view, int width, int height, const char* resourcePath, bool verbose);
bool vkapp_is_initialized();
void vkapp_draw();
void vkapp_resize(int width, int height);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
