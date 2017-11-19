Implementing Per Vognsen's Mu API

Per Vognsen designed this API as an alternative to libraries like SDL,
SML. I find myself agreeing with many of its design decisions.

He documented it at:
- @url: https://gist.github.com/pervognsen/6a67966c5dc4247a0021b95c8d0a7b72 (code as of the stream 2)
- @url: https://www.youtube.com/watch?v=NG_mUhc8LRw
- @url: https://www.youtube.com/watch?v=pAIdfsT7-EU

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

Other principles: 

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

Experiments to try:

- The input/output struct is plain old data (if you except the
  platform specific handles) ; which means it should be trivial to
  record sequence of values to replay back the application eventually.

Some other personal comments:

A push API for audio is certainly possible, however I personally think
if it make sense, it should prevent gaps in audio as the result of a
late frame.

Another comment I can make is that the choice of int16_t for audio
samples, while convenient for mixing in samples coming from audio
files, make synthesis cases less natural. It's easier and less error
prone to use float within the 0..1 interval as a generic
representation of audio samples.

- The win32 implementation deals with recursive main loops using
Windows coroutine/fiber API. On Macos, there are examples of people
doing the same: @url{https://github.com/tomaka/winit/issues/219}

