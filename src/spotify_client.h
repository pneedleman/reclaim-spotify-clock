#ifndef SPOTIFY_CLIENT_H
#define SPOTIFY_CLIENT_H

#include "globals.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// Spotify Structures
struct SpotifyDevice {
    String id;
    String name;
};
const int MAX_SPOTIFY_DEVICES = 5;

struct SpotifyPlaylist {
    String uri;
    String name;
};
const int MAX_SPOTIFY_PLAYLISTS = 5;

// Command queue types
enum SpotifyCommandType {
    SPOTIFY_CMD_NONE = 0,
    SPOTIFY_CMD_VOLUME,
    SPOTIFY_CMD_PLAY,
    SPOTIFY_CMD_PAUSE,
    SPOTIFY_CMD_NEXT,
    SPOTIFY_CMD_PREV,
    SPOTIFY_CMD_TOGGLE_PLAY_PAUSE,
    SPOTIFY_CMD_SELECT_DEVICE,
    SPOTIFY_CMD_PLAY_PLAYLIST,
    SPOTIFY_CMD_SET_SHUFFLE,
    SPOTIFY_CMD_FETCH_DEVICES,
    SPOTIFY_CMD_FETCH_PLAYLISTS
};

struct SpotifyCommand {
    SpotifyCommandType type;
    int intParam;
    bool boolParam;
    char stringParam[160];
};

// Shared variables
extern SpotifyDevice spotifyDevices[MAX_SPOTIFY_DEVICES];
extern int numSpotifyDevices;
extern int selectedSpotifyDeviceIndex;
extern bool isFetchingDevices;

const int MAX_ALL_PLAYLISTS = 50;

extern SpotifyPlaylist spotifyPlaylists[MAX_SPOTIFY_PLAYLISTS];
extern int numSpotifyPlaylists;
extern int selectedSpotifyPlaylistIndex;
extern bool isFetchingPlaylists;
extern bool spotifyShuffleState;
extern bool editSpotifyShuffleState;

extern SpotifyPlaylist allSpotifyPlaylists[MAX_ALL_PLAYLISTS];
extern int numAllSpotifyPlaylists;
extern String favoritePlaylistUris[MAX_SPOTIFY_PLAYLISTS];

extern QueueHandle_t spotifyCommandQueue;
extern SemaphoreHandle_t spotifyStateMutex;

extern String spotifyAccessToken;
extern unsigned long spotifyTokenExpiryTime;
extern String spotifyTrackText;
extern bool spotifyIsPlaying;
extern int spotifyVolumePercent;

// Setup and queue helpers
void initSpotifyClient();
bool postSpotifyCommand(const SpotifyCommand& cmd);
bool receiveSpotifyCommand(SpotifyCommand& out, TickType_t waitTicks);
void processSpotifyCommand(const SpotifyCommand& cmd);
void sendPendingSpotifyVolume();
void setPendingSpotifyVolume(int percent);
void flushPendingSpotifyVolume();

// Thread-safe state snapshots / setters
String getSpotifyTrackText();
bool getSpotifyIsPlaying();
int getSpotifyVolumePercent();
bool getSpotifyShuffleState();
void setSpotifyIsPlaying(bool value);
void setSpotifyVolumePercent(int value);
void setSpotifyTrackText(const String& text);
void setSpotifyShuffleState(bool value);
String getSpotifyLastError();

int getAllSpotifyNumPlaylists();
String getAllSpotifyPlaylistName(int idx);
String getAllSpotifyPlaylistUri(int idx);
void saveFavoritePlaylists(const String favs[MAX_SPOTIFY_PLAYLISTS]);
void loadFavoritePlaylists();

int getSpotifyNumDevices();
int getSpotifyNumPlaylists();
String getSpotifyDeviceId(int idx);
String getSpotifyDeviceName(int idx);
String getSpotifyPlaylistUri(int idx);
String getSpotifyPlaylistName(int idx);
int getSpotifySelectedDeviceIndex();
void setSpotifySelectedDeviceIndex(int idx);
int getSpotifySelectedPlaylistIndex();
void setSpotifySelectedPlaylistIndex(int idx);

// Spotify API calls (intended to be called only from spotifyTask)
bool refreshSpotifyToken();
void sendSpotifyCommand(const char* method, const char* endpoint, const char* payload = "");
void pollSpotifyPlaybackState();
void fetchSpotifyDevices();
void selectSpotifyDevice(String deviceId, bool play = true);
void fetchSpotifyPlaylists();
void playSpotifyPlaylist(String playlistUri);
void continuePendingSpotifyPlayback();
void toggleSpotifyShuffle(bool shuffleState);
void syncSpotifyShuffleState();

#endif // SPOTIFY_CLIENT_H
