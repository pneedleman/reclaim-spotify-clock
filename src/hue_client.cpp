#include "hue_client.h"
#include "display_manager.h"
#include "settings_menu.h"

// Define Philips Hue Globals
bool hueLightsEnabled = false;
int hueBrightness = 100;
bool hueSunriseEnabled = true;
int hueSunriseDuration = 30;
String hueBridgeIP = HUE_BRIDGE_IP;
String hueUsername = HUE_BRIDGE_USERNAME;

// Define Philips Hue Scene Globals
HueScene hueScenes[10];
int numHueScenes = 0;
int selectedHueSceneIndex = 0;
String activeHueSceneID = "";
String activeHueSceneName = "None";
String activeHueGroupID = HUE_GROUP_ID; // Pre-initialized to configured Group ID

// Define Philips Hue Edit Buffers
bool editHueLightsEnabled = false;
int editHueBrightness = 100;
bool editHueSunriseEnabled = true;
int editHueSunriseDuration = 30;

void updateHueLightState() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        Serial.println("[Hue] Error: IP or Username not set.");
        return;
    }
    
    if (hueLightsEnabled && activeHueSceneID != "") {
        recallHueScene(activeHueSceneID);
        return;
    }
    
    WiFiClient client;
    client.setTimeout(400);
    HTTPClient http;
    http.setTimeout(400);
    String url = "http://" + hueBridgeIP + "/api/" + hueUsername + "/groups/" + String(HUE_GROUP_ID) + "/action";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    
    String payload;
    if (hueLightsEnabled) {
        int bri = (hueBrightness * 254) / 100;
        if (bri < 1) bri = 1;
        payload = "{\"on\":true,\"bri\":" + String(bri) + "}";
    } else {
        payload = "{\"on\":false}";
    }
    
    int httpResponseCode = http.PUT(payload);
    if (httpResponseCode > 0) {
        Serial.printf("[Hue] State updated, HTTP response: %d\r\n", httpResponseCode);
    } else {
        Serial.printf("[Hue] Error updating state: %s\r\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

void fetchHueLightState() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        Serial.println("[Hue] Error: IP or Username not set.");
        return;
    }

    WiFiClient client;
    client.setTimeout(400);
    HTTPClient http;
    http.setTimeout(400);
    String url = "http://" + hueBridgeIP + "/api/" + hueUsername + "/groups/" + String(HUE_GROUP_ID);
    http.begin(client, url);
    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (!error) {
            bool isOn = doc["action"]["on"] | false;
            int bri = doc["action"]["bri"] | 254;
            hueLightsEnabled = isOn;
            hueBrightness = (bri * 100) / 254;
            if (hueBrightness < 0) hueBrightness = 0;
            if (hueBrightness > 100) hueBrightness = 100;
            Serial.printf("[Hue] Polled state: ON=%s, Bri=%d%%\r\n", isOn ? "true" : "false", hueBrightness);
        }
    }
    http.end();
}

void triggerHueSunriseTransition() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        Serial.println("[Hue] Error: IP or Username not set.");
        return;
    }
    
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + hueBridgeIP + "/api/" + hueUsername + "/groups/" + String(HUE_GROUP_ID) + "/action";
    
    // Step 1: Set to brightness 1 and warmest color temperature (500 mireds / 2000K) instantly
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    String step1 = "{\"on\":true,\"bri\":1,\"ct\":500,\"transitiontime\":0}";
    http.PUT(step1);
    http.end();
    
    delay(50); // Minimal yield for bridge to register step 1 without blocking audio loop
    
    // Step 2: Transition slowly to max brightness (254) and cool daylight white (200 mireds / 5000K)
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int transitionUnits = hueSunriseDuration * 60 * 10;
    String step2 = "{\"bri\":254,\"ct\":200,\"transitiontime\":" + String(transitionUnits) + "}";
    http.PUT(step2);
    http.end();
    
    Serial.printf("[Hue] Sunrise transition triggered: %d minutes (ct 500 -> 200).\r\n", hueSunriseDuration);
}

static void triggerHueSunriseTask(void* pvParameters) {
    triggerHueSunriseTransition();
    vTaskDelete(NULL);
}

void triggerHueSunriseTransitionAsync() {
    xTaskCreate(
        triggerHueSunriseTask,
        "HueSunriseTask",
        4096,
        NULL,
        1,
        NULL
    );
}

void turnOffHueLight() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        return;
    }
    
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + hueBridgeIP + "/api/" + hueUsername + "/groups/" + String(HUE_GROUP_ID) + "/action";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.PUT("{\"on\":false}");
    http.end();
    Serial.println("[Hue] Light turned off.");
}

void fetchHueScenes() {
    numHueScenes = 0;
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        Serial.println("[Hue] Error: IP or Username not set.");
        return;
    }

    activeHueGroupID = HUE_GROUP_ID;

    WiFiClient client;
    client.setTimeout(3000);
    HTTPClient http;
    http.setTimeout(3000);

    // Fetch Scenes and filter by activeHueGroupID (HUE_GROUP_ID)
    String scenesUrl = "http://" + hueBridgeIP + "/api/" + hueUsername + "/scenes";
    http.begin(client, scenesUrl);
    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
        JsonDocument filter;
        filter["*"]["name"] = true;
        filter["*"]["group"] = true;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        if (!error && doc.is<JsonObject>()) {
            JsonObject root = doc.as<JsonObject>();
            for (JsonPair p : root) {
                JsonObject scene = p.value().as<JsonObject>();
                bool match = false;
                
                String groupVal = scene["group"].as<String>();
                if (groupVal == String(HUE_GROUP_ID) || groupVal == "1") {
                    match = true;
                }

                if (match) {
                    hueScenes[numHueScenes].id = p.key().c_str();
                    hueScenes[numHueScenes].name = scene["name"].as<String>();
                    Serial.printf("[Hue] Found matching Scene ID: %s Name: %s\r\n", hueScenes[numHueScenes].id.c_str(), hueScenes[numHueScenes].name.c_str());
                    numHueScenes++;
                    if (numHueScenes >= 10) break; // limit to 10
                }
            }
        } else if (error) {
            Serial.printf("[Hue] Deserialization error: %s\r\n", error.c_str());
        }
    }
    http.end();
}

void recallHueScene(String sceneId) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        Serial.println("[Hue] Error: IP or Username not set for recalling scene.");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    String url = "http://" + hueBridgeIP + "/api/" + hueUsername + "/groups/" + String(HUE_GROUP_ID) + "/action";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"scene\":\"" + sceneId + "\"}";
    int httpResponseCode = http.PUT(payload);
    if (httpResponseCode > 0) {
        Serial.printf("[Hue] Scene recalled: %s, HTTP response: %d\r\n", sceneId.c_str(), httpResponseCode);
    } else {
        Serial.printf("[Hue] Error recalling scene: %s\r\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

void startHueSleepTimerFade(int durationMinutes) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (hueBridgeIP == "" || hueUsername == "") {
        Serial.println("[Hue] Error: IP or Username not set for sleep fade.");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    String url = "http://" + hueBridgeIP + "/api/" + hueUsername + "/groups/" + String(HUE_GROUP_ID) + "/action";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    int transitionUnits = durationMinutes * 60 * 10;
    // Fades to deep lavender (hue 50000, sat 200) and 15% brightness (bri 38) over transition time
    String payload = "{\"on\":true,\"hue\":50000,\"sat\":200,\"bri\":38,\"transitiontime\":" + String(transitionUnits) + "}";
    int httpResponseCode = http.PUT(payload);
    if (httpResponseCode > 0) {
        Serial.printf("[Hue] Sleep timer fade triggered: lavender (15%%) over %d mins.\r\n", durationMinutes);
    } else {
        Serial.printf("[Hue] Error starting sleep fade: %s\r\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

