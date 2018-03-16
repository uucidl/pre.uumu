// @language: c11

// NOTE(nicolas): @url{https://www.3dgep.com/introduction-to-directx-11/#Input-Assembler_Stage}

//
// test program, to test for dropped vsync, dropped video frames,
// audio/video synchronization and gamepad support.
//

#define BYTE unsigned char
#include "flat_ps.h"
#include "ortho_vs.h"
#include "tex_ps.h"
#undef BYTE

#include "xxxx_mu.h"

#if defined(__APPLE__)
#include "OpenGL/gl.h"     // @todo: @platform{macos} specific location
#include "xxxx_mu_cocoa.h" // @todo: @platform{macos} because I need the platform specific keyname
#endif

#if defined(_WIN32)
#include "xxxx_mu_win32.h"

#if !defined(APIENTRY)
#define APIENTRY __stdcall
#endif
#if !defined(WINGDIAPI)
#define WINGDIAPI __declspec(dllimport)
#endif
#include <gl/GL.h>
#undef APIENTRY
#undef WINGDIAPI
#include <d3d11.h>
#endif

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
typedef uintptr_t atomic_uint;
static inline uintptr_t atomic_load(uintptr_t volatile *object) {
    return *object;
}
static inline void atomic_store(uintptr_t volatile *object, uintptr_t value) {
    *object = value;
}
static inline void atomic_init(uintptr_t volatile *object, uintptr_t value) {
    atomic_store(object, value);
}
#else
#if defined(__STDC_NO_ATOMICS__)
#error "Error: C11 atomics not found"
#endif
#include <stdatomic.h>
#endif

#define MU_TEST_INTERNAL static
#define MU_TEST_FN_STATE static
// @note: marks assets
#define MU_TEST_ASSET(x__) x__

struct Mu_Test_AudioNote_InitParameters {
    float pitch_hz;
    struct Mu_AudioBuffer *optional_source;
};

struct Mu_Test_AudioNote_Playing {
    struct Mu_Test_AudioNote_InitParameters init_parameters;
    double phase;
    int source_frame_i;
};

enum {
    MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER = 3,
    MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_COUNT = 1 << MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER,
    MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_MASK = (1 << MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER) - 1,
    MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY = 64,
};

struct Mu_Test_AudioSynth {
    atomic_uint input_notes_rb_write_n;                                                                      // @shared
    atomic_uint input_notes_rb_read_n;                                                                       // @shared
    struct Mu_Test_AudioNote_InitParameters input_notes_rb[MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_COUNT]; // @shared

    int playing_notes_n;
    struct Mu_Test_AudioNote_Playing playing_notes[MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY];
};

MU_TEST_INTERNAL
struct Mu_Test_AudioSynth mu_test_audiosynth;

MU_TEST_INTERNAL
double db_to_amp(double volume_in_db) {
    return pow(exp(volume_in_db), log(10.0) / 20.0);
}

MU_TEST_INTERNAL
int audio_mix(struct Mu_AudioBuffer const *source, struct Mu_AudioBuffer *dest, double amp, int s_frame_i) {
    int16_t *const s_sample_f = source->samples;
    int16_t *const s_sample_l = s_sample_f + source->samples_count;
    int16_t *s_sample = s_sample_f + s_frame_i;
    int16_t *const d_sample_f = dest->samples;
    int16_t *const d_sample_l = dest->samples + dest->samples_count;
    int16_t *d_sample = d_sample_f;
    if (source->format.channels == 1) {
        while (d_sample != d_sample_l && s_sample != s_sample_l) {
            int16_t x = amp * (*s_sample);
            for (int n = dest->format.channels; n--;) {
                *d_sample++ += x;
            }
            ++s_sample;
        }
    } else {
        while (d_sample != d_sample_l && s_sample != s_sample_l) {
            int s_channels = source->format.channels;
            int d_channels = dest->format.channels;
            int channels_n = d_channels < s_channels ? d_channels : s_channels;
            for (int channel_i = 0; channel_i < channels_n; ++channel_i) {
                d_sample[channel_i] += amp * s_sample[channel_i];
            }
            d_sample += d_channels;
            s_sample += s_channels;
        }
    }
    return s_sample - s_sample_f;
}

MU_TEST_INTERNAL
void main_audio_callback(struct Mu_AudioBuffer *audiobuffer) {
    int16_t *const stereo_frames = audiobuffer->samples;
    int const frames_n = audiobuffer->samples_count / audiobuffer->format.channels;
    double const sr_hz = audiobuffer->format.samples_per_second;
    memset(stereo_frames, 0, frames_n * audiobuffer->format.channels * audiobuffer->format.bytes_per_sample);

    struct Mu_Test_AudioSynth *const synth = &mu_test_audiosynth;

    unsigned int input_notes_n = atomic_load(&synth->input_notes_rb_write_n);
    unsigned int input_notes_read_n = atomic_load(&synth->input_notes_rb_read_n);
    for (unsigned int input_note_i = input_notes_read_n; input_note_i != input_notes_n; input_note_i++) {
        int next_note_i = synth->playing_notes_n;
        if (next_note_i != MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY) {
            int note_i = input_note_i & MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_MASK;
            synth->playing_notes_n++;
            synth->playing_notes[next_note_i] = (struct Mu_Test_AudioNote_Playing){
                    .init_parameters = synth->input_notes_rb[note_i],
            };
            ++input_notes_read_n;
        }
    }
    atomic_store(&synth->input_notes_rb_read_n, input_notes_read_n);

    static const double TAU = 6.2831853071795864769252;
    for (int note_i = 0; note_i < synth->playing_notes_n;) {
        struct Mu_Test_AudioNote_Playing *const note = &synth->playing_notes[note_i];
        double phase = note->phase;
        double phase_delta = note->init_parameters.pitch_hz / sr_hz;
        double const amp = db_to_amp(-20.0);
        _Bool note_has_ended = false;
        for (int frame_i = 0; frame_i < frames_n; ++frame_i) {
            double const env = fmin(1.0, exp(-6.0 * phase * 0.001));
            float const y = (float)(env * amp * sin(TAU * phase));
            stereo_frames[2 * frame_i] += 32767.0f * y;
            stereo_frames[2 * frame_i + 1] += 32767.0f * y;
            phase += phase_delta;
            note_has_ended = env < 0.01;
        }
        struct Mu_AudioBuffer *source = note->init_parameters.optional_source;
        if (source) {
            note->source_frame_i = audio_mix(source, audiobuffer, amp, note->source_frame_i);
        }
        note->phase = phase;
        if (note_has_ended) {
            synth->playing_notes_n--;
            if (synth->playing_notes_n != note_i) {
                memcpy(note, &synth->playing_notes[synth->playing_notes_n], sizeof *note);
            }
        } else {
            note_i++;
        }
    }
}

MU_TEST_INTERNAL
void mu_test_audiosynth_initialize(struct Mu_Test_AudioSynth *const synth) {
    atomic_init(&synth->input_notes_rb_read_n, 0);
    atomic_init(&synth->input_notes_rb_write_n, 0);
}

MU_TEST_INTERNAL
bool mu_test_audiosynth_push_event(struct Mu_Test_AudioSynth *const synth, double const pitch_hz, struct Mu_AudioBuffer *optional_source) {
    unsigned int write_n = atomic_load(&synth->input_notes_rb_write_n);
    unsigned int read_n = atomic_load(&synth->input_notes_rb_read_n);
    if (write_n - read_n >= MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_COUNT) {
        return false;
    }
    unsigned int input_note_i = (write_n & MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_MASK);
    synth->input_notes_rb[input_note_i] = (struct Mu_Test_AudioNote_InitParameters){
            .pitch_hz = pitch_hz,
            .optional_source = optional_source,
    };
    atomic_store(&synth->input_notes_rb_write_n, write_n + 1);
    return true;
}

MU_TEST_INTERNAL
bool mu_test_audiosynth_push_note(struct Mu_Test_AudioSynth *const synth, double const pitch_hz) {
    mu_test_audiosynth_push_event(synth, pitch_hz, NULL);
    return true;
}

MU_TEST_INTERNAL
bool mu_test_audiosynth_push_sample(struct Mu_Test_AudioSynth *const synth, struct Mu_AudioBuffer *source) {
    mu_test_audiosynth_push_event(synth, 0.0, source);
    return true;
}

MU_TEST_INTERNAL
int platform_get_resource_path(char *buffer, int buffer_n, char const *const relative_path, int relative_path_n);

#define COM_CALL0(comptr, method_name) (comptr->lpVtbl->method_name)(comptr)
#define COM_CALL(comptr, method_name, ...) (comptr->lpVtbl->method_name)(comptr, __VA_ARGS__)

// stretchy buffers, invented (?) by sean barrett

typedef struct BufHdr {
    size_t len;
    size_t cap;
    char buf[0];
} BufHdr;

#define buf__hdr(b) ((BufHdr *)((char *)b - offsetof(BufHdr, buf)))
#define buf__fits(b, n) (buf_len(b) + (n) <= buf_cap(b))
#define buf__fit(b, n) (buf__fits(b, n) ? 0 : ((b) = buf__grow((b), buf_len(b) + (n), sizeof(*(b)))))

#define buf_len(b) ((b) ? buf__hdr(b)->len : 0)
#define buf_cap(b) ((b) ? buf__hdr(b)->cap : 0)
#define buf_push(b, x) (buf__fit(b, 1), b[buf_len(b)] = (x), buf__hdr(b)->len++)
#define buf_free(b) ((b) ? free(buf__hdr(b)) : 0)

#define buf_end(b__) (&b__[buf_len(b__)])

void *buf__grow(const void *buf, size_t new_len, size_t elem_size) {
    size_t next_cap = 1 + 2 * buf_cap(buf);
    size_t new_cap = new_len >= next_cap ? new_len : next_cap;
    assert(new_len <= new_cap);
    size_t new_size = offsetof(BufHdr, buf) + new_cap * elem_size;
    BufHdr *new_hdr;
    if (buf) {
        new_hdr = realloc(buf__hdr(buf), new_size);
    } else {
        new_hdr = malloc(new_size);
        new_hdr->len = 0;
    }
    new_hdr->cap = new_cap;
    return new_hdr->buf;
}

struct FlatVertex2d {
    float posx, posy, posz;
    float colr, colg, colb, cola;
    float tx, ty;
};

const D3D11_INPUT_ELEMENT_DESC FlatVertex2d_InputElements[] = {
        {.SemanticName = "POSITION",
         .Format = DXGI_FORMAT_R32G32B32_FLOAT,
         .AlignedByteOffset = offsetof(struct FlatVertex2d, posx),
         .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA},
        {.SemanticName = "COLOR",
         .Format = DXGI_FORMAT_R32G32B32_FLOAT,
         .AlignedByteOffset = offsetof(struct FlatVertex2d, colr),
         .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA},
        {.SemanticName = "TEXCOORD",
         .Format = DXGI_FORMAT_R32G32_FLOAT,
         .AlignedByteOffset = offsetof(struct FlatVertex2d, tx),
         .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA},
};
const size_t FlatVertex2d_InputElementsN = sizeof FlatVertex2d_InputElements / sizeof FlatVertex2d_InputElements[0];

struct GfxQuadCommand {
    float x0, x1, y0, y1;
    float r, g, b, a;
    struct ID3D11Texture2D *tx;
    float tx0, tx1, ty0, ty1;
};

int main(int argc, char **argv) {
    mu_test_audiosynth_initialize(&mu_test_audiosynth);
    struct Mu mu = {
            .window.position.x = 640,
            .window.size.x = 640,
            .window.size.y = 480,
            .gamepad.left_trigger.threshold = 1.0 / 50.0f,
            .gamepad.right_trigger.threshold = 1.0 / 50.0f,
            .gamepad.left_thumb_stick.threshold = 1.0 / 50.0f,
            .gamepad.right_thumb_stick.threshold = 1.0 / 50.0f,
            .audio.callback = main_audio_callback,
    };
    if (!Mu_Initialize(&mu)) {
        printf("ERROR: Mu could not initialize: '%s'\n", mu.error);
        return 1;
    }
    if (mu.win32->wgl_context) {
        GLuint defGL_MAJOR_VERSION = 0x821B;
        GLuint defGL_MINOR_VERSION = 0x821C;
        GLint gl_version[2] = {1, 0};
        glGetIntegerv(defGL_MAJOR_VERSION, &gl_version[0]);
        glGetIntegerv(defGL_MINOR_VERSION, &gl_version[1]);
        printf("INFO: GL version %d.%d\n", gl_version[0], gl_version[1]);
    }
    if (!mu.win32->d3d11_device_context) {
        printf("ERROR: D3D11 missing");
        return 1;
    }
    struct Mu_AudioBuffer test_audio;
    Mu_Bool test_audio_loaded = MU_FALSE;
    char const *test_sound_path = MU_TEST_ASSET("test_assets/chime.wav");
    char buffer[4096];
    int const buffer_n = sizeof buffer;
    if (buffer_n != platform_get_resource_path(buffer, buffer_n, test_sound_path, strlen(test_sound_path))) {
        test_audio_loaded = Mu_LoadAudio(buffer, &test_audio);
        if (!test_audio_loaded)
            printf("ERROR: Mu could not load file: '%s'\n", buffer);
    }
    if (!test_audio_loaded)
        test_audio_loaded = Mu_LoadAudio(test_sound_path, &test_audio);
    if (!test_audio_loaded) {
        printf("ERROR: Mu could not load file: '%s'\n", test_sound_path);
    }

    struct Mu_Image test_image = {
            0,
    };
    char const *test_image_path = MU_TEST_ASSET("test_assets/ln2.png");
    if (buffer_n != platform_get_resource_path(buffer, buffer_n, test_image_path, strlen(test_image_path))) {
        Mu_Bool test_image_loaded = Mu_LoadImage(buffer, &test_image);
        if (!test_image_loaded)
            printf("ERROR: Mu could not load file: '%s'\n", buffer);
    }

    struct ID3D11PixelShader *flat_ps = NULL;
    {
        HRESULT hr;
        if (hr = COM_CALL(mu.win32->d3d11_device, CreatePixelShader, d3d11_flat_ps_bc, sizeof d3d11_flat_ps_bc, NULL, &flat_ps), hr != 0) {
            printf("ERROR: CreatePixelShader(flat_ps) returned 0x%x\n", hr);
            return 1;
        }
    }

    struct ID3D11PixelShader *tex_ps = NULL;
    {
        HRESULT hr;
        if (hr = COM_CALL(mu.win32->d3d11_device, CreatePixelShader, d3d11_tex_ps_bc, sizeof d3d11_tex_ps_bc, NULL, &tex_ps), hr != 0) {
            printf("ERROR: CreatePixelShader(tex_ps) returned 0x%x\n", hr);
            return 1;
        }
    }

    struct ID3D11VertexShader *ortho_vs = NULL;
    {
        HRESULT hr;
        if (hr = COM_CALL(mu.win32->d3d11_device, CreateVertexShader, d3d11_ortho_vs_bc, sizeof d3d11_ortho_vs_bc, NULL, &ortho_vs), hr != 0) {
            printf("ERROR: CreateVertexShader(ortho_vs) returned 0x%x\n", hr);
            return 1;
        }
    }

    float theta = 0.0f;
    int frame_i = 0;
    struct ID3D11Texture2D *test_image_texture = NULL;

    struct ButtonTransitionCheck {
        struct Mu_DigitalButton *button;
        char const *name;
        int n;
        int max_n;
        int press_n;
        float last_press_seconds;
    } button_checks[] = {
            {
                    &mu.mouse.left_button,
                    "left button",
            },
            {
                    &mu.mouse.right_button,
                    "right button",
            },
    };
    int const button_checks_n = sizeof button_checks / sizeof button_checks[0];

    uint64_t last_nanoseconds = mu.time.nanoseconds;
    uint8_t last_nanoseconds_wraparound_n = 0;

    struct ID3D11RasterizerState *rasterizer_state = NULL;
    {
        HRESULT hr;
        D3D11_RASTERIZER_DESC desc = {
                .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE, /* move to front later */
        };
        if (hr = COM_CALL(mu.win32->d3d11_device, CreateRasterizerState, &desc, &rasterizer_state), hr != 0) {
            printf("ERROR: CreateRasterizerState(rasterizer_state) with hr: 0x%x\n", hr);
            return 1;
        }
    }

    enum { IMM_VERTEX_BUFFER_N = 65536 };
    struct ID3D11Buffer *imm_vertex_buffer = NULL;
    {
        D3D11_BUFFER_DESC desc = {
                .ByteWidth = (sizeof(struct FlatVertex2d)) * IMM_VERTEX_BUFFER_N,
                .Usage = D3D11_USAGE_DYNAMIC,
                .BindFlags = D3D11_BIND_VERTEX_BUFFER,
                .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
                .StructureByteStride = sizeof(struct FlatVertex2d),
        };
        HRESULT hr;
        if (hr = COM_CALL(mu.win32->d3d11_device, CreateBuffer, &desc, NULL, &imm_vertex_buffer), hr != 0) {
            printf("ERROR: CreateBuffer(imm_vertex_buffer) with hr: 0x%x\n", hr);
            return 1;
        }
    }

    enum { IMM_INDEX_BUFFER_N = 65536 };
    struct ID3D11Buffer *imm_index_buffer = NULL;
    {
        D3D11_BUFFER_DESC desc = {
                .ByteWidth = (sizeof(uint16_t)) * IMM_INDEX_BUFFER_N,
                .Usage = D3D11_USAGE_DYNAMIC,
                .BindFlags = D3D11_BIND_INDEX_BUFFER,
                .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
                .StructureByteStride = sizeof(uint16_t),
        };
        HRESULT hr;
        if (hr = COM_CALL(mu.win32->d3d11_device, CreateBuffer, &desc, NULL, &imm_index_buffer), hr != 0) {
            printf("ERROR: CreateBuffer(imm_index_buffer) with hr: 0x%x\n", hr);
            return 1;
        }
    }

    struct ID3D11InputLayout *imm_layout;
    {
        const D3D11_INPUT_ELEMENT_DESC *descs = FlatVertex2d_InputElements;
        UINT descs_n = FlatVertex2d_InputElementsN;
        HRESULT hr;
        if (hr = COM_CALL(mu.win32->d3d11_device, CreateInputLayout, descs, descs_n, d3d11_ortho_vs_bc, sizeof d3d11_ortho_vs_bc, &imm_layout), hr != 0) {
            printf("ERROR: CreateInputLayout(imm_layout) with hr: 0x%x\n", hr);
            return 1;
        }
    }

    struct GfxQuadCommand *cb = NULL;

    struct FlatVertex2d *vb = NULL;
    uint16_t *ib = NULL;

    while (Mu_Pull(&mu)) {
        if (cb)
            buf__hdr(cb)->len = 0;
        if (vb)
            buf__hdr(vb)->len = 0;
        if (ib)
            buf__hdr(ib)->len = 0;
        if (test_image_texture == 0) {
            D3D11_TEXTURE2D_DESC desc = {
                    .Width = test_image.width,
                    .Height = test_image.height,
                    .MipLevels = 1,
                    .ArraySize = 1,
                    .Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                    .SampleDesc.Count = 1,
                    .Usage = D3D11_USAGE_IMMUTABLE,
                    .BindFlags = D3D11_BIND_SHADER_RESOURCE,
            };
            D3D11_SUBRESOURCE_DATA data = {
                    .pSysMem = test_image.pixels,
                    .SysMemPitch = test_image.width * test_image.channels,
            };
            HRESULT hr = COM_CALL(mu.win32->d3d11_device, CreateTexture2D, &desc, &data, &test_image_texture);
            if (hr != 0) {
                printf("ERROR: CreateTexture2D(test_image_texture) 0x%x\n", hr);
                return 1;
            }
        }

        struct ID3D11RenderTargetView *output_view = NULL;
        {
            ID3D11Resource *resource = NULL;
            HRESULT hr;
            if (hr = COM_CALL(mu.win32->dxgi_swap_chain, GetBuffer, 0, &IID_ID3D11Resource, &resource), hr != 0) {
                printf("ERROR: GetBuffer(dxgi_swap_chain) with hr: 0x%x\n", hr);
                return 1;
            }
            if (hr = COM_CALL(mu.win32->d3d11_device, CreateRenderTargetView, resource, NULL, &output_view), hr != 0) {
                printf("ERROR: CreateRenderTargetView(output_view) with hr: 0x%x\n", hr);
                return 1;
            }
            COM_CALL0(resource, Release), resource = NULL;
        }
        int px = mu.window.size.x == 0 ? 0 : ((int)rint(mu.time.seconds / 3.0 * 640)) % mu.window.size.x;
        int cy = 10;
        int cx = 10;

        /* dropped frame indicator */ {
            float colr, colg, colb;
            if (frame_i & 1) {
                colr = 1.0f, colg = 0.0f, colb = 0.0f;
            } else {
                colr = 0.0f, colg = 1.0f, colb = 1.0f;
            }
            buf_push(cb,
                     ((struct GfxQuadCommand){
                             .x0 = 10,
                             .x1 = 10 + 100,
                             .y0 = cy,
                             .y1 = cy + 100,
                             .r = colr,
                             .g = colg,
                             .b = colb,
                             .a = 1.0f,
                     }));
        }
        cy += 100;

        /* time wraparound */ {
            if (mu.time.nanoseconds < last_nanoseconds) {
                ++last_nanoseconds_wraparound_n;
            }
            last_nanoseconds = mu.time.nanoseconds;
            cy += 10;
            float colr, colg, colb;
            if (last_nanoseconds_wraparound_n & 1) {
                colr = 1.0f, colg = 0.0f, colb = 0.0f;
            } else {
                colr = 0.0f, colg = 1.0f, colb = 1.0f;
            }
            buf_push(cb,
                     ((struct GfxQuadCommand){
                             .x0 = 10,
                             .x1 = 10 + 100,
                             .y0 = cy,
                             .y1 = cy + 100,
                             .r = colr,
                             .g = colg,
                             .b = colb,
                             .a = 1.0f,
                     }));
        }
        cy += 100;

        /* logo */ {
            cy += 10;
            int w = test_image.width;
            int h = test_image.height;
            float scale = 1.0f;
            w *= scale;
            h *= scale;
            buf_push(cb,
                     ((struct GfxQuadCommand){
                             .x0 = 10,
                             .x1 = 10 + w,
                             .y0 = cy,
                             .y1 = cy + h,
                             .tx = test_image_texture,
                             .tx0 = 0.0f,
                             .tx1 = 1.0f,
                             .ty0 = 0.0f,
                             .ty1 = 1.0f,
                     }));
            cy += h;
        }

        /* frame time */ {
            cy += 10;
            int y = cy;
            int h = mu.window.size.y - y;
            int fty = (int)(h * (mu.time.delta_milliseconds * 60 / 2) / 1000.0);
            buf_push(cb, ((struct GfxQuadCommand){.x0 = 10, .x1 = 10 + 20, .y0 = cy, .y1 = cy + fty, .r = 1.0f, .g = 0.0f, .b = 0.0f}));
            cy += 10;
        }

        /* show time as a "playhead" */ {
            int const ey = mu.window.size.y;
            int pw = 3;
            buf_push(cb, ((struct GfxQuadCommand){.x0 = px, .x1 = px + pw, .y0 = ey, .y1 = 0.0f, .r = 1.0f, .g = 1.0f, .b = 1.0f}));
        }

        for (char *p = mu.text, *const p_l = mu.text + mu.text_length; p != p_l; ++p) {
            if (*p == 033 /* escape */) {
                mu.quit = MU_TRUE;
            }
        }

        if (mu.keys[/* F1 on mac */ 0x7A].pressed) {
            printf("received f1\n");
        }
        if (mu.keys[MU_SHIFT].pressed) {
            printf("received shift\n");
        }

        if (mu.text_length > 0) {
            printf("received text: %*s\n", (int)mu.text_length, mu.text);
        }

        if (mu.mouse.delta_wheel != 0) {
            printf("wheel event: %d\n", mu.mouse.wheel);
        }

        if (mu.gamepad.connected) {
            /* show stick state */
            int cx = 210;
            int cy = 110;
            struct {
                struct Mu_Stick stick;
                struct Mu_AnalogButton growth;
            } sticks[] = {{mu.gamepad.left_thumb_stick, mu.gamepad.left_trigger}, {mu.gamepad.right_thumb_stick, mu.gamepad.right_trigger}};
            float black_rgb[3] = {0.04f, 0.04f, 0.04f};
            float grey_rgb[3] = {0.94f, 0.94f, 0.94f};
            int const sticks_n = sizeof sticks / sizeof *sticks;
            for (int stick_i = 0; stick_i < sticks_n; ++stick_i) {
                int bw = 50;
                int bx = cx;
                int by = cy;
                float *color = grey_rgb;
                buf_push(cb, ((struct GfxQuadCommand){.x0 = bx, .x1 = bx + bw, .y0 = by - bw, .y1 = by, .r = color[0], .g = color[1], .b = color[2]}));

                float const x = sticks[stick_i].stick.x;
                float const y = sticks[stick_i].stick.y;
                int pw = 4 + (bw - 4) * sticks[stick_i].growth.value;
                int px = bx + bw / 2 + bw / 2 * x - pw / 2;
                int py = by - bw + bw / 2 + (-bw / 2 * y) + pw / 2;

                color = black_rgb;
                buf_push(cb, ((struct GfxQuadCommand){.x0 = px, .x1 = px + pw, .y0 = py - pw, .y1 = py, .r = color[0], .g = color[1], .b = color[2]}));
                cx += bw + 10;
            }
            cy += 50 + 10;
            cx -= 10 + 10 / 2 + 50;
            int rw = 25;
            struct {
                struct Mu_DigitalButton dir;
                struct Mu_Int2 point;
            } dpad[] = {
                    {mu.gamepad.up_button, {0, -rw}},
                    {mu.gamepad.down_button, {0, +rw}},
                    {mu.gamepad.left_button, {-rw, 0}},
                    {mu.gamepad.right_button, {+rw, 0}},
            };
            int dpad_n = sizeof dpad / sizeof *dpad;
            for (int dpad_i = 0; dpad_i < dpad_n; ++dpad_i) {
                int bw = 16;
                int bx = cx + dpad[dpad_i].point.x - bw / 2;
                int by = cy + dpad[dpad_i].point.y - bw / 2;
                float *color = dpad[dpad_i].dir.down ? black_rgb : grey_rgb;
                buf_push(cb, ((struct GfxQuadCommand){.x0 = bx, .x1 = bx + bw, .y0 = by - bw, .y1 = by, .r = color[0], .g = color[1], .b = color[2]}));
            }
        }

        /* show mouse position and clicks */ {
            for (struct ButtonTransitionCheck *c = &button_checks[0]; c < &button_checks[button_checks_n]; ++c) {
                c->n += c->button->pressed ? 1 : 0;
                c->n -= c->button->released ? 1 : 0;
                if (c->button->pressed) {
                    c->last_press_seconds = mu.time.seconds;
                    c->press_n += c->button->pressed ? 1 : 0;
                } else if (mu.time.seconds - c->last_press_seconds >= 1.0) {
                    c->press_n = 0;
                }

                int o_max_n = c->max_n;
                c->max_n = c->max_n < c->n ? c->n : c->max_n;
                if (c->max_n > o_max_n)
                    printf("%s: max transition increased to: %d\n", c->name, c->max_n);
            }
            struct Mu_Int2 mp = mu.mouse.position;
            int b = mu.mouse.left_button.down ? 8 : 4;
            int cx = mp.x;
            int cy = mp.y;
            buf_push(cb, ((struct GfxQuadCommand){.x0 = cx - b, .x1 = cx + b, .y0 = cy - b, .y1 = cy + b, .r = 0.6f, .g = 0.9f, .b = 0.8f}));

            cy += 2 * b;

            for (struct ButtonTransitionCheck *c = &button_checks[0]; c < &button_checks[button_checks_n]; ++c) {
                cy += 4;
                b = 16;
                cy += b;
                int bcx = cx;
                for (int press_i = 0; press_i < c->press_n; ++press_i) {
                    buf_push(cb, ((struct GfxQuadCommand){.x0 = bcx - b, .x1 = bcx + b, .y0 = cy - b, .y1 = cy + b, .r = 0.6f, .g = 0.9f, .b = 0.8f}));
                    bcx += 4 + 2 * b;
                }
                cy += b;
            }
        }

        if (mu.mouse.left_button.pressed) {
            mu_test_audiosynth_push_note(&mu_test_audiosynth, 432.0 * pow(2.0, mu.mouse.position.x / 240.0));
        }
        if (mu.mouse.right_button.pressed) {
            mu_test_audiosynth_push_sample(&mu_test_audiosynth, &test_audio);
        }

        if (mu.gamepad.a_button.pressed) {
            printf("A button was pressed\n");
        }

        // Render to D3D

        COM_CALL(mu.win32->d3d11_device_context, OMSetRenderTargets, 1, &output_view, NULL);
        {
            float ClearColor[4] = {0.5f, 0.5f, 0.5f, 1.0f};
            COM_CALL(mu.win32->d3d11_device_context, ClearRenderTargetView, output_view, ClearColor);
        }

        COM_CALL(mu.win32->d3d11_device_context, RSSetState, rasterizer_state);
        {
            D3D11_VIEWPORT viewport = {
                    .TopLeftX = 0,
                    .TopLeftY = 0,
                    .Width = mu.window.size.x,
                    .Height = mu.window.size.y,
                    .MinDepth = 0.0,
                    .MaxDepth = 1.0,
            };
            COM_CALL(mu.win32->d3d11_device_context, RSSetViewports, 1, &viewport);
        }

        if (buf_len(cb) > 0) {
            struct GfxQuadCommand *command_l = buf_end(cb);
            struct GfxQuadCommand *command_i = &cb[0];
            size_t draw_batches_n = 0;
            while (command_i < command_l) {
                // truncate intermediary vertex and index buffers
                if (vb)
                    buf__hdr(vb)->len = 0;
                if (ib)
                    buf__hdr(ib)->len = 0;

                struct ID3D11Texture2D *batch_tx = NULL;

                for (uint64_t prev_material = command_i < command_l ? (uint64_t)command_i->tx : 0, material = prev_material;
                     (command_i < command_l && prev_material == (material = (uint64_t)command_i->tx));
                     ++command_i, prev_material = material) {
                    batch_tx = command_i->tx;
                    struct GfxQuadCommand *c_i = command_i;
                    int v_f = buf_len(vb);
                    buf_push(vb,
                             ((struct FlatVertex2d){
                                     .posx = c_i->x0,
                                     .posy = c_i->y0,
                                     .tx = c_i->tx0,
                                     .ty = c_i->ty0,
                             }));
                    buf_push(vb,
                             ((struct FlatVertex2d){
                                     .posx = c_i->x1,
                                     .posy = c_i->y0,
                                     .tx = c_i->tx1,
                                     .ty = c_i->ty0,
                             }));
                    buf_push(vb,
                             ((struct FlatVertex2d){
                                     .posx = c_i->x1,
                                     .posy = c_i->y1,
                                     .tx = c_i->tx1,
                                     .ty = c_i->ty1,
                             }));
                    buf_push(vb,
                             ((struct FlatVertex2d){
                                     .posx = c_i->x0,
                                     .posy = c_i->y1,
                                     .tx = c_i->tx0,
                                     .ty = c_i->ty1,
                             }));
                    int v_l = buf_len(vb);
                    for (int v_i = v_f; v_i < v_l; v_i++) {
                        vb[v_i].colr = c_i->r;
                        vb[v_i].colg = c_i->g;
                        vb[v_i].colb = c_i->b;
                        vb[v_i].cola = c_i->a;
                        vb[v_i].posx = (vb[v_i].posx - 0.5f) * (2.0) / mu.window.size.x - 1.0f;
                        vb[v_i].posy = (vb[v_i].posy - 0.5f) * (-2.0) / mu.window.size.y + 1.0f;
                    }
                    buf_push(ib, v_f + 0);
                    buf_push(ib, v_f + 1);
                    buf_push(ib, v_f + 2);
                    buf_push(ib, v_f + 2);
                    buf_push(ib, v_f + 3);
                    buf_push(ib, v_f + 0);
                }
                // flush primitives
                if (buf_len(vb) > 0) {
                    struct FlatVertex2d *d_vb;
                    struct ID3D11Resource *res = (struct ID3D11Resource *)imm_vertex_buffer;
                    HRESULT hr;
                    D3D11_MAPPED_SUBRESOURCE mapped_data;
                    if (hr = COM_CALL(mu.win32->d3d11_device_context, Map, res, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_data) == 0) {
                        size_t bytes_n = buf_len(vb) * sizeof vb[0];
                        d_vb = mapped_data.pData;
                        memcpy(d_vb, vb, bytes_n);
                        COM_CALL(mu.win32->d3d11_device_context, Unmap, res, 0);
                    } else {
                        printf("ERROR: Map(imm_vertex_buffer) with 0x%x\n", hr);
                        return 1;
                    }
                }
                if (buf_len(ib) > 0) {
                    uint16_t *d_ib;
                    struct ID3D11Resource *res = (struct ID3D11Resource *)imm_index_buffer;
                    HRESULT hr;
                    D3D11_MAPPED_SUBRESOURCE mapped_data;
                    if (hr = COM_CALL(mu.win32->d3d11_device_context, Map, res, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_data) == 0) {
                        size_t bytes_n = buf_len(ib) * sizeof ib[0];
                        d_ib = mapped_data.pData;
                        memcpy(d_ib, ib, bytes_n);
                        COM_CALL(mu.win32->d3d11_device_context, Unmap, res, 0);
                    } else {
                        printf("ERROR: Map(imm_index_buffer) with 0x%x\n", hr);
                        return 1;
                    }
                }
                COM_CALL(mu.win32->d3d11_device_context, IASetIndexBuffer, imm_index_buffer, DXGI_FORMAT_R16_UINT, 0);
                COM_CALL(mu.win32->d3d11_device_context, IASetPrimitiveTopology, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                UINT imm_vertex_buffer_stride = sizeof(struct FlatVertex2d);
                UINT imm_vertex_buffer_offset = 0;
                COM_CALL(mu.win32->d3d11_device_context, IASetVertexBuffers, 0, 1, &imm_vertex_buffer, &imm_vertex_buffer_stride, &imm_vertex_buffer_offset);
                COM_CALL(mu.win32->d3d11_device_context, IASetInputLayout, imm_layout);
                COM_CALL(mu.win32->d3d11_device_context, VSSetShader, ortho_vs, NULL, 0);
                if (batch_tx != NULL) {
                    HRESULT hr;
                    ID3D11Resource *resource = (ID3D11Resource *)batch_tx;
                    ID3D11ShaderResourceView *view;
                    if (hr = COM_CALL(mu.win32->d3d11_device, CreateShaderResourceView, resource, NULL, &view), hr != 0) {
                        printf("ERROR: CreateShaderResourceView(batch_tx) with hr: 0x%x\n", hr);
                        return 1;
                    }
                    COM_CALL(mu.win32->d3d11_device_context, PSSetShaderResources, 0, 1, &view);
                    COM_CALL0(view, Release);
                    COM_CALL(mu.win32->d3d11_device_context, PSSetShader, tex_ps, NULL, 0);
                } else {
                    COM_CALL(mu.win32->d3d11_device_context, PSSetShaderResources, 0, 0, NULL);
                    COM_CALL(mu.win32->d3d11_device_context, PSSetShader, flat_ps, NULL, 0);
                }

                COM_CALL(mu.win32->d3d11_device_context, DrawIndexed, buf_len(ib), 0, 0);
                draw_batches_n++;
            }
        }

        Mu_Push(&mu);
        COM_CALL0(output_view, Release), output_view = NULL;

        ++frame_i;
        theta += 0.01f;
    }
    return 0;
}

#if defined(__APPLE__)
#include "mu_test_macos.c"
#elif defined(_WIN32)
#include "mu_test_win32.c"
#endif

#undef MU_TEST_INTERNAL

#if defined(_WIN32) && defined(_MSC_VER)
// needed by pervognsen_mu
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "Windowscodecs.lib")
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#pragma comment(lib, "dxguid.lib")
#endif
