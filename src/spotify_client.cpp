#include "spotify_client.h"
#include "sinric_manager.h"

// Define global/shared variables
static String s_pendingPlaybackUri;
static String s_pendingPlaybackDeviceId;
static unsigned long s_pendingPlaybackStartTime = 0;
static bool s_pendingPlayback = false;

SpotifyDevice spotifyDevices[MAX_SPOTIFY_DEVICES];
int numSpotifyDevices = 0;
int selectedSpotifyDeviceIndex = 0;
bool isFetchingDevices = false;

SpotifyPlaylist spotifyPlaylists[MAX_SPOTIFY_PLAYLISTS];
int numSpotifyPlaylists = 0;
int selectedSpotifyPlaylistIndex = 0;
bool isFetchingPlaylists = false;
bool spotifyShuffleState = false;
bool editSpotifyShuffleState = false;

SpotifyPlaylist allSpotifyPlaylists[MAX_ALL_PLAYLISTS];
int numAllSpotifyPlaylists = 0;
String favoritePlaylistUris[MAX_SPOTIFY_PLAYLISTS];

void loadFavoritePlaylists() {
    Preferences prefs;
    prefs.begin("alarm_clock", true);
    for (int i = 0; i < MAX_SPOTIFY_PLAYLISTS; i++) {
        char key[12];
        snprintf(key, sizeof(key), "fav_pl_%d", i);
        favoritePlaylistUris[i] = prefs.getString(key, "");
    }
    prefs.end();
}

void saveFavoritePlaylists(const String favs[MAX_SPOTIFY_PLAYLISTS]) {
    Preferences prefs;
    prefs.begin("alarm_clock", false);
    for (int i = 0; i < MAX_SPOTIFY_PLAYLISTS; i++) {
        char key[12];
        snprintf(key, sizeof(key), "fav_pl_%d", i);
        favoritePlaylistUris[i] = favs[i];
        prefs.putString(key, favs[i]);
    }
    prefs.end();
}

QueueHandle_t spotifyCommandQueue = NULL;
SemaphoreHandle_t spotifyStateMutex = NULL;

String spotifyAccessToken = "";
unsigned long spotifyTokenExpiryTime = 0;
String spotifyTrackText = "Not Playing";
bool spotifyIsPlaying = false;
int spotifyVolumePercent = 0;

static int s_pendingSpotifyVolume = -1;
static unsigned long s_nextSpotifyVolumeSendTime = 0;
static unsigned long s_lastVolumeAdjustTime = 0;

bool refreshSpotifyToken() {
    if (WiFi.status() != WL_CONNECTED) return false;
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    
    http.begin(client, "https://accounts.spotify.com/api/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
    String payload = "grant_type=refresh_token&refresh_token=" + String(SPOTIFY_REFRESH_TOKEN) + 
                     "&client_id=" + String(SPOTIFY_CLIENT_ID) + 
                     "&client_secret=" + String(SPOTIFY_CLIENT_SECRET);
                     
    int httpResponseCode = http.POST(payload);
    if (httpResponseCode == 200) {
        // parse HTTP response stream directly
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (!error) {
            spotifyAccessToken = doc["access_token"].as<String>();
            int expiresIn = doc["expires_in"].as<int>();
            spotifyTokenExpiryTime = millis() + (expiresIn - 60) * 1000;
            Serial.println("[Spotify] Token refreshed successfully.");
            http.end();
            return true;
        }
    }
    Serial.printf("[Spotify] Token refresh failed, HTTP: %d\r\n", httpResponseCode);
    http.end();
    return false;
}

static String s_lastSpotifyError = "Initialized";
String getSpotifyLastError() { return s_lastSpotifyError; }

static unsigned long s_spotifyRateLimitUntil = 0;

void sendSpotifyCommand(const char* method, const char* endpoint, const char* payload) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (millis() < s_spotifyRateLimitUntil) {
        s_lastSpotifyError = "Rate limited. Cooldown: " + String((s_spotifyRateLimitUntil - millis()) / 1000) + "s";
        return;
    }
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setReuse(false);
        
        String url = "https://api.spotify.com/v1/me/player/" + String(endpoint);
        // Do NOT constrain pause, volume, next, previous, shuffle, or play/resume commands to a specific device ID — apply globally to active playback
        bool isTransportCmd = (strcmp(endpoint, "pause") == 0 || strncmp(endpoint, "pause?", 6) == 0 ||
                               strncmp(endpoint, "volume", 6) == 0 ||
                               strcmp(endpoint, "next") == 0 ||
                               strcmp(endpoint, "previous") == 0 ||
                               strncmp(endpoint, "shuffle", 7) == 0 ||
                               (strcmp(endpoint, "play") == 0 && (payload == nullptr || strlen(payload) == 0)));
        if (!isTransportCmd) {
            String devId = getSpotifyDeviceId(getSpotifySelectedDeviceIndex());
            if (devId.length() > 0) {
                if (strchr(endpoint, '?') == nullptr) {
                    url += "?device_id=" + devId;
                } else {
                    url += "&device_id=" + devId;
                }
            }
        }
        
        http.begin(client, url);
        const char* retryHeaders[] = {"Retry-After"};
        http.collectHeaders(retryHeaders, 1);
        http.setTimeout(4000);
        http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
        http.addHeader("Content-Type", "application/json");
        size_t payloadLen = payload ? strlen(payload) : 0;
        http.addHeader("Content-Length", String(payloadLen));
        
        int httpResponseCode = 0;
        if (strcmp(method, "POST") == 0) {
            httpResponseCode = http.POST(payload ? payload : "");
        } else if (strcmp(method, "PUT") == 0) {
            httpResponseCode = http.PUT(payload ? payload : "");
        }
        
        if (httpResponseCode == 200 || httpResponseCode == 204) {
            s_lastSpotifyError = "OK: " + String(method) + " " + String(endpoint);
            Serial.printf("[Spotify] Command %s %s succeeded.\r\n", method, endpoint);
            http.end();
            break;
        } else if (httpResponseCode == 401 && attempt == 0) {
            Serial.printf("[Spotify] Command %s %s received 401 Unauthorized. Refreshing token & retrying...\r\n", method, endpoint);
            http.end();
            refreshSpotifyToken();
            continue;
        } else if (httpResponseCode == 404 && (strcmp(endpoint, "play") == 0 || strcmp(endpoint, "pause") == 0) && attempt == 0) {
            // Command failed without device; retry targeting configured device
            http.end();
            String devId = getSpotifyDeviceId(getSpotifySelectedDeviceIndex());
            if (devId.length() > 0) {
                Serial.printf("[Spotify] %s on active session returned 404. Retrying targeting device: %s\r\n", endpoint, devId.c_str());
                String retryUrl = "https://api.spotify.com/v1/me/player/" + String(endpoint) + "?device_id=" + devId;
                http.begin(client, retryUrl);
                http.setTimeout(4000);
                http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
                http.addHeader("Content-Type", "application/json");
                http.addHeader("Content-Length", "0");
                int retryCode = (strcmp(method, "POST") == 0) ? http.POST("") : http.PUT("");
                if (retryCode == 200 || retryCode == 204) {
                    s_lastSpotifyError = "OK (retried): " + String(endpoint);
                    Serial.printf("[Spotify] %s targeting device succeeded.\r\n", endpoint);
                    http.end();
                    break;
                }
            }
            continue;
        } else if (httpResponseCode == 429) {
            int retrySec = 60;
            if (http.hasHeader("Retry-After")) {
                retrySec = http.header("Retry-After").toInt();
                if (retrySec <= 0) retrySec = 60;
            }
            s_spotifyRateLimitUntil = millis() + (retrySec * 1000L);
            s_lastSpotifyError = "Rate limited (429). Cooldown " + String(retrySec) + "s";
            Serial.printf("[Spotify] Command hit 429 rate limit. Cooldown for %d seconds.\r\n", retrySec);
            http.end();
            break;
        } else {
            s_lastSpotifyError = "Fail HTTP " + String(httpResponseCode) + ": " + String(method) + " " + String(endpoint);
            Serial.printf("[Spotify] Command %s %s failed, HTTP: %d (url: %s)\r\n", method, endpoint, httpResponseCode, url.c_str());
            http.end();
            break;
        }
    }
}

void pollSpotifyPlaybackState() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (millis() < s_spotifyRateLimitUntil) return;
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    
    http.begin(client, "https://api.spotify.com/v1/me/player");
    const char* retryHeaders[] = {"Retry-After"};
    http.collectHeaders(retryHeaders, 1);
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    
    int httpResponseCode = http.GET();
    String newTrackText = spotifyTrackText;
    bool newIsPlaying = spotifyIsPlaying;
    bool newShuffle = spotifyShuffleState;
    if (httpResponseCode == 200) {
        // parse HTTP response stream directly
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (!error) {
            newIsPlaying = doc["is_playing"].as<bool>();
            if (!doc["shuffle_state"].isNull()) {
                newShuffle = doc["shuffle_state"].as<bool>();
            }
            JsonObject device = doc["device"].as<JsonObject>();
            if (!device.isNull()) {
                int vol = device["volume_percent"].as<int>();
                if (s_pendingSpotifyVolume < 0 && (millis() - s_lastVolumeAdjustTime >= 4000)) {
                    setSpotifyVolumePercent(vol);
                }
            }
            JsonObject item = doc["item"].as<JsonObject>();
            if (!item.isNull()) {
                String trackName = item["name"].as<String>();
                String artistName = "";
                JsonArray artists = item["artists"].as<JsonArray>();
                if (artists.size() > 0) {
                    artistName = artists[0]["name"].as<String>();
                }
                if (!artistName.isEmpty()) {
                    newTrackText = artistName + " - " + trackName;
                } else {
                    newTrackText = trackName;
                }
            } else {
                newTrackText = "Spotify Active";
            }
        }
    } else if (httpResponseCode == 204) {
        newIsPlaying = false;
        newTrackText = "Not Playing";
    } else if (httpResponseCode == 429) {
        int retrySec = 60;
        if (http.hasHeader("Retry-After")) {
            retrySec = http.header("Retry-After").toInt();
            if (retrySec <= 0) retrySec = 60;
        }
        s_spotifyRateLimitUntil = millis() + (retrySec * 1000L);
        s_lastSpotifyError = "HTTP 429: Rate limited (" + String(retrySec) + "s cooldown)";
        Serial.printf("[Spotify] Playback poll hit 429. Cooldown for %d seconds.\r\n", retrySec);
    } else {
        Serial.printf("[Spotify] Playback poll failed, HTTP: %d\r\n", httpResponseCode);
    }
    http.end();

    setSpotifyIsPlaying(newIsPlaying);
    setSpotifyTrackText(newTrackText);
    setSpotifyShuffleState(newShuffle);
}

void fetchSpotifyDevices() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    SpotifyDevice tempDevices[MAX_SPOTIFY_DEVICES];
    int tempCount = 0;
    int tempSelected = 0;
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    
    http.begin(client, "https://api.spotify.com/v1/me/player/devices");
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    
    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
        String response = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response.c_str());
        if (!error) {
            JsonArray deviceArray = doc["devices"].as<JsonArray>();
            for (JsonObject dev : deviceArray) {
                if (tempCount >= MAX_SPOTIFY_DEVICES) break;
                String id = dev["id"].as<String>();
                String name = dev["name"].as<String>();
                if (id.length() > 0 && name.length() > 0) {
                    tempDevices[tempCount].id = id;
                    tempDevices[tempCount].name = name;
                    tempCount++;
                }
            }
            Serial.printf("[Spotify] Found %d active devices from API.\r\n", tempCount);
        } else {
            Serial.printf("[Spotify] Device JSON parse error: %s\r\n", error.c_str());
        }
    } else {
        String errStr = http.getString();
        Serial.printf("[Spotify] Device fetch failed, HTTP %d: %s\r\n", httpResponseCode, errStr.c_str());
    }
    http.end();

    // If no active devices returned from API (e.g. Echo is in standby), use configured default speaker ID
    if (tempCount == 0 && String(DEFAULT_SPOTIFY_DEVICE_ID).length() > 0) {
        Serial.println("[Spotify] No active devices returned by API. Using default speaker fallback: " DEFAULT_SPOTIFY_DEVICE_NAME);
        tempDevices[0].id = DEFAULT_SPOTIFY_DEVICE_ID;
        tempDevices[0].name = DEFAULT_SPOTIFY_DEVICE_NAME;
        tempCount = 1;
        tempSelected = 0;
    }

    // Auto-select target device matching DEFAULT_SPOTIFY_DEVICE_NAME (e.g. Echo)
    for (int i = 0; i < tempCount; i++) {
        if (tempDevices[i].name.indexOf(DEFAULT_SPOTIFY_DEVICE_NAME) >= 0) {
            tempSelected = i;
            Serial.printf("[Spotify] Auto-selected target speaker: %s\r\n", tempDevices[i].name.c_str());
            break;
        }
    }

    if (spotifyStateMutex && xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < tempCount; i++) {
            spotifyDevices[i].id = tempDevices[i].id;
            spotifyDevices[i].name = tempDevices[i].name;
        }
        for (int i = tempCount; i < MAX_SPOTIFY_DEVICES; i++) {
            spotifyDevices[i].id = "";
            spotifyDevices[i].name = "";
        }
        numSpotifyDevices = tempCount;
        selectedSpotifyDeviceIndex = tempSelected;
        xSemaphoreGive(spotifyStateMutex);
    }
}

void selectSpotifyDevice(String deviceId, bool play) {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    
    http.begin(client, "https://api.spotify.com/v1/me/player");
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"device_ids\":[\"" + deviceId + "\"],\"play\":" + (play ? "true" : "false") + "}";
    http.addHeader("Content-Length", String(payload.length()));
    
    int httpResponseCode = http.PUT(payload);
    if (httpResponseCode == 200 || httpResponseCode == 204) {
        Serial.println("[Spotify] Playback transferred successfully!");
    } else {
        String errBody = http.getString();
        Serial.printf("[Spotify] Playback transfer response: HTTP %d: %s\r\n", httpResponseCode, errBody.c_str());
    }
    http.end();
}

void fetchSpotifyPlaylists() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    loadFavoritePlaylists();

    SpotifyPlaylist masterPlaylists[MAX_ALL_PLAYLISTS];
    int masterCount = 0;
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    
    http.begin(client, "https://api.spotify.com/v1/me/playlists?limit=50");
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    
    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
        JsonDocument filter;
        filter["items"][0]["uri"] = true;
        filter["items"][0]["name"] = true;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        if (!error) {
            JsonArray playlistArray = doc["items"].as<JsonArray>();
            for (JsonObject pl : playlistArray) {
                if (masterCount >= MAX_ALL_PLAYLISTS) break;
                String uri = pl["uri"].as<String>();
                String name = pl["name"].as<String>();
                if (uri.length() > 0 && name.length() > 0) {
                    masterPlaylists[masterCount].uri = uri;
                    masterPlaylists[masterCount].name = name;
                    masterCount++;
                }
            }
            Serial.printf("[Spotify] Successfully fetched %d playlists via filtered stream.\r\n", masterCount);
        } else {
            Serial.printf("[Spotify] Playlist JSON parse error: %s\r\n", error.c_str());
        }
    } else {
        Serial.printf("[Spotify] Playlist fetch failed, HTTP %d\r\n", httpResponseCode);
    }
    http.end();

    if (spotifyStateMutex && xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < masterCount; i++) {
            allSpotifyPlaylists[i] = masterPlaylists[i];
        }
        for (int i = masterCount; i < MAX_ALL_PLAYLISTS; i++) {
            allSpotifyPlaylists[i].uri = "";
            allSpotifyPlaylists[i].name = "";
        }
        numAllSpotifyPlaylists = masterCount;

        // Build 5 LCD screen playlists from user's favorite URIs saved in NVS
        int lcdCount = 0;
        bool hasFavs = false;
        for (int i = 0; i < MAX_SPOTIFY_PLAYLISTS; i++) {
            if (favoritePlaylistUris[i].length() > 0) {
                hasFavs = true;
                break;
            }
        }

        if (hasFavs) {
            for (int i = 0; i < MAX_SPOTIFY_PLAYLISTS; i++) {
                String targetUri = favoritePlaylistUris[i];
                if (targetUri.length() > 0) {
                    for (int j = 0; j < masterCount; j++) {
                        if (masterPlaylists[j].uri == targetUri) {
                            spotifyPlaylists[lcdCount++] = masterPlaylists[j];
                            break;
                        }
                    }
                }
            }
        }

        // Fill remaining LCD slots with standard master playlists
        for (int j = 0; j < masterCount && lcdCount < MAX_SPOTIFY_PLAYLISTS; j++) {
            bool alreadyAdded = false;
            for (int k = 0; k < lcdCount; k++) {
                if (spotifyPlaylists[k].uri == masterPlaylists[j].uri) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                spotifyPlaylists[lcdCount++] = masterPlaylists[j];
            }
        }

        for (int i = lcdCount; i < MAX_SPOTIFY_PLAYLISTS; i++) {
            spotifyPlaylists[i].uri = "";
            spotifyPlaylists[i].name = "";
        }
        numSpotifyPlaylists = lcdCount;
        selectedSpotifyPlaylistIndex = 0;
        xSemaphoreGive(spotifyStateMutex);
    }
    Serial.printf("[Spotify] Master playlists: %d, LCD Clock Playlists: %d\r\n", masterCount, numSpotifyPlaylists);
}

int getAllSpotifyNumPlaylists() {
    if (spotifyStateMutex == NULL) return numAllSpotifyPlaylists;
    int res = 0;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        res = numAllSpotifyPlaylists;
        xSemaphoreGive(spotifyStateMutex);
    }
    return res;
}

String getAllSpotifyPlaylistName(int idx) {
    if (spotifyStateMutex == NULL) return "";
    String res = "";
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < numAllSpotifyPlaylists) {
            res = allSpotifyPlaylists[idx].name;
        }
        xSemaphoreGive(spotifyStateMutex);
    }
    return res;
}

String getAllSpotifyPlaylistUri(int idx) {
    if (spotifyStateMutex == NULL) return "";
    String res = "";
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < numAllSpotifyPlaylists) {
            res = allSpotifyPlaylists[idx].uri;
        }
        xSemaphoreGive(spotifyStateMutex);
    }
    return res;
}

void playSpotifyPlaylist(String playlistUri) {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    if (getSpotifyNumDevices() == 0) {
        fetchSpotifyDevices();
    }

    String devId = getSpotifyDeviceId(getSpotifySelectedDeviceIndex());
    if (devId.length() == 0 && getSpotifyNumDevices() > 0) {
        devId = getSpotifyDeviceId(0);
    }

    if (devId.length() == 0) {
        // Speaker not visible to Spotify yet; wake it and defer playback
        triggerSinricProWakeup();
        s_pendingPlaybackUri = playlistUri;
        s_pendingPlaybackDeviceId = devId;
        s_pendingPlaybackStartTime = millis() + 1500;
        s_pendingPlayback = true;
        setSpotifyTrackText("Waking Echo...");
        Serial.println("[Spotify] Echo not in device list. Wake-up sent, playback deferred.");
        return;
    }

    // Speaker is in Spotify's device list; try playing immediately
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);

    String targetUrl = "https://api.spotify.com/v1/me/player/play";
    if (devId.length() > 0) {
        targetUrl += "?device_id=" + devId;
    }
    http.begin(client, targetUrl);
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"context_uri\":\"" + playlistUri + "\"}";
    http.addHeader("Content-Length", String(payload.length()));

    int httpResponseCode = http.PUT(payload);
    String errBody = http.getString();
    http.end();

    if (httpResponseCode == 200 || httpResponseCode == 204) {
        Serial.println("[Spotify] Playlist playback started directly (no wake needed).");
        setSpotifyIsPlaying(true);
        setSpotifyTrackText("Playing...");
        return;
    }

    Serial.printf("[Spotify] Direct play failed, HTTP %d: %s\r\n", httpResponseCode, errBody.c_str());

    // Direct play failed; wake the speaker and retry later
    triggerSinricProWakeup();
    s_pendingPlaybackUri = playlistUri;
    s_pendingPlaybackDeviceId = devId;
    s_pendingPlaybackStartTime = millis() + 1500;
    s_pendingPlayback = true;
    setSpotifyTrackText("Waking Echo...");
    Serial.println("[Spotify] Echo appears in list but did not play. Wake-up sent, playback deferred.");
}

void continuePendingSpotifyPlayback() {
    if (!s_pendingPlayback || millis() < s_pendingPlaybackStartTime) {
        return;
    }
    s_pendingPlayback = false;

    if (WiFi.status() != WL_CONNECTED) return;
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }

    // Refresh active device table after the wake-up window
    fetchSpotifyDevices();

    String devId = s_pendingPlaybackDeviceId;
    if (devId.length() == 0 && getSpotifyNumDevices() > 0) {
        devId = getSpotifyDeviceId(0);
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);

    for (int retry = 0; retry < 3; retry++) {
        String targetUrl = "https://api.spotify.com/v1/me/player/play";
        if (devId.length() > 0) {
            targetUrl += "?device_id=" + devId;
        }
        Serial.printf("[Spotify] Play attempt %d/3 on target device: %s\r\n", retry + 1, devId.c_str());

        http.begin(client, targetUrl);
        http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
        http.addHeader("Content-Type", "application/json");

        String payload = "{\"context_uri\":\"" + s_pendingPlaybackUri + "\"}";
        http.addHeader("Content-Length", String(payload.length()));

        int httpResponseCode = http.PUT(payload);
        if (httpResponseCode == 200 || httpResponseCode == 204) {
            Serial.println("[Spotify] Playlist playback started successfully!");
            setSpotifyIsPlaying(true);
            setSpotifyTrackText("Playing...");
            http.end();
            return;
        } else {
            String errBody = http.getString();
            Serial.printf("[Spotify] Attempt %d/3 got HTTP %d: %s\r\n", retry + 1, httpResponseCode, errBody.c_str());
            http.end();

            if (httpResponseCode == 404 && retry < 2) {
                Serial.println("[Spotify] Echo in cloud handshake. Waiting 1.5s & refreshing devices...");
                delay(1500);
                fetchSpotifyDevices();
                if (getSpotifyNumDevices() > 0) {
                    devId = getSpotifyDeviceId(0);
                }
            } else {
                break;
            }
        }
    }
}

void toggleSpotifyShuffle(bool shuffle) {
    char endpoint[32];
    snprintf(endpoint, sizeof(endpoint), "shuffle?state=%s", shuffle ? "true" : "false");
    sendSpotifyCommand("PUT", endpoint);
    setSpotifyShuffleState(shuffle);
}

void syncSpotifyShuffleState() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (spotifyAccessToken == "" || millis() >= spotifyTokenExpiryTime) {
        if (!refreshSpotifyToken()) return;
    }
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    
    http.begin(client, "https://api.spotify.com/v1/me/player");
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    
    bool newShuffle = getSpotifyShuffleState();
    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
        // parse HTTP response stream directly
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (!error) {
            if (!doc["shuffle_state"].isNull()) {
                newShuffle = doc["shuffle_state"].as<bool>();
            }
        }
    }
    http.end();
    setSpotifyShuffleState(newShuffle);
}

void initSpotifyClient() {
    spotifyCommandQueue = xQueueCreate(8, sizeof(SpotifyCommand));
    spotifyStateMutex = xSemaphoreCreateMutex();
}

bool postSpotifyCommand(const SpotifyCommand& cmd) {
    if (spotifyCommandQueue == NULL) return false;
    return xQueueSend(spotifyCommandQueue, &cmd, 0) == pdTRUE;
}

bool receiveSpotifyCommand(SpotifyCommand& out, TickType_t waitTicks) {
    if (spotifyCommandQueue == NULL) return false;
    return xQueueReceive(spotifyCommandQueue, &out, waitTicks) == pdTRUE;
}

void setPendingSpotifyVolume(int percent) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        s_pendingSpotifyVolume = percent;
        s_nextSpotifyVolumeSendTime = millis() + 400;
        s_lastVolumeAdjustTime = millis();
        xSemaphoreGive(spotifyStateMutex);
    }
}

void flushPendingSpotifyVolume() {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (s_pendingSpotifyVolume >= 0) {
            s_nextSpotifyVolumeSendTime = millis();
        }
        xSemaphoreGive(spotifyStateMutex);
    }
}

void sendPendingSpotifyVolume() {
    if (spotifyStateMutex == NULL) return;
    int vol = -1;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (s_pendingSpotifyVolume >= 0 && millis() >= s_nextSpotifyVolumeSendTime) {
            vol = s_pendingSpotifyVolume;
            s_pendingSpotifyVolume = -1;
        }
        xSemaphoreGive(spotifyStateMutex);
    }
    if (vol >= 0) {
        char endpoint[32];
        snprintf(endpoint, sizeof(endpoint), "volume?volume_percent=%d", vol);
        sendSpotifyCommand("PUT", endpoint);
    }
}

void processSpotifyCommand(const SpotifyCommand& cmd) {
    switch (cmd.type) {
        case SPOTIFY_CMD_VOLUME:
            setSpotifyVolumePercent(cmd.intParam);
            setPendingSpotifyVolume(cmd.intParam);
            break;
        case SPOTIFY_CMD_PLAY:
            sendSpotifyCommand("PUT", "play");
            setSpotifyIsPlaying(true);
            setSpotifyTrackText("Playing...");
            break;
        case SPOTIFY_CMD_PAUSE:
            sendSpotifyCommand("PUT", "pause");
            setSpotifyIsPlaying(false);
            setSpotifyTrackText("Paused");
            break;
        case SPOTIFY_CMD_NEXT:
            sendSpotifyCommand("POST", "next");
            break;
        case SPOTIFY_CMD_PREV:
            sendSpotifyCommand("POST", "previous");
            break;
        case SPOTIFY_CMD_TOGGLE_PLAY_PAUSE:
            if (getSpotifyIsPlaying()) {
                sendSpotifyCommand("PUT", "pause");
                setSpotifyIsPlaying(false);
                setSpotifyTrackText("Paused");
            } else {
                sendSpotifyCommand("PUT", "play");
                setSpotifyIsPlaying(true);
                setSpotifyTrackText("Playing...");
            }
            break;
        case SPOTIFY_CMD_SELECT_DEVICE:
            selectSpotifyDevice(String(cmd.stringParam));
            setSpotifyTrackText("Connecting...");
            break;
        case SPOTIFY_CMD_PLAY_PLAYLIST:
            playSpotifyPlaylist(String(cmd.stringParam));
            setSpotifyTrackText("Connecting...");
            break;
        case SPOTIFY_CMD_SET_SHUFFLE:
            toggleSpotifyShuffle(cmd.boolParam);
            break;
        case SPOTIFY_CMD_FETCH_DEVICES:
            isFetchingDevices = true;
            fetchSpotifyDevices();
            isFetchingDevices = false;
            break;
        case SPOTIFY_CMD_FETCH_PLAYLISTS:
            isFetchingPlaylists = true;
            fetchSpotifyPlaylists();
            isFetchingPlaylists = false;
            break;
        default:
            break;
    }
}

String getSpotifyTrackText() {
    if (spotifyStateMutex == NULL) return spotifyTrackText;
    String result;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = spotifyTrackText;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

bool getSpotifyIsPlaying() {
    if (spotifyStateMutex == NULL) return spotifyIsPlaying;
    bool result = false;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = spotifyIsPlaying;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

bool getSpotifyShuffleState() {
    if (spotifyStateMutex == NULL) return spotifyShuffleState;
    bool result = false;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = spotifyShuffleState;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

void setSpotifyIsPlaying(bool value) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        spotifyIsPlaying = value;
        xSemaphoreGive(spotifyStateMutex);
    }
}

int getSpotifyVolumePercent() {
    if (spotifyStateMutex == NULL) return spotifyVolumePercent;
    int result = 0;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = spotifyVolumePercent;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

void setSpotifyVolumePercent(int value) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        spotifyVolumePercent = value;
        xSemaphoreGive(spotifyStateMutex);
    }
}

void setSpotifyTrackText(const String& text) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        spotifyTrackText = text;
        xSemaphoreGive(spotifyStateMutex);
    }
}

void setSpotifyShuffleState(bool value) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        spotifyShuffleState = value;
        xSemaphoreGive(spotifyStateMutex);
    }
}

int getSpotifyNumDevices() {
    if (spotifyStateMutex == NULL) return numSpotifyDevices;
    int result = 0;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = numSpotifyDevices;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

int getSpotifyNumPlaylists() {
    if (spotifyStateMutex == NULL) return numSpotifyPlaylists;
    int result = 0;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = numSpotifyPlaylists;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

String getSpotifyDeviceId(int idx) {
    if (spotifyStateMutex == NULL) return (idx >= 0 && idx < numSpotifyDevices) ? spotifyDevices[idx].id : "";
    String result;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < numSpotifyDevices) result = spotifyDevices[idx].id;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

String getSpotifyDeviceName(int idx) {
    if (spotifyStateMutex == NULL) return (idx >= 0 && idx < numSpotifyDevices) ? spotifyDevices[idx].name : "";
    String result;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < numSpotifyDevices) result = spotifyDevices[idx].name;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

String getSpotifyPlaylistUri(int idx) {
    if (spotifyStateMutex == NULL) return (idx >= 0 && idx < numSpotifyPlaylists) ? spotifyPlaylists[idx].uri : "";
    String result;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < numSpotifyPlaylists) result = spotifyPlaylists[idx].uri;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

String getSpotifyPlaylistName(int idx) {
    if (spotifyStateMutex == NULL) return (idx >= 0 && idx < numSpotifyPlaylists) ? spotifyPlaylists[idx].name : "";
    String result;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < numSpotifyPlaylists) result = spotifyPlaylists[idx].name;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

int getSpotifySelectedDeviceIndex() {
    if (spotifyStateMutex == NULL) return selectedSpotifyDeviceIndex;
    int result = 0;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = selectedSpotifyDeviceIndex;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

void setSpotifySelectedDeviceIndex(int idx) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        selectedSpotifyDeviceIndex = idx;
        xSemaphoreGive(spotifyStateMutex);
    }
}

int getSpotifySelectedPlaylistIndex() {
    if (spotifyStateMutex == NULL) return selectedSpotifyPlaylistIndex;
    int result = 0;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        result = selectedSpotifyPlaylistIndex;
        xSemaphoreGive(spotifyStateMutex);
    }
    return result;
}

void setSpotifySelectedPlaylistIndex(int idx) {
    if (spotifyStateMutex == NULL) return;
    if (xSemaphoreTake(spotifyStateMutex, portMAX_DELAY) == pdTRUE) {
        selectedSpotifyPlaylistIndex = idx;
        xSemaphoreGive(spotifyStateMutex);
    }
}
