#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "naomi/video.h"
#include "naomi/maple.h"
#include "common.h"
#include "config.h"
#include "screens.h"
#include "packet.h"
#include "message.h"
#include "controls.h"

#define READ_ONLY_ALWAYS -1
#define READ_ONLY_NEVER -2

typedef struct
{
    unsigned int enabled;
    char description[60];
} patch_t;

typedef struct
{
    int setting;
    unsigned int value_count;
    uint32_t *values;
    unsigned int negate;
} read_only_t;

typedef struct
{
    uint32_t value;
    char description[60];
} value_t;

typedef struct
{
    char name[64];
    unsigned int value_count;
    value_t *values;
    uint32_t current;
    read_only_t read_only;
} setting_t;

typedef struct
{
    unsigned int selected_game;
    unsigned int patch_count;
    patch_t *patches;
    unsigned int system_settings_count;
    setting_t *system_settings;
    unsigned int game_settings_count;
    setting_t *game_settings;
} game_options_t;

static int selected_game = -1;
static int expecting_boot = 0;
static int sending_game_size = 0;
static game_options_t *game_options = 0;

void free_setting(setting_t *setting)
{
    if (setting->values)
    {
        free(setting->values);
    }

    if (setting->read_only.values)
    {
        free(setting->read_only.values);
    }
}

int parse_setting(uint8_t *data, unsigned int length, setting_t *setting, unsigned int *expected_length)
{
    host_printf("TODO!!");
    return 0;
}

void free_game_options(game_options_t *parsed_options)
{
    for (unsigned int fs = 0; fs < parsed_options->game_settings_count; fs++)
    {
        free_setting(&parsed_options->game_settings[fs]);
    }

    if (parsed_options->game_settings)
    {
        free(parsed_options->game_settings);
    }

    for (unsigned int fs = 0; fs < parsed_options->system_settings_count; fs++)
    {
        free_setting(&parsed_options->system_settings[fs]);
    }

    if (parsed_options->system_settings)
    {
        free(parsed_options->system_settings);
    }
    if (parsed_options->patches)
    {
        free(parsed_options->patches);
    }

    free(parsed_options);
}

game_options_t *parse_game_options(uint8_t *data, unsigned int length)
{
    game_options_t *parsed_options = malloc(sizeof(game_options_t));
    memset(parsed_options, 0, sizeof(game_options_t));
    unsigned int expected_length = 0;

    if (length < (expected_length + 4))
    {
        host_printf("Not enough data for selected game!");
        free(parsed_options);
        return 0;
    }

    memcpy(&parsed_options->selected_game, &data[expected_length], 4);
    expected_length += 4;

    if (length < (expected_length + 4))
    {
        host_printf("Not enough data for patch count!");
        free(parsed_options);
        return 0;
    }

    memcpy(&parsed_options->patch_count, &data[expected_length], 4);
    expected_length += 4;

    if (parsed_options->patch_count > 0)
    {
        parsed_options->patches = malloc(sizeof(patch_t) * parsed_options->patch_count);

        for (unsigned int patchno = 0; patchno < parsed_options->patch_count; patchno++)
        {
            if (length < (expected_length + 64))
            {
                host_printf("Not enough data for patch %d!", patchno);
                free(parsed_options->patches);
                free(parsed_options);
                return 0;
            }

            memcpy(&parsed_options->patches[patchno].enabled, &data[expected_length], 4);
            memcpy(&parsed_options->patches[patchno].description, &data[expected_length + 4], 60);
            expected_length += 64;
        }
    }

    if (length < (expected_length + 4))
    {
        host_printf("Not enough data for system settings count!");
        if (parsed_options->patches)
        {
            free(parsed_options->patches);
        }
        free(parsed_options);
        return 0;
    }

    memcpy(&parsed_options->system_settings_count, &data[expected_length], 4);
    expected_length += 4;

    if (parsed_options->system_settings_count > 0)
    {
        parsed_options->system_settings = malloc(sizeof(setting_t) * parsed_options->system_settings_count);

        for (unsigned int settingno = 0; settingno < parsed_options->system_settings_count; settingno ++)
        {
            if (parse_setting(data, length, &parsed_options->system_settings[settingno], &expected_length) == 0)
            {
                host_printf("Not enough data for system setting %d!", settingno);

                for (unsigned int fs = 0; fs < settingno; fs++)
                {
                    free_setting(&parsed_options->system_settings[fs]);
                }

                free(parsed_options->system_settings);
                if (parsed_options->patches)
                {
                    free(parsed_options->patches);
                }
                free(parsed_options);
            }
        }
    }

    memcpy(&parsed_options->game_settings_count, &data[expected_length], 4);
    expected_length += 4;

    if (parsed_options->game_settings_count > 0)
    {
        parsed_options->game_settings = malloc(sizeof(setting_t) * parsed_options->game_settings_count);

        for (unsigned int settingno = 0; settingno < parsed_options->game_settings_count; settingno ++)
        {
            if (parse_setting(data, length, &parsed_options->game_settings[settingno], &expected_length) == 0)
            {
                host_printf("Not enough data for game setting %d!", settingno);

                for (unsigned int fs = 0; fs < settingno; fs++)
                {
                    free_setting(&parsed_options->game_settings[fs]);
                }

                free(parsed_options->game_settings);

                for (unsigned int fs = 0; fs < parsed_options->system_settings_count; fs++)
                {
                    free_setting(&parsed_options->system_settings[fs]);
                }

                if (parsed_options->system_settings)
                {
                    free(parsed_options->system_settings);
                }
                if (parsed_options->patches)
                {
                    free(parsed_options->patches);
                }
                free(parsed_options);
            }
        }
    }

    return parsed_options;
}

void send_game_options(game_options_t *parsed_options)
{
    unsigned int total_length = 8 + (parsed_options->patch_count * 4);
    unsigned int current_loc = 0;
    uint8_t *senddata = malloc(total_length);

    // Basic stuff.
    memcpy(&senddata[current_loc], &parsed_options->selected_game, 4);
    current_loc += 4;

    memcpy(&senddata[current_loc], &parsed_options->patch_count, 4);
    current_loc += 4;

    // Now, send back the patch selection.
    for (unsigned int patchno = 0; patchno < parsed_options->patch_count; patchno++)
    {
        memcpy(&senddata[current_loc], &parsed_options->patches[patchno].enabled, 4);
        current_loc += 4;
    }

    // Send it and free our buffer.
    message_send(MESSAGE_SAVE_SETTINGS_DATA, senddata, total_length);
    free(senddata);
}

#define SCREEN_MAIN_MENU 0
#define SCREEN_COMM_ERROR 1
#define SCREEN_GAME_SETTINGS_LOAD 2
#define SCREEN_GAME_SETTINGS 3
#define SCREEN_GAME_SETTINGS_SAVE 4
#define SCREEN_CONFIGURATION 5
#define SCREEN_CONFIGURATION_SAVE 6
#define SCREEN_GAME_LOAD 7

#define MAX_WAIT_FOR_COMMS 3.0
#define MAX_WAIT_FOR_SAVE 5.0

#define ERROR_BOX_WIDTH 300
#define ERROR_BOX_HEIGHT 50
#define ERROR_BOX_TOP 100

void display_test_error(state_t *state)
{
    unsigned int halfwidth = video_width() / 2;
    video_fill_box(
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        rgb(32, 32, 32)
    );
    video_draw_box(
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        rgb(255, 0, 0)
    );

    video_draw_text(
        halfwidth - (ERROR_BOX_WIDTH / 2) + 22,
        ERROR_BOX_TOP + 10,
        state->font_12pt,
        rgb(255, 0, 0),
        "Cannot edit menu settings on this screen!"
    );
    video_draw_text(
        halfwidth - (ERROR_BOX_WIDTH / 2) + 12,
        ERROR_BOX_TOP + 25,
        state->font_12pt,
        rgb(255, 0, 0),
        "Please edit settings from the main menu only!"
    );
}

extern unsigned int up_png_width;
extern unsigned int up_png_height;
extern void *up_png_data;

extern unsigned int dn_png_width;
extern unsigned int dn_png_height;
extern void *dn_png_data;

extern unsigned int cursor_png_width;
extern unsigned int cursor_png_height;
extern void *cursor_png_data;

unsigned int main_menu(state_t *state, int reinit)
{
    // Grab our configuration.
    static unsigned int count = 0;
    static games_list_t *games = 0;

    // Leave 24 pixels of padding on top and bottom of the games list.
    // Space out games 16 pixels across.
    static unsigned int maxgames = 0;

    // Where we are on the screen for both our cursor and scroll position.
    static unsigned int cursor = 0;
    static unsigned int top = 0;

    // Whether we're currently waiting to be rebooted for a game to send to us.
    static unsigned int controls_locked = 0;
    static unsigned int booting = 0;
    static double booting_animation = 0.0;
    static unsigned int holding = 0;
    static double holding_animation = 0.0;

    if (reinit)
    {
        games = get_games_list(&count);
        maxgames = (video_height() - (24 + 16)) / 21;
        if (selected_game < 0)
        {
            selected_game = state->config->boot_selection;
        }
        cursor = selected_game;
        top = 0;
        if (cursor >= (top + maxgames))
        {
            top = cursor - (maxgames - 1);
        }
        controls_locked = 0;
        booting = 0;
        booting_animation = 0.0;
        holding = 0;
        holding_animation = 0.0;

        // Clear any error screens.
        state->test_error_counter = 0.0;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_MAIN_MENU;

    // Get our controls, including repeats.
    controls_t controls = get_controls(state, reinit);

    if (controls.test_pressed)
    {
        // Request to go into our configuration screen.
        if (booting == 0 && holding == 0)
        {
            selected_game = cursor;
            new_screen = SCREEN_CONFIGURATION;
        }
    }
    else
    {
        if (controls.start_pressed)
        {
            // Possibly long-pressing to get into game settings menu.
            if (!controls_locked)
            {
                controls_locked = 1;
                if (booting == 0 && holding == 0)
                {
                    holding = 1;
                    holding_animation = state->animation_counter;
                }
            }
        }
        if (controls.start_released)
        {
            if (booting == 0 && holding == 1)
            {
                // Made a selection!
                booting = 1;
                holding = 0;
                booting_animation = state->animation_counter;
                message_send(MESSAGE_SELECTION, &cursor, 4);
            }
            else if(booting == 1)
            {
                // Ignore everything, we're waiting to boot at this point.
            }
            else
            {
                // Somehow got here, maybe start held on another screen?
                booting = 0;
                holding = 0;
                controls_locked = 0;
            }
        }
        if (!controls_locked)
        {
            if (controls.up_pressed)
            {
                // Moved cursor up.
                if (cursor > 0)
                {
                    cursor --;
                }
                if (cursor < top)
                {
                    top = cursor;
                }
            }
            else if (controls.down_pressed)
            {
                // Moved cursor down.
                if (cursor < (count - 1))
                {
                    cursor ++;
                }
                if (cursor >= (top + maxgames))
                {
                    top = cursor - (maxgames - 1);
                }
            }
        }
    }

    // See if we got any messages from the host.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // Grab the current progress so we can display it.
                memcpy(&sending_game_size, &data[0], 4);
                new_screen = SCREEN_GAME_LOAD;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }
    }

    // Now, render the actual list of games.
    {
        unsigned int scroll_indicator_move_amount[4] = { 1, 2, 1, 0 };
        int scroll_offset = scroll_indicator_move_amount[((int)(state->animation_counter * 4.0)) & 0x3];
        int cursor_offset = 0;

        if (holding > 0)
        {
            unsigned int cursor_move_amount[10] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
            unsigned int which = (int)((state->animation_counter - holding_animation) * 10.0);
            if (which >= 10)
            {
                // Held for 1 second, so lets go to game settings.
                selected_game = cursor;
                if (new_screen != SCREEN_GAME_LOAD)
                {
                    new_screen = SCREEN_GAME_SETTINGS_LOAD;
                }
                which = 9;
            }
            cursor_offset = cursor_move_amount[which];
        }

        if (booting > 0 && new_screen != SCREEN_GAME_LOAD)
        {
            if ((state->animation_counter - holding_animation) >= MAX_WAIT_FOR_COMMS)
            {
                // We failed to boot, display an error.
                new_screen = SCREEN_COMM_ERROR;
            }
        }

        if (top > 0)
        {
            video_draw_sprite(video_width() / 2 - 10, 10 - scroll_offset, up_png_width, up_png_height, up_png_data);
        }

        for (unsigned int game = top; game < top + maxgames; game++)
        {
            if (game >= count)
            {
                // Ran out of games to display.
                break;
            }

            // Draw cursor itself.
            if (game == cursor && (!booting))
            {
                video_draw_sprite(24 + cursor_offset, 24 + ((game - top) * 21), cursor_png_width, cursor_png_height, cursor_png_data);
            }

            unsigned int away = abs(game - cursor);
            int horizontal_offset = 0;
            if (away > 0 && booting > 0)
            {
                // How far behind should this animation play? this means that the animation plays in
                // waves starting at the cursor and fanning out.
                double x = ((state->animation_counter - booting_animation) * 1.25) - (((double)away) * 0.1);
                if (x <= 0)
                {
                    horizontal_offset = 0;
                }
                else
                {
                    // Reduce to half wave by 10 away from the cursor. This makes the animation less
                    // pronounced the further away it gets.
                    double coeff = -(900.0 - 450.0 * ((double)(away >= 10 ? 10 : away) / 10.0));

                    // Quadratic equation that puts the text in the same spot at 0.6 seconds into the
                    // animation, and has a maximum positive horizontal displacement of ~90 pixels.
                    // Of course this gets flattened the further out from the cursor you go, due to the
                    // above coeff calculation.
                    horizontal_offset = (int)((coeff * x) * (x - 0.6));
                }
            }

            // Draw game, highlighted if it is selected.
            video_draw_text(48 + horizontal_offset, 22 + ((game - top) * 21), state->font_18pt, game == cursor ? rgb(255, 255, 20) : rgb(255, 255, 255), games[game].name);
        }

        if ((top + maxgames) < count)
        {
            video_draw_sprite(video_width() / 2 - 10, 24 + (maxgames * 21) + scroll_offset, dn_png_width, dn_png_height, dn_png_data);
        }
    }

    return new_screen;
}

unsigned int game_settings_load(state_t *state, int reinit)
{
    static double load_start = 0.0;
    static unsigned int ack_received = 0;

    if (reinit)
    {
        // Attempt to fetch the game settings for this game.
        uint32_t which_game = selected_game;
        message_send(MESSAGE_LOAD_SETTINGS, &which_game, 4);
        load_start = state->animation_counter;
        ack_received = 0;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_GAME_SETTINGS_LOAD;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    // Check to see if we got a response in time.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_LOAD_SETTINGS_ACK && length == 4)
            {
                uint32_t which_game;
                memcpy(&which_game, data, 4);

                if (which_game == selected_game)
                {
                    // Menu got our request, it should be gathering and sending settings
                    // to us at the moment.
                    ack_received = 1;
                }
            }
            else if (type == MESSAGE_LOAD_SETTINGS_DATA)
            {
                game_options = parse_game_options(data, length);
                if (game_options == 0)
                {
                    // Uh oh, failed to parse data.
                    new_screen = SCREEN_COMM_ERROR;
                    host_printf("Failed to parse game settings!");
                }
                if (game_options->selected_game != selected_game)
                {
                    // Uh oh, failed to parse data.
                    new_screen = SCREEN_COMM_ERROR;
                    host_printf("Wrong game settings returned!");
                }

                new_screen = SCREEN_GAME_SETTINGS;
            }
            else if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // Grab the current progress so we can display it.
                memcpy(&sending_game_size, &data[0], 4);
                new_screen = SCREEN_GAME_LOAD;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }

        if ((!ack_received) && ((state->animation_counter - load_start) >= MAX_WAIT_FOR_COMMS))
        {
            // Uh oh, no ack.
            new_screen = SCREEN_COMM_ERROR;
        }
    }

    video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(0, 255, 0), "Fetching game settings...");

    return new_screen;
}

unsigned int game_settings(state_t *state, int reinit)
{
    static unsigned int cursor = 0;
    static unsigned int total = 0;
    static unsigned int top = 0;
    static unsigned int maxoptions = 0;

    if (reinit)
    {
        cursor = 0;
        top = 0;
        maxoptions = (video_height() - (24 + 16 + 21 + 21 + 21)) / 21;

        // Calculate total options.
        total = game_options->patch_count + game_options->system_settings_count + game_options->game_settings_count + 3;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_GAME_SETTINGS;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }
    else if(controls.up_pressed)
    {
        if (cursor > 0)
        {
            cursor--;
        }
    }
    else if(controls.down_pressed)
    {
        if (cursor < (total - 1))
        {
            cursor++;
        }
    }
    else if(controls.left_pressed)
    {
        /*if (options[cursor] > 0)
        {
            options[cursor]--;
        }*/
    }
    else if(controls.right_pressed)
    {
        /*if (options[cursor] < maximums[cursor])
        {
            options[cursor]++;
        }*/
    }
    else if(controls.start_pressed)
    {
        if (cursor < game_options->patch_count)
        {
            // Toggle enabled patch.
            unsigned int patchcursor = cursor;
            game_options->patches[patchcursor].enabled = game_options->patches[patchcursor].enabled ? 0 : 1;
        }
        else if (cursor < (game_options->patch_count + game_options->system_settings_count))
        {
            // TODO
            //unsigned int systemcursor = cursor - game_options->patch_count;
        }
        else if (cursor < (game_options->patch_count + game_options->system_settings_count + game_options->game_settings_count))
        {
            // TODO
            //unsigned int gamecursor = cursor - (game_options->patch_count - game_options->system_settings_count);
        }
        else
        {
            unsigned int menucursor = cursor - (game_options->patch_count + game_options->system_settings_count + game_options->game_settings_count);
            switch (menucursor)
            {
                case 0:
                {
                    // Send our updated options back to the host.
                    send_game_options(game_options);

                    // Send a request to boot the game.
                    message_send(MESSAGE_SELECTION, &game_options->selected_game, 4);
                    expecting_boot = 1;
                    new_screen = SCREEN_GAME_SETTINGS_SAVE;

                    break;
                }
                case 1:
                {
                    // Send our updated options back to the host.
                    send_game_options(game_options);
                    expecting_boot = 0;
                    new_screen = SCREEN_GAME_SETTINGS_SAVE;

                    break;
                }
                case 2:
                {
                    // Just go back to the main menu.
                    selected_game = game_options->selected_game;
                    new_screen = SCREEN_MAIN_MENU;

                    break;
                }
            }
        }
    }

    // See if we got any messages from the host.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // Grab the current progress so we can display it.
                memcpy(&sending_game_size, &data[0], 4);
                new_screen = SCREEN_GAME_LOAD;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }
    }

    // Actually draw the menu
    {
        video_draw_text(video_width() / 2 - 70, 22, state->font_18pt, rgb(0, 255, 255), "Game Configuration");

        for (unsigned int option = top; option < top + maxoptions; option++)
        {
            if (option >= total)
            {
                // Ran out of options to display.
                break;
            }

            // Draw cursor itself.
            if (option == cursor)
            {
                video_draw_sprite(24, 24 + 21 + 21 + ((option - top) * 21), cursor_png_width, cursor_png_height, cursor_png_data);
            }

            uint32_t option_color = (option == cursor ? rgb(255, 255, 20) : rgb(255, 255, 255));

            // Draw the menu entry itself.
            if (option < game_options->patch_count)
            {
                unsigned int patchoption = option;

                video_draw_character(
                    48,
                    22 + 21 + 21 + ((option - top) * 21),
                    state->font_18pt,
                    option_color,
                    0x2610   // Ballot box unicode glyph.
                );

                if (game_options->patches[patchoption].enabled)
                {
                    video_draw_character(
                        48 + 2,
                        22 + 21 + 21 + ((option - top) * 21),
                        state->font_18pt,
                        option_color,
                        0x2713  // Checkbox unicode glyph.
                    );
                }

                video_draw_text(
                    48 + 24,
                    22 + 21 + 21 + ((option - top) * 21),
                    state->font_18pt,
                    option_color,
                    game_options->patches[patchoption].description
                );
            }
            else if (option < (game_options->patch_count + game_options->system_settings_count))
            {
                // TODO
                //unsigned int systemoption = option - game_options->patch_count;
            }
            else if (option < (game_options->patch_count + game_options->system_settings_count + game_options->game_settings_count))
            {
                // TODO
                //unsigned int gameoption = option - (game_options->patch_count - game_options->system_settings_count);
            }
            else
            {
                unsigned int menuoption = option - (game_options->patch_count + game_options->system_settings_count + game_options->game_settings_count);
                switch (menuoption)
                {
                    case 0:
                        video_draw_text(
                            48,
                            22 + 21 + 21 + ((option - top) * 21),
                            state->font_18pt,
                            option_color,
                            "save and launch game"
                        );
                        break;
                    case 1:
                        video_draw_text(
                            48,
                            22 + 21 + 21 + ((option - top) * 21),
                            state->font_18pt,
                            option_color,
                            "save and go back to main menu"
                        );
                        break;
                    case 2:
                        video_draw_text(
                            48,
                            22 + 21 + 21 + ((option - top) * 21),
                            state->font_18pt,
                            option_color,
                            "go back to main menu without saving"
                        );
                        break;
                    default:
                        video_draw_text(
                            48,
                            22 + 21 + 21 + ((option - top) * 21),
                            state->font_18pt,
                            option_color,
                            "WTF?"
                        );
                        break;
                }
            }
        }

        // Draw asterisk for some settings.
        if (game_options->game_settings_count == 0)
        {
            video_draw_text(48, 22 + 21 + 21 + (maxoptions * 21), state->font_12pt, rgb(255, 255, 255), "Game EEPROM settings are not available for this game!");
        }
    }

    if (new_screen != SCREEN_GAME_SETTINGS)
    {
        free_game_options(game_options);
        game_options = 0;
    }

    return new_screen;
}

unsigned int game_settings_save(state_t *state, int reinit)
{
    static double load_start = 0.0;
    static double boot_start = 0.0;

    if (reinit)
    {
        // Attempt to fetch the game settings for this game.
        load_start = state->animation_counter;
        boot_start = 0.0;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_GAME_SETTINGS_SAVE;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    // Check to see if we got a response in time.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_SAVE_SETTINGS_ACK && length == 0)
            {
                // Successfully acknowledged, time to go back to main screen.
                if (expecting_boot)
                {
                    boot_start = state->animation_counter;
                }
                else
                {
                    new_screen = SCREEN_MAIN_MENU;
                }
            }
            else if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // Grab the current progress so we can display it.
                memcpy(&sending_game_size, &data[0], 4);
                new_screen = SCREEN_GAME_LOAD;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }

        if (((state->animation_counter - load_start) >= MAX_WAIT_FOR_SAVE))
        {
            // Uh oh, no ack.
            new_screen = SCREEN_COMM_ERROR;
        }
        if (boot_start > 0.0 && ((state->animation_counter - boot_start) >= MAX_WAIT_FOR_COMMS))
        {
            // Uh oh, no boot.
            new_screen = SCREEN_COMM_ERROR;
        }
    }

    video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(0, 255, 0), "Saving game settings...");

    return new_screen;
}

unsigned int comm_error(state_t *state, int reinit)
{
    if (reinit)
    {
        // Nothing to re-init, this screen is stuck. If we get here it means
        // the menu software on the other side is gone so there is no point in
        // trying to do anything.
    }

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    video_draw_text(video_width() / 2 - 50, 100, state->font_18pt, rgb(255, 0, 0), "Comm Error!");
    video_draw_text(
        video_width() / 2 - 130,
        130,
        state->font_12pt,
        rgb(255, 255, 255),
        "We seem to have lost communication with the\n"
        "controlling software! Cycle your cabinet power\n"
        "and run the menu software to try again!"
    );

    return SCREEN_COMM_ERROR;
}

unsigned int configuration(state_t *state, int reinit)
{
    static uint32_t options[7];
    static uint32_t maximums[7];
    static uint32_t lockable[7];
    static uint32_t disabled[7];
    static unsigned int cursor = 0;
    static unsigned int top = 0;
    static unsigned int maxoptions = 0;
    static int locked = -1;

    static uint8_t joy1_hcenter;
    static uint8_t joy1_vcenter;
    static uint8_t joy2_hcenter;
    static uint8_t joy2_vcenter;
    static uint8_t joy1_hmin;
    static uint8_t joy1_hmax;
    static uint8_t joy1_vmin;
    static uint8_t joy1_vmax;
    static uint8_t joy2_hmin;
    static uint8_t joy2_hmax;
    static uint8_t joy2_vmin;
    static uint8_t joy2_vmax;

    if (reinit)
    {
        options[0] = state->config->enable_analog;
        options[1] = state->config->system_region;
        options[2] = state->config->use_filenames;
        options[3] = 0;
        options[4] = 0;
        maximums[0] = 1;
        maximums[1] = 3;
        maximums[2] = 1;
        maximums[3] = 0;
        maximums[4] = 0;
        lockable[0] = 0;
        lockable[1] = 0;
        lockable[2] = 0;
        lockable[3] = 1;
        lockable[4] = 1;
        disabled[0] = 0;
        disabled[1] = 0;
        disabled[2] = 0;
        disabled[3] = 0;
        disabled[4] = state->settings->system.players == 1;

        // Dummy options for save and exit.
        options[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        options[((sizeof(options) / sizeof(options[0])) - 2)] = 0;
        maximums[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        maximums[((sizeof(options) / sizeof(options[0])) - 2)] = 0;
        lockable[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        lockable[((sizeof(options) / sizeof(options[0])) - 2)] = 0;
        disabled[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        disabled[((sizeof(options) / sizeof(options[0])) - 2)] = 0;

        // Calibration special case.
        joy1_hcenter = state->config->joy1_hcenter;
        joy1_vcenter = state->config->joy1_vcenter;
        joy2_hcenter = state->config->joy2_hcenter;
        joy2_vcenter = state->config->joy2_vcenter;
        joy1_hmin = state->config->joy1_hmin;
        joy1_hmax = state->config->joy1_hmax;
        joy1_vmin = state->config->joy1_vmin;
        joy1_vmax = state->config->joy1_vmax;
        joy2_hmin = state->config->joy2_hmin;
        joy2_hmax = state->config->joy2_hmax;
        joy2_vmin = state->config->joy2_vmin;
        joy2_vmax = state->config->joy2_vmax;

        cursor = 0;
        top = 0;
        maxoptions = (video_height() - (24 + 16 + 21 + 21)) / 21;
        locked = -1;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_CONFIGURATION;

    // Calculate disabled controls.
    if (options[0])
    {
        disabled[3] = 0;
        disabled[4] = state->settings->system.players == 1;
    }
    else
    {
        disabled[3] = 1;
        disabled[4] = 1;
    }

    // Get our controls, including repeats.
    controls_t controls = get_controls(state, reinit);

    if (controls.test_pressed)
    {
        // Test cycles as a safeguard.
        if (cursor == ((sizeof(options) / sizeof(options[0])) - 1))
        {
            // Exit without save.
            new_screen = SCREEN_MAIN_MENU;
        }
        else if (cursor == ((sizeof(options) / sizeof(options[0])) - 2))
        {
            // Exit with save.
            new_screen = SCREEN_MAIN_MENU;

            state->config->enable_analog = options[0];
            state->config->system_region = options[1];
            state->config->use_filenames = options[2];

            // Calibration special case.
            state->config->joy1_hcenter = joy1_hcenter;
            state->config->joy1_vcenter = joy1_vcenter;
            state->config->joy2_hcenter = joy2_hcenter;
            state->config->joy2_vcenter = joy2_vcenter;
            state->config->joy1_hmin = joy1_hmin;
            state->config->joy1_hmax = joy1_hmax;
            state->config->joy1_vmin = joy1_vmin;
            state->config->joy1_vmax = joy1_vmax;
            state->config->joy2_hmin = joy2_hmin;
            state->config->joy2_hmax = joy2_hmax;
            state->config->joy2_vmin = joy2_vmin;
            state->config->joy2_vmax = joy2_vmax;

            // Send back to PC.
            message_send(MESSAGE_SAVE_CONFIG, state->config, 64);
            new_screen = SCREEN_CONFIGURATION_SAVE;
        }
        else if (!disabled[cursor])
        {
            if (lockable[cursor])
            {
                if (cursor == locked)
                {
                    // Unlock control.
                    locked = -1;
                }
                else
                {
                    // Lock to this control.
                    locked = cursor;
                }
            }
            else if (locked == -1)
            {
                // Only edit controls if locking is diabled.
                if (options[cursor] < maximums[cursor])
                {
                    options[cursor]++;
                }
                else
                {
                    options[cursor] = 0;
                }
            }
        }
    }
    else if (controls.start_pressed)
    {
        // Test cycles as a safeguard.
        if (cursor == ((sizeof(options) / sizeof(options[0])) - 1))
        {
            // Exit without save.
            new_screen = SCREEN_MAIN_MENU;
        }
        else if (cursor == ((sizeof(options) / sizeof(options[0])) - 2))
        {
            // Exit with save.
            new_screen = SCREEN_MAIN_MENU;

            state->config->enable_analog = options[0];
            state->config->system_region = options[1];
            state->config->use_filenames = options[2];

            // Calibration special case.
            state->config->joy1_hcenter = joy1_hcenter;
            state->config->joy1_vcenter = joy1_vcenter;
            state->config->joy2_hcenter = joy2_hcenter;
            state->config->joy2_vcenter = joy2_vcenter;
            state->config->joy1_hmin = joy1_hmin;
            state->config->joy1_hmax = joy1_hmax;
            state->config->joy1_vmin = joy1_vmin;
            state->config->joy1_vmax = joy1_vmax;
            state->config->joy2_hmin = joy2_hmin;
            state->config->joy2_hmax = joy2_hmax;
            state->config->joy2_vmin = joy2_vmin;
            state->config->joy2_vmax = joy2_vmax;

            // Send back to PC.
            message_send(MESSAGE_SAVE_CONFIG, state->config, 64);
            new_screen = SCREEN_CONFIGURATION_SAVE;
        }
        else if (!disabled[cursor])
        {
            if (lockable[cursor])
            {
                if (cursor == locked)
                {
                    // Unlock control.
                    locked = -1;
                }
                else
                {
                    // Lock to this control.
                    locked = cursor;
                }
            }
        }
    }
    else if(locked == -1)
    {
        if(controls.up_pressed)
        {
            if (cursor > 0)
            {
                cursor--;
            }
        }
        else if(controls.down_pressed)
        {
            if (cursor < ((sizeof(options) / sizeof(options[0])) - 1))
            {
                cursor++;
            }
        }
        else if(controls.service_pressed)
        {
            // Service cycles as a safeguard.
            if (cursor < ((sizeof(options) / sizeof(options[0])) - 1))
            {
                cursor++;
            }
            else
            {
                cursor = 0;
            }
        }
        else if (!disabled[cursor])
        {
            if(controls.left_pressed)
            {
                if (options[cursor] > 0)
                {
                    options[cursor]--;
                }
            }
            else if(controls.right_pressed)
            {
                if (options[cursor] < maximums[cursor])
                {
                    options[cursor]++;
                }
            }
        }
    }

    if (locked == 3)
    {
        // 1P calibration.
        jvs_buttons_t held = maple_buttons_current();

        joy1_vcenter = held.player1.analog1;
        joy1_hcenter = held.player1.analog2;

        joy1_hmin = min(joy1_hmin, joy1_hcenter);
        joy1_hmax = max(joy1_hmax, joy1_hcenter);
        joy1_vmin = min(joy1_vmin, joy1_vcenter);
        joy1_vmax = max(joy1_vmax, joy1_vcenter);
    }
    else if (locked == 4)
    {
        // 2P calibration.
        jvs_buttons_t held = maple_buttons_current();

        joy2_vcenter = held.player2.analog1;
        joy2_hcenter = held.player2.analog2;

        joy2_hmin = min(joy2_hmin, joy2_hcenter);
        joy2_hmax = max(joy2_hmax, joy2_hcenter);
        joy2_vmin = min(joy2_vmin, joy2_vcenter);
        joy2_vmax = max(joy2_vmax, joy2_vcenter);
    }

    // See if we got any messages from the host.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // Grab the current progress so we can display it.
                memcpy(&sending_game_size, &data[0], 4);
                new_screen = SCREEN_GAME_LOAD;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }
    }

    // Actually draw the menu
    {
        video_draw_text(video_width() / 2 - 70, 22, state->font_18pt, rgb(0, 255, 255), "Menu Configuration");

        for (unsigned int option = top; option < top + maxoptions; option++)
        {
            if (option >= (sizeof(options) / sizeof(options[0])))
            {
                // Ran out of options to display.
                break;
            }

            // Draw cursor itself.
            if (option == cursor && locked == -1)
            {
                video_draw_sprite(24, 24 + 21 + ((option - top) * 21), cursor_png_width, cursor_png_height, cursor_png_data);
            }

            // Draw option, highlighted if it is selected.
            char buffer[64];
            switch(option)
            {
                case 0:
                {
                    // Enable analog
                    sprintf(buffer, "Analog controls: %s", options[option] ? "enabled" : "disabled");
                    break;
                }
                case 1:
                {
                    // System region
                    char *regions[4] = {"japan", "usa", "export", "korea"};
                    sprintf(buffer, "Naomi region: %s*", regions[options[option]]);
                    break;
                }
                case 2:
                {
                    // Filename display
                    sprintf(buffer, "Game name display: %s*", options[option] ? "from filename" : "from ROM");
                    break;
                }
                case 3:
                {
                    if (locked == 3)
                    {
                        // 1P analog calibration
                        sprintf(
                            buffer,
                            "h: %02X, v: %02X, max: %02X %02X %02X %02X",
                            joy1_hcenter,
                            joy1_vcenter,
                            joy1_hmin,
                            joy1_hmax,
                            joy1_vmin,
                            joy1_vmax
                        );
                    }
                    else
                    {
                        strcpy(buffer, "Player 1 analog calibration");
                    }
                    break;
                }
                case 4:
                {
                    if (locked == 4)
                    {
                        // 2P analog calibration
                        sprintf(
                            buffer,
                            "h: %02X, v: %02X, max: %02X %02X %02X %02X",
                            joy2_hcenter,
                            joy2_vcenter,
                            joy2_hmin,
                            joy2_hmax,
                            joy2_vmin,
                            joy2_vmax
                        );
                    }
                    else
                    {
                        strcpy(buffer, "Player 2 analog calibration");
                    }
                    break;
                }
                case ((sizeof(options) / sizeof(options[0])) - 2):
                {
                    // Save and exit display
                    strcpy(buffer, "Save and exit");
                    break;
                }
                case ((sizeof(options) / sizeof(options[0])) - 1):
                {
                    // Save and exit display
                    strcpy(buffer, "Exit without save");
                    break;
                }
                default:
                {
                    // Uh oh??
                    strcpy(buffer, "WTF?");
                    break;
                }
            }

            video_draw_text(
                48,
                22 + 21 + ((option - top) * 21),
                state->font_18pt,
                disabled[option] ? rgb(128, 128, 128) : (option == cursor ? (cursor == locked ? rgb(0, 255, 0) : rgb(255, 255, 20)) : rgb(255, 255, 255)),
                buffer
            );
        }

        // Draw asterisk for some settings.
        video_draw_text(48, 22 + 21 + (maxoptions * 21), state->font_12pt, rgb(255, 255, 255), "Options marked with an asterisk (*) take effect only on the next boot.");
    }

    return new_screen;
}

unsigned int configuration_save(state_t *state, int reinit)
{
    static double load_start = 0.0;

    if (reinit)
    {
        // Attempt to fetch the game settings for this game.
        load_start = state->animation_counter;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_CONFIGURATION_SAVE;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    // Check to see if we got a response in time.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_SAVE_CONFIG_ACK && length == 0)
            {
                // Successfully acknowledged, time to go back to main screen.
                new_screen = SCREEN_MAIN_MENU;
            }
            else if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // Grab the current progress so we can display it.
                memcpy(&sending_game_size, &data[0], 4);
                new_screen = SCREEN_GAME_LOAD;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }

        if (((state->animation_counter - load_start) >= MAX_WAIT_FOR_SAVE))
        {
            // Uh oh, no ack.
            new_screen = SCREEN_COMM_ERROR;
        }
    }

    video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(0, 255, 0), "Saving configuration...");

    return new_screen;
}

unsigned int game_load(state_t *state, int reinit)
{
    static double load_start = 0.0;
    static int width = 0;
    static int game_size = 0;
    static int game_progress = 0;

    if (reinit)
    {
        // Attempt to load the game to DIMM.
        load_start = state->animation_counter;

        // The size of our progress bar, minus 50 pixels on each side.
        width = video_width() - 100;

        game_size = sending_game_size;
        game_progress = 0;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_GAME_LOAD;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    // Check the scratch registers in the comms code to see if we were updated on progress.
    {
        if (packetlib_read_scratch1() != game_progress)
        {
            // We got some progress, reset our timeout.
            load_start = state->animation_counter;
            game_progress = packetlib_read_scratch1();
        }
    }

    // Check to see if we got a response in time.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_LOAD_PROGRESS && length == 8)
            {
                // We got some progress, reset our timeout.
                load_start = state->animation_counter;

                // Grab the current progress so we can display it.
                memcpy(&game_size, &data[0], 4);
                memcpy(&game_progress, &data[4], 4);
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }

        if (((state->animation_counter - load_start) >= MAX_WAIT_FOR_COMMS))
        {
            // Uh oh, no ack.
            new_screen = SCREEN_COMM_ERROR;
        }
    }

    // Draw the progress bar and percentage.
    {
        video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(255, 255, 255), "Loading game...");
        video_fill_box(50, 150, 50 + width, 170, rgb(32, 32, 32));
        video_draw_box(50, 150, 50 + width, 170, rgb(255, 255, 255));

        int actual_percent = 0;
        if (game_size > 0)
        {
            int actual_width = (int)(((double)game_progress / (double)game_size) * (width - 2));
            video_fill_box(51, 151, 51 + actual_width, 169, rgb(0, 0, 255));

            actual_percent = (int)(((double)game_progress / (double)game_size) * 100);
        }

        video_draw_text(video_width() / 2 - 10, 154, state->font_12pt, rgb(255, 255, 255), "%d%%", actual_percent);
    }

    return new_screen;
}

void display_error_dialogs(state_t *state)
{
    if (state->test_error_counter > 0.0)
    {
        // Only display for 3 seconds.
        if ((state->animation_counter - state->test_error_counter) >= 3.0)
        {
            state->test_error_counter = 0.0;
        }
        else
        {
            display_test_error(state);
        }
    }
}

void draw_screen(state_t *state)
{
    // What screen we're on right now.
    static unsigned int curscreen = SCREEN_MAIN_MENU;
    static unsigned int oldscreen = -1;

    // The screen we are requested to go to next.
    unsigned int newscreen;

    switch(curscreen)
    {
        case SCREEN_MAIN_MENU:
            newscreen = main_menu(state, curscreen != oldscreen);
            break;
        case SCREEN_GAME_SETTINGS_LOAD:
            newscreen = game_settings_load(state, curscreen != oldscreen);
            break;
        case SCREEN_GAME_SETTINGS:
            newscreen = game_settings(state, curscreen != oldscreen);
            break;
        case SCREEN_GAME_SETTINGS_SAVE:
            newscreen = game_settings_save(state, curscreen != oldscreen);
            break;
        case SCREEN_COMM_ERROR:
            newscreen = comm_error(state, curscreen != oldscreen);
            break;
        case SCREEN_CONFIGURATION:
            newscreen = configuration(state, curscreen != oldscreen);
            break;
        case SCREEN_CONFIGURATION_SAVE:
            newscreen = configuration_save(state, curscreen != oldscreen);
            break;
        case SCREEN_GAME_LOAD:
            newscreen = game_load(state, curscreen != oldscreen);
            break;
        default:
            // Should never happen, but still, whatever.
            newscreen = curscreen;
            break;
    }

    // Draw any error dialog boxes we should see above any screens.
    display_error_dialogs(state);

    // Track what screen we are versus what we were so we know when we
    // switch screens.
    oldscreen = curscreen;
    curscreen = newscreen;
}