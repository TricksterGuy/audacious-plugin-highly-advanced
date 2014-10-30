#include <libaudcore/i18n.h>
#include <libaudcore/input.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>

extern "C" int LengthFromString(const char * timestring);

struct GSFPreferences
{
    bool loop_forever;
    bool ignore_track_length;
    int track_length;
    bool detect_silence;
    int silence_length;
    bool enable_low_pass_filter;
} preferences;

void debugMessage(const char *str)
{
#ifdef DEBUG
    timeval curTime;
    gettimeofday(&curTime, NULL);
    int milli = curTime.tv_usec / 1000;

    char buffer [80];
    strftime(buffer, 80, "%H:%M:%S", localtime(&curTime.tv_sec));

    char currentTime[84] = "";
    sprintf(currentTime, "%s:%d", buffer, milli);
    printf("[%s] Debug: %s\n", currentTime, str);
#endif
}

extern "C" {
    #include "VBA/psftag.h"
    #include "gsf.h"
}

extern "C" {
    int defvolume = 1000;
    int relvolume = 1000;
    int TrackLength = 0;
    int FadeLength = 0;
    int IgnoreTrackLength, DefaultLength = 150000;
    int playforever = 0;
    int TrailingSilence = 1000;
    int DetectSilence = 0, silencedetected = 0, silencelength = 5;
}

int cpupercent = 0, sndSamplesPerSec, sndNumChannels;
int sndBitsPerSample = 16;

int deflen = 120, deffade = 4;

extern unsigned short soundFinalWave[1470];
extern int soundBufferLen;

extern char soundEcho;
extern char soundLowPass;
extern char soundReverse;
extern char soundQuality;

double decode_pos_ms; // current decoding position, in milliseconds
int seek_needed; // if != -1, it is the point that the decode thread should seek to, in ms.

#define CFG_ID "highlyadvanced"  //ID for storing in audacious

#define DEFAULT_ENABLE_LOW_PASS_FILTER "1"
#define DEFAULT_DETECT_SILENCE "1"
#define DEFAULT_SILENCE_LENGTH "5"
#define DEFAULT_IGNORE_TRACK_LENGTH "0"
#define DEFAULT_TRACK_LENGTH "150" // in seconds
#define DEFAULT_LOOP_FOREVER "1"

static const char* const defaults[] =
{
    "enable_low_pass_filter", DEFAULT_ENABLE_LOW_PASS_FILTER,
    "detect_silence",         DEFAULT_DETECT_SILENCE,
    "silence_length",         DEFAULT_SILENCE_LENGTH,
    "ignore_track_length",    DEFAULT_IGNORE_TRACK_LENGTH,
    "track_length",           DEFAULT_TRACK_LENGTH,
    "loop_forever",           DEFAULT_LOOP_FOREVER,
    NULL
};

void update_gsf_settings()
{
    playforever = preferences.loop_forever;
    IgnoreTrackLength = preferences.ignore_track_length;
    DefaultLength = preferences.track_length * 1000;
    DetectSilence = preferences.detect_silence;
    silencelength = preferences.silence_length;
    soundLowPass = preferences.enable_low_pass_filter;
}

void gsf_cfg_load()
{
    debugMessage("cfg_load called");
    aud_config_set_defaults(CFG_ID, defaults);

    preferences.loop_forever = aud_get_bool(CFG_ID, "loop_forever");
    preferences.ignore_track_length = aud_get_int(CFG_ID, "ignore_track_length");
    preferences.track_length = aud_get_int(CFG_ID, "track_length") * 1000;
    preferences.detect_silence = aud_get_bool(CFG_ID, "detect_silence");
    preferences.silence_length = aud_get_int(CFG_ID, "silence_length");
    preferences.enable_low_pass_filter = aud_get_bool(CFG_ID, "enable_low_pass_filter");
    update_gsf_settings();
}

void gsf_cfg_save()
{
    debugMessage("cfg_save called");
    aud_set_bool(CFG_ID, "loop_forever", preferences.loop_forever);
    aud_set_int(CFG_ID, "ignore_track_length", preferences.ignore_track_length);
    aud_set_int(CFG_ID, "track_length", preferences.track_length);
    aud_set_bool(CFG_ID, "detect_silence", preferences.detect_silence);
    aud_set_int(CFG_ID, "silence_length", preferences.silence_length);
    aud_set_bool(CFG_ID, "enable_low_pass_filter", preferences.enable_low_pass_filter);
}

bool gsf_init()
{
    debugMessage("init");
    gsf_cfg_load();
    debugMessage("after load cfg");
    return true;
}

void gsf_cleanup()
{
    debugMessage("cleanup");
    gsf_cfg_save();
}

extern "C" void end_of_track()
{
}

extern "C" void writeSound(void)
{
	int ret = soundBufferLen;
    aud_input_write_audio(soundFinalWave, ret);
	decode_pos_ms += (ret / (2 * sndNumChannels) * 1000.0f) / sndSamplesPerSec;
}

extern "C" void signal_handler(int sig)
{
}

bool gsf_play(const char * filename, VFSFile * file)
{
    decode_pos_ms = 0;
    seek_needed = -1;
    TrailingSilence = 1000;

    int r = GSFRun(filename);
    if (!r) return false;

    aud_input_set_bitrate(sndSamplesPerSec * 2 * sndNumChannels);

    if (!aud_input_open_audio(FMT_S16_LE, sndSamplesPerSec, sndNumChannels))
        return false;

    while (!aud_input_check_stop())
    {
        int seek_value = aud_input_check_seek();
        if (seek_value > 0)
        {
            if (seek_value < decode_pos_ms)
            {
                GSFClose();
                r = GSFRun(filename);
                if (!r) return false;
            }
            seek_needed = seek_value;
        }

        EmulationLoop();
    }

    GSFClose();

    return true;
}

// called every time the user adds a new file to playlist
Tuple gsf_probe_for_tuple(const char *filename, VFSFile *file)
{
    debugMessage("probe for tuple");
    Tuple tuple;

    char tag[50001];
    char tmp_str[256];
    psftag_readfromfile(tag, filename);

    tuple.set_filename(filename);

    if (!psftag_getvar(tag, "title", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(FIELD_TITLE, tmp_str);

    if (!psftag_getvar(tag, "artist", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(FIELD_ARTIST, tmp_str);

    if (!psftag_getvar(tag, "game", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(FIELD_ALBUM, tmp_str);

    if (!psftag_getvar(tag, "year", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(FIELD_DATE, tmp_str);

    if (!psftag_getvar(tag, "copyright", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(FIELD_COPYRIGHT, tmp_str);

    if (!psftag_getvar(tag, "tagger", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(-1, tmp_str);

    if (!psftag_raw_getvar(tag, "length", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_int(FIELD_LENGTH, LengthFromString(tmp_str) + FadeLength);

    if (!psftag_getvar(tag, "comment", tmp_str, sizeof(tmp_str) - 1))
        tuple.set_str(FIELD_COMMENT, tmp_str);

    tuple.set_str(FIELD_CODEC, "GameBoy Advanced Audio (GSF)");
    tuple.set_str(FIELD_QUALITY, "sequenced");

    return tuple;
}

const char gsf_about[] =
{
  "audacious-highly-advanced version: 1.0\n\n"
  "ported to audacious 3.5.1 by Brandon Whitehead\n"
};

static const PreferencesWidget gsf_widgets[] = {
    WidgetLabel(N_("<b>HighlyAdvanced Config</b>")),
    WidgetCheck(N_("Loop Forever:"), WidgetBool(preferences.loop_forever)),
    WidgetCheck(N_("Ignore Track Length:"), WidgetBool(preferences.ignore_track_length)),
    WidgetSpin(N_("Track Length:"), WidgetInt(preferences.track_length), {1, 2147483647, 1}),
    WidgetCheck(N_("Detect Silence:"), WidgetBool(preferences.detect_silence)),
    WidgetSpin(N_("Silence Length:"), WidgetInt(preferences.silence_length), {1, 2147483647, 1}),
    WidgetCheck(N_("Enable Low Pass Filter:"), WidgetBool(preferences.enable_low_pass_filter)),
};

static const PluginPreferences gsf_prefs = {
    {gsf_widgets},
    gsf_cfg_load,
    gsf_cfg_save
};

const char *gsf_exts[] =
{
    "gsf",
    "minigsf",
};

#define AUD_PLUGIN_NAME        N_("Highly Advanced")
#define AUD_PLUGIN_DOMAIN      "gsf"
#define AUD_PLUGIN_ABOUT       gsf_about
#define AUD_PLUGIN_INIT        gsf_init
#define AUD_PLUGIN_CLEANUP     gsf_cleanup
#define AUD_PLUGIN_PREFS       & gsf_prefs
#define AUD_INPUT_IS_OUR_FILE  nullptr
#define AUD_INPUT_PLAY         gsf_play
#define AUD_INPUT_READ_TUPLE   gsf_probe_for_tuple
#define AUD_INPUT_EXTS         gsf_exts

#define AUD_DECLARE_INPUT
#include <libaudcore/plugin-declare.h>

