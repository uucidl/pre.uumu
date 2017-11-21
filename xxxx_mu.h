/*
 * @url: https://gist.github.com/pervognsen/6a67966c5dc4247a0021b95c8d0a7b72
 * @lang: c99
 * @taglist: platform
 * @dependencylist: xxxx_mu_win32
 *
 * A streamlined platform layer for audio/visual interactive programs.
 *
 * API style is push/pull; coroutine; client/server type.
 */

#include <stdint.h>
#include <stdlib.h>

enum {
    MU_FALSE = 0,
    MU_TRUE = 1,
    MU_MAX_KEYS = 256,
    MU_MAX_TEXT = 256,
    MU_MAX_ERROR = 1024,
    MU_MAX_AUDIO_BUFFER = 2 * 1024
};

typedef uint8_t Mu_Bool;

struct Mu_Int2 {
    int x;
    int y;
};

struct Mu_DigitalButton {
    Mu_Bool down;
    Mu_Bool pressed;
    Mu_Bool released;
};

struct Mu_AnalogButton {
    float threshold;
    float value;
    Mu_Bool down;
    Mu_Bool pressed;
    Mu_Bool released;
};

struct Mu_Stick {
    float threshold;
    float x;
    float y;
};

struct Mu_Gamepad {
    Mu_Bool connected;
    struct Mu_DigitalButton a_button;
    struct Mu_DigitalButton b_button;
    struct Mu_DigitalButton x_button;
    struct Mu_DigitalButton y_button;
    struct Mu_AnalogButton left_trigger;
    struct Mu_AnalogButton right_trigger;
    struct Mu_DigitalButton left_shoulder_button;
    struct Mu_DigitalButton right_shoulder_button;
    struct Mu_DigitalButton up_button;
    struct Mu_DigitalButton down_button;
    struct Mu_DigitalButton left_button;
    struct Mu_DigitalButton right_button;
    struct Mu_Stick left_thumb_stick;
    struct Mu_Stick right_thumb_stick;
    struct Mu_DigitalButton left_thumb_button;
    struct Mu_DigitalButton right_thumb_button;
    struct Mu_DigitalButton back_button;
    struct Mu_DigitalButton start_button;
};

struct Mu_Mouse {
    struct Mu_DigitalButton left_button;
    struct Mu_DigitalButton right_button;
    int wheel;
    int delta_wheel;
    struct Mu_Int2 position;
    struct Mu_Int2 delta_position;
};

struct Mu_Window {
    char *title; // @todo: can be made `char const*`
    struct Mu_Int2 position;
    struct Mu_Int2 size;
    Mu_Bool resized;
};

struct Mu_AudioFormat {
    uint32_t samples_per_second; // number of "frames" of n=channels samples per second
    uint32_t channels;
    uint32_t bytes_per_sample;
};

struct Mu_AudioBuffer {
    int16_t *samples;
    size_t samples_count; // total number of samples in the interleaved buffer, with n=channels samples per frame
    struct Mu_AudioFormat format;
};

typedef void (*Mu_AudioCallback)(struct Mu_AudioBuffer *buffer);

struct Mu_Audio {
    struct Mu_AudioFormat format;
    Mu_AudioCallback callback;
};

struct Mu_Time {
    uint64_t delta_ticks;
    uint64_t delta_nanoseconds;
    uint64_t delta_microseconds;
    uint64_t delta_milliseconds;
    float delta_seconds;

    uint64_t ticks;
    uint64_t nanoseconds;
    uint64_t microseconds;
    uint64_t milliseconds;
    float seconds;

    uint64_t initial_ticks;
    uint64_t ticks_per_second;
};

/* @platform{win32} */ struct Mu_Win32;
/* @platform{macos} */ struct Mu_Cocoa;

struct Mu {
    Mu_Bool initialized;
    Mu_Bool quit;

    char *error;
    char error_buffer[MU_MAX_ERROR];

    struct Mu_Window window;
    struct Mu_DigitalButton keys[MU_MAX_KEYS];
    struct Mu_Gamepad gamepad;
    struct Mu_Mouse mouse;

    char text[MU_MAX_TEXT];
    size_t text_length;

    struct Mu_Time time;
    struct Mu_Audio audio;
    /* @platform{win32} */ struct Mu_Win32 *win32;
    /* @platform{macos} */ struct Mu_Cocoa *cocoa;
};

/*
 * The input struct MUST be initialized to zero and/or its part
 * customized. See `struct Mu_Window` for instance to customize the window to
 * create.
 *
 * @return: `MU_FALSE` on error, fills up `mu->error`
 */
Mu_Bool Mu_Initialize(struct Mu *mu);

/*
 * @return MU_FALSE on error, fills up mu->error
 */
Mu_Bool Mu_Pull(struct Mu *mu);

void Mu_Push(struct Mu *mu);

struct Mu_Image {
    uint8_t *pixels;
    uint32_t channels;
    uint32_t width;
    uint32_t height;
};

/*
 * @return: MU_FALSE on error
 */
Mu_Bool Mu_LoadImage(const char *filename, struct Mu_Image *image);

/*
 * @return: MU_FALSE on error
 */
Mu_Bool Mu_LoadAudio(const char *filename, struct Mu_AudioBuffer *audio);
