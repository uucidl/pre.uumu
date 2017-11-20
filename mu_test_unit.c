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

struct Mu_Test_AudioNote_InitParameters
{
     float pitch_hz;
};

struct Mu_Test_AudioNote_Playing
{
     struct Mu_Test_AudioNote_InitParameters init_parameters;
     uint64_t origin_sample;
     double phase;
};

enum {
     MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER = 2,
     MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY = 32,
};

struct Mu_Test_AudioSynth
{
     atomic_uint input_notes_rb_write_n; // @shared
     atomic_uint input_notes_rb_read_n; // @shared
     struct Mu_Test_AudioNote_InitParameters input_notes_rb[1 << MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER]; // @shared

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
void main_audio_callback(struct Mu_AudioBuffer *audiobuffer)
{
     int16_t * const stereo_frames = audiobuffer->samples;
     int const frames_n = audiobuffer->samples_count;
     double const sr_hz = audiobuffer->format.samples_per_second;

     struct Mu_Test_AudioSynth * const synth = &mu_test_audiosynth;
     
     unsigned int input_notes_n = atomic_load(&synth->input_notes_rb_write_n);
     unsigned int input_notes_read_n = atomic_load(&synth->input_notes_rb_read_n);
     for (unsigned int input_note_i = input_notes_read_n; input_note_i != input_notes_n; input_note_i++) {
	  int next_note_i = synth->playing_notes_n;
	  if (next_note_i != MU_TEST_AUDIOSYNTH_PLAYING_NOTES_CAPACITY) {
	       synth->playing_notes_n++;
	       synth->playing_notes[next_note_i] = (struct Mu_Test_AudioNote_Playing){
		    { .pitch_hz = synth->input_notes_rb[input_note_i & MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER].pitch_hz },
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
	  note->phase = phase;
	  if (note_has_ended) {
	       synth->playing_notes_n--;
	       if (synth->playing_notes_n > 0) {
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
void mu_test_audiosynth_push_note(struct Mu_Test_AudioSynth * const synth, double const pitch_hz)
{
     int write_n = atomic_load(&synth->input_notes_rb_write_n);
     int read_n = atomic_load(&synth->input_notes_rb_read_n);
     if (write_n - read_n < (1<<MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER)) {
	  unsigned int input_note_i = (write_n & MU_TEST_AUDIOSYNTH_INPUT_NOTES_RINGBUFFER_POWER);
	  synth->input_notes_rb[input_note_i] = (struct Mu_Test_AudioNote_InitParameters){
	       .pitch_hz = pitch_hz,
	  };
	  atomic_store(&synth->input_notes_rb_write_n, write_n + 1);
     }
}

int main(int argc, char **argv)
{
     mu_test_audiosynth_initialize(&mu_test_audiosynth);
     struct Mu mu = {
	  .window.position.x=640,
	  .window.size.x=640,
	  .window.size.y=480,
	  .gamepad.left_trigger.threshold=0.01f,
	  .gamepad.right_trigger.threshold=0.01f,
	  .audio.callback = main_audio_callback,
     };
     if (!Mu_Initialize(&mu)) {
	  printf("ERROR: Mu could not initialize: '%s'\n", mu.error);
	  return 1;
     }
     glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
     glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

     float theta = 0.0f;
     int frame_i = 0;
     while (Mu_Pull(&mu)) {
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

	  /* dropped frame indicator */ {
	       if (frame_i & 1) {
		    glColor3f(1.0f, 0.0f, 0.0f);
	       } else {
		    glColor3f(0.0f, 1.0f, 1.0f);
	       }
	       glBegin(GL_QUADS);
	       glVertex2f(10, 10);
	       glVertex2f(10+100, 10);
	       glVertex2f(10+100, 10+100);
	       glVertex2f(10, 10+100);
	       glEnd();
	  }

	  /* frame time */ {
	       glColor3f(1.0f, 0.0f, 0.0f);
	       int y = 100 + 10;
	       int h = mu.window.size.y - y;
	       int fty = (int)(h * (mu.time.delta_milliseconds * 60 / 2) / 1000.0);
	       glBegin(GL_QUADS);
	       glVertex2f(10, 120);
	       glVertex2f(10 + 20, 120);
	       glVertex2f(10 + 20, 120 + fty);
	       glVertex2f(10, 120 + fty);
	       glEnd();
	  }

	  if (mu.keys[/* escape key on mac */ 0x35].pressed) {
	       mu.quit = MU_TRUE;
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
		    int pw = 4 + 8*sticks[stick_i].growth.value;
		    int px = bx + bw/2 + bw/2*x - pw/2;
		    int py = by - bw + bw/2 + bw/2*y + pw/2;
		    
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
	       glColor3f(0.5f, 0.7f, 0.2f);
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
#undef MU_TEST_INTERNAL
