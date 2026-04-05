#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <conio.h>
#include <cbm.h>
#include <peekpoke.h>

#define LIVE_BUFFER_ADDR      0xC800u
#define LIVE_MAGIC            0xA5u
#define POLL_TICKS            (CLOCKS_PER_SEC / 5u)
#define POLL_INTERVAL_UNIT_S  15u
#define STALE_POLL_LIMIT      900u //Fallback live timeout in seconds for 15-minute API updates
#define STALE_STATUS_GRACE    5u   //Extra seconds beyond the PHP poll interval before showing WAITING
#define SHOW_DEBUG_BUFFER     0u   //Set to 0u to hide from the screen
#define SHOW_DEBUG_SEQ        0u   //Set to 0u to hide from the screen
#define SCREEN_WIDTH          40u
#define TITLE_TEXT            "C64U LIVE WEATHER"
#define QUIT_TEXT             "PRESS ANY KEY TO QUIT"

typedef struct LiveData {
    unsigned char seq;
    signed int    temp_c;
    unsigned int  humidity;
    unsigned int  wind_kph;
    unsigned int  rain_mm;
    unsigned char icon_code;
    char          temp_unit;
    unsigned int  poll_interval_s;
    char          title[21];
    char          location[21];
    char          condition[17];
    char          stamp[17];
} LiveData;

static void copy_text (char* dst, const char* src, unsigned char max_len)
{
    strncpy (dst, src, max_len - 1u);
    dst[max_len - 1u] = '\0';
}

static void read_field (char* dst,
                        const unsigned char* src,
                        unsigned char len,
                        unsigned char dst_len)
{
    unsigned char i;

    for (i = 0u; i < len && i < (unsigned char) (dst_len - 1u); ++i) {
        char ch = (char) src[i];

        if (ch == '\0') {
            break;
        }

        dst[i] = ch;
    }

    dst[i] = '\0';

    while (i > 0u && dst[i - 1u] == ' ') {
        --i;
        dst[i] = '\0';
    }
}

static void set_defaults (LiveData* data)
{
    data->seq = 0u;
    data->temp_c = 0;
    data->humidity = 0u;
    data->wind_kph = 0u;
    data->rain_mm = 0u;
    data->icon_code = 1u;
    data->temp_unit = 'C';
    data->poll_interval_s = 0u;
    copy_text (data->title, "OPEN METEO", sizeof (data->title));
    copy_text (data->location, "ULTIMATE API", sizeof (data->location));
    copy_text (data->condition, "WAITING", sizeof (data->condition));
    copy_text (data->stamp, "NO PUSH YET", sizeof (data->stamp));
}

static unsigned char load_live_data (LiveData* data)
{
    const unsigned char* raw = (const unsigned char*) LIVE_BUFFER_ADDR;

    set_defaults (data);

    if (raw[0] != LIVE_MAGIC) {
        return 0u;
    }

    data->seq = raw[1];
    data->temp_c = (signed int) raw[2] - 100;
    data->humidity = raw[3];
    data->wind_kph = raw[4];
    data->rain_mm = raw[5];
    data->icon_code = raw[6];

    if (raw[7] == 0x46u || raw[7] == 0x66u) {
        data->temp_unit = 'F';
        data->poll_interval_s = 0u;
    } else {
        data->temp_unit = (raw[7] & 0x01u) != 0u ? 'F' : 'C';
        data->poll_interval_s = (unsigned int) ((raw[7] >> 1u) * POLL_INTERVAL_UNIT_S);
    }

    read_field (data->title, &raw[8], 20u, sizeof (data->title));
    read_field (data->location, &raw[28], 20u, sizeof (data->location));
    read_field (data->condition, &raw[48], 16u, sizeof (data->condition));
    read_field (data->stamp, &raw[64], 16u, sizeof (data->stamp));

    return 1u;
}

static clock_t get_live_timeout_ticks (const LiveData* data)
{
    unsigned int timeout_seconds = STALE_POLL_LIMIT;

    if (data->poll_interval_s > timeout_seconds) {
        timeout_seconds = data->poll_interval_s;
    }

    if (timeout_seconds == 0u) {
        return 0;
    }

    timeout_seconds += STALE_STATUS_GRACE;
    return (clock_t) timeout_seconds * CLOCKS_PER_SEC;
}

static void draw_frame (void)
{
    unsigned char i;

    textcolor (COLOR_LIGHTBLUE);

    for (i = 0u; i < 40u; ++i) {
        cputcxy (i, 0u, '*');
        cputcxy (i, 24u, '*');
    }

    for (i = 1u; i < 24u; ++i) {
        cputcxy (0u, i, '*');
        cputcxy (39u, i, '*');
    }
}

static void clear_block (unsigned char x, unsigned char y, unsigned char width, unsigned char height)
{
    unsigned char row;
    unsigned char col;

    for (row = 0u; row < height; ++row) {
        for (col = 0u; col < width; ++col) {
            cputcxy ((unsigned char) (x + col), (unsigned char) (y + row), ' ');
        }
    }
}

static void draw_condition_icon (unsigned char icon_code)
{
    clear_block (3u, 6u, 9u, 4u);

    switch (icon_code) {
        case 2u:
        case 3u:
            textcolor (COLOR_WHITE);
            cputsxy (3u, 6u, "  .--.  ");
            cputsxy (3u, 7u, ".(____).");
            textcolor (icon_code == 3u ? COLOR_LIGHTRED : COLOR_CYAN);
            cputsxy (3u, 8u, " / / /'  ");
            break;

        case 4u:
        case 5u:
        case 1u:
            textcolor (COLOR_WHITE);
            cputsxy (3u, 6u, "   .--. ");
            cputsxy (3u, 7u, " .(    ).");
            cputsxy (3u, 8u, "(___.__) ");
            break;

        default:
            textcolor (COLOR_YELLOW);
            cputsxy (3u, 6u, " \\|/  ");
            cputsxy (3u, 7u, "--O-- ");
            cputsxy (3u, 8u, " /|\\  ");
            break;
    }
}

static void draw_meter (unsigned char x,
                        unsigned char y,
                        const char* label,
                        signed int value,
                        unsigned int max_value,
                        unsigned char color)
{
    unsigned int  scaled_value;
    unsigned char bar_width;
    unsigned char i;

    if (value < 0) {
        scaled_value = 0u;
    } else {
        scaled_value = (unsigned int) value;
    }

    if (scaled_value > max_value) {
        scaled_value = max_value;
    }

    bar_width = (unsigned char) ((scaled_value * 18u) / max_value);

    textcolor (COLOR_WHITE);
    cputsxy (x, y, label);
    gotoxy ((unsigned char) (x + 8u), y);
    cprintf ("%3d", value);

    textcolor (color);
    revers (1);
    for (i = 0u; i < bar_width; ++i) {
        cputcxy ((unsigned char) (x + 13u + i), y, ' ');
    }
    revers (0);

    textcolor (COLOR_LIGHTBLUE);
    for (; i < 18u; ++i) {
        cputcxy ((unsigned char) (x + 13u + i), y, '.');
    }

    textcolor (COLOR_WHITE);
    gotoxy ((unsigned char) (x + 32u), y);
    cprintf ("%3u", max_value);
}

static void render_dashboard (const LiveData* data, unsigned char has_live_push)
{
    const char*  temp_label = "TEMP C";
    const char*  unit_text = "C";
    unsigned int temp_scale = 45u;

    if (data->temp_unit == 'F') {
        temp_label = "TEMP F";
        unit_text = "F";
        temp_scale = 120u;
    }

    clrscr ();
    bordercolor (COLOR_BLUE);
    bgcolor (COLOR_BLACK);
    textcolor (COLOR_WHITE);

    draw_frame ();

    textcolor (COLOR_YELLOW);
    cputsxy ((SCREEN_WIDTH - (sizeof (TITLE_TEXT) - 1u)) / 2u, 1u, TITLE_TEXT);

    textcolor (COLOR_CYAN);
    cputsxy (2u, 3u, "FEED    :");
    cputsxy (2u, 4u, "LOCATION:");
    cputsxy (2u, 5u, "UPDATED :");

    textcolor (COLOR_WHITE);
    cputsxy (11u, 3u, data->title);
    cputsxy (11u, 4u, data->location);
    cputsxy (11u, 5u, data->stamp);

    draw_condition_icon (data->icon_code);

    textcolor (COLOR_LIGHTRED);
    cputsxy (15u, 7u, "TEMP NOW");
    gotoxy (16u, 8u);
    cprintf ("%3d", data->temp_c);
    cputsxy (19u, 8u, " ");
    cputsxy (20u, 8u, unit_text);

    textcolor (COLOR_LIGHTGREEN);
    cputsxy (15u, 10u, data->condition);

    draw_meter (2u, 13u, temp_label, data->temp_c, temp_scale, COLOR_LIGHTRED);
    draw_meter (2u, 15u, "HUMIDITY ", (signed int) data->humidity, 100u, COLOR_CYAN);
    draw_meter (2u, 17u, "WIND  ", (signed int) data->wind_kph, 80u, COLOR_YELLOW);
    draw_meter (2u, 19u, "RAIN  ", (signed int) data->rain_mm, 40u, COLOR_LIGHTGREEN);

    textcolor (COLOR_CYAN);
    clear_block (2u, 21u, 36u, 2u);

#if SHOW_DEBUG_BUFFER
    cputsxy (2u, 21u, "BUFFER:$C800");
#endif

#if SHOW_DEBUG_SEQ
    gotoxy (28u, 21u);
    cprintf ("SEQ %3u", data->seq);
#endif

    if (has_live_push) {
        textcolor (COLOR_LIGHTGREEN);
        cputsxy (4u, 22u, "LIVE DATA ACTIVE");
    } else {
        textcolor (COLOR_ORANGE);
        cputsxy (4u, 22u, "WAITING FOR LIVE DATA...");
    }

    textcolor (COLOR_WHITE);
    cputsxy ((SCREEN_WIDTH - (sizeof (QUIT_TEXT) - 1u)) / 2u, 23u, QUIT_TEXT);
}

static void draw_activity_indicator (unsigned char has_live_push, unsigned char phase)
{
    unsigned char color;

    switch (phase % 6u) {
        case 0u:
            color = COLOR_LIGHTGREEN;
            break;
        case 1u:
            color = COLOR_CYAN;
            break;
        case 2u:
            color = COLOR_YELLOW;
            break;
        case 3u:
            color = COLOR_LIGHTBLUE;
            break;
        case 4u:
            color = COLOR_WHITE;
            break;
        default:
            color = COLOR_LIGHTRED;
            break;
    }

    textcolor (has_live_push ? color : COLOR_ORANGE);
    revers (1);
    cputcxy (2u, 22u, ' ');
    revers (0);
}

static unsigned char idle_wait (void)
{
    clock_t deadline = clock () + POLL_TICKS;

    while (clock () < deadline) {
        if (kbhit ()) {
            (void) cgetc ();
            return 1u;
        }
    }

    return 0u;
}

int main (void)
{
    LiveData      data;
    LiveData      previous_data;
    unsigned char has_live_push;
    unsigned char display_live_status = 0u;
    unsigned char activity_phase = 0u;
    clock_t       stale_deadline = 0;

    set_defaults (&data);
    set_defaults (&previous_data);
    render_dashboard (&data, 0u);
    draw_activity_indicator (0u, 0u);

    while (1) {
        has_live_push = load_live_data (&data);

        if (has_live_push) {
            if (memcmp (&data, &previous_data, sizeof (LiveData)) != 0) {
                clock_t timeout_ticks = get_live_timeout_ticks (&data);

                display_live_status = 1u;
                stale_deadline = timeout_ticks != 0 ? clock () + timeout_ticks : 0;
                render_dashboard (&data, 1u);
                previous_data = data;
            } else if (display_live_status != 0u && stale_deadline != 0 && clock () >= stale_deadline) {
                display_live_status = 0u;
                render_dashboard (&data, 0u);
            }
        } else {
            stale_deadline = 0;

            if (display_live_status != 0u || memcmp (&data, &previous_data, sizeof (LiveData)) != 0) {
                set_defaults (&data);
                render_dashboard (&data, 0u);
                previous_data = data;
                display_live_status = 0u;
            }
        }

        draw_activity_indicator (display_live_status, activity_phase);
        activity_phase = (unsigned char) ((activity_phase + 1u) % 6u);

        if (idle_wait ()) {
            break;
        }
    }

    clrscr ();
    return EXIT_SUCCESS;
}

