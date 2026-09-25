#include "school_calendar.h"
#include "config.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// Update interval: once per day (in milliseconds)
#define SCHOOL_CALENDAR_UPDATE_INTERVAL (24 * 60 * 60 * 1000)

// Global variables
SchoolDayOff schoolDaysOff[MAX_SCHOOL_DAYS_OFF];
int numSchoolDaysOff = 0;
bool schoolCalendarLoaded = false;
unsigned long lastSchoolCalendarUpdate = 0;

// Cache for getNextSchoolDayOff to avoid recomputing the same day repeatedly
static int cachedCurrentYmd = -1;
static unsigned long cachedUpdateTime = 0;
static int cachedDaysUntil = -1;
static String cachedDayOffName = "";

static int ymdKey(struct tm* t) {
    return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
}

static void updateSchoolCache(int ymd, int days, const String& name) {
    cachedCurrentYmd = ymd;
    cachedUpdateTime = lastSchoolCalendarUpdate;
    cachedDaysUntil = days;
    cachedDayOffName = name;
}

void setupSchoolCalendar() {
    Serial.println("[School] School calendar system initialized.");
    // Will update on first WiFi connection
}

void updateSchoolCalendar() {
    unsigned long now = millis();
    
    // Check if we need to update (daily interval or never loaded)
    if (schoolCalendarLoaded && (now - lastSchoolCalendarUpdate < SCHOOL_CALENDAR_UPDATE_INTERVAL)) {
        return;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    Serial.println("[School] Fetching DCPS school calendar JSON...");
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    
    if (String(SCHOOL_CALENDAR_JSON_URL).isEmpty()) {
        Serial.println("[SchoolCalendar] No remote calendar URL defined. Skipping remote fetch.");
        return;
    }
    if (!http.begin(client, SCHOOL_CALENDAR_JSON_URL)) {
        Serial.println("[School] Failed to begin HTTP connection.");
        return;
    }
    
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(6000);
    
    int httpCode = http.GET();
    if (httpCode != 200) {
        Serial.printf("[School] HTTP GET failed, code: %d\r\n", httpCode);
        http.end();
        return;
    }
    
    String payload = http.getString();
    http.end();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload.c_str());
    if (error) {
        Serial.printf("[School] JSON parse failed: %s\r\n", error.c_str());
        return;
    }
    
    JsonArray daysOffArray = doc["days_off"].as<JsonArray>();
    if (daysOffArray.isNull()) {
        Serial.println("[School] No days_off array found in JSON.");
        return;
    }
    
    numSchoolDaysOff = 0;
    
    for (JsonObject dayOff : daysOffArray) {
        if (numSchoolDaysOff >= MAX_SCHOOL_DAYS_OFF) {
            Serial.println("[School] Reached maximum day-off storage limit.");
            break;
        }
        
        const char* dateStr = dayOff["date"];
        const char* nameStr = dayOff["name"];
        if (!dateStr || !nameStr) {
            continue;
        }
        
        // Parse ISO date YYYY-MM-DD
        if (strlen(dateStr) != 10) {
            continue;
        }
        
        int year = String(dateStr).substring(0, 4).toInt();
        int month = String(dateStr).substring(5, 7).toInt();
        int day = String(dateStr).substring(8, 10).toInt();
        
        if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) {
            continue;
        }
        
        schoolDaysOff[numSchoolDaysOff].year = year;
        schoolDaysOff[numSchoolDaysOff].month = month;
        schoolDaysOff[numSchoolDaysOff].day = day;
        schoolDaysOff[numSchoolDaysOff].description = String(nameStr);
        numSchoolDaysOff++;
    }
    
    Serial.printf("[School] Calendar loaded with %d school days off.\r\n", numSchoolDaysOff);
    schoolCalendarLoaded = true;
    lastSchoolCalendarUpdate = now;
}

bool isSchoolDayOff(int year, int month, int day) {
    if (!schoolCalendarLoaded) return false;
    
    for (int i = 0; i < numSchoolDaysOff; i++) {
        if (schoolDaysOff[i].year == year && 
            schoolDaysOff[i].month == month && 
            schoolDaysOff[i].day == day) {
            return true;
        }
    }
    return false;
}

bool isSchoolDay(struct tm* timeinfo) {
    // Check if it's a weekend (Saturday=6, Sunday=0)
    if (timeinfo->tm_wday == 0 || timeinfo->tm_wday == 6) {
        return false;
    }
    
    // Check if it's a school day off from calendar
    int year = timeinfo->tm_year + 1900;
    int month = timeinfo->tm_mon + 1;
    int day = timeinfo->tm_mday;
    
    if (isSchoolDayOff(year, month, day)) {
        return false;
    }
    
    return true;
}

static bool isConsecutiveDay(const SchoolDayOff& a, const SchoolDayOff& b) {
    if (a.year == b.year && a.month == b.month && b.day == a.day + 1) return true;
    if (a.year == b.year && b.month == a.month + 1 && a.day >= 28 && b.day == 1) return true;
    if (b.year == a.year + 1 && a.month == 12 && b.month == 1 && a.day == 31 && b.day == 1) return true;
    return false;
}

String getNextSchoolDayOff(struct tm* currentDate, int* daysUntil) {
    if (daysUntil) *daysUntil = -1;
    int currentYmd = ymdKey(currentDate);
    if (!schoolCalendarLoaded || numSchoolDaysOff == 0) {
        updateSchoolCache(currentYmd, -1, "None");
        return cachedDayOffName;
    }

    if (cachedCurrentYmd == currentYmd && cachedUpdateTime == lastSchoolCalendarUpdate) {
        if (daysUntil) *daysUntil = cachedDaysUntil;
        return cachedDayOffName;
    }

    int currentYear = currentDate->tm_year + 1900;
    int currentMonth = currentDate->tm_mon + 1;
    int currentDay = currentDate->tm_mday;

    // Find the first future day off
    int startIdx = -1;
    for (int i = 0; i < numSchoolDaysOff; i++) {
        SchoolDayOff& dayOff = schoolDaysOff[i];
        if (dayOff.year > currentYear ||
            (dayOff.year == currentYear && dayOff.month > currentMonth) ||
            (dayOff.year == currentYear && dayOff.month == currentMonth && dayOff.day > currentDay)) {
            startIdx = i;
            break;
        }
    }

    if (startIdx == -1) {
        updateSchoolCache(currentYmd, -1, "None");
        return cachedDayOffName;
    }

    // Find run of consecutive days off
    int endIdx = startIdx;
    for (int i = startIdx; i < numSchoolDaysOff - 1; i++) {
        if (isConsecutiveDay(schoolDaysOff[i], schoolDaysOff[i + 1])) {
            endIdx = i + 1;
        } else {
            break;
        }
    }

    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    SchoolDayOff& start = schoolDaysOff[startIdx];
    SchoolDayOff& endDay = schoolDaysOff[endIdx];

    if (daysUntil) *daysUntil = daysUntilDate(start.year, start.month, start.day, currentDate);
    int cacheDays = daysUntil ? *daysUntil : -1;

    if (startIdx == endIdx) {
        // Single day: "Mon 10/12" -> "No Sch: Mon 10/12"
        struct tm tmDate = {};
        tmDate.tm_year = start.year - 1900;
        tmDate.tm_mon = start.month - 1;
        tmDate.tm_mday = start.day;
        tmDate.tm_hour = 12;
        mktime(&tmDate);
        char buf[16];
        snprintf(buf, sizeof(buf), "%s %d/%d", days[tmDate.tm_wday], start.month, start.day);
        updateSchoolCache(currentYmd, cacheDays, String(buf));
    return cachedDayOffName;
    }

    // Consecutive days off range (Guaranteed <= 8 chars)
    char buf[16];
    if (start.month == endDay.month && start.year == endDay.year) {
        // Same month: "10/12-13" -> "No Sch: 10/12-13"
        snprintf(buf, sizeof(buf), "%d/%d-%d", start.month, start.day, endDay.day);
    } else {
        // Cross month: "12/23-1/2" -> "No Sch: 12/23-1/2"
        snprintf(buf, sizeof(buf), "%d/%d-%d/%d", start.month, start.day, endDay.month, endDay.day);
    }
    updateSchoolCache(currentYmd, cacheDays, String(buf));
    return cachedDayOffName;
}

int daysUntilDate(int year, int month, int day, struct tm* currentDate) {
    struct tm now = *currentDate;
    now.tm_hour = 0;
    now.tm_min = 0;
    now.tm_sec = 0;
    now.tm_isdst = -1;
    time_t nowTime = mktime(&now);

    struct tm target = {0};
    target.tm_year = year - 1900;
    target.tm_mon = month - 1;
    target.tm_mday = day;
    target.tm_hour = 0;
    target.tm_min = 0;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    time_t targetTime = mktime(&target);

    return (int)round(difftime(targetTime, nowTime) / 86400.0);
}

String getTodaySchoolDayOffName(struct tm* currentDate) {
    if (!schoolCalendarLoaded) return "";
    int year = currentDate->tm_year + 1900;
    int month = currentDate->tm_mon + 1;
    int day = currentDate->tm_mday;

    for (int i = 0; i < numSchoolDaysOff; i++) {
        if (schoolDaysOff[i].year == year && 
            schoolDaysOff[i].month == month && 
            schoolDaysOff[i].day == day) {
            return schoolDaysOff[i].description;
        }
    }
    return "";
}

void getSchoolDayOffDescription(int index, String& desc, int& month, int& day) {
    if (index >= 0 && index < numSchoolDaysOff) {
        desc = schoolDaysOff[index].description;
        month = schoolDaysOff[index].month;
        day = schoolDaysOff[index].day;
    } else {
        desc = "";
        month = 0;
        day = 0;
    }
}
