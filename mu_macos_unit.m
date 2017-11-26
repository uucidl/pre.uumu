// @author: nicolas@uucidl.com
// @license: MIT
// @date: 2017-11
// @language: c11
// @language: objective-c
// @platform: macos
// @framework_list: AppKit, AudioToolbox, CoreAudio, IOKit, OpenGL

// Integration:
// ------------
//
// this file MUST be compiled in its own translation unit, unless you
// define the preprocessor macro _XOPEN_SOURCE=600 or
// MU_MACOS_RUN_MODE_PLAIN.

// Configuration macros:
// ---------------------
#define MU_MACOS_RUN_MODE_PLAIN (0xaf49a739)
#define MU_MACOS_RUN_MODE_COROUTINE (0x4693402a)
#if defined(MU_MACOS_RUN_MODE)
#if MU_MACOS_RUN_MODE != MU_MACOS_RUN_MODE_PLAIN && MU_MACOS_RUN_MODE != MU_MACOS_RUN_MODE_COROUTINE
#error "Unknown MU_MACOS_RUN_MODE, must be either MU_MACOS_RUN_MODE_PLAIN or MU_MACOS_RUN_MODE_COROUTINE"
#endif
#else
// use plain if this crashes for you
#define MU_MACOS_RUN_MODE MU_MACOS_RUN_MODE_COROUTINE
#endif

// Gamepad not supported?
// ----------------------
//
// Gamepad support has been implemented for the Sony DUALSHOCK4. In
// theory it's easy to add new mappings for a USB-HID compliant
// controllers, check @fn{mu_gamepad_initialize} and how it sets the
// hid mapping table.
//
//
// Implementation Details
// ----------------------
//
// To let the user-code run continously during a resize, we implement
// a fiber-based solution similar to what Per Vognsen implemented on
// windows, this time using the setcontext/getcontext/swapcontext BSD
// APIs. These have been deprecated from POSIX but BSD kernels like
// darwin should still support them.
//
// Nevertheless if you have any problem with them, simply define
// MU_MACOS_RUN_MODE_PLAIN before compiling.

#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
#define _XOPEN_SOURCE 600
#include <ucontext.h>
#endif

#include "xxxx_mu.h" // public API as published by Per Vognsen
#include "xxxx_mu_cocoa.h"

#include <AppKit/AppKit.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <IOKit/hid/IOHIDLib.h>
#include <OpenGL/gl.h>

#include <mach/mach_time.h>

#include <assert.h>

#if defined(__STDC_NO_ATOMICS__)
#error "Error: C11 atomics not found"
#endif
#include <stdatomic.h>

#define MU_MACOS_INTERNAL static
#define MU_MACOS_TRACEF(...) printf("Mu: " __VA_ARGS__);

enum {
     MU_MAX_AUDIO_CHANNELS = 2,
     MU_DEFAULT_WIDTH = 640,
     MU_DEFAULT_HEIGHT = 480,
};

// @see @url{https://en.wikipedia.org/wiki/X_Macro} to understand the
// idiom used for the XENUM macros below.

#define MU_GAMEPAD_DIGITAL_BUTTONS_XENUM \
     X(a_button)			 \
     X(b_button)			 \
     X(x_button)			 \
     X(y_button)			 \
     X(left_shoulder_button)		 \
     X(right_shoulder_button)		 \
     X(up_button)			 \
     X(down_button)			 \
     X(left_button)			 \
     X(right_button)			 \
     X(left_thumb_button)		 \
     X(right_thumb_button)		 \
     X(back_button)			 \
     X(start_button)

#define MU_GAMEPAD_STICKS_XENUM \
     X(left_thumb_stick) \
     X(right_thumb_stick)

#define MU_GAMEPAD_ANALOG_BUTTONS_XENUM \
     X(left_trigger) \
     X(right_trigger)

@interface Mu_WindowDelegate : NSObject<NSWindowDelegate>
@end

@interface Mu_OpenGLView : NSOpenGLView
{
     @public struct Mu_Session *session;
}
@end

// For most gamepads, it will be sufficient to do a mapping between
// their HID mapping profile and the typical user expectations.
//
// For performance with the most common gamepads it might make sense
// to process their input reports directly. (Like for the Sony DS4 for
// which we have bit layout)
//
// Also an extension might allow getting control specific niceties
// like the touchpad or the lights of the Sony DS4.
//
// Some documentation about hid:
// @url: http://www.usb.org/developers/hidpage/
//
// We here define some data-structures to map normal HID elements to
// what we effectively want for the gamepad.
//
// On Macos, use ioreg -p IOUSB to see USB devices
// ioreg -l to show all their properties.


// @idea{
// Thresholds and deadzones for thumbsticks
// see @url: http://www.third-helix.com/2013/04/12/doing-thumbstick-dead-zones-right.html
//
// The author recommend rather than independendant fabs(x) thresholds to use either:
//
// - axial deadzone (independant) - helps with 4 way movements
// - radial deadzone (euclidean) - helps with sweeps
// - normalized radial deadzone - for 3d pointing
//
// The best API style would allow users to set those up depending on the interaction style.
// }

// @todo @idea{
// @url: https://github.com/gabomdq/SDL_GameControllerDB
// }
struct Mu_HIDAddress
{
     uint16_t usage_page;
     uint16_t usage;
};

enum
{
     MU_HID_MAPPING_MAX_STATES = 4,
};

struct Mu_Gamepad_HID_Mapping_DigitalButton
{
     struct Mu_HIDAddress address;
     // states_n == 0 for normal buttons, otherwise states_n <=
     // MU_HID_MAPPING_MAX_STATES for HID buttons that trigger on
     // different discrete values. (Hatswitch)
     int states_n;
     int states[MU_HID_MAPPING_MAX_STATES];     
};

struct Mu_Gamepad_HID_Mapping_AnalogButton
{
     struct Mu_HIDAddress address;
     float xmin, xmax;
};

struct Mu_Gamepad_HID_Mapping_Stick
{
     struct Mu_HIDAddress x_address;
     float xmin, xmax;
     struct Mu_HIDAddress y_address;
     float ymin, ymax;
};

// Descriptors of how HID elements are bound to members of Mu_Gamepad
struct Mu_Gamepad_HID_Mapping
{
#define X(button_name) struct Mu_Gamepad_HID_Mapping_DigitalButton button_name;
     MU_GAMEPAD_DIGITAL_BUTTONS_XENUM
#undef X
     
#define X(analog_button_name) struct Mu_Gamepad_HID_Mapping_AnalogButton analog_button_name;
     MU_GAMEPAD_ANALOG_BUTTONS_XENUM
#undef X

#define X(stick_name) struct Mu_Gamepad_HID_Mapping_Stick stick_name;
     MU_GAMEPAD_STICKS_XENUM
#undef X
};

// Persistent data-structure holding resources and data that are
// maintained for the API.
struct Mu_Session
{
     // public resources, as published in the Mu structure.
     struct Mu_Cocoa cocoa_resources;

     struct Mu *pull_destination;
     
#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
     char* run_loop_fiber_stack;
     ucontext_t run_loop_fiber;
     ucontext_t main_fiber;
#endif
     // gamepad    
     IOHIDManagerRef hidmanager;
     struct Mu_Gamepad_HID_Mapping gamepad_hid_mapping;

     // can be touched more than once per `Mu_Pull`, due to the event
     // based nature of the Macos IOKit APIs.
     struct Mu_Gamepad gamepad; 
     
     // video&opengl session state
     Mu_WindowDelegate *window_delegate;
     Mu_OpenGLView *opengl_view;
     Mu_Bool macos_wants_us_to_quit;
     
     // audio session state
     atomic_flag output_audio_isdefault;
     struct Mu_Audio audio;
     AudioDeviceID DeviceID;
     AudioDeviceIOProcID IOProcID;
     int audio_channels_n;
     int audio_channels[MU_MAX_AUDIO_CHANNELS];
#if !defined(NDEBUG)
     // @debug signal
     double audio_debug_signal_phase;
     double audio_debug_signal_phase_inc;
#endif
};

// Mu Audio:

MU_MACOS_INTERNAL
struct Mu_AudioFormat const mu_audio_audioformat = { 48000, 2, sizeof (int16_t) };

MU_MACOS_INTERNAL
void mu_audio_emit_silence(struct Mu_AudioBuffer* buffer)
{
     memset(buffer->samples, 0, buffer->samples_count * buffer->format.bytes_per_sample);
}

// partition the formats array into good formats (first) and bad formats (last) and return the size of the good format partition (partition point)
MU_MACOS_INTERNAL
int mu_coreaudio_formats_partition(int formats_n, AudioStreamRangedDescription formats[formats_n], struct Mu_AudioFormat const audioformat)
{
     int formats_good_n = 0;
     
     while (formats_good_n < formats_n) {
	  AudioStreamRangedDescription format = formats[formats_good_n];
	  if (format.mFormat.mChannelsPerFrame < audioformat.channels) goto known_bad;
	  if (format.mFormat.mFormatID != kAudioFormatLinearPCM) {
	       goto known_bad;
	  }
	  if (format.mFormat.mFormatFlags != (kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked)) {
	       goto known_bad;
	  }
	  if (kAudioStreamAnyRate == format.mFormat.mSampleRate) {
	       if (audioformat.samples_per_second < format.mSampleRateRange.mMinimum) goto known_bad;
	       if (audioformat.samples_per_second > format.mSampleRateRange.mMaximum) goto known_bad;
	  } else if (fabs(audioformat.samples_per_second - format.mFormat.mSampleRate) > 0.01) {
	       goto known_bad;
	  }

	  ++formats_good_n;
	  continue;

     known_bad:
	  --formats_n;
	  if (formats_good_n < formats_n) {
	       AudioStreamRangedDescription temp = formats[formats_n];
	       formats[formats_n] = format;
	       formats[formats_good_n] = temp;
	  }
     }
     return formats_good_n;
}

MU_MACOS_INTERNAL
AudioObjectPropertyAddress const MU_MACOS_COREAUDIO_DEFAULT_OUTPUT_DEVICE_PROPERTY_ADDRESS = (AudioObjectPropertyAddress){.mSelector=kAudioHardwarePropertyDefaultOutputDevice, .mScope=kAudioObjectPropertyScopeGlobal, .mElement=kAudioObjectPropertyElementMaster};


MU_MACOS_INTERNAL
OSStatus mu_coreaudio_property_listener(AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses, void *inClientData)
{
     struct Mu_Session *session = inClientData;
     for (AudioObjectPropertyAddress const *address_c = inAddresses; address_c != inAddresses + inNumberAddresses; ++address_c) {
	  if (0 == memcmp(address_c, &MU_MACOS_COREAUDIO_DEFAULT_OUTPUT_DEVICE_PROPERTY_ADDRESS, sizeof *address_c)) {
	       atomic_flag_clear(&session->output_audio_isdefault);
	  }
     }
     return noErr;
}

MU_MACOS_INTERNAL
OSStatus
mu_coreaudio_callback(
     AudioDeviceID           inDevice,
     const AudioTimeStamp*   inNow,
     const AudioBufferList*  inInputData,
     const AudioTimeStamp*   inInputTime,
     AudioBufferList*        outOutputData,
     const AudioTimeStamp*   inOutputTime,
     void* inClientData)
{
     struct Mu_Session *session = inClientData;
     struct Output {
	  float *dest;
	  int frame_stride;
	  int frame_n;
	  int channel;
     } outputs[2] = {
	  { .channel = session->audio_channels[0] },
	  { .channel = session->audio_channels[1] },
     };
     int const outputs_n = 2;
     struct Output* outputs_l = outputs + outputs_n;
     int found_outputs_n = 0;

     /* bind channels to output buffers */ {
	  int channel = 1;
	  for (int buffer_i = 0; found_outputs_n != 2 && buffer_i < outOutputData->mNumberBuffers; ++buffer_i) {
	       AudioBuffer * const buffer = &outOutputData->mBuffers[buffer_i];
	       int const channel_n = buffer->mNumberChannels;
	       for (int buffer_channel_i = 0; found_outputs_n != 2 && buffer_channel_i < channel_n; ++buffer_channel_i) {
		    struct Output *output = outputs + found_outputs_n;
		    while (output != outputs_l && output->channel != channel) {
			 ++output;
		    }
		    if (output != outputs_l) {
			 output->dest = ((float*)buffer->mData) + buffer_channel_i;
			 output->frame_stride = channel_n;
			 output->frame_n = buffer->mDataByteSize / channel_n / sizeof *output->dest;
			 ++found_outputs_n;
		    }
		    ++channel;
	       }
	  }
     }

     if (found_outputs_n < outputs_n) return noErr;
     if (outputs[0].frame_n != outputs[1].frame_n) return noErr; // actually that's weird
     if (outputs[0].frame_n == 0) return noErr; // that's weird too
     
     // generate into temporary buffers
     struct Mu_AudioFormat audioformat = {
	  .samples_per_second = session->audio.format.samples_per_second,
	  .channels = 2,
	  .bytes_per_sample = sizeof (int16_t),
     };
     int const frame_n = outputs[0].frame_n;
     int16_t client_buffer[audioformat.channels*frame_n];
#if !defined(NDEBUG)
     // @debug default signal to let users know they should fill up the buffer
     {
          int const channels_n = audioformat.channels;
          double phase = session->audio_debug_signal_phase;
          double const phase_inc = session->audio_debug_signal_phase_inc;
          for (int frame_i = 0; frame_i < frame_n; frame_i++, phase += phase_inc) {
               double const y = 328*cos(6.2831853071795864769252*phase);
               for (int channel_i = 0; channel_i < channels_n; ++channel_i) {
                    client_buffer[channels_n*frame_i + channel_i] = y;
               }
          }
          while (phase >= 1.0) phase -= 1.0;
          session->audio_debug_signal_phase = phase;
     }
#endif
     struct Mu_AudioBuffer audiobuffer = {
	  .samples = client_buffer,
	  .samples_count = frame_n * audioformat.channels,
	  .format = audioformat
     };
     session->audio.callback(&audiobuffer);

     // convert+emit to destination
     for (int c_i = 0; c_i < 2; ++c_i) {
	  for (int frame_i = 0; frame_i < frame_n; ++frame_i) {
	       struct Output const output = outputs[c_i];
	       output.dest[frame_i * output.frame_stride] = (float) client_buffer[2*frame_i + c_i]/32768.f;
	  }
     }
     return noErr;
}

MU_MACOS_INTERNAL
Mu_Bool
mu_audio_output_start(struct Mu *mu, struct Mu_Session *session)
{
     OSStatus status = noErr;
     AudioDeviceID output_device;
     AudioObjectPropertyAddress default_output_device_property_address = (AudioObjectPropertyAddress){.mSelector=kAudioHardwarePropertyDefaultOutputDevice, .mScope=kAudioObjectPropertyScopeGlobal, .mElement=kAudioObjectPropertyElementMaster};
     for (UInt32 size = sizeof output_device; status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_output_device_property_address, 0, NULL, &size, &output_device), (status != noErr || output_device == 0); ) {
	  mu->error = "could not obtain default audio device";
	  goto error;
     }
     UInt32 streams_size;
     AudioObjectPropertyAddress streams_address = { .mSelector=kAudioDevicePropertyStreams, .mScope=kAudioDevicePropertyScopeOutput, .mElement=kAudioObjectPropertyElementMaster};
     if (status = AudioObjectGetPropertyDataSize(output_device, &streams_address, 0, NULL, &streams_size), status != noErr) {
	  mu->error = "could not obtain output stream size";
	  goto error;
     }
     AudioStreamID *streams;
     int const streams_n = streams_size / sizeof *streams;
     streams = calloc(streams_n + 1, sizeof *streams);
     if (status = AudioObjectGetPropertyData(output_device, &streams_address, 0, NULL, &streams_size, streams), status != noErr) {
	  mu->error = "could not obtain output streams";
	  goto error_allocated_streams;
     }

     // NOTE(nicolas): coupling with mu_audio_audioformat.channels
     UInt32 left_right_channels[2];
     for (UInt32 size = sizeof left_right_channels; status = AudioObjectGetPropertyData(output_device, &((AudioObjectPropertyAddress){.mSelector=kAudioDevicePropertyPreferredChannelsForStereo, .mScope=kAudioDevicePropertyScopeOutput, .mElement=kAudioObjectPropertyElementMaster}), 0, NULL, &size, &left_right_channels), status != noErr; ) {
	  mu->error = "could not obtain default stereo channels";
	  goto error_allocated_streams;
     }
     MU_MACOS_TRACEF("got l/r channels: %d/%d\n", left_right_channels[0], left_right_channels[1]);

     int selected_stream_i = streams_n;
     AudioStreamRangedDescription selected_format;

     AudioStreamRangedDescription *formats = calloc(1, sizeof *formats);
     
     for(int stream_i = 0; stream_i < streams_n; ++stream_i) {
	  AudioStreamID const stream = streams[stream_i];
	  OSStatus stream_status = noErr;

	  UInt32 starting_channel;
	  for (UInt32 size = sizeof starting_channel; stream_status = AudioObjectGetPropertyData(stream, &((AudioObjectPropertyAddress){.mSelector=kAudioStreamPropertyStartingChannel, .mScope=kAudioObjectPropertyScopeGlobal, .mElement=kAudioObjectPropertyElementMaster}), 0, NULL, &size, &starting_channel), stream_status != noErr; ) {
	       continue;
	  }
	  if (starting_channel != left_right_channels[0] && starting_channel != left_right_channels[1]) {
	       continue;
	  }

	  UInt32 formats_size;
	  if (stream_status = AudioObjectGetPropertyDataSize(stream, &((AudioObjectPropertyAddress){.mSelector=kAudioStreamPropertyAvailableVirtualFormats, .mScope=kAudioObjectPropertyScopeGlobal, .mElement=kAudioObjectPropertyElementMaster}), 0, NULL, &formats_size), stream_status != noErr) {
	       continue;
	  }
	  int const formats_n = formats_size / sizeof *formats;
	  formats = realloc(formats, (formats_n + 1) * (sizeof *formats));
	  if (stream_status = AudioObjectGetPropertyData(stream, &((AudioObjectPropertyAddress){.mSelector=kAudioStreamPropertyAvailableVirtualFormats, .mScope=kAudioObjectPropertyScopeGlobal, .mElement=kAudioObjectPropertyElementMaster}), 0, NULL, &formats_size, formats), stream_status != noErr) {
	       continue;
	  }

	  MU_MACOS_TRACEF("got stream with formats: %d\n", formats_n);

	  int matching_formats_l = mu_coreaudio_formats_partition(formats_n, formats, mu_audio_audioformat);
	  if (matching_formats_l == 0) {
	       continue;
	  }
	  session->audio_channels_n = mu_audio_audioformat.channels;
	  session->audio_channels[0] = left_right_channels[0];
	  session->audio_channels[1] = left_right_channels[1];
	  MU_MACOS_TRACEF("found formats: %d\n", matching_formats_l);

	  selected_stream_i = stream_i;
	  selected_format = formats[0];
     }
     free(formats);

     if (selected_stream_i == streams_n) {
	  mu->error = mu->error_buffer;
	  snprintf(mu->error_buffer, MU_MAX_ERROR, "could not obtain stream: Mu_AudioFormat{ %dhz, %d channels, %d bytes per sample }", mu_audio_audioformat.samples_per_second, mu_audio_audioformat.channels, mu_audio_audioformat.bytes_per_sample);
	  goto error_allocated_streams;
     }

     // configure stream & format
     AudioStreamID selected_stream = streams[selected_stream_i];
     free(streams), streams = NULL;
     if (selected_format.mFormat.mSampleRate == kAudioStreamAnyRate) {
	  selected_format.mFormat.mSampleRate = mu_audio_audioformat.samples_per_second;
	  for (UInt32 size = sizeof selected_format.mFormat; status = AudioObjectSetPropertyData(selected_stream, &((AudioObjectPropertyAddress){.mSelector=kAudioStreamPropertyVirtualFormat, .mScope=kAudioObjectPropertyScopeGlobal, .mElement=kAudioObjectPropertyElementMaster}), 0, NULL, size, &selected_format.mFormat), status != noErr; ) {
	       mu->error = "could not set audio sample rate";
	       goto error;
	  }
     }

     mu->audio.format = mu_audio_audioformat;
     mu->audio.format.samples_per_second = selected_format.mFormat.mSampleRate;
     session->DeviceID = output_device;
     session->audio = mu->audio;
#if !defined(NDEBUG)
     session->audio_debug_signal_phase_inc = 1000.0 / mu->audio.format.samples_per_second;
#endif
     
     if (status = AudioDeviceCreateIOProcID(output_device, mu_coreaudio_callback, session, &session->IOProcID), status != noErr) {
	  mu->error = "could not create audio callback";
	  goto error;
     }

     if (status = AudioDeviceStart(output_device, session->IOProcID), status != noErr) {
	  mu->error = "coult not start audio device";
	  goto error;
     }
     return MU_TRUE;
error_allocated_streams:
     free(streams);
error:
     return MU_FALSE;
}

MU_MACOS_INTERNAL
Mu_Bool
mu_audio_initialize(struct Mu* mu, struct Mu_Session *session)
{
     OSStatus status = noErr;
     if (!mu->audio.callback) mu->audio.callback = mu_audio_emit_silence;
     
     if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &MU_MACOS_COREAUDIO_DEFAULT_OUTPUT_DEVICE_PROPERTY_ADDRESS, mu_coreaudio_property_listener, session), status != noErr) {
	  mu->error = "could not listen to default output device";
	  goto error;
     }

     atomic_flag_test_and_set(&session->output_audio_isdefault);
     return mu_audio_output_start(mu, session);
error:
     return MU_FALSE;
}

MU_MACOS_INTERNAL
void
mu_audio_output_close(struct Mu* mu, struct Mu_Session* session)
{
     if (session->DeviceID && session->IOProcID) {
	  if (AudioDeviceStop(session->DeviceID, mu_coreaudio_callback) != noErr
	      || AudioDeviceDestroyIOProcID(session->DeviceID, session->IOProcID) != noErr) {
	       MU_MACOS_TRACEF("could not close audio device");
	  }
	  session->DeviceID = 0;
	  session->IOProcID = 0;
     }
}

@implementation Mu_WindowDelegate
{
     struct Mu_Session* session;
}
-(instancetype)initWithSession: (struct Mu_Session*)session_
{
     session = session_;
     return self;
}
- (BOOL)windowShouldClose:(id)sender { return YES; }
- (void)windowWillClose:(NSNotification *)notification
{
     session->macos_wants_us_to_quit = MU_TRUE;
}
@end

MU_MACOS_INTERNAL
void mu_time_update(struct Mu* mu, uint64_t ticks)
{
  uint64_t const t0 = mu->time.initial_ticks;
  uint64_t const tps = mu->time.ticks_per_second;
  mu->time.delta_ticks = (ticks - t0) - mu->time.ticks;
  mu->time.ticks = ticks - t0;
  
  mu->time.nanoseconds = mu->time.ticks * 1000 * 1000 * 1000 / tps;
  mu->time.microseconds = mu->time.nanoseconds / 1000; 
  mu->time.milliseconds = mu->time.microseconds / 1000;
  mu->time.seconds = (float)mu->time.ticks / (float)tps;

  mu->time.delta_nanoseconds = mu->time.delta_ticks * 1000 * 1000 * 1000 / tps;
  mu->time.delta_microseconds = mu->time.delta_nanoseconds / 1000; 
  mu->time.delta_milliseconds = mu->time.delta_microseconds / 1000;
  mu->time.delta_seconds = (float)mu->time.delta_ticks / (float)tps;
}

MU_MACOS_INTERNAL
Mu_Bool mu_time_initialize(struct Mu *mu)
{
  mach_timebase_info_data_t timebase;
  if (mach_timebase_info(&timebase)) return MU_FALSE;
  mu->time.initial_ticks = mach_absolute_time();
  mu->time.ticks_per_second = 1000*1000*1000*timebase.numer/timebase.denom;
  mu_time_update(mu, mu->time.initial_ticks);
  return MU_TRUE;
}

MU_MACOS_INTERNAL
void mu_time_pull(struct Mu* mu)
{
  mu_time_update(mu, mach_absolute_time());
}

MU_MACOS_INTERNAL
Mu_Bool mu_application_initialize(struct Mu *mu, struct Mu_Session* session)
{
     [NSApplication sharedApplication];
     [NSApp setActivationPolicy: NSApplicationActivationPolicyRegular]; // act as a bundled app, even if not bundled
     /* create menu bar */ {
	  NSMenu *menubar = [[NSMenu new] autorelease];
	  
	  NSMenu *app_menu = [[NSMenu new] autorelease];
	  [app_menu addItem: [[[NSMenuItem alloc] initWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"] autorelease]];

	  NSMenuItem *app_menu_menubar_item = [[NSMenuItem new] autorelease];
	  [app_menu_menubar_item setSubmenu: app_menu];
	  [menubar addItem: app_menu_menubar_item];

	  [NSApp setMainMenu: menubar];
     }
     return MU_TRUE;
}

enum {
#if defined(__MAC_10_12) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  Mu_NSWindowStyleMaskTitled = NSWindowStyleMaskTitled,
  Mu_NSWindowStyleMaskClosable = NSWindowStyleMaskClosable,
  Mu_NSWindowStyleMaskMiniaturizable = NSWindowStyleMaskMiniaturizable,
  Mu_NSWindowStyleMaskResizable = NSWindowStyleMaskResizable,
  Mu_NSWindowStyleMaskTexturedBackground = NSWindowStyleMaskTexturedBackground,
#else
  Mu_NSWindowStyleMaskTitled = NSTitledWindowMask,
  Mu_NSWindowStyleMaskClosable = NSClosableWindowMask,
  Mu_NSWindowStyleMaskMiniaturizable = NSMiniaturizableWindowMask,
  Mu_NSWindowStyleMaskResizable = NSResizableWindowMask,
  Mu_NSWindowStyleMaskTexturedBackground = NSTexturedBackgroundWindowMask,
#endif
};

typedef
#if defined(__MAC_10_12) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
NSWindowStyleMask
#else
int
#endif
Mu_NSWindowStyleMask;

MU_MACOS_INTERNAL
Mu_Bool mu_window_initialize(struct Mu *mu, struct Mu_Session* session)
{
#define get_opt(x, x_ifnull) (x)? (x) : (x_ifnull)
     char const* const window_title = get_opt(mu->window.title, "Mu");
     mu->window.title = (char*)window_title;
     int window_x = get_opt(mu->window.position.x, 0);
     int window_y = get_opt(mu->window.position.y, 0);
     int const window_width = get_opt(mu->window.size.x, MU_DEFAULT_WIDTH);
     int const window_height = get_opt(mu->window.size.y, MU_DEFAULT_HEIGHT);
#undef get_opt
     
     NSWindow *window = [[NSWindow alloc] autorelease];
     /* setup window */ {
	  Mu_NSWindowStyleMask const window_style_mask = Mu_NSWindowStyleMaskTitled | Mu_NSWindowStyleMaskClosable | Mu_NSWindowStyleMaskMiniaturizable | Mu_NSWindowStyleMaskResizable | Mu_NSWindowStyleMaskTexturedBackground;
	  [window initWithContentRect: NSMakeRect(window_x, window_y, window_width, window_height)
			    styleMask: window_style_mask
			      backing: NSBackingStoreBuffered
				defer: NO];
          [window setOpaque: YES];
	  [window setTitle: [[NSString stringWithFormat: @"%s", window_title] autorelease]];
	  session->window_delegate = [[Mu_WindowDelegate alloc] initWithSession: session];
	  [window setDelegate: session->window_delegate];
	  if (window_x == 0 || window_y == 0) {
	       [window center];
	       NSRect content_rect = [NSWindow contentRectForFrameRect: [window frame] styleMask: window_style_mask];
	       if (window_x != 0) content_rect.origin.x = window_x;
	       if (window_y != 0) content_rect.origin.y = window_y;
	       NSRect new_frame_rect = [NSWindow frameRectForContentRect: content_rect styleMask: window_style_mask];
	       [window setFrameOrigin: new_frame_rect.origin];
	       window_x = content_rect.origin.x;
	       window_y = content_rect.origin.y;
	  }
     }
     /* setup content view */ {
	  Mu_OpenGLView *view = [[[Mu_OpenGLView alloc] initWithFrame: NSMakeRect(window_x, window_y, window_width, window_height) pixelFormat: [Mu_OpenGLView defaultPixelFormat]] autorelease];
          view->session = session;
	  [window setContentView: view];
	  session->opengl_view = [view retain];
     }
     session->cocoa_resources.window = [window retain];
     return MU_TRUE;
}

MU_MACOS_INTERNAL
struct Mu_Session* mu_get_session(struct Mu *mu)
{
     return (struct Mu_Session*)mu->cocoa;
}

#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
MU_MACOS_INTERNAL
Mu_Bool mu_fibers_initialize(struct Mu *mu, struct Mu_Session *session);

MU_MACOS_INTERNAL
void mu_fibers_close(struct Mu *mu, struct Mu_Session *session);

MU_MACOS_INTERNAL
void mu_fibers_switch_to_run_loop(struct Mu_Session *session, struct Mu *mu);

MU_MACOS_INTERNAL
void mu_fibers_switch_to_main(struct Mu_Session *session);

#endif

MU_MACOS_INTERNAL
void mu_update_digital_button(struct Mu_DigitalButton *button, Mu_Bool is_down)
{
     Mu_Bool was_down = button->down;
     button->down = is_down;
     button->pressed = !was_down && is_down;
     button->released = was_down && !is_down;
}

MU_MACOS_INTERNAL
void mu_update_analog_button(struct Mu_AnalogButton *button, float const value)
{
     Mu_Bool is_down = (value >= button->threshold);
     Mu_Bool was_down = button->down;
     button->value = value;
     button->down = is_down;
     button->pressed = !was_down && is_down;
     button->released = was_down && !is_down;
}

MU_MACOS_INTERNAL
void mu_update_stick(struct Mu_Stick *stick, float const x, float const y)
{
     stick->x = x;
     stick->y = y;
     if (fabs(stick->x) <= stick->threshold) stick->x = 0.0f;
     if (fabs(stick->y) <= stick->threshold) stick->y = 0.0f;
}


// Mu Gamepads

MU_MACOS_INTERNAL
Mu_Bool mu_gamepad_hidaddress_equals(struct Mu_HIDAddress a, struct Mu_HIDAddress b)
{
     return a.usage_page == b.usage_page && a.usage == b.usage;
}

MU_MACOS_INTERNAL
void mu_gamepad_hid_update_digital_button(struct Mu_DigitalButton *button, struct Mu_Gamepad_HID_Mapping_DigitalButton const * mapping, int state)
{
     if (!mapping->states_n) {
	  mu_update_digital_button(button, state != 0);
     } else {
	  int state_i;
	  for (state_i = 0; state_i < mapping->states_n && mapping->states[state_i] != state; ++state_i) {
	       continue;
	  }
	  mu_update_digital_button(button, state_i != mapping->states_n);
     }
}

MU_MACOS_INTERNAL
void mu_gamepad_hid_update_stick(struct Mu_Stick *stick, struct Mu_Gamepad_HID_Mapping_Stick const *mapping, struct Mu_HIDAddress address, float const scaled_value)
{
     if (mu_gamepad_hidaddress_equals(mapping->x_address, address)) {
	  stick->x = 2.0f*((scaled_value - mapping->xmin)/(mapping->xmax - mapping->xmin) - 0.5f);
     } else if (mu_gamepad_hidaddress_equals(mapping->y_address, address)) {
	  stick->y = 2.0f*((scaled_value - mapping->ymin)/(mapping->ymax - mapping->ymin) - 0.5f);
     }
}

MU_MACOS_INTERNAL
void mu_gamepad_hid_update_analog_button(struct Mu_AnalogButton *analog_button, struct Mu_Gamepad_HID_Mapping_AnalogButton const *mapping, float const scaled_value)
{
     float const value = (scaled_value - mapping->xmin)/(mapping->xmax - mapping->xmin);
     mu_update_analog_button(analog_button, value);
}

MU_MACOS_INTERNAL
void mu_gamepad_hid_input_value_callback(void *context, IOReturn result, void *sender, IOHIDValueRef value)
{
     if (result != kIOReturnSuccess) return;

     struct Mu_Session *session = context;
     struct Mu_Gamepad_HID_Mapping *mapping = &session->gamepad_hid_mapping;
     struct Mu_Gamepad *gamepad = &session->gamepad; // accumulated state, which will be later published to Mu
     
     IOHIDElementRef const element = IOHIDValueGetElement(value);
     int state = (int)IOHIDValueGetIntegerValue(value);
     float const scaled_value = IOHIDValueGetScaledValue(value, kIOHIDValueScaleTypePhysical);

     struct Mu_HIDAddress address = {
	  .usage_page = IOHIDElementGetUsagePage(element),
	  .usage = IOHIDElementGetUsage(element)
     };

#define X(button_name) if (mu_gamepad_hidaddress_equals(mapping->button_name.address, address)) mu_gamepad_hid_update_digital_button(&gamepad->button_name, &mapping->button_name, state);
     MU_GAMEPAD_DIGITAL_BUTTONS_XENUM;
#undef X

#define X(analog_button_name) if (mu_gamepad_hidaddress_equals(mapping->analog_button_name.address, address)) mu_gamepad_hid_update_analog_button(&gamepad->analog_button_name, &mapping->analog_button_name, scaled_value);
     MU_GAMEPAD_ANALOG_BUTTONS_XENUM;
#undef X
     
#define X(stick_name) mu_gamepad_hid_update_stick(&gamepad->stick_name, &mapping->stick_name, address, scaled_value);
     MU_GAMEPAD_STICKS_XENUM;
#undef X
     
#if 0 // @debug show HID values/messages
     float analog = IOHIDValueGetScaledValue(value, kIOHIDValueScaleTypePhysical);
     // Button debugging
     if (address.usage_page == 0x09 && state != 0)
	  MU_MACOS_TRACEF("Gamepad: InputValue: usagePage: 0x%02X, usage 0x%02X, value: %d / %f\n", address.usage_page, address.usage, state, analog);
      MU_MACOS_TRACEF("Gamepad: InputValue: usagePage: 0x%02X, usage 0x%02X, value: %d / %f\n", address.usage_page, address.usage, state, analog);
#endif
}

#if 0 // (experimental) low-level DS4 report
enum {
     Mu_DS4_MAGIC = 0x0004ff05,
     Mu_DS4_VENDOR_ID = 0x54c,
     Mu_DS4_PRODUCT_ID = 0x5c4,
};
#pragma pack(push, 1)
struct Mu_DS4_Touch_Report {
     uint32_t id : 7;
     uint32_t inactive : 1;
     uint32_t x : 12;
     uint32_t y : 12;
};
#pragma pack(pop)
#pragma pack(push, 1)
struct Mu_DS4_Input_Report {
     uint8_t prepad;
     uint8_t leftx, lefty, rightx, righty;
     uint16_t buttons;
     uint8_t trackpad_ps : 2;
     uint8_t timestamp : 6;
     uint8_t l2, r2;
     uint8_t pad[2];
     uint8_t battery;
     int16_t accel[3], gyro[3];
     uint8_t pad0[35 - 25];
     struct Mu_DS4_Touch_Report touch[2];
     uint8_t pad2[64 - 43];
};
#pragma pack(pop)

MU_MACOS_INTERNAL
void mu_gamepad_hid_input_report_callback(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *report, CFIndex reportLength)
{
     if (result != kIOReturnSuccess) return;
     struct Mu_DS4_Input_Report ds4_report;
     assert(reportLength == sizeof ds4_report);
     memcpy(&ds4_report, report, reportLength);
     MU_MACOS_TRACEF("Gamepad: DS4InputReport, battery: %f\n", ds4_report.battery/255.f);
}
#endif // experimental

MU_MACOS_INTERNAL
struct Mu_Gamepad_HID_Mapping const mu_ds4_mapping = {
     .a_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x02, },},
     .b_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x03 },},
     .x_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x01 },},
     .y_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x04 },},
     .left_shoulder_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x05 },},
     .right_shoulder_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x06 },},
     .left_thumb_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0xB },},
     .right_thumb_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0xC },},
     .back_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x9 },}, // share button
     .start_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0xA },}, // option button
     .up_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 0, 1, 7 } },
     .down_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 3, 4, 5} },
     .left_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 7, 6, 5 } },
     .right_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 3, 2, 1, } },
     .left_trigger = {
	  .address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Rx },
	  .xmin = 0.0f, .xmax = 255.0f
     },
     .right_trigger = {
	  .address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Ry },
	  .xmin = 0.0f, .xmax = 255.0f
     },
     .left_thumb_stick = {
	  .x_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_X }, .y_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Y },
	  .xmin = 0.0f, .xmax = 255.0f, .ymin = 0.0f, .ymax = 255.0f,
     },
     .right_thumb_stick = {
	  .x_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Z }, .y_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Rz },
	  .xmin = 0.0f, .xmax = 255.0f, .ymin = 0.0f, .ymax = 255.0f,
     },
};

// This is niche, but I have one of these and its nice to have an example of an alternate mapping
MU_MACOS_INTERNAL
struct Mu_Gamepad_HID_Mapping const mu_huijia_3_0xe8f_0x3013_mapping = {
     .a_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x03, },},
     .b_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x02 },},
     .x_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x04 },},
     .y_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x01 },},
     .left_shoulder_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x07 },},
     .right_shoulder_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x08 },},
     .left_thumb_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0xB },},
     .right_thumb_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0xC },},
     .back_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0x9 },}, // share button
     .start_button = { .address = { .usage_page = kHIDPage_Button, .usage = 0xA },}, // option button
     .up_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 0, 1, 7 } },
     .down_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 3, 4, 5} },
     .left_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 7, 6, 5 } },
     .right_button = { .address = { .usage_page = kHIDPage_GenericDesktop, .usage = 0x39 }, .states_n = 3, .states = { 3, 2, 1, } },
     .left_trigger = {
	  .address = { .usage_page = kHIDPage_Button, .usage = 0x05 },
	  .xmin = 0.0f, .xmax = 1.0f
     },
     .right_trigger = {
	  .address = { .usage_page = kHIDPage_Button, .usage = 0x06 },
	  .xmin = 0.0f, .xmax = 1.0f
     },
     .left_thumb_stick = {
	  .x_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_X }, .y_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Y },
	  .xmin = 0.0f, .xmax = 255.0f, .ymin = 0.0f, .ymax = 255.0f,
     },
     .right_thumb_stick = {
	  .x_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Rz },
	  .y_address = { .usage_page = kHIDPage_GenericDesktop, .usage = kHIDUsage_GD_Z },
	  .xmin = 0.0f, .xmax = 255.0f, .ymin = 0.0f, .ymax = 255.0f,
     },
};

MU_MACOS_INTERNAL
Mu_Bool mu_gamepad_initialize(struct Mu *mu, struct Mu_Session *session)
{
     IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, 0);
     if (IOHIDManagerOpen(manager, 0) != kIOReturnSuccess) {
	  mu->error = "IOHIDManagerOpen failed";
	  goto error;
     }
     NSArray *device_matching_array = @[
	  @{@(kIOHIDDeviceUsagePageKey): @(kHIDPage_GenericDesktop), @(kIOHIDDeviceUsageKey): @(kHIDUsage_GD_Joystick)},
          @{@(kIOHIDDeviceUsagePageKey): @(kHIDPage_GenericDesktop), @(kIOHIDDeviceUsageKey): @(kHIDUsage_GD_GamePad)}, // DS4
	  @{@(kIOHIDDeviceUsagePageKey): @(kHIDPage_GenericDesktop), @(kIOHIDDeviceUsageKey): @(kHIDUsage_GD_MultiAxisController)},
     ];
     
     NSArray *input_value_matching_array = @[
	  @{@(kIOHIDElementUsagePageKey): @(kHIDPage_GenericDesktop)},
	  @{@(kIOHIDElementUsagePageKey): @(kHIDPage_Button)},	  
     ];
     [input_value_matching_array retain];
     IOHIDManagerSetDeviceMatchingMultiple(manager, (__bridge CFArrayRef)device_matching_array);

     // Initial set
     NSSet *devices = (__bridge NSSet*)IOHIDManagerCopyDevices(manager);
     MU_MACOS_TRACEF("Gamepad: found %lu controllers\n", [devices count]);
     IOHIDDeviceRef device;
     for (NSEnumerator *device_enumerator = [devices objectEnumerator];
	  !mu->gamepad.connected && (device = (IOHIDDeviceRef)[device_enumerator nextObject]); ) {
	  NSUInteger vid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey)) unsignedIntegerValue];
	  NSUInteger pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey)) unsignedIntegerValue];
	  if (vid == /* Sony */ 0x054c && pid == /* Dual Shock 4 */ 0x05c4) {
	       MU_MACOS_TRACEF("Gamepad: found Sony DUALSHOCK 4\n");
	       IOHIDDeviceRegisterInputValueCallback(device, mu_gamepad_hid_input_value_callback, (void *)session);
	       IOHIDDeviceSetInputValueMatchingMultiple(device, (__bridge CFArrayRef)input_value_matching_array);

#if 0 // experimental native DS4 support
	       struct Mu_DS4_Input_Report *report_buffer = calloc(1, sizeof *report_buffer); // @todo @leak
	       IOHIDDeviceRegisterInputReportCallback(device, (uint8_t*)report_buffer, sizeof *report_buffer, mu_gamepad_hid_input_report_callback, session);
#endif // experimental
	       session->gamepad_hid_mapping = mu_ds4_mapping;
	       mu->gamepad.connected = MU_TRUE;
	  } else if (vid == 0xe8f && pid == 0x3013) {
	       MU_MACOS_TRACEF("Gamepad: found HuiJia USB Gamepad connector\n");       
	       IOHIDDeviceRegisterInputValueCallback(device, mu_gamepad_hid_input_value_callback, (void *)session);
	       IOHIDDeviceSetInputValueMatchingMultiple(device, (__bridge CFArrayRef)input_value_matching_array);
	       session->gamepad_hid_mapping = mu_huijia_3_0xe8f_0x3013_mapping;
	       mu->gamepad.connected = MU_TRUE;
	  } else {
	       MU_MACOS_TRACEF("Gamepad: skipping device 0x%0lx 0x%0lx\n", vid, pid);
	       mu->gamepad.connected = MU_TRUE;
	       session->gamepad_hid_mapping = mu_ds4_mapping;
	  }
     }
     [input_value_matching_array release], input_value_matching_array = nil;
     
     // @todo establish live feedback. However doesn't that necessitate changing the API?
     IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
     
     session->hidmanager = manager;
     return MU_TRUE;
error:
     IOHIDManagerClose(manager, 0);
     return MU_FALSE;
}

MU_MACOS_INTERNAL
void mu_gamepad_close(struct Mu *mu, struct Mu_Session *session)
{
     IOHIDManagerClose(session->hidmanager, 0), session->hidmanager = NULL;
}

MU_MACOS_INTERNAL
void mu_gamepad_pull(struct Mu *mu, struct Mu_Session *session)
{
     // Copy incremental state from the session to the user data:
#define X(button_name) mu_update_digital_button(&mu->gamepad.button_name, session->gamepad.button_name.down);
     MU_GAMEPAD_DIGITAL_BUTTONS_XENUM;
#undef X
#define X(analog_button_name) mu_update_analog_button(&mu->gamepad.analog_button_name, session->gamepad.analog_button_name.value);
     MU_GAMEPAD_ANALOG_BUTTONS_XENUM;
#undef X
#define X(stick_name) mu_update_stick(&mu->gamepad.stick_name, session->gamepad.stick_name.x, session->gamepad.stick_name.y);
     MU_GAMEPAD_STICKS_XENUM
#undef X

#if 0 // Show each press @debug
#define X(button_name) if (mu->gamepad.button_name.pressed) MU_MACOS_TRACEF("pressed: " #button_name "\n");
     MU_GAMEPAD_DIGITAL_BUTTONS_XENUM;
     MU_GAMEPAD_ANALOG_BUTTONS_XENUM;
#undef X
#endif
}

Mu_Bool Mu_Initialize(struct Mu *mu)
{
     struct Mu_Session *session = calloc(sizeof(struct Mu_Session), 1);
     @autoreleasepool {
          if (!mu_time_initialize(mu)) return MU_FALSE;
	  if (!mu_application_initialize(mu, session)) return MU_FALSE;
	  if (!mu_window_initialize(mu, session)) return MU_FALSE;
	  if (!mu_audio_initialize(mu, session)) return MU_FALSE;
	  if (!mu_gamepad_initialize(mu, session)) return MU_FALSE;
	  mu->initialized = MU_TRUE; // partially
	  mu->cocoa = &session->cocoa_resources;
	  [NSApp finishLaunching];
          [mu->cocoa->window makeKeyAndOrderFront: nil];
          [mu->cocoa->window makeMainWindow];
          [NSApp activateIgnoringOtherApps: YES];
     }

#if MU_MACOS_RUN_MODE==MU_MACOS_RUN_MODE_COROUTINE
     if (!mu_fibers_initialize(mu, session)) return MU_FALSE;
#endif
     return MU_TRUE;
}

enum {
#if defined(__MAC_10_12) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  Mu_NSEventMaskAny = NSEventMaskAny,
#else
  Mu_NSEventMaskAny = NSAnyEventMask,
#endif
};

enum {
#if defined(__MAC_10_12) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  Mu_NSEventTypeFlagsChanged = NSEventTypeFlagsChanged,
  Mu_NSEventTypeKeyDown = NSEventTypeKeyDown,
  Mu_NSEventTypeKeyUp = NSEventTypeKeyUp,
  Mu_NSEventTypeLeftMouseDown = NSEventTypeLeftMouseDown,
  Mu_NSEventTypeLeftMouseDragged = NSEventTypeLeftMouseDragged,
  Mu_NSEventTypeLeftMouseUp = NSEventTypeLeftMouseUp,
  Mu_NSEventTypeMouseMoved = NSEventTypeMouseMoved,
  Mu_NSEventTypeRightMouseDown = NSEventTypeRightMouseDown,
  Mu_NSEventTypeRightMouseDragged = NSEventTypeRightMouseDragged,
  Mu_NSEventTypeRightMouseUp = NSEventTypeRightMouseUp,
  Mu_NSEventTypeScrollWheel = NSEventTypeScrollWheel,
#else
  Mu_NSEventTypeFlagsChanged = NSFlagsChanged,
  Mu_NSEventTypeKeyDown = NSKeyDown,
  Mu_NSEventTypeKeyUp = NSKeyUp,
  Mu_NSEventTypeLeftMouseDown = NSLeftMouseDown,
  Mu_NSEventTypeLeftMouseDragged = NSLeftMouseDragged,
  Mu_NSEventTypeLeftMouseUp = NSLeftMouseUp,
  Mu_NSEventTypeMouseMoved = NSMouseMoved,
  Mu_NSEventTypeRightMouseDown = NSRightMouseDown,
  Mu_NSEventTypeRightMouseDragged = NSRightMouseDragged,
  Mu_NSEventTypeRightMouseUp = NSRightMouseUp,
  Mu_NSEventTypeScrollWheel = NSScrollWheel,
#endif
};

enum {
#if defined(__MAC_10_12) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  Mu_NSEventModifierFlagCommand = NSEventModifierFlagCommand,
  Mu_NSEventModifierFlagControl = NSEventModifierFlagControl,
  Mu_NSEventModifierFlagOption = NSEventModifierFlagOption,
  Mu_NSEventModifierFlagShift = NSEventModifierFlagShift,
#else
  Mu_NSEventModifierFlagCommand = NSCommandKeyMask,
  Mu_NSEventModifierFlagControl = NSControlKeyMask,
  Mu_NSEventModifierFlagOption = NSAlternateKeyMask,
  Mu_NSEventModifierFlagShift = NSShiftKeyMask,
#endif
};

MU_MACOS_INTERNAL
Mu_Bool mu_nsevent_process(struct Mu * const mu, struct Mu_Session const * const session, NSEvent const * const event)
{
     switch ([event type]) {
     case Mu_NSEventTypeFlagsChanged: {
	  NSEventModifierFlags flags = [event modifierFlags];
	  unsigned short keyCode = [event keyCode];
	  Mu_Bool is_down = MU_FALSE;
	  int e_p = 0;
	  switch(keyCode) {
	  case MU_CTRL: is_down=(flags & Mu_NSEventModifierFlagControl)? MU_TRUE:MU_FALSE; e_p=1; break;
	  case MU_CMD: is_down=(flags & Mu_NSEventModifierFlagCommand)? MU_TRUE:MU_FALSE; e_p=1; break;
	  case MU_OPTION: is_down=(flags & Mu_NSEventModifierFlagOption)? MU_TRUE:MU_FALSE; e_p=1; break;
	  case MU_SHIFT: is_down=(flags & Mu_NSEventModifierFlagShift)? MU_TRUE:MU_FALSE; e_p=1; break;
	  }
	  if (e_p && keyCode < MU_MAX_KEYS) {
	       mu_update_digital_button(&mu->keys[keyCode], is_down);
	  }
     } break;
     case Mu_NSEventTypeKeyDown: {
	  unsigned short keyCode = [event keyCode];
	  if (keyCode < MU_MAX_KEYS) {
	       mu_update_digital_button(&mu->keys[keyCode], MU_TRUE);
	  }
	  NSString *str = [event characters];
	  for (char const* utf8_str = [str UTF8String]; *utf8_str && mu->text_length < MU_MAX_TEXT - 1; ++mu->text_length, ++utf8_str) {
	       mu->text[mu->text_length] = *utf8_str;
	  }
	  mu->text[mu->text_length] = 0;
     } break;
     case Mu_NSEventTypeKeyUp: {
	  unsigned short keyCode = [event keyCode];
	  if (keyCode < MU_MAX_KEYS) {
	       mu_update_digital_button(&mu->keys[keyCode], MU_FALSE);
	  }
     } break;
     case Mu_NSEventTypeLeftMouseDown: {
	  mu_update_digital_button(&mu->mouse.left_button, MU_TRUE);
     } break;
     case Mu_NSEventTypeRightMouseDown: {
	  mu_update_digital_button(&mu->mouse.right_button, MU_TRUE);
     } break;
     case Mu_NSEventTypeLeftMouseUp: {
	  mu_update_digital_button(&mu->mouse.left_button, MU_FALSE);
     } break;
     case Mu_NSEventTypeRightMouseUp: {
	  mu_update_digital_button(&mu->mouse.right_button, MU_FALSE);
     } break;
     case Mu_NSEventTypeRightMouseDragged: /* fallthrough */
     case Mu_NSEventTypeLeftMouseDragged: /* fallthrough */
     case Mu_NSEventTypeMouseMoved: {
	  NSPoint ep = [event locationInWindow];
	  if ([event window] == nil) {
	       ep = [session->cocoa_resources.window convertRectFromScreen: NSMakeRect(ep.x, ep.y, 1, 1)].origin;
	       ep = [session->opengl_view convertPoint: ep fromView: nil];
	  }
	  struct Mu_Int2 const pp = mu->mouse.position;
	  mu->mouse.position.x = ep.x;
	  mu->mouse.position.y = mu->window.size.y - (ep.y+1);
	  mu->mouse.delta_position.x = mu->mouse.position.x - pp.x;
	  mu->mouse.delta_position.y = mu->mouse.position.y - pp.y;
     } break;
     case Mu_NSEventTypeScrollWheel: {
	  mu->mouse.delta_wheel = [event deltaY];
	  mu->mouse.wheel += mu->mouse.delta_wheel;
     } break;
     default:
          return MU_FALSE; // events we don't know about
     }
     return MU_TRUE;
}

@implementation Mu_OpenGLView
{
     NSTimer *nstimer;
}
// together `acceptsFirstResponder` & `keyDown` prevent the key from reaching
// NSWindow, which would otherwise beep in response.
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)theEvent {}
#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
- (void) interruptMainLoop
{
     mu_fibers_switch_to_main(session);
}
- (void)viewWillStartLiveResize
{
     assert(!nstimer);
     nstimer = [NSTimer timerWithTimeInterval: 0.001
				       target: self
				     selector: @selector(interruptMainLoop)
				     userInfo: nil
				      repeats: YES]; 
     [[NSRunLoop currentRunLoop] addTimer: nstimer forMode: NSRunLoopCommonModes];
     struct Mu *mu;
     if ((mu = session->pull_destination) && mu->mouse.left_button.down) {
          mu->mouse = (struct Mu_Mouse) {{0}};
     }
}
- (void)viewDidEndLiveResize
{
     [nstimer invalidate], nstimer = nil;
}
#endif
@end

MU_MACOS_INTERNAL
void mu_window_pull(struct Mu * const mu, struct Mu_Session * const session)
{
     // We implement the main loop manually, to provide a pull interface
     @autoreleasepool {
          NSDate *deadline = [[NSDate dateWithTimeIntervalSinceNow: 1e-3] retain];
          int events_capacity = 8;
          int events_n = 0;
          NSEvent *events_buffer[events_capacity];
          NSEvent **events = events_buffer;
          // quickly pull events from the queue; next events will have to wait next frame
          for (NSEvent *event; (event = [NSApp nextEventMatchingMask:Mu_NSEventMaskAny untilDate: deadline inMode:NSDefaultRunLoopMode dequeue:YES]);) {
               if (events_n == events_capacity) {
                    events_capacity *= 2;
		    int was_on_stack = events==events_buffer;
                    events = realloc(was_on_stack?NULL:events, events_capacity * sizeof *events);
		    if (was_on_stack) memcpy(events, events_buffer, sizeof events_buffer);
               }
               events[events_n] = event;
               ++events_n;
          }
	  [deadline release], deadline = nil;
          // process them
          for (int event_i = 0; event_i < events_n; ++event_i) {
               NSEvent* event = events[event_i];
	       mu_nsevent_process(mu, session, event);
               [NSApp sendEvent: event];
               [NSApp updateWindows];
	  }
          if (events != events_buffer) free(events), events = NULL;
     }
            
     mu->quit = mu->quit || session->macos_wants_us_to_quit;
}

#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
MU_MACOS_INTERNAL
void mu_run_loop_fiber(struct Mu_Session* session)
{
     for (;;) {
	  mu_window_pull(session->pull_destination, session);
	  mu_fibers_switch_to_main(session);
     }
}
#endif

Mu_Bool Mu_Pull(struct Mu *mu)
{
     if (!mu->initialized) return MU_FALSE;
     struct Mu_Session* session = mu_get_session(mu);

     if (!atomic_flag_test_and_set(&session->output_audio_isdefault)) {
	  mu_audio_output_close(mu, session);
	  mu_audio_output_start(mu, session);
     }

     // reset window state
     mu->window.resized = MU_FALSE;

     // reset gamepad state
#define X(button_name) mu_update_digital_button(&mu->gamepad.button_name, mu->gamepad.button_name.down);
     MU_GAMEPAD_DIGITAL_BUTTONS_XENUM;
#undef X

#define X(analog_button_name) mu_update_analog_button(&mu->gamepad.analog_button_name, mu->gamepad.analog_button_name.value);
     MU_GAMEPAD_ANALOG_BUTTONS_XENUM;
#undef X
     
     // reset mouse state
     mu->mouse.left_button = (struct Mu_DigitalButton){.down=mu->mouse.left_button.down};
     mu->mouse.right_button = (struct Mu_DigitalButton){.down=mu->mouse.right_button.down};
     mu->mouse.delta_wheel = 0;
     mu->mouse.delta_position = (struct Mu_Int2){0};

     // reset keyboard state
     for (int key_i = 0; key_i < MU_MAX_KEYS; ++key_i) {
       mu->keys[key_i] = (struct Mu_DigitalButton){.down=mu->keys[key_i].down};
     }
     mu->text[0] = 0;
     mu->text_length = 0;

     session->pull_destination = mu;
#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
     mu_fibers_switch_to_run_loop(session, mu);
#else
     mu_window_pull(mu, session);
#endif
     mu_time_pull(mu);
     mu_gamepad_pull(mu, session);
     session->pull_destination = NULL;
     
     if (mu->quit) {
	  [session->window_delegate release];
	  [session->opengl_view release];
	  mu_audio_output_close(mu, session);
#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
          mu_fibers_close(mu, session);
#endif
	  mu_gamepad_close(mu, session);
	  mu->cocoa = NULL;
	  free(session);
	  session = NULL;
	  return MU_FALSE;
     }

     NSRect contentRect =		
	  [session->cocoa_resources.window convertRectToScreen:		
		       [session->opengl_view frame]];		
     struct Mu_Window old_window = mu->window;		
     mu->window.position.x = contentRect.origin.x;		
     mu->window.position.y = contentRect.origin.y;		
     mu->window.size.x = contentRect.size.width;		
     mu->window.size.y = contentRect.size.height;
     mu->window.resized = old_window.size.x != mu->window.size.x ||
	  old_window.size.y != mu->window.size.y;
     
     [[session->opengl_view openGLContext] makeCurrentContext];
#if !defined(NDEBUG)
     // @debug debug background
     glClearColor(1.0,105/255.0f,180/255.0f,0);
     glClear(GL_COLOR_BUFFER_BIT);
#endif
     return MU_TRUE;
}

void Mu_Push(struct Mu *mu)
{
     if (!mu->initialized) return;
     assert([NSOpenGLContext currentContext] == [mu_get_session(mu)->opengl_view openGLContext]);
     glFlush();
     [[NSOpenGLContext currentContext] flushBuffer];
}

Mu_Bool Mu_LoadImage(const char *filename, struct Mu_Image *image)
{
     return MU_FALSE;
}

#include <AudioToolbox/ExtendedAudioFile.h>

Mu_Bool Mu_LoadAudio(const char *filename, struct Mu_AudioBuffer *audio)
{
     CFURLRef file_url = CFURLCreateWithBytes(kCFAllocatorDefault, (UInt8 const*)filename, strlen(filename), kCFStringEncodingUTF8, NULL);
     ExtAudioFileRef audiofile;
     OSStatus st = ExtAudioFileOpenURL(file_url, &audiofile);
     CFRelease(file_url), file_url = NULL;
     if (st != noErr) return MU_FALSE;
     AudioStreamBasicDescription description={0};
     for (UInt32 size = sizeof description; ExtAudioFileGetProperty(audiofile, kExtAudioFileProperty_FileDataFormat, &size, &description) != noErr; ) {
       MU_MACOS_TRACEF("ERROR: could not get description\n");
       goto error_with_audiofile_open;
     }
     struct Mu_AudioFormat format = {
       .samples_per_second = description.mSampleRate,
       .channels = description.mChannelsPerFrame,
       .bytes_per_sample = description.mBitsPerChannel / 8,
     };
     description = (struct AudioStreamBasicDescription) {
          .mSampleRate = format.samples_per_second,
          .mFormatID = kAudioFormatLinearPCM,
          .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
          .mBitsPerChannel = sizeof(float)*8,
          .mChannelsPerFrame = format.channels,
          .mFramesPerPacket = 1,
     };
     description.mBytesPerFrame = description.mChannelsPerFrame * sizeof(float);
     description.mBytesPerPacket = description.mBytesPerFrame;
     if (st = ExtAudioFileSetProperty(audiofile, kExtAudioFileProperty_ClientDataFormat, sizeof description, &description), st != noErr) {
          MU_MACOS_TRACEF("ERROR: could not set format: got %x\n", st);
          goto error_with_audiofile_open;
     }
     format = (struct Mu_AudioFormat){
       .samples_per_second = description.mSampleRate,
       .channels = description.mChannelsPerFrame,
       .bytes_per_sample = description.mBitsPerChannel / 8,
     };
     if (!(description.mFormatFlags & kAudioFormatLinearPCM)) {
       MU_MACOS_TRACEF("ERROR: bad format\n");
       goto error_with_audiofile_open;       
     }
     int in_buffer_capacity = 4096;
     char* in_buffer = malloc(in_buffer_capacity);
     AudioBufferList io_audiobufferlist = {
          .mNumberBuffers = 1,
          .mBuffers = {
               (AudioBuffer){
                    .mData = in_buffer,
                    .mDataByteSize = in_buffer_capacity,
                    .mNumberChannels = format.channels,
               }
          },
     };
     int dest_buffer_capacity = 4096;
     int16_t *dest_buffer = (int16_t*)malloc(dest_buffer_capacity * sizeof *dest_buffer);
     int dest_buffer_n = 0;
     UInt32 frames_n = 0;
     int16_t max_sample = 0;
     while ((frames_n = 512), (st = ExtAudioFileRead(audiofile, &frames_n, &io_audiobufferlist)), (st == noErr && frames_n != 0)) {
          int read_samples_n = frames_n * format.channels;
          while (dest_buffer_n + read_samples_n >= dest_buffer_capacity) {
               dest_buffer_capacity *= 2;
          }
          dest_buffer = realloc(dest_buffer, dest_buffer_capacity * sizeof *dest_buffer);
          float *s_sample = (float*)io_audiobufferlist.mBuffers[0].mData;
          int16_t *d_sample = dest_buffer + dest_buffer_n;
          for (int frame_i = 0; frame_i < frames_n; ++frame_i) {
               for (int channel_i = 0; channel_i < format.channels; ++channel_i) {
                    *d_sample = 32767*(*s_sample);
                    max_sample = max_sample<*d_sample? *d_sample:max_sample;
                    ++d_sample;
                    ++s_sample;
               }
          }
          dest_buffer_n += read_samples_n;
     }
     free(in_buffer), in_buffer = NULL, in_buffer_capacity = 0;
     if (st != noErr && st != kAudioFileEndOfFileError) goto error_with_audiofile_open;
     ExtAudioFileDispose(audiofile);
     format.bytes_per_sample = sizeof(int16_t);
     audio->samples = dest_buffer;
     audio->samples_count = dest_buffer_n;
     audio->format = format;
     return MU_TRUE;

error_with_audiofile_open:
     ExtAudioFileDispose(audiofile);
     return MU_FALSE;
}

#if MU_MACOS_RUN_MODE == MU_MACOS_RUN_MODE_COROUTINE
// Coroutine/fiber support
// -----------------------
//
// If this stops working, or if the definitions are removed, then
// we'll just have to reimplement them in assembly.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

MU_MACOS_INTERNAL
Mu_Bool mu_fibers_initialize(struct Mu *mu, struct Mu_Session *session)
{
     if (0 != getcontext(&session->run_loop_fiber)) {
          strerror_r(errno, mu->error_buffer, MU_MAX_ERROR);
          mu->error = mu->error_buffer;
          return MU_FALSE;
     }
     session->run_loop_fiber.uc_stack.ss_size = 512*1024;
     session->run_loop_fiber_stack = calloc(1, session->run_loop_fiber.uc_stack.ss_size);
     session->run_loop_fiber.uc_stack.ss_sp = session->run_loop_fiber_stack;
     session->run_loop_fiber.uc_link = NULL;
     makecontext(&session->run_loop_fiber, mu_run_loop_fiber, 1, session);

     if (0 != getcontext(&session->main_fiber)) {
          strerror_r(errno, mu->error_buffer, MU_MAX_ERROR);
          mu->error = mu->error_buffer;
          return MU_FALSE;
     }
     return MU_TRUE;
}

MU_MACOS_INTERNAL
void mu_fibers_close(struct Mu *mu, struct Mu_Session *session)
{
     free(session->run_loop_fiber_stack), session->run_loop_fiber_stack = NULL;
}

MU_MACOS_INTERNAL
void mu_fibers_switch_to_run_loop(struct Mu_Session *session, struct Mu *mu)
{
     swapcontext(&session->main_fiber, &session->run_loop_fiber);
}

MU_MACOS_INTERNAL
void mu_fibers_switch_to_main(struct Mu_Session *session)
{
     swapcontext(&session->run_loop_fiber, &session->main_fiber);
}

#pragma clang diagnostic pop

#endif

#undef MU_MACOS_RUN_MODE_PLAIN
#undef MU_MACOS_RUN_MODE_COROUTINE
#undef MU_MACOS_RUN_MODE
#undef MU_MACOS_INTERNAL
#undef MU_MACOS_TRACEF
#undef MU_GAMEPAD_DIGITAL_BUTTONS_XENUM
#undef MU_GAMEPAD_STICKS_XENUM
#undef MU_GAMEPAD_ANALOG_BUTTONS_XENUM
