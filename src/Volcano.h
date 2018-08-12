#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

int vkapp_create(void* view, int width, int height, const char* resourcePath);
void vkapp_draw(unsigned int frameIndex);
void vkapp_destroy(void);

#ifdef __cplusplus
}
#endif
