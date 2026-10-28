#pragma once
/* A sound file's samples as float, channel after channel (malloc'd), or NULL and a message in err. The path is
   UTF-8. WAV (any encoding, RF64, W64), AIFF, FLAC, MP3, Ogg Vorbis. */
float *decode_audio(const char *path, int *chans, long *frames, int *rate, char *err, int cap);
