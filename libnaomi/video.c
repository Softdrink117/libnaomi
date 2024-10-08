#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "naomi/video.h"
#include "naomi/system.h"
#include "naomi/dimmcomms.h"
#include "naomi/eeprom.h"
#include "naomi/maple.h"
#include "naomi/console.h"
#include "naomi/interrupt.h"
#include "naomi/thread.h"
#include "naomi/ta.h"
#include "video-internal.h"
#include "irqinternal.h"
#include "holly.h"
#include "font.h"

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

// The size of the VRAM scratch area that can be used by anyone, it is effectively
// the 3rd double-buffer location.
#define GLOBAL_BUFFER_SCRATCH_SIZE (1024 * 128)

// Default video modes
// 31kHz default - 480p (640x480, 60Hz, progressive, 524 lines)
static video_mode_t highres_mode = {
    .width = 640,
    .height = 480,
    .h_pos = 166,
    .v_pos = 35,
    .interlaced = 0,
    .linedouble = 0,
    .pixeldouble = 0,
    .pixel_clock_double = 1,
    .hblank_start = 0x345,
    .hblank_end = 0x7E,
    .vblank_int_start = 480 + 40,
    .vblank_int_end = 40,
    .vblank_start = 480 + 40,
    .vblank_end = 40,
    .hsync = 857,
    .vsync = 524
};
// 15kHz default - 480i (640x480, 60Hz, interlaced, 536 lines)
static video_mode_t lowres_mode = {
    .width = 640,
    .height = 480,
    .h_pos = 164,
    .v_pos = 22,
    .interlaced = 1,
    .linedouble = 0,
    .pixeldouble = 0,
    .pixel_clock_double = 0,
    .hblank_start = 0x345,
    .hblank_end = 0x7E,
    .vblank_int_start = (480 + 40) / 2,
    .vblank_int_end = 40,
    .vblank_start = 480 + 40,
    .vblank_end = 40,
    .hsync = 851,
    .vsync = 536
};

// The current video mode in use
static video_mode_t active_mode;
// Dither override control and pre-calculated fb_render_cfg
uint8_t dither_enable = 1;
uint32_t render_cfg = (RENDER_CFG_RGB0888 << 0);

// Static members that don't need to be accessed anywhere else.
static uint32_t global_background_color = 0;
static uint32_t global_background_fill_start = 0;
static uint32_t global_background_fill_end = 0;
static uint32_t global_background_fill_color[8] = { 0 };
static unsigned int global_background_set = 0;
static unsigned int global_video_15khz = 0;

// We only use two of these for rendering. The third is so we can
// give a pointer out to scratch VRAM for other code to use.
// The chunk between global_buffer_offset[2] and the next megabyte
// boundary is "free" to use, but in practice gets used for system
// textures. So this is mostly for code that doesn't use the TA/PVR
// to render and unit tests.
uint32_t global_buffer_offset[3] = { 0, 0, 0 };

// Remember HBLANK/VBLANK set up by BIOS in case we need to return there.
static uint32_t saved_hvint = 0;
static uint32_t initialized = 0;

// Nonstatic so that other video modules can use them as well.
unsigned int global_video_width = 0;
unsigned int global_video_height = 0;
unsigned int cached_actual_width = 0;
unsigned int cached_actual_height = 0;
unsigned int global_video_depth = 0;
unsigned int global_video_vertical = 0;
void *buffer_base = 0;

// Our current framebuffer location, for double buffering. The current_buffer_loc
// is the one currently displayed to the screen, and the next_buffer_loc is the
// one we are drawing on and will flip to on the next vblank.
unsigned int buffer_loc = 0;
#define current_buffer_loc ((buffer_loc) ? 1 : 0)
#define next_buffer_loc ((buffer_loc) ? 0 : 1)

// Defines in thread.c which help us to handle vblank interrupts.
void _thread_wait_vblank_swapbuffers();

void _video_swap_vbuffers()
{
    volatile unsigned int *videobase = (volatile unsigned int *)POWERVR2_BASE;

    // Swap buffers in HW.
    videobase[POWERVR2_FB_DISPLAY_ADDR_1] = global_buffer_offset[current_buffer_loc];
    videobase[POWERVR2_FB_DISPLAY_ADDR_2] = global_buffer_offset[current_buffer_loc] + (global_video_width * global_video_depth);

    // Swap buffer pointer in SW.
    buffer_loc = next_buffer_loc;
    buffer_base = (void *)((VRAM_BASE + global_buffer_offset[current_buffer_loc]) | UNCACHED_MIRROR);
}

void video_display_on_vblank()
{
    volatile unsigned int *videobase = (volatile unsigned int *)POWERVR2_BASE;

    // Draw any registered console to the screen.
    console_render();

    // Handle filling the background of the other screen while we wait.
    if (global_background_set)
    {
        global_background_fill_start = ((VRAM_BASE + global_buffer_offset[next_buffer_loc]) | UNCACHED_MIRROR);
        global_background_fill_end = global_background_fill_start + ((global_video_width * global_video_height * global_video_depth));
    }
    else
    {
        global_background_fill_start = 0;
        global_background_fill_end = 0;
    }

    // First, figure out if we're running with disabled interrupts. If so, we can't use
    // the hardware to wait for VBLANK.
    if (_irq_is_disabled(_irq_get_sr()))
    {
        // Wait for us to enter the VBLANK portion of the frame scan. This is the same
        // spot that we would get a VBLANK interrupt if we were using threads.
        uint32_t vblank_in_position = videobase[POWERVR2_VBLANK_INTERRUPT] & 0x1FF;
        while((videobase[POWERVR2_SYNC_STAT] & 0x1FF) != vblank_in_position) { ; }

        // Swap buffers in HW.
        _video_swap_vbuffers();
    }
    else
    {
        // Wait for hardware vblank interrupt.
        _thread_wait_vblank_swapbuffers();
    }

    // Finish filling in the background. Gotta do this now, fast or slow, because
    // when we exit this function the user is fully expected to start drawing new
    // graphics.
    if (global_background_fill_start < global_background_fill_end)
    {
        // Try the fast way, and if we don't have the HW access (another thread owned it before we disabled irqs),
        // then we need to do it the slow way.
        if (hw_memset((void *)global_background_fill_start, global_background_fill_color[0], global_background_fill_end - global_background_fill_start) == 0)
        {
            while (global_background_fill_start < global_background_fill_end)
            {
                memcpy((void *)global_background_fill_start, global_background_fill_color, 32);
                global_background_fill_start += 32;
            }
        }
    }

    // Set these back to empty, since we no longer need to handle them.
    global_background_fill_start = 0;
    global_background_fill_end = 0;
}

unsigned int video_width()
{
    return cached_actual_width;
}

unsigned int video_height()
{
    return cached_actual_height;
}

unsigned int video_depth()
{
    return global_video_depth;
}

void *video_framebuffer()
{
    return buffer_base;
}

unsigned int video_is_vertical()
{
    return global_video_vertical;
}

unsigned int video_is_interlaced()
{
    return global_video_15khz;
}

void _vblank_init()
{
    uint32_t old_interrupts = irq_disable();
    if ((HOLLY_INTERNAL_IRQ_2_MASK & HOLLY_INTERNAL_INTERRUPT_VBLANK_IN) == 0)
    {
        HOLLY_INTERNAL_IRQ_2_MASK = HOLLY_INTERNAL_IRQ_2_MASK | HOLLY_INTERNAL_INTERRUPT_VBLANK_IN;
    }
    if ((HOLLY_INTERNAL_IRQ_2_MASK & HOLLY_INTERNAL_INTERRUPT_VBLANK_OUT) == 0)
    {
        HOLLY_INTERNAL_IRQ_2_MASK = HOLLY_INTERNAL_IRQ_2_MASK | HOLLY_INTERNAL_INTERRUPT_VBLANK_OUT;
    }
    if ((HOLLY_INTERNAL_IRQ_2_MASK & HOLLY_INTERNAL_INTERRUPT_HBLANK) == 0)
    {
        HOLLY_INTERNAL_IRQ_2_MASK = HOLLY_INTERNAL_IRQ_2_MASK | HOLLY_INTERNAL_INTERRUPT_HBLANK;
    }
    irq_restore(old_interrupts);
}

void _vblank_free()
{
    uint32_t old_interrupts = irq_disable();
    if ((HOLLY_INTERNAL_IRQ_2_MASK & HOLLY_INTERNAL_INTERRUPT_VBLANK_IN) != 0)
    {
        HOLLY_INTERNAL_IRQ_2_MASK = HOLLY_INTERNAL_IRQ_2_MASK & (~HOLLY_INTERNAL_INTERRUPT_VBLANK_IN);
    }
    if ((HOLLY_INTERNAL_IRQ_2_MASK & HOLLY_INTERNAL_INTERRUPT_VBLANK_OUT) != 0)
    {
        HOLLY_INTERNAL_IRQ_2_MASK = HOLLY_INTERNAL_IRQ_2_MASK & (~HOLLY_INTERNAL_INTERRUPT_VBLANK_OUT);
    }
    if ((HOLLY_INTERNAL_IRQ_2_MASK & HOLLY_INTERNAL_INTERRUPT_HBLANK) != 0)
    {
        HOLLY_INTERNAL_IRQ_2_MASK = HOLLY_INTERNAL_IRQ_2_MASK & (~HOLLY_INTERNAL_INTERRUPT_HBLANK);
    }
    irq_restore(old_interrupts);
}

void video_set_lowres_mode(video_mode_t new_mode)
{
    lowres_mode = new_mode;
}
void video_set_highres_mode(video_mode_t new_mode)
{
    highres_mode = new_mode;
}
void video_set_dither(uint8_t enabled)
{
    dither_enable = enabled;
}

void _video_set_ta_registers()
{
    volatile unsigned int *videobase = (volatile unsigned int *)POWERVR2_BASE;

    // Set up framebuffer config to enable display, set pixel mode.
    videobase[POWERVR2_FB_RENDER_CFG] = render_cfg;

    // Set up render modulo, (bpp * width) / 8.
    videobase[POWERVR2_FB_RENDER_MODULO] = (global_video_depth * global_video_width) / 8;

    // Set up horizontal clipping.
    videobase[POWERVR2_FB_CLIP_X] = ((global_video_width - 1) << 16) | (0 << 0);
    
    if(active_mode.interlaced)
    {
        videobase[POWERVR2_SCALER] = SCALER_CFG_INTERLACED;
    }
    else
    {
        videobase[POWERVR2_SCALER] = SCALER_CFG_PROGRESSIVE;
    }

    // Set up vertical clipping.
    videobase[POWERVR2_FB_CLIP_Y] = ((global_video_height - 1) << 16) | (0 << 0);
}

void _video_init(int colordepth, int init_ta)
{
    if (colordepth != VIDEO_COLOR_1555 && colordepth != VIDEO_COLOR_8888)
    {
        // Really no option but to exit, we don't even have video to display an error.
        return;
    }

    uint32_t old_interrupts = irq_disable();
    volatile unsigned int *videobase = (volatile unsigned int *)POWERVR2_BASE;

    // Read inputs to determine if lowres (15kHz) or highres (31kHz) mode should be setup.
    jvs_buttons_t buttons;
    maple_request_jvs_buttons(0x01, &buttons);
    global_video_15khz = buttons.dip1 ? 1 : 0;

    // Fetch the video mode based on the resolution setting
    active_mode = (global_video_15khz ? lowres_mode : highres_mode);

    // Determine render cfg
    if(colordepth == VIDEO_COLOR_8888)
    {
        render_cfg = (RENDER_CFG_RGB0888 << 0); // RGB0888 mode, no alpha threshold.
    }
    else
    {
        if(dither_enable)
        {
            render_cfg = (
                0x1 << 3 |                  // Enable dithering
                RENDER_CFG_RGB0555 << 0     // RGB555 mode, no alpha threshold.
            );
        }
        else
        {
            render_cfg = (RENDER_CFG_RGB0555 << 0); // RGB555 mode, no alpha threshold.
        }
    }
    

    global_video_width = active_mode.width;
    global_video_height = active_mode.height;
    global_video_depth = colordepth;
    global_background_color = 0;
    global_background_set = 0;
    global_buffer_offset[0] = 0;
    global_buffer_offset[1] = global_buffer_offset[0] + (global_video_width * global_video_height * global_video_depth);
    global_buffer_offset[2] = global_buffer_offset[1] + (global_video_width * global_video_height * global_video_depth);

    // Since framebuffer size can change, need to make sure to preserve VRAM scratch areas.
    // EG: 320 x 240 buffer is 1/4 size compared to 640x480 buffer. Ensure that the global_buffer_offset[2] location
    // is always located at least two 640x480 locations away from [0], to avoid chewed up texture RAM.
    uint32_t reference_buffer_size = (640 * 480 * global_video_depth);
    if(global_buffer_offset[2] < global_buffer_offset[0] + (reference_buffer_size * 2))
    {
        global_buffer_offset[2] = global_buffer_offset[0] + reference_buffer_size * 2;
    }

    // Read the EEPROM and figure out if we're vertical orientation.
    eeprom_t eeprom;
    eeprom_read(&eeprom);
    global_video_vertical = eeprom.system.monitor_orientation == MONITOR_ORIENTATION_VERTICAL ? 1 : 0;

    if (global_video_vertical) {
        cached_actual_width = global_video_height;
    } else {
        cached_actual_width = global_video_width;
    }
    if (global_video_vertical) {
        cached_actual_height = global_video_width;
    } else {
        cached_actual_height = global_video_height;
    }

    // Now, initialize the tile accelerator so it can be used for drawing.
    if (init_ta)
    {
        _ta_init();
    }

    // Now, zero out the screen so there's no garbage if we never display.
    void *zero_base = (void *)((VRAM_BASE + global_buffer_offset[0]) | UNCACHED_MIRROR);
    if (!hw_memset(zero_base, 0, global_video_width * global_video_height * global_video_depth * 2))
    {
        // Gotta do the slow method.
        memset(zero_base, 0, global_video_width * global_video_height * global_video_depth * 2);
    }

    // Set up video timings copied from Naomi BIOS.
    videobase[POWERVR2_VRAM_CFG3] = 0x15D1C955;
    videobase[POWERVR2_VRAM_CFG1] = 0x00000020;

    // Make sure video is not in reset.
    videobase[POWERVR2_RESET] = 0;

    // Set border color to black.
    videobase[POWERVR2_BORDER_COL] = 0;

    // Enable pixel double if needed, disable fullscreen border
    if(active_mode.pixeldouble) 
    {
        videobase[POWERVR2_VIDEO_CFG] = (0x1 << 8) | (0x16 << 16);
    }
    else 
    {
        videobase[POWERVR2_VIDEO_CFG] = (0x16 << 16);
    }

    // Set up display configuration.
    // Initialize to zero, then enable at the end of video_init
    uint32_t fb_display_cfg = 0x0;

    if (global_video_depth == 2)
    {
        fb_display_cfg |= DISPLAY_CFG_RGB1555 << 2;  // RGB1555 mode.
    }
    else if (global_video_depth == 4)
    {
        fb_display_cfg |= DISPLAY_CFG_RGB0888 << 2;  // RGB0888 mode.
    }

    if(active_mode.pixel_clock_double)
    {
        fb_display_cfg |= (0x1 << 23);  // Double pixel clock.
    }

    if(active_mode.linedouble)
    {
        fb_display_cfg |= (0x1 << 1);     // Enable linedouble read mode
    }

    videobase[POWERVR2_FB_DISPLAY_CFG] = fb_display_cfg;

    // Set up registers that appear to be reset with TA resets.
    _video_set_ta_registers();

    // Set up even/odd field video base address, shifted by bpp.
    videobase[POWERVR2_FB_DISPLAY_ADDR_1] = global_buffer_offset[current_buffer_loc];
    videobase[POWERVR2_FB_DISPLAY_ADDR_2] = global_buffer_offset[current_buffer_loc] + (global_video_width * global_video_depth);

    // Swap buffer pointer in SW.
    buffer_loc = next_buffer_loc;
    buffer_base = (void *)((VRAM_BASE + global_buffer_offset[current_buffer_loc]) | UNCACHED_MIRROR);

    // Cache BIOS vblank interrupt configuration
    if (!initialized)
    {
        saved_hvint = videobase[POWERVR2_VBLANK_INTERRUPT];
        initialized = 1;
    }

    // Set up display size.
    if(active_mode.interlaced)
    {
        videobase[POWERVR2_FB_DISPLAY_SIZE] = (
            (((global_video_width / 4) * global_video_depth) + 1) << 20 | // Interlace skip modulo if we are interlaced ((width / 4) * bpp) + 1
            ((global_video_height - 1) / 2) << 10 |                       // (height - 1) / 2
            (((global_video_width / 4) * global_video_depth) - 1) << 0    // ((width / 4) * bpp) - 1
        );
    }
    else 
    {
        videobase[POWERVR2_FB_DISPLAY_SIZE] = (
            1 << 20 |                                                       // Set up size without a skip modulus in progressive modes                   // Interlace skip modulo if we are interlaced ((width / 4) * bpp) + 1
            (global_video_height - 1) << 10 |                               // (height - 1)
            (((global_video_width / 4) * global_video_depth) - 1) << 0      // ((width / 4) * bpp) - 1
        );
    }

    // Set up Vblank interrupts
    videobase[POWERVR2_VBLANK_INTERRUPT] = (
        active_mode.vblank_int_end << 16 |     // Out of vblank.
        active_mode.vblank_int_start << 0      // In vblank.
    );
    
    // Set up horizontal position
    videobase[POWERVR2_HPOS] = active_mode.h_pos;

    // Set up vertical position (max 10-bit values)
    videobase[POWERVR2_VPOS] = (
        active_mode.v_pos << 16 |  // Even position (16-25).
        active_mode.v_pos << 0     // Odd position (0-9).
    );

    // Set up Vblank
    videobase[POWERVR2_VBLANK] = (
        active_mode.vblank_end << 16 |     // Start.
        active_mode.vblank_start << 0      // End.
    );

    // Set up Hblank (may not be strictly necessary)
    videobase[POWERVR2_HBLANK] = (
        active_mode.hblank_end << 16 |     // Out of hblank.
        active_mode.hblank_start << 0      // In hblank.
    );

    // Set up refresh rate / sync.
    videobase[POWERVR2_SYNC_LOAD] = (
        active_mode.vsync << 16  |  // Vsync - number of lines -1
        active_mode.hsync << 0      // Hsync - number of clocks per line
    );

    videobase[POWERVR2_SYNC_CFG] = (
        0x1 << 8 |                                 // Enable sync generator (required)
        (active_mode.interlaced & 0x1) << 6 |      // Flag for NTSC format video (required for interlacing)
        (active_mode.interlaced & 0x1) << 4 |      // Flag for interlacing
        0 << 2 |                                   // Negate H-sync
        0 << 1                                     // Negate V-sync
    );

    // Reenable display output and framebuffer
    videobase[POWERVR2_VIDEO_CFG] = videobase[POWERVR2_VIDEO_CFG] & ~(0x1 << 3);        // Enable video output
    videobase[POWERVR2_FB_DISPLAY_CFG] = videobase[POWERVR2_FB_DISPLAY_CFG] | 0x1;      // Framebuffer enable

    // Wait for vblank like games do.
    uint32_t vblank_in_position = videobase[POWERVR2_VBLANK_INTERRUPT] & 0x1FF;
    while((videobase[POWERVR2_SYNC_STAT] & 0x1FF) != vblank_in_position) { ; }

    // Now, ask the TA to set up its buffers since we have working video now.
    if (init_ta)
    {
        _ta_init_buffers();
    }

    // Finally, its safe to enable interrupts and move on.
    irq_restore(old_interrupts);
}

void video_init(int colordepth)
{
    _video_init(colordepth, 1);
}

void video_free()
{
    uint32_t old_interrupts = irq_disable();
    volatile unsigned int *videobase = (volatile unsigned int *)POWERVR2_BASE;

    // We only want to restore the saved HVinterrupt values on the last free.
    if (initialized)
    {
        initialized = 0;
        videobase[POWERVR2_VBLANK_INTERRUPT] = saved_hvint;
    }

    // Kill our vblank interrupt.
    _vblank_free();

    // Kill the tile accelerator.
    _ta_free();

    // De-init our globals.
    global_video_width = 0;
    global_video_height = 0;
    global_video_depth = 0;
    global_background_color = 0;
    global_background_set = 0;
    global_buffer_offset[0] = 0;
    global_buffer_offset[1] = 0;
    global_buffer_offset[2] = 0;

    // We're done, safe for interrupts to come back.
    irq_restore(old_interrupts);
}

void video_fill_screen(color_t color)
{
    if(global_video_depth == 2)
    {
        uint32_t actualcolor = RGB0555(color.r, color.g, color.b);
        uint32_t multvalue = global_video_15khz ? 1 : 2;
        if (!hw_memset(buffer_base, (actualcolor & 0xFFFF) | ((actualcolor << 16) & 0xFFFF0000), global_video_width * global_video_height * multvalue))
        {
            memset(buffer_base, (actualcolor & 0xFFFF) | ((actualcolor << 16) & 0xFFFF0000), global_video_width * global_video_height * multvalue);
        }
    }
    else if(global_video_depth == 4)
    {
        uint32_t actualcolor = RGB0888(color.r, color.g, color.b);
        uint32_t multvalue = global_video_15khz ? 2 : 4;
        if (!hw_memset(buffer_base, actualcolor, global_video_width * global_video_height * multvalue))
        {
            memset(buffer_base, actualcolor, global_video_width * global_video_height * multvalue);
        }
    }
}

void video_set_background_color(color_t color)
{
    video_fill_screen(color);
    global_background_set = 1;

    if(global_video_depth == 2)
    {
        uint32_t actualcolor = RGB0555(color.r, color.g, color.b);
        for (int offset = 0; offset < 8; offset++)
        {
            global_background_fill_color[offset] = (actualcolor & 0xFFFF) | ((actualcolor << 16) & 0xFFFF0000);
        }
    }
    else if(global_video_depth == 4)
    {
        uint32_t actualcolor = RGB0888(color.r, color.g, color.b);
        for (int offset = 0; offset < 8; offset++)
        {
            global_background_fill_color[offset] = actualcolor;
        }
    }
}

void video_fill_box(int x0, int y0, int x1, int y1, color_t color)
{
    int low_x;
    int high_x;
    int low_y;
    int high_y;

    if (x1 < x0)
    {
        low_x = x1;
        high_x = x0;
    }
    else
    {
        low_x = x0;
        high_x = x1;
    }
    if (y1 < y0)
    {
        low_y = y1;
        high_y = y0;
    }
    else
    {
        low_y = y0;
        high_y = y1;
    }

    if (high_x < 0 || low_x >= cached_actual_width || high_y < 0 || low_y >= cached_actual_height)
    {
        return;
    }
    if (low_x < 0)
    {
        low_x = 0;
    }
    if (low_y < 0)
    {
        low_y = 0;
    }
    if (high_x >= cached_actual_width)
    {
        high_x = cached_actual_width - 1;
    }
    if (high_y >= cached_actual_height)
    {
        high_y = cached_actual_height - 1;
    }

    if(global_video_depth == 2)
    {
        uint32_t actualcolor = RGB0555(color.r, color.g, color.b);
        if(global_video_vertical)
        {
            for(int col = low_x; col <= high_x; col++)
            {
                for(int row = high_y; row >= low_y; row--)
                {
                    SET_PIXEL_V_2(buffer_base, col, row, actualcolor);
                }
            }
        }
        else
        {
            for(int row = low_y; row <= high_y; row++)
            {
                for(int col = low_x; col <= high_x; col++)
                {
                    SET_PIXEL_H_2(buffer_base, col, row, actualcolor);
                }
            }
        }
    }
    else if(global_video_depth == 4)
    {
        uint32_t actualcolor = RGB0888(color.r, color.g, color.b);
        if(global_video_vertical)
        {
            for(int col = low_x; col <= high_x; col++)
            {
                for(int row = high_y; row >= low_y; row--)
                {
                    SET_PIXEL_V_4(buffer_base, col, row, actualcolor);
                }
            }
        }
        else
        {
            for(int row = low_y; row <= high_y; row++)
            {
                for(int col = low_x; col <= high_x; col++)
                {
                    SET_PIXEL_H_4(buffer_base, col, row, actualcolor);
                }
            }
        }
    }
}

void video_draw_pixel(int x, int y, color_t color)
{
    // Let's do some basic bounds testing.
    if (((uint32_t)(x | y)) & 0x80000000) { return; }
    if (x >= cached_actual_width || y >= cached_actual_height) { return; }

    if (global_video_depth == 2)
    {
        uint32_t actualcolor = RGB0555(color.r, color.g, color.b);
        if (global_video_vertical)
        {
            SET_PIXEL_V_2(buffer_base, x, y, actualcolor);
        }
        else
        {
            SET_PIXEL_H_2(buffer_base, x, y, actualcolor);
        }
    }
    else if(global_video_depth == 4)
    {
        uint32_t actualcolor = RGB0888(color.r, color.g, color.b);
        if (global_video_vertical)
        {
            SET_PIXEL_V_4(buffer_base, x, y, actualcolor);
        }
        else
        {
            SET_PIXEL_H_4(buffer_base, x, y, actualcolor);
        }
    }
}

color_t video_get_pixel(int x, int y)
{
    uint32_t color;
    color_t retval;

    if (global_video_depth == 2)
    {
        if (global_video_vertical)
        {
            color = GET_PIXEL_V_2(buffer_base, x, y);
        }
        else
        {
            color = GET_PIXEL_H_2(buffer_base, x, y);
        }

        retval.a = 0;
        EXPLODE0555(color, retval.r, retval.g, retval.b);
    }
    else if(global_video_depth == 4)
    {
        if (global_video_vertical)
        {
            color = GET_PIXEL_V_4(buffer_base, x, y);
        }
        else
        {
            color = GET_PIXEL_H_4(buffer_base, x, y);
        }

        retval.a = 0;
        EXPLODE0888(color, retval.r, retval.g, retval.b);
    }
    else
    {
        retval.r = 0;
        retval.g = 0;
        retval.b = 0;
        retval.a = 0;
    }

    return retval;
}

void video_draw_line(int x0, int y0, int x1, int y1, color_t color)
{
    int dy = y1 - y0;
    int dx = x1 - x0;
    int sx, sy;

    if(dy < 0)
    {
        dy = -dy;
        sy = -1;
    }
    else
    {
        sy = 1;
    }

    if(dx < 0)
    {
        dx = -dx;
        sx = -1;
    }
    else
    {
        sx = 1;
    }

    if (dx == 0 && dy == 0)
    {
        video_draw_pixel(x0, y0, color);
        return;
    }

    dy <<= 1;
    dx <<= 1;

    video_draw_pixel(x0, y0, color);
    if(dx > dy)
    {
        int frac = dy - (dx >> 1);
        while(x0 != x1)
        {
            if(frac >= 0)
            {
                y0 += sy;
                frac -= dx;
            }
            x0 += sx;
            frac += dy;
            video_draw_pixel(x0, y0, color);
        }
    }
    else
    {
        int frac = dx - (dy >> 1);
        while(y0 != y1)
        {
            if(frac >= 0)
            {
                x0 += sx;
                frac -= dy;
            }
            y0 += sy;
            frac += dx;
            video_draw_pixel(x0, y0, color);
        }
    }
}

void video_draw_box(int x0, int y0, int x1, int y1, color_t color)
{
    int low_x;
    int high_x;
    int low_y;
    int high_y;

    if (x1 < x0)
    {
        low_x = x1;
        high_x = x0;
    }
    else
    {
        low_x = x0;
        high_x = x1;
    }
    if (y1 < y0)
    {
        low_y = y1;
        high_y = y0;
    }
    else
    {
        low_y = y0;
        high_y = y1;
    }

    video_draw_line(low_x, low_y, high_x, low_y, color);
    video_draw_line(low_x, high_y, high_x, high_y, color);
    video_draw_line(low_x, low_y, low_x, high_y, color);
    video_draw_line(high_x, low_y, high_x, high_y, color);
}

void video_draw_debug_character(int x, int y, color_t color, char ch)
{
    if (ch < 0x20 || ch > 0x7F)
    {
        return;
    }

    for(int row = y; row < y + 8; row++)
    {
        uint8_t c = __font_data[(ch * 8) + (row - y)];

        /* Draw top half unrolled */
        switch( c & 0xF0 )
        {
            case 0x10:
                video_draw_pixel( x + 3, row, color );
                break;
            case 0x20:
                video_draw_pixel( x + 2, row, color );
                break;
            case 0x30:
                video_draw_pixel( x + 2, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
            case 0x40:
                video_draw_pixel( x + 1, row, color );
                break;
            case 0x50:
                video_draw_pixel( x + 1, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
            case 0x60:
                video_draw_pixel( x + 1, row, color );
                video_draw_pixel( x + 2, row, color );
                break;
            case 0x70:
                video_draw_pixel( x + 1, row, color );
                video_draw_pixel( x + 2, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
            case 0x80:
                video_draw_pixel( x, row, color );
                break;
            case 0x90:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
            case 0xA0:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 2, row, color );
                break;
            case 0xB0:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 2, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
            case 0xC0:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 1, row, color );
                break;
            case 0xD0:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 1, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
            case 0xE0:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 1, row, color );
                video_draw_pixel( x + 2, row, color );
                break;
            case 0xF0:
                video_draw_pixel( x, row, color );
                video_draw_pixel( x + 1, row, color );
                video_draw_pixel( x + 2, row, color );
                video_draw_pixel( x + 3, row, color );
                break;
        }

        /* Draw bottom half unrolled */
        switch( c & 0x0F )
        {
            case 0x01:
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x02:
                video_draw_pixel( x + 6, row, color );
                break;
            case 0x03:
                video_draw_pixel( x + 6, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x04:
                video_draw_pixel( x + 5, row, color );
                break;
            case 0x05:
                video_draw_pixel( x + 5, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x06:
                video_draw_pixel( x + 5, row, color );
                video_draw_pixel( x + 6, row, color );
                break;
            case 0x07:
                video_draw_pixel( x + 5, row, color );
                video_draw_pixel( x + 6, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x08:
                video_draw_pixel( x + 4, row, color );
                break;
            case 0x09:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x0A:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 6, row, color );
                break;
            case 0x0B:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 6, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x0C:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 5, row, color );
                break;
            case 0x0D:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 5, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
            case 0x0E:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 5, row, color );
                video_draw_pixel( x + 6, row, color );
                break;
            case 0x0F:
                video_draw_pixel( x + 4, row, color );
                video_draw_pixel( x + 5, row, color );
                video_draw_pixel( x + 6, row, color );
                video_draw_pixel( x + 7, row, color );
                break;
        }
    }
}

#define UNROLLED_1BPP_ALPHA_BLEND(orientation, bitdepth) \
    uint16_t pixel = pixels[col + (row * width)]; \
    if (pixel & 0x8000) \
    { \
        SET_PIXEL_ ## orientation ## _ ## bitdepth(buffer_base, x + col, y + row, pixel); \
    }

#define UNROLLED_8BPP_ALPHA_BLEND(orientation, bitdepth) \
    uint32_t pixel = pixels[col + (row * width)]; \
    unsigned int alpha = (pixel >> 24) & 0xFF; \
    \
    if (alpha) \
    { \
        if (alpha >= 255) \
        { \
            SET_PIXEL_ ## orientation ## _ ## bitdepth(buffer_base, x + col, y + row, pixel); \
        } \
        else \
        { \
            /* First grab the actual RGB values of the source alpha. */ \
            unsigned int sr; \
            unsigned int sg; \
            unsigned int sb; \
            EXPLODE0888(pixel, sr, sg, sb); \
            \
            /* Now grab the actual RGB values (don't care about alpha) for the dest. */ \
            unsigned int dr; \
            unsigned int dg; \
            unsigned int db; \
            unsigned int negalpha = (~alpha) & 0xFF; \
            EXPLODE0888(GET_PIXEL_ ## orientation ## _ ## bitdepth(buffer_base, x + col, y + row), dr, dg, db); \
            \
            /* Technically it should be divided by 255, but this should be much much faster for an 0.4% accuracy loss. */ \
            dr = ((sr * alpha) + (dr * negalpha)) >> 8; \
            dg = ((sg * alpha) + (dg * negalpha)) >> 8; \
            db = ((sb * alpha) + (db * negalpha)) >> 8; \
            SET_PIXEL_ ## orientation ## _ ## bitdepth(buffer_base, x + col, y + row, RGB0888(dr, dg, db)); \
        } \
    } \

void video_draw_sprite(int x, int y, int width, int height, void *data)
{
    int low_x = 0;
    int high_x = width;
    int low_y = 0;
    int high_y = height;

    if (x < 0)
    {
        if (x + width <= 0)
        {
            return;
        }

        low_x = -x;
    }
    if (y < 0)
    {
        if (y + height <= 0)
        {
            return;
        }

        low_y = -y;
    }
    if ((x + width) >= cached_actual_width)
    {
        if (x >= cached_actual_width)
        {
            return;
        }

        high_x = cached_actual_width - x;
    }
    if (y + height >= cached_actual_height)
    {
        if (y >= cached_actual_height)
        {
            return;
        }

        high_y = cached_actual_height - y;
    }

    if(global_video_depth == 2)
    {
        uint16_t *pixels = (uint16_t *)data;

        if(global_video_vertical)
        {
            for(int col = low_x; col < high_x; col++)
            {
                for(int row = (high_y - 1); row >= low_y; row--)
                {
                    UNROLLED_1BPP_ALPHA_BLEND(V, 2);
                }
            }
        }
        else
        {
            for(int row = low_y; row < high_y; row++)
            {
                for(int col = low_x; col < high_x; col++)
                {
                    UNROLLED_1BPP_ALPHA_BLEND(H, 2);
                }
            }
        }
    }
    else if(global_video_depth == 4)
    {
        uint32_t *pixels = (uint32_t *)data;

        if(global_video_vertical)
        {
            for(int col = low_x; col < high_x; col++)
            {
                for(int row = (high_y - 1); row >= low_y; row--)
                {
                    UNROLLED_8BPP_ALPHA_BLEND(V, 4);
                }
            }
        }
        else
        {
            for(int row = low_y; row < high_y; row++)
            {
                for(int col = low_x; col < high_x; col++)
                {
                    UNROLLED_8BPP_ALPHA_BLEND(H, 4);
                }
            }
        }
    }
}

void __video_draw_debug_text(int x, int y, color_t color, const char * const msg)
{
    if( msg == 0 ) { return; }

    int tx = x;
    int ty = y;
    const char *text = (const char *)msg;

    while( *text )
    {
        switch( *text )
        {
            case '\r':
            case '\n':
                tx = x;
                ty += 8;
                break;
            case ' ':
                tx += 8;
                break;
            case '\t':
                tx += 8 * 5;
                break;
            default:
                video_draw_debug_character( tx, ty, color, *text );
                tx += 8;
                break;
        }

        if ((tx + 8) >= cached_actual_width)
        {
            tx = 0;
            ty += 8;
        }

        text++;
    }
}

void video_draw_debug_text(int x, int y, color_t color, const char * const msg, ...)
{
    if (msg)
    {
        char buffer[2048];
        va_list args;
        va_start(args, msg);
        int length = vsnprintf(buffer, 2047, msg, args);
        va_end(args);

        if (length > 0)
        {
            buffer[min(length, 2047)] = 0;
            __video_draw_debug_text(x, y, color, buffer);
        }
    }
}

void *video_scratch_area()
{
    return(void *)((VRAM_BASE + global_buffer_offset[2]) | UNCACHED_MIRROR);
}

unsigned int video_scratch_size()
{
    return GLOBAL_BUFFER_SCRATCH_SIZE;
}