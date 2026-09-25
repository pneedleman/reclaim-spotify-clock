#include "sinric_manager.h"
#include "secrets.h"
#include <SinricPro.h>
#include <SinricProDoorbell.h>

static bool sinricInitialized = false;

void setupSinricPro() {
    String appKey    = String(SECRET_SINRICPRO_APP_KEY);
    String appSecret = String(SECRET_SINRICPRO_APP_SECRET);
    String deviceId  = String(SECRET_SINRICPRO_SWITCH_ID);

    if (appKey.length() == 0 || appSecret.length() == 0 || deviceId.length() == 0) {
        Serial.println("[SinricPro] Credentials not configured in secrets.h. SinricPro disabled.");
        return;
    }

    SinricProDoorbell& myDoorbell = SinricPro[deviceId.c_str()];
    SinricPro.begin(appKey.c_str(), appSecret.c_str());
    SinricPro.restoreDeviceStates(false);
    sinricInitialized = true;
    Serial.println("[SinricPro] Doorbell connected & ready for Alexa wake-up triggers!");
}

void handleSinricPro() {
    if (sinricInitialized && WiFi.status() == WL_CONNECTED) {
        SinricPro.handle();
    }
}

void triggerSinricProWakeup() {
    String deviceId = String(SECRET_SINRICPRO_SWITCH_ID);
    if (!sinricInitialized || deviceId.length() == 0) {
        Serial.println("[SinricPro] Skipping wake trigger (SinricPro not configured).");
        return;
    }
    Serial.println("[SinricPro] Sending Alexa Doorbell Press wake-up event...");
    SinricProDoorbell& myDoorbell = SinricPro[deviceId.c_str()];
    myDoorbell.sendDoorbellEvent();
}
