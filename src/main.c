#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <conio.h>
#include <cbm.h>
#include <peekpoke.h>

#define LIVE_BUFFER_ADDR      0xC800u
#define LIVE_MAGIC            0xA5u
#define LIVE_PACKET_SIZE      80u
#define POLL_TICKS            (CLOCKS_PER_SEC / 5u)
#define POLL_INTERVAL_UNIT_S  15u
#define STALE_POLL_LIMIT      900u //Fallback live timeout in seconds for 15-minute API updates
#define STALE_STATUS_GRACE    5u   //Extra seconds beyond the PHP poll interval before showing WAITING
#define SHOW_DEBUG_BUFFER     0u   //Set to 0u to hide from the screen
#define SHOW_DEBUG_SEQ        0u   //Set to 0u to hide from the screen
#define SCREEN_WIDTH           40u
#define MEATLOAF_DEVICE_NUM    16u /* Change if your Meatloaf network device uses a different IEC ID */
#define MEATLOAF_CHANNEL       2u
#define MEATLOAF_CMD_CHANNEL   15u
#define MEATLOAF_FETCH_TICKS   (CLOCKS_PER_SEC * STALE_POLL_LIMIT)
#define MEATLOAF_RETRY_TICKS   MEATLOAF_FETCH_TICKS
#define FEED_TEXT              "api.open-meteo.com"
#define MEATLOAF_LOCATION_TEXT "NEW YORK"
#define MEATLOAF_DEFAULT_LAT   "40.7128"
#define MEATLOAF_DEFAULT_LON   "-74.0060"
#define MEATLOAF_TEMP_UNIT     'F'
#define MEATLOAF_API_PREFIX_1      "https://api.open-meteo.com/v1/"
#define MEATLOAF_CONFIG_DEVICE        8u
#define MEATLOAF_CONFIG_CHAN          4u
#define MEATLOAF_CONFIG_FILE_READ     "live-weather.cfg,s,r"
#define MEATLOAF_CONFIG_FILE_WRITE    "live-weather.cfg,s,w"
#define MEATLOAF_CONFIG_FILE_REPLACE  "@0:live-weather.cfg,s,w"
#define MEATLOAF_JSON_USCORE          "\x5f"
#define TITLE_TEXT                 "C64 LIVE WEATHER"
#define QUIT_TEXT              "PRESS ANY KEY TO QUIT"

typedef struct LiveData {
    unsigned char seq;
    signed int    temp_c;
    unsigned int  humidity;
    unsigned int  wind_kph;
    unsigned int  rain_mm;
    unsigned char icon_code;
    char          temp_unit;
    unsigned int  poll_interval_s;
    char          feed[21];
    char          location[21];
    char          condition[17];
    char          stamp[17];
} LiveData;

typedef struct MeatloafConfig {
    char location[21];
    char lat[16];
    char lon[16];
    char temp_unit;
} MeatloafConfig;

enum {
    SOURCE_MODE_ULTIMATE = 0u,
    SOURCE_MODE_MEATLOAF = 1u
};

static unsigned char  selected_source_mode = SOURCE_MODE_MEATLOAF;
static MeatloafConfig meatloaf_config;
static char           meatloaf_api_prefix2[128];
static char           meatloaf_api_tail[160];
static char           meatloaf_config_buffer[192];

static void          trim_field_value (char* value);
static unsigned char read_meatloaf_command_status (const char* command, char* status, unsigned char max_len);

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

static void uppercase_text (char* text)
{
    while (*text != '\0') {
        if (*text >= 'a' && *text <= 'z') {
            *text = (char) (*text - ('a' - 'A'));
        }
        ++text;
    }
}

static void set_meatloaf_config_defaults (MeatloafConfig* config)
{
    copy_text (config->location, MEATLOAF_LOCATION_TEXT, sizeof (config->location));
    copy_text (config->lat, MEATLOAF_DEFAULT_LAT, sizeof (config->lat));
    copy_text (config->lon, MEATLOAF_DEFAULT_LON, sizeof (config->lon));
    config->temp_unit = MEATLOAF_TEMP_UNIT;
}

static void append_text (char* dst, const char* src, size_t max_len)
{
    strncat (dst, src, max_len - strlen (dst) - 1u);
}

static void build_meatloaf_api_request (const MeatloafConfig* config)
{
    meatloaf_api_prefix2[0] = '\0';
    append_text (meatloaf_api_prefix2, "cd,2,forecast?latitude=", sizeof (meatloaf_api_prefix2));
    append_text (meatloaf_api_prefix2, config->lat, sizeof (meatloaf_api_prefix2));
    append_text (meatloaf_api_prefix2, "&longitude=", sizeof (meatloaf_api_prefix2));
    append_text (meatloaf_api_prefix2, config->lon, sizeof (meatloaf_api_prefix2));
    append_text (meatloaf_api_prefix2, "&current=temperature%5f2m", sizeof (meatloaf_api_prefix2));

    meatloaf_api_tail[0] = '\0';
    append_text (meatloaf_api_tail, ",relative%5fhumidity%5f2m,wind%5fspeed%5f10m,precipitation,weather%5fcode", sizeof (meatloaf_api_tail));
    append_text (meatloaf_api_tail, "&wind%5fspeed%5funit=kmh&temperature%5funit=", sizeof (meatloaf_api_tail));
    append_text (meatloaf_api_tail, config->temp_unit == 'F' ? "fahrenheit" : "celsius", sizeof (meatloaf_api_tail));
    append_text (meatloaf_api_tail, "&timezone=auto", sizeof (meatloaf_api_tail));
}

static void apply_meatloaf_config_line (MeatloafConfig* config, char* line)
{
    char* sep;
    char* value;

    trim_field_value (line);
    if (line[0] == '\0' || line[0] == '#') {
        return;
    }

    sep = strchr (line, '=');
    if (sep == 0) {
        return;
    }

    *sep = '\0';
    value = sep + 1;

    trim_field_value (line);
    trim_field_value (value);
    uppercase_text (line);

    if (strcmp (line, "FEED") == 0 || strcmp (line, "TITLE") == 0) {
        return;
    } else if (strcmp (line, "LOCATION") == 0) {
        copy_text (config->location, value, sizeof (config->location));
        uppercase_text (config->location);
    } else if (strcmp (line, "LAT") == 0 || strcmp (line, "LATITUDE") == 0) {
        copy_text (config->lat, value, sizeof (config->lat));
    } else if (strcmp (line, "LON") == 0 || strcmp (line, "LONGITUDE") == 0) {
        copy_text (config->lon, value, sizeof (config->lon));
    } else if (strcmp (line, "TEMP_UNIT") == 0 || strcmp (line, "UNITS") == 0) {
        config->temp_unit = (value[0] == 'f' || value[0] == 'F') ? 'F' : 'C';
    }
}

static void load_meatloaf_config (MeatloafConfig* config)
{
    unsigned int i;
    unsigned int start = 0u;
    int          count;

    set_meatloaf_config_defaults (config);

    if (cbm_open (MEATLOAF_CONFIG_CHAN, MEATLOAF_CONFIG_DEVICE, 2u, MEATLOAF_CONFIG_FILE_READ) != 0) {
        cbm_close (MEATLOAF_CONFIG_CHAN);
        return;
    }

    count = cbm_read (MEATLOAF_CONFIG_CHAN, (unsigned char*) meatloaf_config_buffer, sizeof (meatloaf_config_buffer) - 1u);
    cbm_close (MEATLOAF_CONFIG_CHAN);

    if (count <= 0) {
        return;
    }

    meatloaf_config_buffer[count] = '\0';

    for (i = 0u; ; ++i) {
        char ch = meatloaf_config_buffer[i];

        if (ch == '\r' || ch == '\n' || ch == '\0') {
            meatloaf_config_buffer[i] = '\0';
            apply_meatloaf_config_line (config, meatloaf_config_buffer + start);

            if (ch == '\0') {
                break;
            }

            if (ch == '\r' && meatloaf_config_buffer[i + 1u] == '\n') {
                ++i;
            }

            start = i + 1u;
        }
    }
}

static unsigned char save_meatloaf_config (const MeatloafConfig* config)
{
    int    count;
    size_t len;

    strcpy (meatloaf_config_buffer, "LOCATION=");
    append_text (meatloaf_config_buffer, config->location, sizeof (meatloaf_config_buffer));
    append_text (meatloaf_config_buffer, "\rLAT=", sizeof (meatloaf_config_buffer));
    append_text (meatloaf_config_buffer, config->lat, sizeof (meatloaf_config_buffer));
    append_text (meatloaf_config_buffer, "\rLON=", sizeof (meatloaf_config_buffer));
    append_text (meatloaf_config_buffer, config->lon, sizeof (meatloaf_config_buffer));
    append_text (meatloaf_config_buffer, "\rTEMP_UNIT=", sizeof (meatloaf_config_buffer));

    len = strlen (meatloaf_config_buffer);
    if (len + 2u >= sizeof (meatloaf_config_buffer)) {
        return 0u;
    }

    meatloaf_config_buffer[len++] = config->temp_unit;
    meatloaf_config_buffer[len++] = '\r';
    meatloaf_config_buffer[len] = '\0';

    if (cbm_open (MEATLOAF_CONFIG_CHAN, MEATLOAF_CONFIG_DEVICE, 2u, MEATLOAF_CONFIG_FILE_REPLACE) != 0) {
        cbm_close (MEATLOAF_CONFIG_CHAN);

        if (cbm_open (MEATLOAF_CONFIG_CHAN, MEATLOAF_CONFIG_DEVICE, 2u, MEATLOAF_CONFIG_FILE_WRITE) != 0) {
            cbm_close (MEATLOAF_CONFIG_CHAN);
            return 0u;
        }
    }

    count = cbm_write (MEATLOAF_CONFIG_CHAN, meatloaf_config_buffer, (unsigned int) len);
    cbm_close (MEATLOAF_CONFIG_CHAN);

    return count == (int) len;
}

static const char* get_source_mode_text (unsigned char source_mode)
{
    return source_mode == SOURCE_MODE_ULTIMATE ? "ULTIMATE" : "MEATLOAF";
}

static unsigned char detect_ultimate_packet (void)
{
    const unsigned char* raw = (const unsigned char*) LIVE_BUFFER_ADDR;

    return raw[0] == LIVE_MAGIC;
}

static unsigned char detect_meatloaf_device (void)
{
    char status[64];

    if (read_meatloaf_command_status ("cd,2", status, sizeof (status)) == 0u) {
        return 0u;
    }

    return strstr (status, "prefix cleared") != 0;
}

static unsigned char detect_default_source_mode (void)
{
    if (detect_meatloaf_device () != 0u) {
        return SOURCE_MODE_MEATLOAF;
    }

    if (detect_ultimate_packet () != 0u) {
        return SOURCE_MODE_ULTIMATE;
    }

    return SOURCE_MODE_MEATLOAF;
}

static unsigned char is_config_edit_char (char ch, unsigned char allow_letters)
{
    if ((ch >= '0' && ch <= '9') || ch == ' ' || ch == '.' || ch == '-' || ch == '/' || ch == ':') {
        return 1u;
    }

    if (allow_letters != 0u && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) {
        return 1u;
    }

    return 0u;
}

static void prompt_config_value (const char* label,
                                 char* value,
                                 unsigned char max_len,
                                 unsigned char allow_letters)
{
    unsigned char len = (unsigned char) strlen (value);

    while (1) {
        char ch;

        clrscr ();
        bordercolor (COLOR_BLUE);
        bgcolor (COLOR_BLACK);
        textcolor (COLOR_YELLOW);
        gotoxy (2u, 2u);
        cputs ("EDIT ");
        cputs (label);

        textcolor (COLOR_CYAN);
        cputsxy (2u, 4u, "RETURN=SAVE  DEL=BACKSPACE");

        textcolor (COLOR_WHITE);
        cputsxy (2u, 6u, value);

        ch = (char) cgetc ();
        if (ch == '\r' || ch == '\n') {
            break;
        }

        if (ch == 0x14 || ch == 0x08) {
            if (len > 0u) {
                value[--len] = '\0';
            }
            continue;
        }

        if (is_config_edit_char (ch, allow_letters) != 0u && len < (unsigned char) (max_len - 1u)) {
            if (ch >= 'a' && ch <= 'z') {
                ch = (char) (ch - ('a' - 'A'));
            }

            value[len++] = ch;
            value[len] = '\0';
        }
    }

    trim_field_value (value);
    if (allow_letters != 0u) {
        uppercase_text (value);
    }
}

static void show_startup_config_screen (void)
{
    selected_source_mode = detect_default_source_mode ();

    if (selected_source_mode == SOURCE_MODE_ULTIMATE) {
        build_meatloaf_api_request (&meatloaf_config);
        return;
    }

    while (1) {
        char ch;

        clrscr ();
        bordercolor (COLOR_BLUE);
        bgcolor (COLOR_BLACK);

        textcolor (COLOR_YELLOW);
        cputsxy (8u, 1u, "LIVE WEATHER SETUP");

        textcolor (COLOR_CYAN);
        cputsxy (2u, 3u, "M MODE   :");

        textcolor (COLOR_WHITE);
        cputsxy (13u, 3u, get_source_mode_text (selected_source_mode));

        if (selected_source_mode == SOURCE_MODE_MEATLOAF) {
            textcolor (COLOR_CYAN);
            cputsxy (2u, 4u, "FEED     :");
            cputsxy (2u, 6u, "L LOCN   :");
            cputsxy (2u, 7u, "A LAT    :");
            cputsxy (2u, 8u, "O LON    :");
            cputsxy (2u, 9u, "U UNITS  :");

            textcolor (COLOR_WHITE);
            cputsxy (13u, 4u, FEED_TEXT);
            cputsxy (13u, 6u, meatloaf_config.location);
            cputsxy (13u, 7u, meatloaf_config.lat);
            cputsxy (13u, 8u, meatloaf_config.lon);
            cputcxy (13u, 9u, meatloaf_config.temp_unit);

            textcolor (COLOR_LIGHTGREEN);
            cputsxy (2u, 12u, "RETURN SAVES+STARTS");

            textcolor (COLOR_LIGHTBLUE);
            cputsxy (2u, 14u, "EDIT WITH M/L/A/O/U");
        } else {
            textcolor (COLOR_LIGHTGREEN);
            cputsxy (2u, 12u, "RETURN TO START");

            textcolor (COLOR_LIGHTBLUE);
            cputsxy (2u, 16u, "CONFIGURE IN PHP. SEE README");
            cputsxy (2u, 17u, "ULTIMATE READS LIVE RAM DATA");
        }

        ch = (char) cgetc ();
        switch (ch) {
            case 'm':
            case 'M':
                selected_source_mode = selected_source_mode == SOURCE_MODE_ULTIMATE ? SOURCE_MODE_MEATLOAF : SOURCE_MODE_ULTIMATE;
                break;

            case 'l':
            case 'L':
                if (selected_source_mode == SOURCE_MODE_MEATLOAF) {
                    prompt_config_value ("LOCATION", meatloaf_config.location, sizeof (meatloaf_config.location), 1u);
                }
                break;

            case 'a':
            case 'A':
                if (selected_source_mode == SOURCE_MODE_MEATLOAF) {
                    prompt_config_value ("LATITUDE", meatloaf_config.lat, sizeof (meatloaf_config.lat), 0u);
                }
                break;

            case 'o':
            case 'O':
                if (selected_source_mode == SOURCE_MODE_MEATLOAF) {
                    prompt_config_value ("LONGITUDE", meatloaf_config.lon, sizeof (meatloaf_config.lon), 0u);
                }
                break;

            case 'u':
            case 'U':
                if (selected_source_mode == SOURCE_MODE_MEATLOAF) {
                    meatloaf_config.temp_unit = meatloaf_config.temp_unit == 'F' ? 'C' : 'F';
                }
                break;

            case '\r':
            case '\n':
                (void) save_meatloaf_config (&meatloaf_config);
                build_meatloaf_api_request (&meatloaf_config);
                return;
        }
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
    data->poll_interval_s = 0u;
    copy_text (data->feed, FEED_TEXT, sizeof (data->feed));
    copy_text (data->condition, "WAITING", sizeof (data->condition));
    copy_text (data->stamp, "NO PUSH YET", sizeof (data->stamp));

    if (selected_source_mode == SOURCE_MODE_MEATLOAF) {
        data->temp_unit = meatloaf_config.temp_unit;
        copy_text (data->location, meatloaf_config.location, sizeof (data->location));
    } else {
        data->temp_unit = 'C';
        copy_text (data->location, "ULTIMATE 64", sizeof (data->location));
    }
}

static const char* map_condition_text (unsigned char code)
{
    switch (code) {
        case 0u:
            return "CLEAR";
        case 1u:
        case 2u:
            return "PARTLY CLOUDY";
        case 3u:
            return "CLOUDY";
        case 45u:
        case 48u:
            return "FOG";
        case 51u:
        case 53u:
        case 55u:
        case 56u:
        case 57u:
        case 61u:
        case 63u:
        case 65u:
        case 66u:
        case 67u:
        case 80u:
        case 81u:
        case 82u:
            return "RAIN";
        case 71u:
        case 73u:
        case 75u:
        case 77u:
        case 85u:
        case 86u:
            return "SNOW";
        case 95u:
        case 96u:
        case 99u:
            return "STORM";
        default:
            return "MIXED";
    }
}

static unsigned char map_icon_code_from_weather (unsigned char code)
{
    switch (code) {
        case 0u:
            return 0u;
        case 1u:
        case 2u:
        case 3u:
            return 1u;
        case 45u:
        case 48u:
            return 4u;
        case 51u:
        case 53u:
        case 55u:
        case 56u:
        case 57u:
        case 61u:
        case 63u:
        case 65u:
        case 66u:
        case 67u:
        case 80u:
        case 81u:
        case 82u:
            return 2u;
        case 71u:
        case 73u:
        case 75u:
        case 77u:
        case 85u:
        case 86u:
            return 5u;
        case 95u:
        case 96u:
        case 99u:
            return 3u;
        default:
            return 1u;
    }
}

static void trim_field_value (char* value)
{
    size_t len = strlen (value);

    while (len > 0u && (value[len - 1u] == '\r' || value[len - 1u] == '\n' || value[len - 1u] == ' ' || value[len - 1u] == '"')) {
        value[--len] = '\0';
    }

    while (value[0] == ' ' || value[0] == '\r' || value[0] == '\n' || value[0] == '"') {
        memmove (value, value + 1, strlen (value));
    }
}

static unsigned char read_channel_value (unsigned char channel, char* value, unsigned int max_len)
{
    unsigned int total = 0u;
    int          count;

    if (max_len == 0u) {
        return 0u;
    }

    value[0] = '\0';

    do {
        unsigned int remaining = max_len - 1u - total;

        if (remaining == 0u) {
            break;
        }

        if (remaining > 192u) {
            remaining = 192u;
        }

        count = cbm_read (channel, (unsigned char*) value + total, remaining);
        if (count > 0) {
            total += (unsigned int) count;
        }
    } while (count > 0);

    value[total] = '\0';
    trim_field_value (value);
    return value[0] != '\0';
}

static unsigned char read_meatloaf_command_status (const char* command, char* status, unsigned char max_len)
{
    int count;

    if (max_len == 0u) {
        return 0u;
    }

    status[0] = '\0';

    if (cbm_open (MEATLOAF_CMD_CHANNEL, MEATLOAF_DEVICE_NUM, MEATLOAF_CMD_CHANNEL, command) != 0) {
        cbm_close (MEATLOAF_CMD_CHANNEL);
        return 0u;
    }

    count = cbm_read (MEATLOAF_CMD_CHANNEL, (unsigned char*) status, max_len - 1u);
    cbm_close (MEATLOAF_CMD_CHANNEL);

    if (count <= 0) {
        return 0u;
    }

    status[count] = '\0';
    return 1u;
}

static unsigned char send_meatloaf_command (const char* command)
{
    char status[64];
    int  status_code;

    if (read_meatloaf_command_status (command, status, sizeof (status)) == 0u) {
        return 0u;
    }

    status_code = atoi (status);

    if (status_code >= 0) {
        return 1u;
    }

    if (strstr (status, "prefix cleared") != 0) {
        return 1u;
    }

    return 0u;
}

static unsigned char query_meatloaf_value (const char* query, char* value, unsigned char max_len)
{
    char command[48];

    strcpy (command, "jq,2,");
    strncat (command, query, sizeof (command) - strlen (command) - 1u);

    if (!send_meatloaf_command (command)) {
        return 0u;
    }

    return read_channel_value (MEATLOAF_CHANNEL, value, max_len);
}

static unsigned char fetch_live_data_from_meatloaf (LiveData* data)
{
    static unsigned char sequence = 0u;
    char                 value[32];
    int                  parsed_value;
    int                  weather_code = 0;

    set_defaults (data);
    build_meatloaf_api_request (&meatloaf_config);
    copy_text (data->feed, FEED_TEXT, sizeof (data->feed));
    copy_text (data->location, meatloaf_config.location, sizeof (data->location));
    data->temp_unit = meatloaf_config.temp_unit;
    data->poll_interval_s = STALE_POLL_LIMIT;

    (void) send_meatloaf_command ("cd,2");

    if (!send_meatloaf_command ("cd,2," MEATLOAF_API_PREFIX_1) ||
        !send_meatloaf_command (meatloaf_api_prefix2)) {
        return 0u;
    }

    if (cbm_open (MEATLOAF_CHANNEL, MEATLOAF_DEVICE_NUM, MEATLOAF_CHANNEL, meatloaf_api_tail) != 0) {
        cbm_close (MEATLOAF_CHANNEL);
        (void) send_meatloaf_command ("cd,2");
        return 0u;
    }

    if (!send_meatloaf_command ("jsonparse,2")) {
        cbm_close (MEATLOAF_CHANNEL);
        (void) send_meatloaf_command ("cd,2");
        return 0u;
    }

    if (query_meatloaf_value ("/current/time", value, sizeof (value))) {
        copy_text (data->stamp, value, sizeof (data->stamp));
    } else {
        copy_text (data->stamp, "LIVE NOW", sizeof (data->stamp));
    }

    if (query_meatloaf_value ("/current/interval", value, sizeof (value))) {
        parsed_value = atoi (value);
        if (parsed_value > 0) {
            data->poll_interval_s = (unsigned int) parsed_value;
        }
    }

    if (query_meatloaf_value ("/current/temperature" MEATLOAF_JSON_USCORE "2m", value, sizeof (value))) {
        data->temp_c = atoi (value);
    }

    if (query_meatloaf_value ("/current/relative" MEATLOAF_JSON_USCORE "humidity" MEATLOAF_JSON_USCORE "2m", value, sizeof (value))) {
        data->humidity = (unsigned int) atoi (value);
    }

    if (query_meatloaf_value ("/current/wind" MEATLOAF_JSON_USCORE "speed" MEATLOAF_JSON_USCORE "10m", value, sizeof (value))) {
        data->wind_kph = (unsigned int) atoi (value);
    }

    if (query_meatloaf_value ("/current/precipitation", value, sizeof (value))) {
        data->rain_mm = (unsigned int) atoi (value);
    }

    if (query_meatloaf_value ("/current/weather" MEATLOAF_JSON_USCORE "code", value, sizeof (value))) {
        weather_code = atoi (value);
    }

    copy_text (data->condition, map_condition_text ((unsigned char) weather_code), sizeof (data->condition));
    data->icon_code = map_icon_code_from_weather ((unsigned char) weather_code);
    data->seq = ++sequence;

    cbm_close (MEATLOAF_CHANNEL);
    (void) send_meatloaf_command ("cd,2");

    return 1u;
}

static unsigned char parse_live_packet (LiveData* data, const unsigned char* raw)
{
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

    copy_text (data->feed, FEED_TEXT, sizeof (data->feed));
    read_field (data->location, &raw[28], 20u, sizeof (data->location));
    read_field (data->condition, &raw[48], 16u, sizeof (data->condition));
    read_field (data->stamp, &raw[64], 16u, sizeof (data->stamp));

    return 1u;
}

static unsigned char load_live_data_from_memory (LiveData* data)
{
    const unsigned char* raw = (const unsigned char*) LIVE_BUFFER_ADDR;

    return parse_live_packet (data, raw);
}

static unsigned char load_live_data_from_iec (LiveData* data)
{
    static LiveData      cached_data;
    static unsigned char have_cached_data = 0u;
    static clock_t       next_fetch_time = 0;

    if (clock () < next_fetch_time) {
        if (have_cached_data != 0u) {
            *data = cached_data;
            return 1u;
        }

        set_defaults (data);
        return 0u;
    }

    if (fetch_live_data_from_meatloaf (&cached_data) != 0u) {
        have_cached_data = 1u;
        next_fetch_time = clock () + MEATLOAF_FETCH_TICKS;
        *data = cached_data;
        return 1u;
    }

    next_fetch_time = clock () + MEATLOAF_RETRY_TICKS;

    if (have_cached_data != 0u) {
        *data = cached_data;
        return 1u;
    }

    set_defaults (data);
    return 0u;
}

static unsigned char load_live_data (LiveData* data)
{
    if (selected_source_mode == SOURCE_MODE_ULTIMATE) {
        return load_live_data_from_memory (data);
    }

    return load_live_data_from_iec (data);
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
    cputsxy (11u, 3u, data->feed);
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

    load_meatloaf_config (&meatloaf_config);
    show_startup_config_screen ();

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

