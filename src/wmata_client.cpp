#include "wmata_client.h"
#include "config.h"

String wmataApiKey = WMATA_DEFAULT_API_KEY;
String wmataStopId = WMATA_DEFAULT_STOP_ID;
String wmataRouteFilter = WMATA_DEFAULT_ROUTE;
WmataMode wmataMode = WMATA_MODE_OFF;

String wmataLine1Text = "WMATA Bus Tracker";
String wmataLine2Text = "No data yet";
bool wmataDataValid = false;

SemaphoreHandle_t wmataMutex = NULL;

static unsigned long lastWmataUpdate = 0;
static const unsigned long WMATA_UPDATE_INTERVAL = 60000; // Fetch every 60 seconds

void setupWmata() {
    wmataMutex = xSemaphoreCreateMutex();
    Serial.println("[WMATA] WMATA Bus Client initialized.");
}

void invalidateWmataCache() {
    lastWmataUpdate = 0;
}

String truncateStopName(const String& rawStopName) {
    String s = rawStopName;

    // Replace plus signs (URL-encoded spaces in WMATA stop names) before uppercasing
    s.replace("+", " ");
    s.toUpperCase();

    // Abbreviate common DC street names and suffixes
    s.replace("PENNSYLVANIA", "Penn");
    s.replace("CONNECTICUT", "Conn");
    s.replace("CHEVY CHASE", "Ch Ch Cir");
    s.replace("MASSACHUSETTS", "Mass");
    s.replace("WISCONSIN", "Wisc");
    s.replace("CONSTITUTION", "Const");
    s.replace("INDEPENDENCE", "Indep");
    s.replace("WASHINGTON", "Wash");
    s.replace("TERMINAL", "Term");
    s.replace("STREET", "St");
    s.replace("AVENUE", "Ave");
    s.replace("BOULEVARD", "Blvd");
    s.replace("ROAD", "Rd");
    s.replace("PLACE", "Pl");
    s.replace("DRIVE", "Dr");
    s.replace(" AND ", "&");
    
    // Remove quadrant suffixes to save room (e.g. "St NW" -> "St")
    s.replace(" NW", "");
    s.replace(" NE", "");
    s.replace(" SW", "");
    s.replace(" SE", "");

    // Collapse multiple spaces
    while (s.indexOf("  ") >= 0) s.replace("  ", " ");

    // Trim whitespace
    s.trim();

    // Limit to 16 characters for 16x2 LCD
    if (s.length() > 16) {
        s = s.substring(0, 16);
    }
    return s;
}

bool isWmataVisible() {
    if (wmataMode == WMATA_MODE_OFF) return false;
    if (wmataMode == WMATA_MODE_ALL_DAY) return true;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return false;
    if (timeinfo.tm_wday == 0 || timeinfo.tm_wday == 6) return false;
    return (timeinfo.tm_hour >= WMATA_MORNING_START_HOUR && timeinfo.tm_hour < WMATA_MORNING_END_HOUR);
}

void updateWmataBusPredictions() {
    if (!isWmataVisible()) return;
    if (wmataStopId.isEmpty()) return;

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    unsigned long now = millis();
    if (lastWmataUpdate != 0 && (now - lastWmataUpdate < WMATA_UPDATE_INTERVAL)) {
        return; // Use cached data
    }
    lastWmataUpdate = now;

    HTTPClient http;
    // WMATA API endpoint: NextBusService.svc/json/jPredictions
    String url = "http://api.wmata.com/NextBusService.svc/json/jPredictions?StopID=" + wmataStopId;
    if (!wmataApiKey.isEmpty()) {
        url += "&api_key=" + wmataApiKey;
    }

    http.begin(url);
    http.setReuse(false);
    http.setTimeout(4000); // 4.0s HTTP timeout
    if (!wmataApiKey.isEmpty()) {
        http.addHeader("api_key", wmataApiKey);
    }

    int httpCode = http.GET();
    Serial.printf("[WMATA] GET %s -> HTTP %d\r\n", url.c_str(), httpCode);

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload.c_str());

        if (!err) {
            JsonArray predictions = doc["Predictions"].as<JsonArray>();
            const char* stopNamePtr = doc["StopName"];
            String rawStopName = stopNamePtr ? String(stopNamePtr) : ("Stop #" + wmataStopId);
            String truncatedStop = truncateStopName(rawStopName);

            String firstRoute = "";
            int minutes1 = -1;
            int minutes2 = -1;

            for (JsonObject p : predictions) {
                const char* rIdPtr = p["RouteID"];
                String rId = rIdPtr ? String(rIdPtr) : "";
                int mins = p["Minutes"] | 0;

                // Filter by route if specified
                if (!wmataRouteFilter.isEmpty() && !rId.equalsIgnoreCase(wmataRouteFilter)) {
                    continue;
                }

                if (firstRoute.isEmpty()) {
                    firstRoute = rId;
                }

                if (minutes1 < 0) {
                    minutes1 = mins;
                } else if (minutes2 < 0) {
                    minutes2 = mins;
                    break;
                }
            }

            String tempLine1 = "";
            // Construct Line 1: "D70 @ CHEVY CHASE" fits 16 chars
            if (!firstRoute.isEmpty()) {
                String prefix = firstRoute + " @ ";
                int remaining = 16 - prefix.length();
                String stopPart = (remaining > 0 && (int)truncatedStop.length() > remaining)
                    ? truncatedStop.substring(0, remaining)
                    : truncatedStop;
                tempLine1 = prefix + stopPart;
            } else {
                String l1 = "Bus @ " + truncatedStop;
                if (l1.length() > 16) l1 = l1.substring(0, 16);
                tempLine1 = l1;
            }

            // Construct Line 2
            String tempLine2 = "";
            if (minutes1 >= 0 && minutes2 >= 0) {
                tempLine2 = "Next: " + String(minutes1) + "m, " + String(minutes2) + "m";
            } else if (minutes1 >= 0) {
                tempLine2 = "Next: " + String(minutes1) + "m";
            } else {
                tempLine2 = "No Bus Arriving";
            }

            while (tempLine2.length() < 16) tempLine2 += " ";

            if (wmataMutex == NULL) {
                wmataMutex = xSemaphoreCreateMutex();
            }
            if (wmataMutex && xSemaphoreTake(wmataMutex, portMAX_DELAY) == pdTRUE) {
                wmataLine1Text = tempLine1;
                wmataLine2Text = tempLine2;
                wmataDataValid = true;
                xSemaphoreGive(wmataMutex);
            } else {
                wmataLine1Text = tempLine1;
                wmataLine2Text = tempLine2;
                wmataDataValid = true;
            }

            Serial.printf("[WMATA] Bus Update Success: Line1='%s', Line2='%s'\r\n", 
                          tempLine1.c_str(), tempLine2.c_str());
        } else {
            Serial.printf("[WMATA] JSON Parse Error: %s\r\n", err.c_str());
        }
    } else {
        Serial.printf("[WMATA] HTTP GET Failed, code %d\r\n", httpCode);
    }
    http.end();
}

String getWmataLine1() {
    String val = "";
    if (wmataMutex == NULL) {
        return wmataLine1Text;
    }
    if (xSemaphoreTake(wmataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        val = wmataLine1Text;
        xSemaphoreGive(wmataMutex);
    }
    return val;
}

String getWmataLine2() {
    String val = "";
    if (wmataMutex == NULL) {
        return wmataLine2Text;
    }
    if (xSemaphoreTake(wmataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        val = wmataLine2Text;
        xSemaphoreGive(wmataMutex);
    }
    return val;
}
