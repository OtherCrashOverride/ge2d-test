#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/dma-buf.h>
#include <sys/time.h>

#include "ion.h"
#include "ge2d.h"
#include "ge2d_cmd.h"


#define ALIGN(val, align)	(((val) + (align) - 1) & ~((align) - 1))


int ion_fd = -1;
int ge2d_fd = -1;
int fb0_fd = -1;


struct ion_surface
{
    int width;
    int height;
    int bits_per_pixel;
    int stride;
    int size;
    uint32_t ion_handle;;
    int share_fd;
};

ion_surface* ion_surface_create(int width, int height, int bits_per_pixel)
{
    if (ion_fd < 0)
    {
        ion_fd = open("/dev/ion", O_RDWR);
        if (ion_fd < 0)
        {
            printf("open /dev/ion failed.\n");
            abort();
        }
    }

    ion_surface* result = (ion_surface*)malloc(sizeof(*result));
    if (!result) abort();

    result->width = width;
    result->height = height;
    result->bits_per_pixel = bits_per_pixel;


    // Allocate a buffer
    result->stride = ALIGN(width * (bits_per_pixel / 8), 64);
    result->size = height * result->stride;

    ion_allocation_data allocation_data = { 0 };
    allocation_data.len = result->size;
    allocation_data.heap_id_mask = (1 << ION_HEAP_TYPE_DMA);
    allocation_data.flags = 0;

    int io = ioctl(ion_fd, ION_IOC_ALLOC, &allocation_data);
    if (io != 0)
    {
        printf("ION_IOC_ALLOC failed.\n");
        abort();
    }
    
    result->ion_handle = allocation_data.handle;


    ion_fd_data ionData = { 0 };
    ionData.handle = result->ion_handle;

    io = ioctl(ion_fd, ION_IOC_SHARE, &ionData);
    if (io != 0)
    {
        printf("ION_IOC_SHARE failed.\n");
        abort();
    }

    result->share_fd = ionData.fd;  


    return result; 
}

void ion_surface_free(ion_surface* surface)
{
    close(surface->share_fd);

    ion_handle_data ionHandleData = { 0 };
    ionHandleData.handle = surface->ion_handle;

    int io = ioctl(ion_fd, ION_IOC_FREE, &ionHandleData);
    if (io != 0)
    {
        printf("ION_IOC_FREE failed.\n");
        abort();
    }

    free(surface);
}


struct display
{
    int width;
    int height;
};

display* display_create()
{
    if (fb0_fd < 0)
    {
        fb0_fd = open("/dev/fb0", O_RDWR);
        if (fb0_fd < 0)
        {
            printf("open /dev/fb0 failed.\n");
            abort();
        }
    }


    fb_var_screeninfo var_info;
    if (ioctl(fb0_fd, FBIOGET_VSCREENINFO, &var_info) < 0)
    {
        printf("FBIOGET_VSCREENINFO failed.\n");
        abort();
    }


    display* result = (display*)malloc(sizeof(*result));
    if (!result) abort();

    result->width = var_info.xres;
    result->height = var_info.yres;


    return result;
}

void display_free(display* disp)
{
    free(disp);
}



enum Rotation
{
    Rotation_0 = 0,
    Rotation_90 = 1,
    Rotation_180 = 2,
    Rotation_270 = 3
};


int main(int argc, char *argv[])
{
    int io;
    Rotation rotation = Rotation_0;

    if (argc > 1)
    {
        int val = atoi(argv[1]);
        if (val >= 0 && val <= Rotation_270)
        {
            rotation = (Rotation)val;
        }
    }


    display* display = display_create();
    

    // Load the test image
    ion_surface* surface = ion_surface_create(600, 503 + 1, 32); // DRM_FORMAT_ARGB8888
    uint8_t* ptr = (uint8_t*)mmap(nullptr, surface->size, PROT_READ | PROT_WRITE, MAP_SHARED, surface->share_fd, 0);
    if ((void*)ptr == MAP_FAILED)
    {
        printf("mmap failed.\n");
        abort();
    }

    struct dma_buf_sync param = {0};
    param.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;

    io = ioctl(surface->share_fd, DMA_BUF_IOCTL_SYNC, &param);
    if (io < 0)
    {
        printf("DMA_BUF_IOCTL_SYNC failed\n");
        abort();
    }


    const int w = surface->width;
    const int h = surface->height;
    const int stride = surface->stride;

    int fd = open("colorwheel.raw", O_RDONLY);
    if (fd < 0) abort();

    for (int y = 0; y < h; ++y)
    {
        // convert DRM_FORMAT_ABGR8888 to DRM_FORMAT_XRGB8888
        read(fd, ptr, 600 * 4);

        for (int x = 0; x < 600; ++x)
        {
            uint32_t srcPixel = ((uint32_t*)ptr)[x];
            uint32_t dstPixel = srcPixel & 0xff000000 | 
                ((srcPixel & 0x00ff0000) >> 16) |
                (srcPixel & 0x0000ff00) |
                ((srcPixel & 0x000000ff) << 16);
            
            ((uint32_t*)ptr)[x] = dstPixel;
        }

        ptr += stride;
    }


    param.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    
    io = ioctl(surface->share_fd, DMA_BUF_IOCTL_SYNC, &param);
    if (io < 0)
    {
        printf("DMA_BUF_IOCTL_SYNC failed\n");
        abort();
    }

    close(fd);
    fd = -1;



    double elapsed = 0;
    int totalFrames = 0;
    bool isRunning = true;
    struct timeval startTime;
    struct timeval endTime;


    // blit the image
    ge2d_fd = open("/dev/ge2d", O_RDWR);
    if (ge2d_fd < 0)
    {
        printf("open /dev/ge2d failed.\n");
        abort();
    }


    while (1)
    {
        gettimeofday(&startTime, NULL);


        config_para_ex_ion_s blit_config = { 0 };


        blit_config.alu_const_color = 0xffffffff;

        blit_config.dst_para.mem_type = CANVAS_OSD0;
        blit_config.dst_para.format = GE2D_FORMAT_S32_ARGB;

        blit_config.dst_para.left = 0;
        blit_config.dst_para.top = 0;
        blit_config.dst_para.width = display->width;
        blit_config.dst_para.height = display->height;
        blit_config.dst_para.x_rev = 0;
        blit_config.dst_para.y_rev = 0;

        switch (rotation)
        {
            case Rotation_0:
                break;

            case Rotation_90:
                blit_config.dst_xy_swap = 1;
                blit_config.dst_para.x_rev = 1;
                break;

            case Rotation_180:
                blit_config.dst_para.x_rev = 1;
                blit_config.dst_para.y_rev = 1;
                break;

            case Rotation_270:
                blit_config.dst_xy_swap = 1;
                blit_config.dst_para.y_rev = 1;
                break;
                
            default:
                break;
        }


        blit_config.src_para.mem_type = CANVAS_ALLOC;
        blit_config.src_para.format = GE2D_FORMAT_S32_ARGB;

        blit_config.src_para.left = 0;
        blit_config.src_para.top = 0;
        blit_config.src_para.width = surface->width;
        blit_config.src_para.height = surface->height;

        blit_config.src_planes[0].shared_fd = surface->share_fd;
        blit_config.src_planes[0].w = surface->stride / (surface->bits_per_pixel / 8);
        blit_config.src_planes[0].h = surface->height;


        io = ioctl(ge2d_fd, GE2D_CONFIG_EX_ION, &blit_config);
        if (io < 0)
        {
            printf("GE2D_CONFIG failed\n");
            abort();
        }


        ge2d_para_s blitRect = { 0 };

        blitRect.src1_rect.x = 0;
        blitRect.src1_rect.y = 0;
        blitRect.src1_rect.w = surface->width;
        blitRect.src1_rect.h = surface->height;

        blitRect.dst_rect.x = 0;
        blitRect.dst_rect.y = 0;
        blitRect.dst_rect.w = display->width;
        blitRect.dst_rect.h = display->height;

        //io = ioctl(ge2d_fd, GE2D_STRETCHBLIT_NOALPHA, &blitRect);
        io = ioctl(ge2d_fd, GE2D_STRETCHBLIT, &blitRect);
        if (io < 0)
        {
            printf("GE2D_BLIT_NOALPHA failed.\n");
            abort();
        }


        gettimeofday(&endTime, NULL);
        ++totalFrames;

        double seconds = (endTime.tv_sec - startTime.tv_sec);
	    double milliseconds = ((double)(endTime.tv_usec - startTime.tv_usec)) / 1000000.0;

        //printf("FRAME: elapsed=%f\n", seconds + milliseconds);

        elapsed += seconds + milliseconds;

        if (elapsed >= 1.0)
        {
            int fps = (int)(totalFrames / elapsed);
            printf("Frames=%d in %f seconds (FPS: %d)\n", totalFrames, elapsed, fps);

            totalFrames = 0;
            elapsed = 0;

            break;
        }
    }

    return EXIT_SUCCESS;
}