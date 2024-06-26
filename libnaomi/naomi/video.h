#ifndef __VIDEO_H
#define __VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "naomi/color.h"

// Note that all of the functions in here are intentionally not thread-safe.
// If you are attempting to update the video buffer from multiple threads at
// once it is indeterminate what will display on the screen. It is recommended
// not to interact with the video system across multiple threads.

// Note that if you wish to mix TA/PVR rendering with the framebuffer rendering
// functions found here, you should only do so after calling ta_render(). The
// TA/PVR will overwrite the entire framebuffer during rendering so the only
// safe time to render using software is after it is finished and before calling
// video_display_on_vblank().

// Defines for the color argument of the below function.
#define VIDEO_COLOR_1555 2
#define VIDEO_COLOR_8888 4

// Initialize the video hardware for software and hardware drawn sprites and
// graphics. Supports 640x480@60fps VGA by default; custom modes are configurable.
// Pass one of the above video color defines to specify color depth.
void video_init(int colordepth);

// Structure allowing for custom-defined video modes to be constructed.
// By default, libnaomi will use a 480p output in 31kHz mode, and a 480i output
// in 15kHz mode. Both default modes use a 640x480 framebuffer.

// By creating a custom video mode and setting it to the lowres or highres slot,
// it is possible to output video in a different format. However, care must be taken
// to construct video modes that are compatible with the NAOMI hardware.

// Note that output resolution is influenced by both framebuffer width/height and the
// linedouble and pixeldouble features.
typedef struct video_mode_t{
    unsigned int width;             // Framebuffer width.
    unsigned int height;            // Framebuffer height.

    uint16_t h_pos;                 // Horizontal position at which to start the displayed raster.
    uint16_t v_pos;                 // Vertical position at which to start the displayed raster.

    uint8_t interlaced;             // Output in interlaced mode.
    uint8_t linedouble;             // Linedouble (write every line of framebuffer to two lines of output). Modifies output resolution.
    uint8_t pixeldouble;            // Pixeldouble (write every pixel on a given line twice consecutively). Modifies output resolution.
    uint8_t pixel_clock_double;     // Double the pixel clock. Necessary when constructing a 31kHz signal.

    uint16_t hblank_start;          // The start (in clocks) of the horizontal blank.
    uint16_t hblank_end;            // The end (in clocks) of the horizontal blank.

    uint16_t vblank_int_start;      // The vertical position (in lines) at which the vertical blank interrupt will begin.
    uint16_t vblank_int_end;        // The vertical position (in lines) at which the vertical blank interrupt will end.

    uint16_t vblank_start;          // The vertical position (in lines) of the vertical blank.
    uint16_t vblank_end;            // The vertical position (in lines) of the vertical blank.

    uint16_t hsync;                 // The number of clocks per line. Used to construct the sync signal.
    uint16_t vsync;                 // The number of lines. Used to construct the sync signal.
}video_mode_t;

// Optional: Set a custom video mode to use when the system is in high-res (31kHz) output mode (DIP1 OFF).
// Must be used before video_init() is called.
void video_set_lowres_mode(video_mode_t new_mode);

// Optional: Set a custom video mode to use when the system is in low-res (15kHz) output mode (DIP1 ON).
// Must be used before video_init() is called.
void video_set_highres_mode(video_mode_t new_mode);

// Optional: Disable or enable dithering in RGBA1555 mode.
// Dithering is normally enabled by default in RGBA1555 , and disabled in RGBA8888 mode.
// This function allows it to be manually disabled or forced in 1555 mode.
// Does nothing in RGBA8888 mode.
// Must be used before video_init() is called.
void video_set_dither(uint8_t enabled);

// Free existing video system so that it can be initialized with another
// call.
void video_free();

// Wait for an appropriate time to swap buffers and then do so. Also fills
// the next screen's background with a previously set background color if
// a background color was set, while waiting for vblank to happen. Releases
// the current thread so other threads can continue processing as long as
// threads are enabled. If threads are disabled, this will instead spinloop
// until ready and no other work can get done.
void video_display_on_vblank();

// Request that every frame be cleared to this color (use rgb() or
// rgba() to generate the color for this). Without this, you are
// responsible for clearing previous-frame garbage using video_fill_screen()
// or similar. Note that if you are using the TA/PVR, you want to instead
// us ta_set_background_color() as documented in ta.h.
void video_set_background_color(color_t color);

// The width in pixels of the drawable video area. This could change
// depending on the monitor orientation.
unsigned int video_width();

// The height in pixels of the drawable video area. This could change
// depending on the monitor orientation.
unsigned int video_height();

// The depth in numer of bytes of the screen. You should expect this to
// be 2 or 4 depending on the video mode.
unsigned int video_depth();

// The current framebuffer that we are rendering to, for instances where
// you need direct access. Note that this is uncached and in VRAM.
void *video_framebuffer();

// Scratch memory area in the VRAM region safe to modify without possibly
// corrupting video contents. Note that you have 128kb (1024 * 128) of
// space here before you hit TA/PVR memory if you are rendering using
// hardware acceleration.
void *video_scratch_area();

// The size in bytes of the above scratch area (as documented above).
unsigned int video_scratch_size();

// Returns nonzero if the screen is in vertical orientation, or zero if
// the screen is in horizontal orientation. This is for convenience, the
// pixel-based drawing functions always treat the top left of the screen
// as (0, 0) from the cabinet player's position. Note that the orientation
// of the cabinet is controlled in the test menu system settings submenu.
unsigned int video_is_vertical();

// Returns nonzero if the screen is in interlaced mode, or zero if the
// screen is in progressive mode. This is for convenience, the pixel-based
// drawing functions always allow you to draw to the entire framebuffer.
// Note that the cabinet will be in interlaced mode if the user has selected
// 15khz on the DIP switches (DIP switch 1 is on), and will be in progressive
// mode if the user has selected 31khz on the DIP switches (DIP switch 1 is
// off).
unsigned int video_is_interlaced();

// Fill the entire framebuffer with one color. Note that this is about
// 3x faster than doing it yourself as it uses hardware features to do so.
void video_fill_screen(color_t color);

// Given a staring and ending x and y coodinate, fills a simple box with
// the given color. This is orientation-aware.
void video_fill_box(int x0, int y0, int x1, int y1, color_t color);

// Given an x, y position and a color, colors that particular pixel with
// that particular color. This is orientation-aware.
void video_draw_pixel(int x, int y, color_t color);

// Given an x, y position, returns the color at that particular pixel. This
// returned color is suitable for passing into any function that requests
// a color parameter. This is orientation-aware.
color_t video_get_pixel(int x, int y);

// Given a starting and ending x and y coordinate, draws a line of a certain
// color between that starting and ending point. This is orientation-aware.
void video_draw_line(int x0, int y0, int x1, int y1, color_t color);

// Given a staring and ending x and y coodinate, draws a simple box with
// the given color. This is orientation-aware.
void video_draw_box(int x0, int y0, int x1, int y1, color_t color);

// Given an x, y coordinate, a sprite width and height, and a packed chunk
// of sprite data (should be video_depth() bytes per pixel in the sprite),
// draws the sprite to the screen at that x, y position. This is orientation
// aware and will skip drawing pixels with an alpha of 0. In VIDEO_COLOR_8888
// mode this will perform software alpha-blending of the sprite with the
// existing pixels in the framebuffer.
void video_draw_sprite(int x, int y, int width, int height, void *data);

// Draw a debug character, string or formatted string of a certain color to
// the screen. This uses a built-in 8x8 fixed-width font and is always
// available regardless of other fonts or libraries. This is orientation aware.
// Also, video_draw_debug_text() takes a standard printf-formatted string with
// additional arguments. Note that this only supports ASCII printable characters.
void video_draw_debug_character(int x, int y, color_t color, char ch);
void video_draw_debug_text(int x, int y, color_t color, const char * const msg, ...);

// Include the freetype extensions for you, so you don't have to include video-freetype.h yourself.
#include "video-freetype.h"

#ifdef __cplusplus
}
#endif

#endif
