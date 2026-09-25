#include "gcal_client.h"
#include "config.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <freertos/semphr.h>
#include <time.h>

#define GCAL_UPDATE_INTERVAL (30 * 60 * 1000) // 30 minutes
static const char* DEFAULT_GCAL_URL = "";

static String gcalUrl = DEFAULT_GCAL_URL;
static String gcalFilterKeyword = "";
static int gcalLookaheadHours = 48; // Default 48 hours (2 days)
static bool gcalEnabled = true;
static bool gcalLoaded = false;
static unsigned long lastGCalUpdate = 0;

static GCalEvent s_nextEvent;
static int s_eventCount = 0;
static SemaphoreHandle_t gcalMutex = NULL;

static void loadGCalPreferences() {
    Preferences prefs;
    prefs.begin("alarm_clock", false);
    gcalEnabled = prefs.getBool("gcal_en", true);
    String savedUrl = prefs.getString("gcal_url", "");
    
    if (savedUrl.length() >= 15) {
        gcalUrl = savedUrl;
        gcalFilterKeyword = prefs.getString("gcal_kw", "");
    } else {
        gcalUrl = DEFAULT_GCAL_URL;
        gcalFilterKeyword = "";
    }
    gcalLookaheadHours = prefs.getInt("gcal_lookahead", 168);
    if (gcalLookaheadHours < 24) gcalLookaheadHours = 168;
    prefs.end();
}

static void saveGCalPreferences() {
    Preferences prefs;
    prefs.begin("alarm_clock", false);
    prefs.putBool("gcal_en", gcalEnabled);
    if (gcalUrl.length() >= 15 && !gcalUrl.startsWith("****")) {
        prefs.putString("gcal_url", gcalUrl);
    }
    prefs.putString("gcal_kw", gcalFilterKeyword);
    prefs.putInt("gcal_lookahead", gcalLookaheadHours);
    prefs.end();
}

void setupGCal() {
    if (gcalMutex == NULL) {
        gcalMutex = xSemaphoreCreateMutex();
    }
    loadGCalPreferences();
    Serial.printf("[GCal] Initialized. Enabled: %s, Filter: '%s', Lookahead: %dh\r\n", 
                  gcalEnabled ? "YES" : "NO", gcalFilterKeyword.c_str(), gcalLookaheadHours);
}

void setGCalUrl(const String& url) {
    String cleanUrl = url;
    cleanUrl.trim();
    if (cleanUrl.length() < 15 || cleanUrl.startsWith("****") || cleanUrl == "********") {
        return;
    }
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        gcalUrl = cleanUrl;
        lastGCalUpdate = 0;
        xSemaphoreGive(gcalMutex);
    }
    saveGCalPreferences();
}

String getGCalUrl() {
    String url;
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        url = gcalUrl;
        xSemaphoreGive(gcalMutex);
    }
    return url;
}

void setGCalFilterKeyword(const String& kw) {
    String cleanKw = kw;
    cleanKw.trim();
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        gcalFilterKeyword = cleanKw;
        lastGCalUpdate = 0;
        xSemaphoreGive(gcalMutex);
    }
    saveGCalPreferences();
}

String getGCalFilterKeyword() {
    String kw;
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        kw = gcalFilterKeyword;
        xSemaphoreGive(gcalMutex);
    }
    return kw;
}

void setGCalLookaheadHours(int hours) {
    if (hours < 24) hours = 24;
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        gcalLookaheadHours = hours;
        lastGCalUpdate = 0;
        xSemaphoreGive(gcalMutex);
    }
    saveGCalPreferences();
}

int getGCalLookaheadHours() {
    int h = 48;
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        h = gcalLookaheadHours;
        xSemaphoreGive(gcalMutex);
    }
    return h;
}

void setGCalEnabled(bool enabled) {
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        gcalEnabled = enabled;
        if (enabled) lastGCalUpdate = 0;
        xSemaphoreGive(gcalMutex);
    }
    saveGCalPreferences();
}

bool isGCalEnabled() {
    bool en = true;
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        en = gcalEnabled;
        xSemaphoreGive(gcalMutex);
    }
    return en;
}

void triggerImmediateGCalSync() {
    lastGCalUpdate = 0;
}

static String cleanEventTitle(const String& rawTitle, const String& keyword) {
    String title = rawTitle;
    title.trim();
    
    // Normalize common UTF-8 punctuation
    title.replace("\xE2\x80\x99", "'"); // Smart right single quote ’
    title.replace("\xE2\x80\x98", "'"); // Smart left single quote ‘
    title.replace("\xE2\x80\x9C", "\""); // Smart left double quote “
    title.replace("\xE2\x80\x9D", "\""); // Smart right double quote ”
    title.replace("\xE2\x80\x93", "-"); // En dash –
    title.replace("\xE2\x80\x94", "-"); // Em dash —
    title.replace("\xE2\x80\xA6", ".."); // Horizontal ellipsis …
    
    // Strip emojis and non-ASCII characters that corrupt LCD character ROM
    String asciiTitle = "";
    for (size_t i = 0; i < title.length(); i++) {
        char c = title[i];
        if ((uint8_t)c >= 32 && (uint8_t)c <= 126) {
            asciiTitle += c;
        }
    }
    title = asciiTitle;
    title.trim();
    
    String kwLower = keyword;
    kwLower.toLowerCase();
    kwLower.trim();
    
    String lower = title;
    lower.toLowerCase();
    
    if (kwLower.length() > 0) {
        String p1 = kwLower + " - ";
        String p2 = kwLower + ": ";
        String p3 = kwLower + " | ";
        String p4 = kwLower + "'s ";
        String p5 = kwLower + " ";
        
        if (lower.startsWith(p1)) title = title.substring(p1.length());
        else if (lower.startsWith(p2)) title = title.substring(p2.length());
        else if (lower.startsWith(p3)) title = title.substring(p3.length());
        else if (lower.startsWith(p4)) title = title.substring(p4.length());
        else if (lower.startsWith(p5)) title = title.substring(p5.length());
        
        String pParen = "(" + kwLower + ")";
        String pBracket = "[" + kwLower + "]";
        lower = title;
        lower.toLowerCase();
        int idxParen = lower.indexOf(pParen);
        if (idxParen != -1) {
            title = title.substring(0, idxParen) + title.substring(idxParen + pParen.length());
        }
        lower = title;
        lower.toLowerCase();
        int idxBracket = lower.indexOf(pBracket);
        if (idxBracket != -1) {
            title = title.substring(0, idxBracket) + title.substring(idxBracket + pBracket.length());
        }
    }
    
    title.trim();
    if (title.length() == 0) title = rawTitle;
    return title;
}

String getGCalNoEventsMessage(int lookaheadHours) {
    if (lookaheadHours <= 24) return "No events (24h)";
    if (lookaheadHours <= 48) return "No events (2d)";
    if (lookaheadHours <= 72) return "No events (3d)";
    return "No events (7d)";
}

static String formatEventDisplay(const GCalEvent& ev) {
    String prefix = "";
    if (!ev.isAllDay && ev.hour >= 0) {
        int hr12 = ev.hour % 12;
        if (hr12 == 0) hr12 = 12;
        const char* ampm = (ev.hour < 12) ? "a" : "p";
        char tBuf[16];
        if (ev.minute == 0) {
            snprintf(tBuf, sizeof(tBuf), "%d%s", hr12, ampm);
        } else {
            snprintf(tBuf, sizeof(tBuf), "%d:%02d%s", hr12, ev.minute, ampm);
        }
        
        if (ev.isToday) {
            prefix = String(tBuf) + " ";
        } else if (ev.isTomorrow) {
            prefix = "Tmw " + String(tBuf) + " ";
        } else {
            const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            struct tm evTm = {0};
            evTm.tm_year = ev.year - 1900;
            evTm.tm_mon = ev.month - 1;
            evTm.tm_mday = ev.day;
            mktime(&evTm);
            prefix = String(days[evTm.tm_wday]) + " " + String(tBuf) + " ";
        }
    } else {
        if (ev.isToday) prefix = "";
        else if (ev.isTomorrow) prefix = "Tmw ";
        else {
            const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            struct tm evTm = {0};
            evTm.tm_year = ev.year - 1900;
            evTm.tm_mon = ev.month - 1;
            evTm.tm_mday = ev.day;
            mktime(&evTm);
            prefix = String(days[evTm.tm_wday]) + " ";
        }
    }
    
    String full = prefix + ev.title;
    if (full.length() > 16) {
        full = full.substring(0, 14) + "..";
    }
    return full;
}

void updateGCal() {
    unsigned long now = millis();
    if (!gcalEnabled) return;
    if (gcalUrl.length() < 15) {
        gcalUrl = DEFAULT_GCAL_URL;
    }
    if (WiFi.status() != WL_CONNECTED) return;
    if (gcalLoaded && (now - lastGCalUpdate < GCAL_UPDATE_INTERVAL)) return;
    if (ESP.getFreeHeap() < 25000) {
        Serial.printf("[GCal] Free heap low (%u bytes), skipping sync.\r\n", ESP.getFreeHeap());
        return;
    }
    
    struct tm curTm;
    if (!getLocalTime(&curTm, 0)) {
        return; // Clock not synced with NTP yet
    }
    
    time_t nowEpoch = mktime(&curTm);
    
    // Calculate start of today (midnight 00:00:00 local time)
    struct tm startTodayTm = curTm;
    startTodayTm.tm_hour = 0;
    startTodayTm.tm_min = 0;
    startTodayTm.tm_sec = 0;
    time_t startTodayEpoch = mktime(&startTodayTm);
    time_t endTodayEpoch = startTodayEpoch + 86400;
    time_t endTmwEpoch = startTodayEpoch + 2 * 86400;
    
    int activeLookaheadHours = gcalLookaheadHours;
    if (activeLookaheadHours < 24) activeLookaheadHours = 48;
    time_t endWindowEpoch = startTodayEpoch + ((time_t)activeLookaheadHours * 3600);
    
    Serial.printf("[GCal] Syncing calendar (nowEpoch: %ld, lookahead: %dh)...\r\n", 
                  (long)nowEpoch, activeLookaheadHours);
    
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(6000);
    
    HTTPClient http;
    if (!http.begin(client, gcalUrl)) {
        Serial.println("[GCal] Failed to connect to iCal URL.");
        lastGCalUpdate = now - GCAL_UPDATE_INTERVAL + 60000;
        return;
    }
    
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000); // 8 seconds total HTTP timeout
    
    int httpCode = http.GET();
    if (httpCode != 200) {
        Serial.printf("[GCal] HTTP GET failed: %d\r\n", httpCode);
        http.end();
        lastGCalUpdate = now - GCAL_UPDATE_INTERVAL + 60000;
        return;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        return;
    }
    
    const int MAX_CANDIDATES = 16;
    GCalEvent candidates[MAX_CANDIDATES];
    time_t candidateEpochs[MAX_CANDIDATES];
    time_t candidateEndEpochs[MAX_CANDIDATES];
    int candidateCount = 0;
    
    bool inEvent = false;
    String rawSummary = "";
    String rawDtStart = "";
    String rawDtEnd = "";
    
    String kwLower = gcalFilterKeyword;
    kwLower.toLowerCase();
    kwLower.trim();
    
    char lineBuf[256];
    unsigned long startRead = millis();
    int linesRead = 0;
    
    while (http.connected() || stream->available()) {
        if (stream->available()) {
            int bytesRead = stream->readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
            if (bytesRead <= 0) continue;
            
            lineBuf[bytesRead] = '\0';
            if (bytesRead > 0 && lineBuf[bytesRead - 1] == '\r') {
                lineBuf[bytesRead - 1] = '\0';
            }
            linesRead++;
            startRead = millis();
            
            if (strncmp(lineBuf, "BEGIN:VEVENT", 12) == 0) {
                inEvent = true;
                rawSummary = "";
                rawDtStart = "";
                rawDtEnd = "";
            } 
            else if (strncmp(lineBuf, "END:VEVENT", 10) == 0) {
                if (inEvent && rawSummary.length() > 0 && rawDtStart.length() >= 8) {
                    const char* val = rawDtStart.c_str();
                    char* colon = strchr(val, ':');
                    if (colon) val = colon + 1;
                        
                        if (strlen(val) >= 8) {
                            int yr = (val[0]-'0')*1000 + (val[1]-'0')*100 + (val[2]-'0')*10 + (val[3]-'0');
                            int mo = (val[4]-'0')*10 + (val[5]-'0');
                            int dy = (val[6]-'0')*10 + (val[7]-'0');
                            int hr = 0, mn = 0;
                            bool isAllDay = true;
                            
                            char* tPos = strchr(val, 'T');
                            if (tPos && strlen(tPos) >= 5) {
                                isAllDay = false;
                                hr = (tPos[1]-'0')*10 + (tPos[2]-'0');
                                mn = (tPos[3]-'0')*10 + (tPos[4]-'0');
                            }
                            
                            struct tm evTm = {0};
                            evTm.tm_year = yr - 1900;
                            evTm.tm_mon = mo - 1;
                            evTm.tm_mday = dy;
                            evTm.tm_hour = hr;
                            evTm.tm_min = mn;
                            evTm.tm_sec = 0;
                            evTm.tm_isdst = curTm.tm_isdst;
                            
                            time_t evEpoch = mktime(&evTm);
                            
                            // If timestamp is UTC ('Z'), convert to local Eastern epoch
                            if (strchr(val, 'Z')) {
                                int offsetSec = (curTm.tm_isdst > 0) ? 14400 : 18000;
                                evEpoch -= offsetSec;
                            }
                            
                            // Parse DTEND if present
                            time_t evEndEpoch = evEpoch + 1800; // default to +30m
                            if (rawDtEnd.length() >= 8) {
                                const char* endVal = rawDtEnd.c_str();
                                char* endColon = strchr(endVal, ':');
                                if (endColon) endVal = endColon + 1;
                                if (strlen(endVal) >= 8) {
                                    int eYr = (endVal[0]-'0')*1000 + (endVal[1]-'0')*100 + (endVal[2]-'0')*10 + (endVal[3]-'0');
                                    int eMo = (endVal[4]-'0')*10 + (endVal[5]-'0');
                                    int eDy = (endVal[6]-'0')*10 + (endVal[7]-'0');
                                    int eHr = 0, eMn = 0;
                                    char* etPos = strchr(endVal, 'T');
                                    if (etPos && strlen(etPos) >= 5) {
                                        eHr = (etPos[1]-'0')*10 + (etPos[2]-'0');
                                        eMn = (etPos[3]-'0')*10 + (etPos[4]-'0');
                                    }
                                    struct tm endTm = {0};
                                    endTm.tm_year = eYr - 1900;
                                    endTm.tm_mon = eMo - 1;
                                    endTm.tm_mday = eDy;
                                    endTm.tm_hour = eHr;
                                    endTm.tm_min = eMn;
                                    endTm.tm_sec = 0;
                                    endTm.tm_isdst = curTm.tm_isdst;
                                    evEndEpoch = mktime(&endTm);
                                    if (strchr(endVal, 'Z')) {
                                        int offsetSec = (curTm.tm_isdst > 0) ? 14400 : 18000;
                                        evEndEpoch -= offsetSec;
                                    }
                                }
                            }
                            
                            bool inWindow = (evEpoch >= startTodayEpoch && evEpoch < endWindowEpoch);
                            
                            if (inWindow) {
                                if (candidateCount < MAX_CANDIDATES) {
                                    struct tm localEv;
                                    localtime_r(&evEpoch, &localEv);
                                    
                                    bool isToday = (evEpoch >= startTodayEpoch && evEpoch < endTodayEpoch);
                                    bool isTmw = (evEpoch >= endTodayEpoch && evEpoch < endTmwEpoch);
                                    
                                    GCalEvent ev;
                                    ev.title = cleanEventTitle(rawSummary, gcalFilterKeyword);
                                    ev.isAllDay = isAllDay;
                                    ev.isToday = isToday;
                                    ev.isTomorrow = isTmw;
                                    ev.year = localEv.tm_year + 1900;
                                    ev.month = localEv.tm_mon + 1;
                                    ev.day = localEv.tm_mday;
                                    ev.hour = isAllDay ? -1 : localEv.tm_hour;
                                    ev.minute = isAllDay ? 0 : localEv.tm_min;
                                    
                                    candidates[candidateCount] = ev;
                                    candidateEpochs[candidateCount] = evEpoch;
                                    candidateEndEpochs[candidateCount] = evEndEpoch;
                                    candidateCount++;
                                    
                                    Serial.printf("[GCal] Candidate #%d: '%s' on %04d-%02d-%02d at %02d:%02d (ends %ld)\r\n",
                                                  candidateCount, ev.title.c_str(), ev.year, ev.month, ev.day, 
                                                  ev.hour, ev.minute, (long)evEndEpoch);
                                }
                        }
                    }
                }
                inEvent = false;
            } 
            else if (inEvent) {
                if (strncmp(lineBuf, "SUMMARY", 7) == 0) {
                    char* colon = strchr(lineBuf, ':');
                    if (colon) {
                        rawSummary = String(colon + 1);
                        rawSummary.trim();
                    }
                } 
                else if (strncmp(lineBuf, "DTSTART", 7) == 0) {
                    rawDtStart = String(lineBuf);
                    rawDtStart.trim();
                }
                else if (strncmp(lineBuf, "DTEND", 5) == 0) {
                    rawDtEnd = String(lineBuf);
                    rawDtEnd.trim();
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
            if (millis() - startRead > 3000) {
                break;
            }
        }
    }
    
    http.end();
    Serial.printf("[GCal] Stream complete (%d lines). Found %d candidates.\r\n", linesRead, candidateCount);
    
    // Sort and pick the single best upcoming event
    GCalEvent bestEvent;
    bool found = false;
    time_t bestEpoch = 0x7FFFFFFF;
    
    // 1. Upcoming timed event TODAY (stays active on screen until its actual scheduled end time)
    for (int i = 0; i < candidateCount; i++) {
        if (candidates[i].isToday && !candidates[i].isAllDay) {
            if (candidateEndEpochs[i] >= nowEpoch) {
                if (candidateEpochs[i] < bestEpoch) {
                    bestEpoch = candidateEpochs[i];
                    bestEvent = candidates[i];
                    found = true;
                }
            }
        }
    }
    
    // 2. Earliest timed upcoming event in future days (tomorrow onwards within lookahead window)
    if (!found) {
        bestEpoch = 0x7FFFFFFF;
        for (int i = 0; i < candidateCount; i++) {
            if (!candidates[i].isToday && !candidates[i].isAllDay && candidateEpochs[i] >= endTodayEpoch) {
                if (candidateEpochs[i] < bestEpoch) {
                    bestEpoch = candidateEpochs[i];
                    bestEvent = candidates[i];
                    found = true;
                }
            }
        }
    }
    
    // 3. All-Day event TODAY
    if (!found) {
        for (int i = 0; i < candidateCount; i++) {
            if (candidates[i].isToday && candidates[i].isAllDay) {
                bestEvent = candidates[i];
                found = true;
                break;
            }
        }
    }
    
    // 4. All-Day event in future days
    if (!found) {
        for (int i = 0; i < candidateCount; i++) {
            if (!candidates[i].isToday && candidates[i].isAllDay && candidateEpochs[i] >= endTodayEpoch) {
                bestEvent = candidates[i];
                found = true;
                break;
            }
        }
    }
    
    if (found) {
        bestEvent.displayString = formatEventDisplay(bestEvent);
        Serial.printf("[GCal] Selected event: '%s' -> '%s'\r\n", 
                      bestEvent.title.c_str(), bestEvent.displayString.c_str());
    } else {
        bestEvent.title = "";
        bestEvent.displayString = getGCalNoEventsMessage(activeLookaheadHours);
        Serial.printf("[GCal] No upcoming events in %dh matching filter.\r\n", activeLookaheadHours);
    }
    
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_nextEvent = bestEvent;
        s_eventCount = candidateCount;
        gcalLoaded = true;
        lastGCalUpdate = now;
        xSemaphoreGive(gcalMutex);
    }
}

GCalSnapshot getGCalSnapshot() {
    GCalSnapshot snap;
    if (gcalMutex && xSemaphoreTake(gcalMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snap.enabled = gcalEnabled;
        snap.loaded = gcalLoaded;
        snap.eventCount = s_eventCount;
        snap.nextEvent = s_nextEvent;
        snap.filterKeyword = gcalFilterKeyword;
        snap.lookaheadHours = gcalLookaheadHours;
        xSemaphoreGive(gcalMutex);
    }
    return snap;
}
