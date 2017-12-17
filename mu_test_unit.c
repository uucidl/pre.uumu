// @language: c11

//
// test program, to test for dropped vsync, dropped video frames,
// audio/video synchronization and gamepad support.
//

#include "xxxx_mu.h"

#include "xxxx_mu_cocoa.h" // @todo: @platform{macos} because I need the platform specific keyname

#include "OpenGL/gl.h" // @todo: @platform{macos} specific location

#if defined(__STDC_NO_ATOMICS__)
#error "Error: C11 atomics not found"
#endif

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MU_TEST_INTERNAL static
// @note: marks assets
#define MU_TEST_ASSET(x__) x__

struct Mu_Test_AudioNote_InitParameters
{
     float pitch_hz;
     struct Mu_AudioBuffer *optional_source;
};

struct Mu_Test_AudioNote_Playing
{
     struct Mu_Test_AudioNote_InitParameters init_parameters;
     double phase;
     int source_frame_i;
};

enum {
     MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER = 3,
    MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_COUNT = 1<<MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER,
    MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_MASK = (1<<MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER) - 1,
     MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY = 64,
};

struct Mu_Test_AudioSynth
{
     atomic_uint input_notes_rb_write_n; // @shared
     atomic_uint input_notes_rb_read_n; // @shared
     struct Mu_Test_AudioNote_InitParameters input_notes_rb[MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_COUNT]; // @shared

     int playing_notes_n;
     struct Mu_Test_AudioNote_Playing playing_notes[MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY];
};

MU_TEST_INTERNAL
struct Mu_Test_AudioSynth mu_test_audiosynth;

MU_TEST_INTERNAL
double db_to_amp (double volume_in_db)
{
    return pow (exp (volume_in_db), log (10.0) / 20.0);
}

MU_TEST_INTERNAL
int audio_mix(struct Mu_AudioBuffer const * source, struct Mu_AudioBuffer *dest, double amp, int s_frame_i)
{
     int16_t* const s_sample_f = source->samples;
     int16_t* const s_sample_l = s_sample_f + source->samples_count;
     int16_t* s_sample = s_sample_f + s_frame_i;
     int16_t* const d_sample_f = dest->samples;
     int16_t* const d_sample_l = dest->samples + dest->samples_count;
     int16_t* d_sample = d_sample_f;
     if (source->format.channels == 1) {
          while (d_sample != d_sample_l && s_sample != s_sample_l) {
               int16_t x = amp * (*s_sample);
               for(int n = dest->format.channels; n--;) {
                    *d_sample++ += x;
               }
               ++s_sample;
          }
     } else {
          while (d_sample != d_sample_l && s_sample != s_sample_l) {
               int s_channels = source->format.channels;
               int d_channels = dest->format.channels;
               int channels_n = d_channels < s_channels ? d_channels:s_channels;
               for (int channel_i=0; channel_i < channels_n; ++channel_i) {
                    d_sample[channel_i] += amp*s_sample[channel_i];
               }
               d_sample += d_channels;
               s_sample += s_channels;
          }
     }
     return s_sample - s_sample_f;
}

MU_TEST_INTERNAL
void main_audio_callback(struct Mu_AudioBuffer *audiobuffer)
{
     int16_t * const stereo_frames = audiobuffer->samples;
     int const frames_n = audiobuffer->samples_count/audiobuffer->format.channels;
     double const sr_hz = audiobuffer->format.samples_per_second;
     memset(stereo_frames, 0, frames_n*audiobuffer->format.channels*audiobuffer->format.bytes_per_sample);

     struct Mu_Test_AudioSynth * const synth = &mu_test_audiosynth;
     
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
	  struct Mu_Test_AudioNote_Playing * const note = &synth->playing_notes[note_i];
	  double phase = note->phase;
	  double phase_delta = note->init_parameters.pitch_hz / sr_hz;
	  double const amp = db_to_amp(-20.0);
	  _Bool note_has_ended = false;
	  for (int frame_i = 0; frame_i < frames_n; ++frame_i) {
	       double const env = fmin(1.0, exp(-6.0 * phase * 0.001));
	       float const y = (float) (env * amp * sin(TAU*phase));
	       stereo_frames[2*frame_i] += 32767.0f*y;
	       stereo_frames[2*frame_i + 1] += 32767.0f*y;
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
void mu_test_audiosynth_initialize(struct Mu_Test_AudioSynth * const synth)
{
     atomic_init(&synth->input_notes_rb_read_n, 0);
     atomic_init(&synth->input_notes_rb_write_n, 0);
}

MU_TEST_INTERNAL
bool mu_test_audiosynth_push_event(struct Mu_Test_AudioSynth * const synth, double const pitch_hz, struct Mu_AudioBuffer *optional_source)
{
    unsigned int write_n = atomic_load(&synth->input_notes_rb_write_n);
    unsigned int read_n = atomic_load(&synth->input_notes_rb_read_n);
    if (write_n - read_n >=MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_COUNT) {
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
bool mu_test_audiosynth_push_note(struct Mu_Test_AudioSynth * const synth, double const pitch_hz)
{
     mu_test_audiosynth_push_event(synth, pitch_hz, NULL);
     return true;
}

MU_TEST_INTERNAL
bool mu_test_audiosynth_push_sample(struct Mu_Test_AudioSynth * const synth, struct Mu_AudioBuffer *source)
{
     mu_test_audiosynth_push_event(synth, 0.0, source);
     return true;
}

MU_TEST_INTERNAL
int platform_get_resource_path(char* buffer, int buffer_n, char const * const relative_path, int relative_path_n);

int main(int argc, char **argv)
{
     mu_test_audiosynth_initialize(&mu_test_audiosynth);
     struct Mu mu = {
	  .window.position.x=640,
	  .window.size.x=640,
	  .window.size.y=480,
	  .gamepad.left_trigger.threshold=1.0/50.0f,
	  .gamepad.right_trigger.threshold=1.0/50.0f,
	  .gamepad.left_thumb_stick.threshold=1.0/50.0f,
	  .gamepad.right_thumb_stick.threshold=1.0/50.0f,
	  .audio.callback = main_audio_callback,
     };
     if (!Mu_Initialize(&mu)) {
	  printf("ERROR: Mu could not initialize: '%s'\n", mu.error);
	  return 1;
     }

     struct Mu_AudioBuffer test_audio;
     Mu_Bool test_audio_loaded = MU_FALSE;
     char const * test_sound_path = MU_TEST_ASSET("test_assets/chime.wav");
     char buffer[4096];
     int const buffer_n = sizeof buffer;
     if (buffer_n != platform_get_resource_path(buffer, buffer_n, test_sound_path, strlen(test_sound_path))) {
          test_audio_loaded = Mu_LoadAudio(buffer, &test_audio);
          if (!test_audio_loaded) printf("ERROR: Mu could not load file: '%s'\n", buffer);
     }
     if (!test_audio_loaded) test_audio_loaded = Mu_LoadAudio(test_sound_path, &test_audio);
     if (!test_audio_loaded) {
          printf("ERROR: Mu could not load file: '%s'\n", test_sound_path);
          return 1;
     }

     struct Mu_Image test_image = {0,};
     char const *test_image_path = "test_assets/ln2.png";
     if (buffer_n != platform_get_resource_path(buffer, buffer_n, test_image_path, strlen(test_image_path))) {
       Mu_Bool test_image_loaded = Mu_LoadImage(buffer, &test_image);
       if(!test_image_loaded) printf("ERROR: Mu could not load file: '%s'\n", buffer);
     }

     float theta = 0.0f;
     int frame_i = 0;
     GLuint test_image_texture_id = 0;
     GLuint const defGL_TEXTURE_RECTANGLE = 0x84F5;

     while (Mu_Pull(&mu)) {
          if (test_image_texture_id == 0) {
               glGenTextures(1, &test_image_texture_id);
               if (test_image.width * test_image.height && test_image.channels == 4) {
                    GLint target = defGL_TEXTURE_RECTANGLE;
                    glBindTexture(target, test_image_texture_id);
                    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexImage2D(target, 0, GL_RGBA, test_image.width, test_image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, test_image.pixels);
                    glBindTexture(target, 0);
               }
          }

          if (frame_i == 0 || mu.window.resized) {
               glViewport(0, 0, mu.window.size.x, mu.window.size.y);
               glMatrixMode(GL_MODELVIEW);
               glLoadIdentity();
               glMatrixMode(GL_PROJECTION);
               glLoadIdentity();
               glOrtho(0.0, mu.window.size.x, mu.window.size.y, 0, -1.0, 1.0);
               printf("ortho %d %d\n", mu.window.size.x, mu.window.size.y);
          }
          // some debugging GL2 style code
          glClearColor(fabs(sin(theta)), fabs(sin(3*theta)), 0.6f, 0.0f);
          glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
          glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
          int px = ((int)rint(mu.time.seconds / 3.0 * 640)) % mu.window.size.x;
          int cy = 10;

          /* dropped frame indicator */ {
               if (frame_i & 1) {
                    glColor3f(1.0f, 0.0f, 0.0f);
               } else {
                    glColor3f(0.0f, 1.0f, 1.0f);
               }
               glBegin(GL_QUADS);
               glVertex2f(10, cy);
               glVertex2f(10+100, cy);
               glVertex2f(10+100, cy+100);
               glVertex2f(10, cy+100);
               glEnd();
          }
          cy += 100;

          /* logo */ {
            cy += 10;
            glBindTexture(defGL_TEXTURE_RECTANGLE, test_image_texture_id);
            glEnable(defGL_TEXTURE_RECTANGLE);
            glColor3f(1.0f, 1.0f, 1.0f);
            int w = test_image.width;
            int h = test_image.height;
            glBegin(GL_QUADS);
            glTexCoord2i(0, 0);                                 glVertex2f(10, cy);
            glTexCoord2i(test_image.width, 0);                  glVertex2f(10+w, cy);
            glTexCoord2i(test_image.width, test_image.height);  glVertex2f(10+w, cy+h);
            glTexCoord2i(0, test_image.height);                 glVertex2f(10, cy+h);
            glDisable(defGL_TEXTURE_RECTANGLE);
            cy += h;
          }

          /* frame time */ {
            cy += 10;
            glColor3f(1.0f, 0.0f, 0.0f);
            int y = cy;
            int h = mu.window.size.y - y;
            int fty = (int)(h * (mu.time.delta_milliseconds * 60 / 2) / 1000.0);
            glBegin(GL_QUADS);
            glVertex2f(10, cy);
            glVertex2f(10 + 20, cy);
            glVertex2f(10 + 20, cy + fty);
            glVertex2f(10, cy + fty);
            glEnd();
            cy += 10;
          }

      for(char *p = mu.text, * const p_l = mu.text + mu.text_length; p != p_l; ++p) {
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
               } sticks[] = {
                    { mu.gamepad.left_thumb_stick, mu.gamepad.left_trigger },
                    { mu.gamepad.right_thumb_stick, mu.gamepad.right_trigger }
               };
               int const sticks_n = sizeof sticks / sizeof *sticks;
               for (int stick_i = 0; stick_i < sticks_n; ++stick_i) {
                    int bw = 50;
                    int bx = cx;
                    int by = cy;
                    glColor3f(0.94f, 0.94f, 0.94f);
                    glBegin(GL_QUADS);
                    glVertex2f(bx, by-bw);
                    glVertex2f(bx+bw, by-bw);
                    glVertex2f(bx+bw, by);
                    glVertex2f(bx, by);
                    glEnd();

                    float const x = sticks[stick_i].stick.x;
                    float const y = sticks[stick_i].stick.y;
                    int pw = 4 + (bw-4)*sticks[stick_i].growth.value;
                    int px = bx + bw/2 + bw/2*x - pw/2;
                    int py = by - bw + bw/2 + (-bw/2*y) + pw/2;

                    glColor3f(0.04f, 0.04f, 0.04f);
                    glBegin(GL_QUADS);
                    glVertex2f(px, py-pw);
                    glVertex2f(px+pw, py-pw);
                    glVertex2f(px+pw, py);
                    glVertex2f(px, py);
                    glEnd();

                    cx += bw + 10;
               }
          }

          /* show mouse position */ {
               struct Mu_Int2 mp = mu.mouse.position;
               int b = mu.mouse.left_button.down? 8:4;
               glColor3f(0.6f, 0.9f, 0.8f);
               glBegin(GL_QUADS);
               glVertex2f(mp.x-b, mp.y-b);
               glVertex2f(mp.x+b, mp.y-b);
               glVertex2f(mp.x+b, mp.y+b);
               glVertex2f(mp.x-b, mp.y+b);
               glEnd();
          }

          if (mu.mouse.left_button.pressed) {
               mu_test_audiosynth_push_note(&mu_test_audiosynth, 432.0 * pow(2.0, mu.mouse.position.x/240.0));
          }
          if (mu.mouse.right_button.pressed) {
               mu_test_audiosynth_push_sample(&mu_test_audiosynth, &test_audio);
          }

          if (mu.gamepad.a_button.pressed) {
               printf("A button was pressed\n");
          }

          int const ey = mu.window.size.y;
          int pw = 3;

          glColor3f(1.0f, 1.0f, 1.0f);
          glBegin(GL_QUADS);
          glVertex2f(px, ey);
          glVertex2f(px+pw, ey);
          glVertex2f(px+pw, 0.0f);
          glVertex2f(px, 0.0f);
          glEnd();

          Mu_Push(&mu);
          ++frame_i;
          theta += 0.01f;
     }
     return 0;
}

#include "mu_test_macos.c"

#undef MU_TEST_INTERNAL
