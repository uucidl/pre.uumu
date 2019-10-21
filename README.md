# Per Vognsen's Mu API

Per Vognsen designed the [Mu API](xxxx_mu.h) as an alternative to libraries like SDL,
SML. I find myself agreeing with many of its design decisions.

He documented it at:
- (code as of the stream 2) @url: https://gist.github.com/pervognsen/6a67966c5dc4247a0021b95c8d0a7b72
- @url: https://www.youtube.com/watch?v=NG_mUhc8LRw
- @url: https://www.youtube.com/watch?v=pAIdfsT7-EU

He also responded with another iteration called Noir: https://github.com/pervognsen/bitwise/blob/master/noir/noir/noir.ion

## Goals and pinciples

His goals:
- minimal platform layer for multimedia apps (in terms of binary/source size)
- not industrial-strengh, but excellent for small apps
- experiment with API design:
  + good, ritual-free, defaults
  + eschews the many granular calls these libraries tend to have to
    set it up or get information from it
  + instead, provide data in/out through datastructure and a minimal set of functions
  + dialogue between the app and the library as if the library was a
  resumable coroutine, to minimize callbacks, which disrupt normal
  code flow

- extension API with media file loading (image, sound, video)

This is achieved with a global datastructure, whose entries serve as
much as possible as both input and output.

Precompute redundant data that is most often useful to users, rather
than doing it lazilly and potentially too often. Even if that's
increasing the surface area of the API.

Provide state of controls rather than exposing an event queue to its
users, since since multimedia applications would anyway have to
sample/refresh at a high enough rate anyway.

In cases where naïve state-capture would lose events, such as for text,
the API prepares buffers of prepared data representing the aggregate
input between two frames.

Compromises:

As of stream #2, audio has been implemented via a callback, pulling
samples regularly from the high priority audio thread.

## Is this an alternative to SDL?

You could see it like that, however this idea is more useful as a seed
for your own application's platform layer. It's a reusable idea first,
and an implementation second.

Take it, add more things to the Mu structure as needed by your
application.

## Experiments to try:

- The input/output struct is plain old data (if you except the
  platform specific handles) ; which means it should be trivial to
  record sequence of values to replay back the application eventually.

## Some other personal comments:

A push API for audio is certainly possible, however I personally think
if it make sense, it should prevent gaps in audio as the result of a
late frame. It does simplify the usual, simple cases because it
removes the need for thread-safe code.

Another comment I can make is that the choice of int16_t for audio
samples, while convenient for mixing in samples coming from audio
files, make synthesis cases less natural. It's easier and less error
prone to use float within the 0..1 interval as a generic
representation of audio samples.

- The win32 implementation deals with recursive main loops using
Windows coroutine/fiber API. On Macos, there are examples of people
doing the same: @url{https://github.com/tomaka/winit/issues/219}

@todo @idea in the same spirit of the redundant converted time values
found in the main part of the api, it would be logical to precompute
the number of frames of interleaved samples and put it in the
audiobuffer structure

