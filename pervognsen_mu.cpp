// @url: https://gist.github.com/pervognsen/6a67966c5dc4247a0021b95c8d0a7b72
//
// TODO(Nicolas): add WM_DEVICECHANGE support to work around potential performance issues
// with XInputGetState on disconnected controllers:
// @url: https://chromium.googlesource.com/chromium/src/+/58e56144d4831f31dba534d538bc7ae75fe03/content/browser/gamepad/data_fetcher_win.cc#98
// @url: https://chromium.googlesource.com/chromium/src/+/58e56144d4831f31dba534d538bc7ae75fe03/content/browser/system_message_window_win.cc#49
#define MU_EXTERN_BEGIN extern "C" {
#define MU_EXTERN_END }

MU_EXTERN_BEGIN
#include "xxxx_mu.h"
#include "xxxx_mu_win32.h"
MU_EXTERN_END

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <malloc.h>
#define _USE_MATH_DEFINES
#include <math.h>
#define _NO_CRT_STDIO_INLINE
#include <stdarg.h>
#include <stdio.h>
#define NO_STRICT
#include <audioclient.h>
#include <audiosessiontypes.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wincodec.h>
#include <windows.h>
#include <xinput.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif

#ifdef _DEBUG
#define Assert(x)                                                                                                                                              \
    if (!(x)) {                                                                                                                                                \
        MessageBoxA(0, #x, "Assertion Failed", MB_OK);                                                                                                         \
        __debugbreak();                                                                                                                                        \
    }
#else
#define Assert(x)
#endif

MU_EXTERN_BEGIN

void Mu_ExitWithError(Mu *mu) {
    MessageBoxA(0, mu->error, "Mu Error", MB_OK);
    ExitProcess(1);
}

void Mu_UpdateDigitalButton(Mu_DigitalButton *button, Mu_Bool down) {
    Mu_Bool was_down = button->down;
    button->down = down;
    button->pressed = !was_down && down;
    button->released = was_down && !down;
}

void Mu_UpdateDigitalButtonAndPreserveTransitions(Mu_DigitalButton *button, Mu_Bool down) {
    Mu_Bool was_down = button->down;
    button->down = down;
    button->pressed = button->pressed || (!was_down && down);
    button->released = button->released || (was_down && !down);
}

void Mu_UpdateAnalogButton(Mu_AnalogButton *button, float value) {
    button->value = value;
    Mu_Bool was_down = button->down;
    button->down = (value >= button->threshold);
    button->pressed = !was_down && button->down;
    button->released = was_down && !button->down;
}

void Mu_UpdateStick(Mu_Stick *stick, float x, float y) {
    if (fabs(x) <= stick->threshold) {
        x = 0.0f;
    }
    stick->x = x;

    if (fabs(y) <= stick->threshold) {
        y = 0.0f;
    }
    stick->y = y;
}

// Mu_Window

static LRESULT CALLBACK Mu_Window_Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT result = 0;
    Mu *mu = (Mu *)GetWindowLongPtrA(window, GWLP_USERDATA);
    switch (message) {
    case WM_NCCREATE: {
        // @feature{HiDPI}
        CREATESTRUCT *createstruct = (CREATESTRUCT *)lparam;
        SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)createstruct->lpCreateParams);
#if 0
        EnableNonClientDpiScaling(window); // @todo GetProcAddress
#endif
        result = DefWindowProcA(window, message, wparam, lparam);
    } break;

    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        SetWindowPos(window, 0, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
    } break;

    case WM_SIZE:
        mu->window.resized = MU_TRUE;
        if (auto dxgi_swap_chain = mu->win32->dxgi_swap_chain) {
            // TODO(nicolas): remember flags
            // TODO(nicolas): problem, this necessitates all references to the swap chain to
            // have been cleared, normally. See remarks at:
            // @url{https://msdn.microsoft.com/fr-fr/library/windows/desktop/bb174577(v=vs.85)}
            mu->win32->d3d11_device_context->ClearState();
            HRESULT hr = 0;
            if (hr = dxgi_swap_chain->ResizeBuffers(
                        0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | 0 * DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
                hr != 0) {
                printf("ERROR: ResizeBuffers() with hr: 0x%x\n", hr);
            }
        }
        break;
    case WM_INPUT: {
        UINT size;
        GetRawInputData((HRAWINPUT)lparam, RID_INPUT, 0, &size, sizeof(RAWINPUTHEADER));
        void *buffer = _alloca(size);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size) {
            RAWINPUT *raw_input = (RAWINPUT *)buffer;
            if (raw_input->header.dwType == RIM_TYPEMOUSE && raw_input->data.mouse.usFlags == MOUSE_MOVE_RELATIVE) {
                // @todo: how can I be seeing double releases?
                // I'm seeing a mouse press, followed by a release, and at that point I press once
                // more and the button appears to be still down, but I don't receive a press event
                // and however I see a release later. I think it could be because we're seeing
                // multiple WM_INPUT events in this sampling loop and we should not use
                // update_digital_button here. I solved this very issue in my own mac version
                mu->mouse.delta_position.x += raw_input->data.mouse.lLastX;
                mu->mouse.delta_position.y += raw_input->data.mouse.lLastY;

                USHORT button_flags = raw_input->data.mouse.usButtonFlags;

                Mu_Bool left_button_down = mu->mouse.left_button.down;
                if (button_flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
                    left_button_down = MU_TRUE;
                }
                if (button_flags & RI_MOUSE_LEFT_BUTTON_UP) {
                    left_button_down = MU_FALSE;
                }
                Mu_UpdateDigitalButtonAndPreserveTransitions(&mu->mouse.left_button, left_button_down);

                Mu_Bool right_button_down = mu->mouse.right_button.down;
                if (button_flags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
                    right_button_down = MU_TRUE;
                }
                if (button_flags & RI_MOUSE_RIGHT_BUTTON_UP) {
                    right_button_down = MU_FALSE;
                }
                Mu_UpdateDigitalButtonAndPreserveTransitions(&mu->mouse.right_button, right_button_down);

                if (button_flags & RI_MOUSE_WHEEL) {
                    mu->mouse.delta_wheel += ((SHORT)raw_input->data.mouse.usButtonData) / WHEEL_DELTA;
                }
            }
        }
        result = DefWindowProcA(window, message, wparam, lparam);
        break;
    }
    case WM_CHAR: {
        WCHAR utf16_character = (WCHAR)wparam;
        char ascii_character;
        uint32_t ascii_length = WideCharToMultiByte(CP_ACP, 0, &utf16_character, 1, &ascii_character, 1, 0, 0);
        if (ascii_length == 1 && mu->text_length + 1 < sizeof(mu->text) - 1) {
            mu->text[mu->text_length] = ascii_character;
            mu->text[mu->text_length + 1] = 0;
            mu->text_length += ascii_length;
        }
        break;
    }
    case WM_DESTROY: mu->quit = MU_TRUE; break;
    case WM_TIMER: SwitchToFiber(mu->win32->main_fiber); break;
    default: result = DefWindowProcA(window, message, wparam, lparam);
    }
    return result;
}

static void CALLBACK Mu_Window_MessageFiberProc(Mu *mu) {
    SetTimer(mu->win32->window, 1, 1, 0);
    for (;;) {
        MSG message;
        while (PeekMessage(&message, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        SwitchToFiber(mu->win32->main_fiber);
    }
}

Mu_Bool Mu_Window_Initialize(Mu *mu) {
    if (!mu->window.title) {
        mu->window.title = "Mu";
    }
    int window_x;
    if (mu->window.position.x) {
        window_x = mu->window.position.x;
    } else {
        window_x = CW_USEDEFAULT;
    }
    int window_y;
    if (mu->window.position.y || window_x != CW_USEDEFAULT /* CreateWindow requirement */) {

        window_y = mu->window.position.y;
    } else {
        window_y = CW_USEDEFAULT;
    }
    int window_width;
    if (mu->window.size.x) {
        window_width = mu->window.size.x;
    } else {
        window_width = CW_USEDEFAULT;
    }
    int window_height;
    if (mu->window.size.y) {
        window_height = mu->window.size.y;
    } else {
        window_height = CW_USEDEFAULT;
    }
    mu->win32 = (Mu_Win32 *)calloc(1, sizeof *mu->win32);
    mu->win32->main_fiber = ConvertThreadToFiber(0);
    Assert(mu->win32->main_fiber);
    mu->win32->message_fiber = CreateFiber(0, (PFIBER_START_ROUTINE)Mu_Window_MessageFiberProc, mu);
    Assert(mu->win32->message_fiber);
    if (window_height != CW_USEDEFAULT && window_width != CW_USEDEFAULT) {
        RECT window_rectangle;
        window_rectangle.left = 0;
        window_rectangle.right = window_width;
        window_rectangle.top = 0;
        window_rectangle.bottom = window_height;
        if (AdjustWindowRect(&window_rectangle, WS_OVERLAPPEDWINDOW, 0)) {
            window_width = window_rectangle.right - window_rectangle.left;
            window_height = window_rectangle.bottom - window_rectangle.top;
        }
    }
    WNDCLASSA window_class = {0};
    window_class.lpfnWndProc = Mu_Window_Proc;
    window_class.lpszClassName = "mu";
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (RegisterClassA(&window_class) == 0) {
        mu->error = "Failed to initialize window class.";
        return MU_FALSE;
    }
    mu->win32->window = CreateWindowA("mu", mu->window.title, WS_OVERLAPPEDWINDOW, window_x, window_y, window_width, window_height, 0, 0, 0, mu);
    if (!mu->win32->window) {
        mu->error = "Failed to create window.";
        return MU_FALSE;
    }
    ShowWindow(mu->win32->window, SW_SHOW);
    mu->win32->device_context = GetDC(mu->win32->window);
    return MU_TRUE;
}

void Mu_Window_Pull(Mu *mu) {
    mu->window.resized = MU_FALSE;
    mu->text[0] = 0;
    mu->text_length = 0;

    mu->mouse.delta_position.x = 0;
    mu->mouse.delta_position.y = 0;
    mu->mouse.delta_wheel = 0;
    mu->mouse.left_button.pressed = MU_FALSE;
    mu->mouse.left_button.released = MU_FALSE;
    mu->mouse.right_button.pressed = MU_FALSE;
    mu->mouse.right_button.released = MU_FALSE;

    SwitchToFiber(mu->win32->message_fiber);
    mu->mouse.wheel += mu->mouse.delta_wheel;

    RECT client_rectangle;
    GetClientRect(mu->win32->window, &client_rectangle);

    mu->window.size.x = client_rectangle.right - client_rectangle.left;
    mu->window.size.y = client_rectangle.bottom - client_rectangle.top;

    POINT window_position = {client_rectangle.left, client_rectangle.top};
    ClientToScreen(mu->win32->window, &window_position);

    mu->window.position.x = window_position.x;
    mu->window.position.y = window_position.y;
}

// Mu_Time

Mu_Bool Mu_Time_Initialize(Mu *mu) {
    LARGE_INTEGER large_integer;
    QueryPerformanceFrequency(&large_integer);
    mu->time.ticks_per_second = large_integer.QuadPart;
    QueryPerformanceCounter(&large_integer);
    mu->time.initial_ticks = large_integer.QuadPart;
    return MU_TRUE;
}

void Mu_Time_Pull(Mu *mu) {
    LARGE_INTEGER large_integer;
    QueryPerformanceCounter(&large_integer);
    uint64_t current_ticks = large_integer.QuadPart;

    mu->time.delta_ticks = (current_ticks - mu->time.initial_ticks) - mu->time.ticks;
    mu->time.ticks = current_ticks - mu->time.initial_ticks;

    mu->time.delta_nanoseconds = (1000 * 1000 * 1000 * mu->time.delta_ticks) / mu->time.ticks_per_second;
    mu->time.delta_microseconds = mu->time.delta_nanoseconds / 1000;
    mu->time.delta_milliseconds = mu->time.delta_microseconds / 1000;
    mu->time.delta_seconds = (float)mu->time.delta_ticks / (float)mu->time.ticks_per_second;

    mu->time.nanoseconds = (1000 * 1000 * 1000 * mu->time.ticks) / mu->time.ticks_per_second;
    mu->time.microseconds = mu->time.nanoseconds / 1000;
    mu->time.milliseconds = mu->time.microseconds / 1000;
    mu->time.seconds = (float)mu->time.ticks / (float)mu->time.ticks_per_second;
}

// Mu_Keyboard

void Mu_Keyboard_Pull(Mu *mu) {
    BYTE keyboard_state[256] = {0};
    GetKeyboardState(keyboard_state);
    for (int key = 0; key < 256; key++) {
        Mu_UpdateDigitalButton(mu->keys + key, keyboard_state[key] >> 7);
    }
}

// Mu_Mouse

Mu_Bool Mu_Mouse_Initialize(Mu *mu) {
    RAWINPUTDEVICE raw_input_device = {0};
    raw_input_device.usUsagePage = 0x01;
    raw_input_device.usUsage = 0x02;
    raw_input_device.hwndTarget = mu->win32->window;
    if (!RegisterRawInputDevices(&raw_input_device, 1, sizeof(raw_input_device))) {
        mu->error = "Failed to register mouse as raw input device.";
        return MU_FALSE;
    }
    return MU_TRUE;
}

void Mu_Mouse_Pull(Mu *mu) {
    POINT mouse_position;
    GetCursorPos(&mouse_position);
    mouse_position.x -= mu->window.position.x;
    mouse_position.y -= mu->window.position.y;
    mu->mouse.position.x = mouse_position.x;
    mu->mouse.position.y = mouse_position.y;
}

// Mu_Gamepad

Mu_Bool Mu_Gamepad_Initialize(Mu *mu) {
    HMODULE xinput_module = LoadLibraryA("xinput1_3.dll");
    if (xinput_module) {
        mu->win32->xinput_get_state = (XINPUTGETSTATE)GetProcAddress(xinput_module, "XInputGetState");
    }
    float trigger_threshold = XINPUT_GAMEPAD_TRIGGER_THRESHOLD / 255.f;
    mu->gamepad.left_trigger.threshold = trigger_threshold;
    mu->gamepad.right_trigger.threshold = trigger_threshold;
    mu->gamepad.left_thumb_stick.threshold = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.f;
    mu->gamepad.right_thumb_stick.threshold = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.f;
    return MU_TRUE;
}

void Mu_Gamepad_Pull(Mu *mu) {
    XINPUT_STATE xinput_state = {0};
    if (!mu->win32->xinput_get_state || mu->win32->xinput_get_state(0, &xinput_state) != ERROR_SUCCESS) {
        mu->gamepad.connected = MU_FALSE;
        return;
    }
    mu->gamepad.connected = MU_TRUE;

    Mu_UpdateDigitalButton(&mu->gamepad.a_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.b_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.x_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.y_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.left_shoulder_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.right_shoulder_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.up_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.down_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.left_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.right_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.left_thumb_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.right_thumb_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.back_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0);
    Mu_UpdateDigitalButton(&mu->gamepad.start_button, (xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0);
    Mu_UpdateAnalogButton(&mu->gamepad.left_trigger, xinput_state.Gamepad.bLeftTrigger / 255.f);
    Mu_UpdateAnalogButton(&mu->gamepad.right_trigger, xinput_state.Gamepad.bRightTrigger / 255.f);
#define CONVERT(x) (2.0f * (((x + 32768) / 65535.f) - 0.5f))
    Mu_UpdateStick(&mu->gamepad.left_thumb_stick, CONVERT(xinput_state.Gamepad.sThumbLX), CONVERT(xinput_state.Gamepad.sThumbLY));
    Mu_UpdateStick(&mu->gamepad.right_thumb_stick, CONVERT(xinput_state.Gamepad.sThumbRX), CONVERT(xinput_state.Gamepad.sThumbRY));
#undef CONVERT
}

// Mu_Audio

static void Mu_Audio_DefaultCallback(Mu_AudioBuffer *buffer) {
    FillMemory(buffer->samples, sizeof(int16_t) * buffer->samples_count, 0);
}

DWORD WINAPI Mu_Audio_ThreadProc(LPVOID parameter) {
    DWORD result = -1;
    Mu *mu = (Mu *)parameter;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE buffer_ready_event = CreateEvent(0, 0, 0, 0);
    if (mu->win32->audio_client->SetEventHandle(buffer_ready_event) < 0) {
        goto done;
    }
    uint32_t max_buffer_frame_count;
    if (mu->win32->audio_client->GetBufferSize(&max_buffer_frame_count) < 0) {
        goto done;
    }
    if (mu->win32->audio_client->Start() < 0) {
        goto done;
    }
    for (;;) {
        if (WaitForSingleObject(buffer_ready_event, INFINITE) != WAIT_OBJECT_0) {
            goto done;
        }

        uint32_t buffer_frame_count;
        if (mu->win32->audio_client->GetBufferSize(&buffer_frame_count) < 0) {
            goto done;
        }
        uint32_t padding_frame_count;
        if (mu->win32->audio_client->GetCurrentPadding(&padding_frame_count) < 0) {
            goto done;
        }
        uint32_t fill_frame_count = buffer_frame_count - padding_frame_count;
        Mu_AudioBuffer buffer;
        if (mu->win32->audio_render_client->GetBuffer(fill_frame_count, (BYTE **)&buffer.samples) < 0) {
            goto done;
        }
        buffer.format = mu->audio.format;
        buffer.samples_count = fill_frame_count * buffer.format.channels;
        memset(buffer.samples, 0, buffer.samples_count * buffer.format.bytes_per_sample);
        mu->audio.callback(&buffer);
        if (mu->win32->audio_render_client->ReleaseBuffer(fill_frame_count, 0) < 0) {
            goto done;
        }
    }
    result = 0;
done:
    mu->win32->audio_client->Stop();
    return result;
}

static WAVEFORMATEX win32_audio_format = {WAVE_FORMAT_PCM, 2, 44100, 44100 * sizeof(int16_t) * 2, sizeof(int16_t) * 2, sizeof(int16_t) * 8, 0};
static Mu_AudioFormat mu_audio_format = {44100, 2, 2};

Mu_Bool Mu_Audio_Initialize(Mu *mu) {
    Mu_Bool result = MU_FALSE;
    IMMDeviceEnumerator *device_enumerator = 0;
    IMMDevice *audio_device = 0;
    if (!mu->audio.callback) {
        mu->audio.callback = Mu_Audio_DefaultCallback;
    }
    CoInitializeEx(0, COINITBASE_MULTITHREADED);
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device_enumerator)) < 0) {
        goto done;
    }
    if (device_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audio_device) < 0) {
        mu->error = "Audio: cannot get default audio device";
        goto done;
    }
    IAudioClient *audio_client;
    if (audio_device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, 0, (void **)&audio_client) < 0) {
        goto done;
    }
    REFERENCE_TIME audio_buffer_duration = 40 * 1000 * 10; // 40 milliseconds
    DWORD audio_client_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    if (audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, audio_client_flags, audio_buffer_duration, 0, &win32_audio_format, 0) < 0) {
        goto done;
    }
    IAudioRenderClient *audio_render_client;
    if (audio_client->GetService(IID_PPV_ARGS(&audio_render_client)) < 0) {
        goto done;
    }
    mu->audio.format = mu_audio_format;
    mu->audio.format.samples_per_second = win32_audio_format.nSamplesPerSec;
    mu->audio.format.channels = win32_audio_format.nChannels;
    mu->audio.format.bytes_per_sample = win32_audio_format.wBitsPerSample / 8;

    mu->win32->audio_client = audio_client;
    mu->win32->audio_render_client = audio_render_client;
    CreateThread(0, 0, Mu_Audio_ThreadProc, mu, 0, 0);
    result = MU_TRUE;
done:
    if (device_enumerator) {
        device_enumerator->Release();
    }
    if (audio_device) {
        audio_device->Release();
    }
    return result;
}

#if !defined(MU_D3D11_ENABLED)
#define MU_D3D11_ENABLED (1)
#endif

#if MU_D3D11_ENABLED
void Mu_D3D11_Push(Mu *mu) {
    if (auto swap_chain = mu->win32->dxgi_swap_chain) {
        swap_chain->Present(1, 0);
    }
}

Mu_Bool Mu_D3D11_Initialize(Mu *mu) {
    HMODULE d3d11_module = LoadLibraryA("d3d11.dll");
    if (!d3d11_module) {
        mu->error = "did not find d3d11.dll";
        return MU_FALSE;
    }
    typedef HRESULT D3D11CreateDeviceAndSwapChainProc(_In_opt_ IDXGIAdapter * pAdapter,
                                                      D3D_DRIVER_TYPE DriverType,
                                                      HMODULE Software,
                                                      UINT Flags,
                                                      _In_opt_ const D3D_FEATURE_LEVEL *pFeatureLevels,
                                                      UINT FeatureLevels,
                                                      UINT SDKVersion,
                                                      _In_opt_ const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                                                      _Out_opt_ IDXGISwapChain **ppSwapChain,
                                                      _Out_opt_ ID3D11Device **ppDevice,
                                                      _Out_opt_ D3D_FEATURE_LEVEL *pFeatureLevel,
                                                      _Out_opt_ ID3D11DeviceContext **ppImmediateContext);

    D3D11CreateDeviceAndSwapChainProc *CreateDeviceAndSwapChain =
            (D3D11CreateDeviceAndSwapChainProc *)GetProcAddress(d3d11_module, "D3D11CreateDeviceAndSwapChain");
    if (!CreateDeviceAndSwapChain) {
        mu->error = "can't find D3D11CreateDeviceAndSwapChain in d3d11.dll"; // TODO(nicolas): path of dll here
        return MU_FALSE;
    }

    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT feature_levels_n = sizeof feature_levels / sizeof feature_levels[0];
    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    {
        auto &o = swap_chain_desc;
        o.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        o.SampleDesc.Count = 1; // Default non-AA
        o.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        o.BufferCount = 2;
        o.OutputWindow = mu->win32->window;
        o.Windowed = TRUE;
        o.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        o.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | 0 * DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    IDXGISwapChain *swap_chain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *device_context = NULL;
    HRESULT hr = CreateDeviceAndSwapChain(NULL /* default adapter */,
                                          D3D_DRIVER_TYPE_HARDWARE,
                                          NULL /* software renderer module */,
                                          0 * D3D11_CREATE_DEVICE_SINGLETHREADED | 0 * D3D11_CREATE_DEVICE_DEBUG | 1 * D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                          NULL /* feature_levels */,
                                          0,
                                          D3D11_SDK_VERSION,
                                          &swap_chain_desc,
                                          &mu->win32->dxgi_swap_chain,
                                          &mu->win32->d3d11_device,
                                          feature_levels /* feature level */,
                                          &mu->win32->d3d11_device_context);
    if (hr != 0) {
        mu->error = "CreateDeviceAndSwapChain returned an error";
        char *d_p = &mu->error_buffer[0];
        int d_n = sizeof mu->error_buffer;
        const char *s_p = "CreateDeviceAndSwapChain returned hr=0x";
        const char nibbles[] = "0123456789ABCDEF";
        for (; d_n && *s_p; d_n--, s_p++, d_p++)
            *d_p = *s_p;
        for (int nibble = (sizeof hr) * 2; d_n && nibble-- > 0; d_n--, d_p++) {
            *d_p = nibbles[(hr >> (4 * nibble)) & 0xF];
        }
        if (d_n) {
            *d_p = '\0';
            mu->error = mu->error_buffer;
        }
        return MU_FALSE;
    }
    return MU_TRUE;
}
#endif

// Mu_OpenGL

void Mu_OpenGL_Push(Mu *mu) {
    SwapBuffers(mu->win32->device_context);
}

Mu_Bool Mu_OpenGL_Initialize(Mu *mu) {
    PIXELFORMATDESCRIPTOR pixel_format_descriptor;
    pixel_format_descriptor.nSize = sizeof(pixel_format_descriptor);
    pixel_format_descriptor.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
    pixel_format_descriptor.iPixelType = PFD_TYPE_RGBA;
    pixel_format_descriptor.cColorBits = 24;
    pixel_format_descriptor.cAlphaBits = 0;
    pixel_format_descriptor.cDepthBits = 24;
    pixel_format_descriptor.cStencilBits = 8;
    int pixel_format = ChoosePixelFormat(mu->win32->device_context, &pixel_format_descriptor);
    if (!pixel_format) {
        return MU_FALSE;
    }
    if (!DescribePixelFormat(mu->win32->device_context, pixel_format, sizeof(pixel_format_descriptor), &pixel_format_descriptor)) {
        return MU_FALSE;
    }
    if (!SetPixelFormat(mu->win32->device_context, pixel_format, &pixel_format_descriptor)) {
        return MU_FALSE;
    }
    mu->win32->wgl_context = wglCreateContext(mu->win32->device_context);
    if (!mu->win32->wgl_context) {
        return MU_FALSE;
    }
    wglMakeCurrent(mu->win32->device_context, mu->win32->wgl_context);
    return MU_TRUE;
}

// Mu

Mu_Bool Mu_Pull(Mu *mu) {
    if (!mu->initialized) {
        if (!mu->error) {
            mu->error = "Mu was not initialized.";
        }
        Mu_ExitWithError(mu);
        return MU_FALSE;
    }
    Mu_Window_Pull(mu);
    Mu_Time_Pull(mu);
    Mu_Keyboard_Pull(mu);
    Mu_Mouse_Pull(mu);
    Mu_Gamepad_Pull(mu);
    return !mu->quit;
}

void Mu_Push(Mu *mu) {
#if MU_D3D11_ENABLED
    Mu_D3D11_Push(mu);
#else
    Mu_OpenGL_Push(mu);
#endif
    RECT client_rectangle;
    GetClientRect(mu->win32->window, &client_rectangle);

    Mu_Int2 size = {};
    size.x = client_rectangle.right - client_rectangle.left;
    size.y = client_rectangle.bottom - client_rectangle.top;

    if ((mu->window.size.x != 0 && mu->window.size.x != size.x) || (mu->window.size.y != 0 && mu->window.size.y != size.y)) {
        /*
        @todo @feature{HiDpi}
        BOOL WINAPI AdjustWindowRectExForDpi(
          _Inout_ LPRECT lpRect,
          _In_    DWORD  dwStyle,
          _In_    BOOL   bMenu,
          _In_    DWORD  dwExStyle,
          _In_    UINT   dpi
          );
        */
    }
}

Mu_Bool Mu_Initialize(Mu *mu) {
    if (!Mu_Window_Initialize(mu)) {
        return MU_FALSE;
    }
    if (!Mu_Time_Initialize(mu)) {
        return MU_FALSE;
    }
    if (!Mu_Mouse_Initialize(mu)) {
        return MU_FALSE;
    }
    if (!Mu_Gamepad_Initialize(mu)) {
        return MU_FALSE;
    }
    if (!Mu_Audio_Initialize(mu)) {
        return MU_FALSE;
    }
#if MU_D3D11_ENABLED
    if (!Mu_D3D11_Initialize(mu)) {
        return MU_FALSE;
    }
#else
    if (!Mu_OpenGL_Initialize(mu)) {
        return MU_FALSE;
    }
#endif
    mu->initialized = MU_TRUE;
    Mu_Pull(mu);
    return MU_TRUE;
}

// Utilities

static IWICImagingFactory *wic_factory;
static SRWLOCK wic_factory_lock = SRWLOCK_INIT;

Mu_Bool Mu_LoadImage(const char *filename, Mu_Image *image) {
    Mu_Bool result = MU_FALSE;
    IWICBitmapDecoder *image_decoder = 0;
    IWICBitmapFrameDecode *image_frame = 0;
    IWICBitmapSource *rgba_image_frame = 0;
    if (!wic_factory) {
        AcquireSRWLockExclusive(&wic_factory_lock);
        if (!wic_factory) {
            CoInitializeEx(0, COINITBASE_MULTITHREADED);
            if (CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory)) < 0) {
                ReleaseSRWLockExclusive(&wic_factory_lock);
                goto done;
            }
        }
        ReleaseSRWLockExclusive(&wic_factory_lock);
    }
    int wide_filename_length = MultiByteToWideChar(CP_UTF8, 0, filename, -1, 0, 0);
    WCHAR *wide_filename = (WCHAR *)_alloca(wide_filename_length * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, wide_filename, wide_filename_length);
    if (wic_factory->CreateDecoderFromFilename(wide_filename, 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &image_decoder) < 0) {
        goto done;
    }
    if (image_decoder->GetFrame(0, &image_frame) < 0) {
        goto done;
    }
    if (WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, image_frame, &rgba_image_frame) < 0) {
        goto done;
    }
    uint32_t width;
    uint32_t height;
    image_frame->GetSize(&width, &height);
    image->width = width;
    image->height = height;
    image->channels = 4;
    uint32_t buffer_size = 4 * width * height;
    image->pixels = (uint8_t *)malloc(buffer_size);
    if (!image->pixels)
        goto done;
    uint32_t buffer_stride = 4 * width;
    if (rgba_image_frame->CopyPixels(0, buffer_stride, buffer_size, image->pixels) < 0) {
        free(image->pixels);
        goto done;
    }
    result = MU_TRUE;
done:
    if (image_decoder) {
        image_decoder->Release();
    }
    if (image_frame) {
        image_frame->Release();
    }
    if (rgba_image_frame) {
        rgba_image_frame->Release();
    }
    return result;
}

static bool mf_initialized;

Mu_Bool Mu_LoadAudio(const char *filename, Mu_AudioBuffer *audio) {
    HRESULT hr = 0;
    Mu_Bool result = MU_FALSE;
    IMFSourceReader *source_reader = 0;
    IMFMediaType *media_type = 0;
    if (!mf_initialized) {
        CoInitializeEx(0, COINIT_MULTITHREADED);
        if (MFStartup(MF_VERSION, 0) < 0) {
            goto done;
        }
        mf_initialized = true;
    }
    int wide_filename_length = MultiByteToWideChar(CP_UTF8, 0, filename, -1, 0, 0);
    WCHAR *wide_filename = (WCHAR *)_alloca(wide_filename_length * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, wide_filename, wide_filename_length);
    for (hr = MFCreateSourceReaderFromURL(wide_filename, 0, &source_reader); hr < 0;) {
        goto done;
    }
    if (MFCreateMediaType(&media_type) < 0) {
        goto done;
    }
    media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, win32_audio_format.nChannels);
    media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, win32_audio_format.nSamplesPerSec);
    media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, win32_audio_format.nBlockAlign);
    media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, win32_audio_format.nAvgBytesPerSec);
    media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, win32_audio_format.wBitsPerSample);
    media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (hr = source_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, media_type), hr != 0) {
        goto done;
    }
    size_t buffer_capacity = mu_audio_format.samples_per_second * mu_audio_format.bytes_per_sample; // 1 second of audio
    size_t buffer_size = 0;
    char *buffer = (char *)malloc(buffer_capacity);
    for (;;) {
        DWORD stream_index;
        DWORD flags;
        LONGLONG timestamp;
        IMFSample *sample;
        if (hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream_index, &flags, &timestamp, &sample), hr != 0) {
            free(buffer);
            goto done;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        IMFMediaBuffer *sample_buffer;
        sample->ConvertToContiguousBuffer(&sample_buffer);
        DWORD sample_buffer_size;
        sample_buffer->GetCurrentLength(&sample_buffer_size);
        size_t new_buffer_size = buffer_size + sample_buffer_size;
        if (new_buffer_size > buffer_capacity) {
            buffer_capacity *= 2;
            if (buffer_capacity < new_buffer_size) {
                buffer_capacity = new_buffer_size;
            }
            buffer = (char *)realloc(buffer, buffer_capacity);
            if (!buffer) {
                goto done;
            }
        }
        BYTE *sample_buffer_pointer;
        sample_buffer->Lock(&sample_buffer_pointer, 0, 0);
        CopyMemory(buffer + buffer_size, sample_buffer_pointer, sample_buffer_size);
        buffer_size += sample_buffer_size;
        sample_buffer->Unlock();
        sample_buffer->Release();
        sample->Release();
    }
    audio->format = mu_audio_format;
    audio->samples = (int16_t *)realloc(buffer, buffer_size);
    audio->samples_count = buffer_size / mu_audio_format.bytes_per_sample;
    result = MU_TRUE;
done:
    if (source_reader) {
        source_reader->Release();
    }
    if (media_type) {
        media_type->Release();
    }
    return result;
}

MU_EXTERN_END
