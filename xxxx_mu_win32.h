/*
 * @url: https://gist.github.com/pervognsen/6a67966c5dc4247a0021b95c8d0a7b72
 * @lang: c99
 * @platform: win32
 */

enum {
     MU_CTRL = 0x11,  // VK_CONTROL
     MU_ALT = 0x12,   // VK_MENU
     MU_SHIFT = 0x10, // VK_SHIFT
}

typedef void *HANDLE;
typedef struct _XINPUT_STATE XINPUT_STATE;
typedef unsigned long(__stdcall *XINPUTGETSTATE)(unsigned long dwUserIndex,
                                                 XINPUT_STATE *pState);

struct IAudioClient;
struct IAudioRenderClient;

struct Mu_Win32 {
    HANDLE window;
    HANDLE device_context;

    void *main_fiber;
    void *message_fiber;

    XINPUTGETSTATE xinput_get_state;

    struct IAudioClient *audio_client;
    struct IAudioRenderClient *audio_render_client;

    HANDLE wgl_context;
};
