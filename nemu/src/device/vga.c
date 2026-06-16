#include "common.h"

#ifdef HAS_IOE

#include "device/mmio.h"
#include <SDL2/SDL.h>

// VGA 显存映射到 guest 物理地址 0x40000，AM 的 fb 也指向这里。
#define VMEM 0x40000

#define SCREEN_H 300
#define SCREEN_W 400

// SDL 窗口、渲染器和纹理分别负责显示窗口、绘制操作和保存像素数据。
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

// vmem 指向 NEMU 分配的显存区域，按 SCREEN_W 宽度组织成二维像素数组。
static uint32_t (*vmem) [SCREEN_W];

// 显存读写回调。当前实现中 guest 直接写 vmem，刷新时统一拷贝到 SDL 纹理。
void vga_vmem_io_handler(paddr_t addr, int len, bool is_write) {
}

// 将显存中的像素更新到 SDL 纹理，并显示到窗口中。
void update_screen() {
  SDL_UpdateTexture(texture, NULL, vmem, SCREEN_W * sizeof(vmem[0][0]));
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);
}

// 初始化 VGA 设备，创建 SDL 窗口，并注册显存的 MMIO 映射。
void init_vga() {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_CreateWindowAndRenderer(SCREEN_W * 2, SCREEN_H * 2, 0, &window, &renderer);
  SDL_SetWindowTitle(window, "NEMU");
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_STATIC, SCREEN_W, SCREEN_H);

  vmem = add_mmio_map(VMEM, 0x80000, vga_vmem_io_handler);
}
#endif	/* HAS_IOE */
