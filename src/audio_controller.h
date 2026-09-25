#ifndef AUDIO_CONTROLLER_H
#define AUDIO_CONTROLLER_H

#include "globals.h"

// Initialize I2S audio subsystem
void setupAudio();

// Start playing a local file or stream (WAV). Set loop=true for seamless file looping.
void startAudioPlayback(const char* path, bool loop = false);

// Play a named sound (e.g. "chime", "chirp", "siren")
void playNamedSound(const String& soundName);

// Stop any currently playing audio file/stream
void stopAudioPlayback();

// Set volume scale on hardware (scales 0-21 volume levels to library requirements)
void updateAudioVolume();

// Mutes/unmutes audio output
void toggleMutePlay();

// Play a brief 2-second preview of an alarm sound
void previewAlarmSound(int soundIdx);

// Checks if audio needs to stop due to preview timeout
void handleAudioPreviewTimeout();

#endif // AUDIO_CONTROLLER_H
