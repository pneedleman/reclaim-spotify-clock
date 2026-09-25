#include "weather_client.h"
#include "config.h"

// Local coordinates cache & Sun times
double localLatitude = 0.0;
double localLongitude = 0.0;
bool geoLocated = false;
String localZipCode = "20015";
unsigned long lastWeatherCheck = 0;
unsigned long lastGeoAttempt = 0;
const unsigned long WEATHER_CHECK_INTERVAL = 30 * 60 * 1000; // 30 minutes in ms

int sunriseHour = 7;
int sunriseMin = 0;
int sunsetHour = 20;
int sunsetMin = 0;
bool sunTimesFetched = false;

SemaphoreHandle_t weatherMutex = NULL;

void setupWeather() {
    weatherMutex = xSemaphoreCreateMutex();
    lastWeatherCheck = 0;
    lastGeoAttempt = 0;
    weatherTemp = 0;
    weatherLabel = "";
    weatherFeelsLike = 0;
    weatherPrecipitationHours = 0.0f;
    weatherPrecipitationChance = 0;
    weatherHighTemp = 0;
    weatherUVIndex = 0;
    weatherUVLabel = "";
    weatherCodeDaily = 0;
    weatherTomorrowHighTemp = 0;
    weatherTomorrowFeelsLike = 0;
    weatherTomorrowUVIndex = 0;
    weatherTomorrowUVLabel = "Low";
    weatherTomorrowPrecipChance = 0;
    weatherTomorrowCodeDaily = 0;
    weatherFetched = false;
}

void triggerImmediateWeatherSync() {
    lastWeatherCheck = 0;
}

String getWeatherLabel(int code, bool isDay) {
    switch (code) {
        case 0: 
            return isDay ? "Sunny" : "Clear";
        case 1:
        case 2: 
            return "Pt. Cloudy";
        case 3:
        case 45:
        case 48: 
            return "Cloudy";
        case 51: case 53: case 55:
        case 61: case 63: case 65:
        case 80: case 81: case 82: 
            return "Rainy";
        case 71: case 73: case 75:
        case 77:
        case 85: case 86: 
            return "Snowy";
        case 95:
        case 96:
        case 99: 
            return "Stormy";
        default: 
            return "----";
    }
}

static String abbreviateAlertTitle(const String& event) {
    if (event.indexOf("Severe Thunderstorm") >= 0) {
        if (event.indexOf("Watch") >= 0) return "!Svr T-Stm Watch";
        return "!Svr T-Stm Warn";
    }
    if (event.indexOf("Thunderstorm") >= 0) {
        if (event.indexOf("Watch") >= 0) return "!T-Storm Watch";
        return "!T-Storm Warning";
    }
    if (event.indexOf("Tornado") >= 0) {
        if (event.indexOf("Watch") >= 0) return "!Tornado Watch";
        return "!Tornado Warning";
    }
    if (event.indexOf("Flash Flood") >= 0) {
        if (event.indexOf("Watch") >= 0) return "!Flash Flood Wt";
        return "!Flash Flood Wrn";
    }
    if (event.indexOf("Flood") >= 0) {
        if (event.indexOf("Watch") >= 0) return "!Flood Watch";
        return "!Flood Warning";
    }
    if (event.indexOf("Winter Storm") >= 0) return "!SnowStorm Warn";
    if (event.indexOf("Blizzard") >= 0) return "!Blizzard Warn";
    if (event.indexOf("Wind") >= 0) return "!High Wind Warn";
    if (event.indexOf("Excessive Heat") >= 0) return "!Excess Ht Warn";
    if (event.indexOf("Heat") >= 0) return "!Heat Advisory";
    if (event.indexOf("Freeze") >= 0) return "!Freeze Warning";
    if (event.indexOf("Frost") >= 0) return "!Frost Advisory";
    return "!" + event.substring(0, 15);
}

static void applyWeatherSnapshot(const WeatherSnapshot& s);

static int getAlertPriority(const String& event) {
    if (event.indexOf("Tornado") >= 0) return 100;
    if (event.indexOf("Severe Thunderstorm") >= 0) return 90;
    if (event.indexOf("Thunderstorm") >= 0) return 80;
    if (event.indexOf("Flash Flood") >= 0) return 75;
    if (event.indexOf("Flood") >= 0) return 70;
    if (event.indexOf("Winter Storm") >= 0 || event.indexOf("Blizzard") >= 0) return 60;
    if (event.indexOf("Wind") >= 0) return 50;
    if (event.indexOf("Excessive Heat") >= 0 || event.indexOf("Heat") >= 0) return 40;
    if (event.indexOf("Freeze") >= 0 || event.indexOf("Frost") >= 0) return 30;
    return 10;
}

static void updateWeatherAlerts(WeatherSnapshot& s) {
    s.alertActive = false;
    s.alertTitle = "";

    if (!geoLocated || WiFi.status() != WL_CONNECTED) {
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(6000);
    HTTPClient http;
    http.setTimeout(6000);
    String url = "https://api.weather.gov/alerts/active?point=" + String(localLatitude, 4) + "," + String(localLongitude, 4);
    
    if (!http.begin(client, url)) {
        Serial.println("[Weather] NWS alerts http.begin failed");
        return;
    }
    
    http.useHTTP10(true); // Disable chunked transfer encoding for rock-solid stream parsing on real hardware
    http.addHeader("User-Agent", "(SpotifyClock, admin@needleputt.local)");
    http.addHeader("Accept", "application/geo+json");
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[Weather] NWS alerts query failed: %d\r\n", httpCode);
        http.end();
        return;
    }
    
    // Stream directly through ArduinoJson filter to drop 21KB of map polygons without heap allocation
    JsonDocument filter;
    filter["features"][0]["properties"]["status"] = true;
    filter["features"][0]["properties"]["event"] = true;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (error) {
        Serial.printf("[Weather] NWS alerts JSON parse error: %s\r\n", error.c_str());
        return;
    }
    
    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull() || features.size() == 0) {
        Serial.println("[Weather] No active NWS weather alerts.");
        return;
    }
    
    // Pick the highest priority active alert (e.g. Severe T-Storm over Flood Watch)
    String bestAlert = "";
    int bestPriority = 0;

    for (JsonObject feature : features) {
        JsonObject props = feature["properties"];
        if (props.isNull()) continue;
        
        const char* status = props["status"];
        if (status && strcmp(status, "Actual") != 0) continue;
        
        const char* event = props["event"];
        if (event) {
            String eventStr = String(event);
            int prio = getAlertPriority(eventStr);
            if (prio > bestPriority) {
                bestPriority = prio;
                bestAlert = eventStr;
            }
        }
    }

    if (!bestAlert.isEmpty()) {
        s.alertTitle = abbreviateAlertTitle(bestAlert);
        s.alertActive = true;
        Serial.printf("[Weather] Active NWS alert prioritized: %s -> '%s' (prio=%d)\r\n", bestAlert.c_str(), s.alertTitle.c_str(), bestPriority);
    }
}

void updateWeather() {
    WeatherSnapshot s;
    s.enabled = weatherEnabled;
    if (!weatherEnabled) {
        s.fetched = false;
        s.alertActive = false;
        applyWeatherSnapshot(s);
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    unsigned long now = millis();

    // 1. Run IP-based Geolocation if we don't have coordinates yet
    if (!geoLocated && (now - lastGeoAttempt >= WEATHER_CHECK_INTERVAL || lastGeoAttempt == 0)) {
        lastGeoAttempt = now;
        HTTPClient http;
        http.begin("http://ip-api.com/json");
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, http.getStream());
            if (!error) {
                    localLatitude = doc["lat"] | 0.0;
                    localLongitude = doc["lon"] | 0.0;
                    if (!doc["zip"].isNull()) {
                        localZipCode = doc["zip"].as<String>();
                    }
                    if (localLatitude != 0.0 || localLongitude != 0.0) {
                        geoLocated = true;
                        Serial.printf("[Weather] IP-Geolocated successfully: Lat=%.4f, Lon=%.4f, Zip=%s\r\n", localLatitude, localLongitude, localZipCode.c_str());
                    } else {
                        Serial.println("[Weather] Geolocation returned 0,0. Using default DC fallback.");
                        localLatitude = DEFAULT_LATITUDE;
                        localLongitude = DEFAULT_LONGITUDE;
                        geoLocated = true;
                    }
            } else {
                Serial.printf("[Weather] Geolocation JSON parse error: %s\r\n", error.c_str());
            }
        } else {
            Serial.printf("[Weather] Geolocation HTTP query failed: %d. Using default DC fallback.\r\n", httpCode);
            localLatitude = DEFAULT_LATITUDE;
            localLongitude = DEFAULT_LONGITUDE;
            geoLocated = true;
        }
        http.end();
    }

    // 2. Fetch Weather if Geolocated and interval elapsed
    if (geoLocated && (now - lastWeatherCheck >= WEATHER_CHECK_INTERVAL || lastWeatherCheck == 0)) {
        lastWeatherCheck = now;  // Set immediately to prevent rapid 1-second retry storms on failure
        HTTPClient http;
        String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(localLatitude, 4) +
                     "&longitude=" + String(localLongitude, 4) +
                     "&current_weather=true&temperature_unit=fahrenheit&daily=precipitation_probability_max,temperature_2m_max,apparent_temperature_max,weather_code,sunrise,sunset&hourly=precipitation,precipitation_probability&timezone=auto&forecast_days=2&models=ncep_nbm_conus";
        
        http.begin(url);
        http.useHTTP10(true); // Avoid chunked transfer encoding so ArduinoJson can stream correctly
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            JsonDocument doc;
            JsonDocument filter;
            filter["current_weather"]["temperature"] = true;
            filter["current_weather"]["weathercode"] = true;
            filter["current_weather"]["is_day"] = true;
            filter["current_weather"]["time"] = true;
            filter["daily"]["precipitation_probability_max"] = true;
            filter["daily"]["temperature_2m_max"] = true;
            filter["daily"]["apparent_temperature_max"] = true;
            filter["daily"]["uv_index_max"] = true;
            filter["daily"]["weather_code"] = true;
            filter["daily"]["sunrise"] = true;
            filter["daily"]["sunset"] = true;
            filter["hourly"]["precipitation"] = true;
            filter["hourly"]["precipitation_probability"] = true;
            DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
            if (!error) {
                float temp = doc["current_weather"]["temperature"] | 0.0f;
                int code = doc["current_weather"]["weathercode"] | 0;
                int isDay = doc["current_weather"]["is_day"] | 1;
                
                String label = getWeatherLabel(code, isDay == 1);
                s.temp = (int)round(temp);
                s.label = label;
                
                if (doc["daily"]["precipitation_probability_max"].is<JsonArray>()) {
                    JsonArray probArr = doc["daily"]["precipitation_probability_max"].as<JsonArray>();
                    s.precipitationChance = probArr[0] | 0;
                    if (probArr.size() > 1) {
                        s.tomorrowPrecipChance = probArr[1] | 0;
                    } else {
                        s.tomorrowPrecipChance = s.precipitationChance;
                    }
                } else {
                    s.precipitationChance = 0;
                    s.tomorrowPrecipChance = 0;
                }

                // 1. Today's remaining precipitation (from curHour up to 6 AM tomorrow morning)
                float todayRainHours = 0.0f;
                int todayPeakChance = 0;
                int todayStartHour = -1;

                // 2. Tomorrow's full-day precipitation (hours 24 to 47)
                float tomorrowRainHours = 0.0f;
                int tomorrowPeakChance = 0;
                int tomorrowStartHour = -1;

                if (doc["hourly"]["precipitation"].is<JsonArray>()) {
                    JsonArray precArr = doc["hourly"]["precipitation"].as<JsonArray>();
                    JsonArray probArr = doc["hourly"]["precipitation_probability"].as<JsonArray>();
                    
                    int curHour = 0;
                    if (doc["current_weather"]["time"].is<const char*>()) {
                        String curTimeStr = doc["current_weather"]["time"].as<String>();
                        int tPos = curTimeStr.indexOf('T');
                        if (tPos >= 0 && curTimeStr.length() >= tPos + 3) {
                            curHour = curTimeStr.substring(tPos + 1, tPos + 3).toInt();
                        }
                    }
                    if (curHour < 0 || curHour > 23) curHour = 0;
                    
                    // Today + overnight window: curHour up to hour 30 (6 AM tomorrow)
                    size_t todayEnd = curHour + (24 - curHour) + 6;
                    if (todayEnd > precArr.size()) todayEnd = precArr.size();
                    if (todayEnd > 30) todayEnd = 30;

                    for (size_t h = curHour; h < todayEnd; h++) {
                        float p = precArr[h] | 0.0f;
                        int prob = probArr[h] | 0;
                        if (p >= 0.1f || prob >= 35) {
                            todayRainHours += 1.0f;
                            if (todayStartHour < 0) {
                                todayStartHour = (int)(h % 24);
                            }
                        }
                        if (prob > todayPeakChance) {
                            todayPeakChance = prob;
                        }
                    }

                    // Tomorrow's full 24-hour day: from index 24 to 47
                    for (size_t h = 24; h < 48 && h < precArr.size(); h++) {
                        float p = precArr[h] | 0.0f;
                        int prob = probArr[h] | 0;
                        if (p >= 0.1f || prob >= 35) {
                            tomorrowRainHours += 1.0f;
                            if (tomorrowStartHour < 0) {
                                tomorrowStartHour = (int)(h - 24);
                            }
                        }
                        if (prob > tomorrowPeakChance) {
                            tomorrowPeakChance = prob;
                        }
                    }

                    s.precipitationHours = todayRainHours;
                    s.rainStartHour = todayStartHour;
                    s.precipitationChance = todayPeakChance;

                    s.tomorrowPrecipitationHours = tomorrowRainHours;
                    s.tomorrowRainStartHour = tomorrowStartHour;
                    s.tomorrowPrecipChance = tomorrowPeakChance;
                } else {
                    s.precipitationHours = 0.0f;
                    s.rainStartHour = -1;
                    s.precipitationChance = 0;
                    s.tomorrowPrecipitationHours = 0.0f;
                    s.tomorrowRainStartHour = -1;
                    s.tomorrowPrecipChance = 0;
                }
                
                if (doc["daily"]["temperature_2m_max"].is<JsonArray>()) {
                    JsonArray tArr = doc["daily"]["temperature_2m_max"].as<JsonArray>();
                    s.highTemp = (int)round(tArr[0] | 0.0f);
                    if (tArr.size() > 1) {
                        s.tomorrowHighTemp = (int)round(tArr[1] | 0.0f);
                    } else {
                        s.tomorrowHighTemp = s.highTemp;
                    }
                } else {
                    s.highTemp = 0;
                    s.tomorrowHighTemp = 0;
                }

                if (doc["daily"]["apparent_temperature_max"].is<JsonArray>()) {
                    JsonArray fArr = doc["daily"]["apparent_temperature_max"].as<JsonArray>();
                    s.feelsLike = (int)round(fArr[0] | 0.0f);
                    if (fArr.size() > 1) {
                        s.tomorrowFeelsLike = (int)round(fArr[1] | 0.0f);
                    } else {
                        s.tomorrowFeelsLike = s.feelsLike;
                    }
                } else {
                    s.feelsLike = s.highTemp;
                    s.tomorrowFeelsLike = s.tomorrowHighTemp;
                }

                if (doc["daily"]["uv_index_max"].is<JsonArray>()) {
                    JsonArray uvArr = doc["daily"]["uv_index_max"].as<JsonArray>();
                    if (uvArr.size() > 1) {
                        float tmrwUv = uvArr[1] | 0.0f;
                        s.tomorrowUVIndex = (int)round(tmrwUv);
                        if (s.tomorrowUVIndex <= 2) s.tomorrowUVLabel = "Low";
                        else if (s.tomorrowUVIndex <= 5) s.tomorrowUVLabel = "Mod";
                        else if (s.tomorrowUVIndex <= 7) s.tomorrowUVLabel = "High";
                        else if (s.tomorrowUVIndex <= 10) s.tomorrowUVLabel = "V.Hi";
                        else s.tomorrowUVLabel = "Extr";
                    }
                }

                if (doc["daily"]["weather_code"].is<JsonArray>()) {
                    JsonArray cArr = doc["daily"]["weather_code"].as<JsonArray>();
                    s.codeDaily = cArr[0] | 0;
                    if (cArr.size() > 1) {
                        s.tomorrowCodeDaily = cArr[1] | 0;
                    } else {
                        s.tomorrowCodeDaily = s.codeDaily;
                    }
                } else {
                    s.codeDaily = 0;
                    s.tomorrowCodeDaily = 0;
                }
                
                // Parse sunrise and sunset ISO strings (e.g. "2026-07-23T06:05")
                if (doc["daily"]["sunrise"].is<JsonArray>() && doc["daily"]["sunrise"][0].is<const char*>()) {
                    String srStr = doc["daily"]["sunrise"][0].as<String>();
                    int tPos = srStr.indexOf('T');
                    if (tPos >= 0 && srStr.length() >= tPos + 6) {
                        s.sunriseHour = srStr.substring(tPos + 1, tPos + 3).toInt();
                        s.sunriseMin = srStr.substring(tPos + 4, tPos + 6).toInt();
                    }
                }
                if (doc["daily"]["sunset"].is<JsonArray>() && doc["daily"]["sunset"][0].is<const char*>()) {
                    String ssStr = doc["daily"]["sunset"][0].as<String>();
                    int tPos = ssStr.indexOf('T');
                    if (tPos >= 0 && ssStr.length() >= tPos + 6) {
                        s.sunsetHour = ssStr.substring(tPos + 1, tPos + 3).toInt();
                        s.sunsetMin = ssStr.substring(tPos + 4, tPos + 6).toInt();
                    }
                }
                sunTimesFetched = true;
                
                // Weather snapshot will be finalized after EPA UV and NWS alerts below

                // 3. Fetch Official EPA Daily UV Index in an isolated scope
                if (localZipCode.length() > 0 && ESP.getFreeHeap() >= 45000) {
                    WiFiClientSecure epaClient;
                    epaClient.setInsecure();
                    epaClient.setTimeout(3000);
                    HTTPClient epaHttp;
                    epaHttp.begin(epaClient, "https://data.epa.gov/efservice/getEnvirofactsUVDAILY/ZIP/" + localZipCode + "/JSON");
                    epaHttp.setTimeout(3000);
                    int epaCode = epaHttp.GET();
                    if (epaCode == HTTP_CODE_OK) {
                        String epaPayload = epaHttp.getString();
                        JsonDocument epaDoc;
                        DeserializationError epaErr = deserializeJson(epaDoc, epaPayload.c_str());
                        if (!epaErr && epaDoc.is<JsonArray>() && epaDoc.size() > 0) {
                            String uvStr = epaDoc[0]["UV_INDEX"].as<String>();
                            int uvVal = uvStr.toInt();
                            s.uvIndex = uvVal;
                            s.tomorrowUVIndex = uvVal;
                            if (uvVal <= 2) { s.uvLabel = "Low"; s.tomorrowUVLabel = "Low"; }
                            else if (uvVal <= 5) { s.uvLabel = "Mod"; s.tomorrowUVLabel = "Mod"; }
                            else if (uvVal <= 7) { s.uvLabel = "High"; s.tomorrowUVLabel = "High"; }
                            else if (uvVal <= 10) { s.uvLabel = "Very Hi"; s.tomorrowUVLabel = "V.Hi"; }
                            else { s.uvLabel = "Extreme"; s.tomorrowUVLabel = "Extr"; }
                            Serial.printf("[Weather] EPA Daily UV: %d (%s) for ZIP: %s\r\n", s.uvIndex, s.uvLabel.c_str(), localZipCode.c_str());
                        }
                    } else {
                        Serial.printf("[Weather] EPA Daily UV HTTP failed: %d\r\n", epaCode);
                    }
                    epaHttp.end();
                } // epaClient destroyed and ~40KB TLS heap freed here

                vTaskDelay(pdMS_TO_TICKS(150)); // Yield to allow TCP/MbedTLS buffers to fully release

                // 4. Fetch NWS severe weather alerts (US only) with heap check
                if (ESP.getFreeHeap() >= 45000) {
                    updateWeatherAlerts(s);
                }
                s.fetched = true;
                Serial.printf("[Weather] Sync success: Temp=%dF, High=%dF, Feels=%dF, Label=%s, Rain=%.1fh (%d%%), UV=%d\r\n",
                              s.temp, s.highTemp, s.feelsLike, s.label.c_str(), s.precipitationHours, s.precipitationChance, s.uvIndex);
                applyWeatherSnapshot(s);
            } else {
                Serial.printf("[Weather] Weather JSON parse error: %s\r\n", error.c_str());
            }
        } else {
            Serial.printf("[Weather] Weather HTTP query failed: %d\r\n", httpCode);
        }
        lastWeatherCheck = now;
        http.end();
    }
}

static void applyWeatherSnapshot(const WeatherSnapshot& s) {
    if (weatherMutex == NULL) return;
    if (xSemaphoreTake(weatherMutex, portMAX_DELAY) == pdTRUE) {
        weatherEnabled = s.enabled;
        weatherFetched = s.fetched;
        weatherTemp = s.temp;
        weatherLabel = s.label;
        weatherFeelsLike = s.feelsLike;
        weatherPrecipitationHours = s.precipitationHours;
        weatherRainStartHour = s.rainStartHour;
        weatherPrecipitationChance = s.precipitationChance;
        weatherHighTemp = s.highTemp;
        weatherUVIndex = s.uvIndex;
        weatherUVLabel = s.uvLabel;
        weatherCodeDaily = s.codeDaily;
        weatherTomorrowHighTemp = s.tomorrowHighTemp;
        weatherTomorrowFeelsLike = s.tomorrowFeelsLike;
        weatherTomorrowUVIndex = s.tomorrowUVIndex;
        weatherTomorrowUVLabel = s.tomorrowUVLabel;
        weatherTomorrowPrecipitationHours = s.tomorrowPrecipitationHours;
        weatherTomorrowRainStartHour = s.tomorrowRainStartHour;
        weatherTomorrowPrecipChance = s.tomorrowPrecipChance;
        weatherTomorrowCodeDaily = s.tomorrowCodeDaily;
        weatherAlertActive = s.alertActive;
        weatherAlertTitle = s.alertTitle;
        sunriseHour = s.sunriseHour;
        sunriseMin = s.sunriseMin;
        sunsetHour = s.sunsetHour;
        sunsetMin = s.sunsetMin;
        xSemaphoreGive(weatherMutex);
    }
}

WeatherSnapshot getWeatherSnapshot() {
    WeatherSnapshot s;
    if (weatherMutex == NULL) {
        s.enabled = weatherEnabled;
        s.fetched = weatherFetched;
        s.temp = weatherTemp;
        s.label = weatherLabel;
        s.feelsLike = weatherFeelsLike;
        s.precipitationHours = weatherPrecipitationHours;
        s.rainStartHour = weatherRainStartHour;
        s.precipitationChance = weatherPrecipitationChance;
        s.highTemp = weatherHighTemp;
        s.uvIndex = weatherUVIndex;
        s.uvLabel = weatherUVLabel;
        s.codeDaily = weatherCodeDaily;
        s.tomorrowHighTemp = weatherTomorrowHighTemp;
        s.tomorrowFeelsLike = weatherTomorrowFeelsLike;
        s.tomorrowUVIndex = weatherTomorrowUVIndex;
        s.tomorrowUVLabel = weatherTomorrowUVLabel;
        s.tomorrowPrecipitationHours = weatherTomorrowPrecipitationHours;
        s.tomorrowRainStartHour = weatherTomorrowRainStartHour;
        s.tomorrowPrecipChance = weatherTomorrowPrecipChance;
        s.tomorrowCodeDaily = weatherTomorrowCodeDaily;
        s.alertActive = weatherAlertActive;
        s.alertTitle = weatherAlertTitle;
        s.sunriseHour = sunriseHour;
        s.sunriseMin = sunriseMin;
        s.sunsetHour = sunsetHour;
        s.sunsetMin = sunsetMin;
        return s;
    }
    if (xSemaphoreTake(weatherMutex, portMAX_DELAY) == pdTRUE) {
        s.enabled = weatherEnabled;
        s.fetched = weatherFetched;
        s.temp = weatherTemp;
        s.label = weatherLabel;
        s.feelsLike = weatherFeelsLike;
        s.precipitationHours = weatherPrecipitationHours;
        s.rainStartHour = weatherRainStartHour;
        s.precipitationChance = weatherPrecipitationChance;
        s.highTemp = weatherHighTemp;
        s.uvIndex = weatherUVIndex;
        s.uvLabel = weatherUVLabel;
        s.codeDaily = weatherCodeDaily;
        s.tomorrowHighTemp = weatherTomorrowHighTemp;
        s.tomorrowFeelsLike = weatherTomorrowFeelsLike;
        s.tomorrowUVIndex = weatherTomorrowUVIndex;
        s.tomorrowUVLabel = weatherTomorrowUVLabel;
        s.tomorrowPrecipitationHours = weatherTomorrowPrecipitationHours;
        s.tomorrowRainStartHour = weatherTomorrowRainStartHour;
        s.tomorrowPrecipChance = weatherTomorrowPrecipChance;
        s.tomorrowCodeDaily = weatherTomorrowCodeDaily;
        s.alertActive = weatherAlertActive;
        s.alertTitle = weatherAlertTitle;
        s.sunriseHour = sunriseHour;
        s.sunriseMin = sunriseMin;
        s.sunsetHour = sunsetHour;
        s.sunsetMin = sunsetMin;
        xSemaphoreGive(weatherMutex);
    }
    return s;
}
