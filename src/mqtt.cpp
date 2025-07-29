#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "mqtt.h"
#include "types.h"
#include "version.h"

// MQTT-Config
const char *mqtt_server = "";
const uint16_t mqtt_port = 0;
const char *mqtt_user = "";
const char *mqtt_pass = "";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

String mqttIdentifier = "coffeegrinder_" + String((uint32_t)ESP.getEfuseMac(), HEX);

void publishConfigsForHA();

// Externe Variablen aus deinem Code
extern float weight;
extern uint16_t presetSmall;
extern uint16_t presetLarge;
extern float scaleFactor;
extern float blockThreshold;
extern volatile State state;

extern unsigned long presetSmallRuns;
extern unsigned long presetLargeRuns;
extern float totalWeight;

extern PresetSelection selectedPreset;

extern String stateToString(State s);

extern Preferences prefs;

extern void savePreferences();
extern void setRemainingTime();
extern void setPreset(PresetSelection selection);

void callback(char *topic, byte *payload, unsigned int length)
{
    String message;
    for (unsigned int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/preset_left/set")
    {
        presetSmall = message.toFloat() * 10;
        savePreferences();
        setRemainingTime();
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/preset_right/set")
    {
        presetLarge = message.toFloat() * 10;
        savePreferences();
        setRemainingTime();
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/block_threshold/set")
    {
        blockThreshold = message.toFloat();
        savePreferences();
        setRemainingTime();
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/start")
    {
        extern bool webStart;
        extern void startGrinding(bool);
        webStart = true;
        startGrinding(true);
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/start_right")
    {
        extern bool webStart;
        extern void startGrinding(bool);
        setPreset(LARGE);
        webStart = true;
        startGrinding(true);
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/start_left")
    {
        extern bool webStart;
        extern void startGrinding(bool);
        setPreset(SMALL);
        webStart = true;
        startGrinding(true);
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/calibrate")
    {
        extern void calibrateScale();
        calibrateScale();
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/tare_scale")
    {
        extern void tareScale();
        tareScale();
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/left")
    {
        setPreset(SMALL);
        LOGF("[SET PRESET] %s\n", "LEFT");
    }
    else if (String(topic) == "coffeegrinder/" + mqttIdentifier + "/cmd/right")
    {
        setPreset(LARGE);
        LOGF("[SET PRESET] %s\n", "RIGHT");
    }
}

void reconnect()
{
    while (!mqttClient.connected())
    {
        if (mqttClient.connect("CoffeeGrinder", mqtt_user, mqtt_pass, ("coffeegrinder/" + mqttIdentifier + "/status").c_str(), 1, true, "offline"))
        {
            // Publish online status after successful connection
            mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/status").c_str(), "online", true);
            mqttClient.subscribe("coffeegrinder/#");

            publishConfigsForHA();
        }
        else
        {
            delay(5000);
        }
    }
}

// Hilfsfunktion: device-Block ergänzen
void addDeviceBlock(JsonObject& device) {
  device["identifiers"][0] = mqttIdentifier;
  device["manufacturer"] = "Danny Smolinsky";
  device["model"] = "CoffeeGrinder";
  device["name"] = "CoffeeGrinder";
  device["sw_version"] = CURRENT_VERSION;
}

// Hilfsfunktion: publish JSON Payload
void publishConfig(const char* topic, std::function<void(JsonDocument&)> buildPayload) {
    JsonDocument doc;
    buildPayload(doc);

    doc.shrinkToFit();

    char payload[512];
    serializeJson(doc, payload);
    LOGF("[TOPIC] %s\n", topic);
    LOGF("[PAYLOAD] %s\n", payload);
    bool result = mqttClient.publish(topic, payload, true);
    LOGF("[MQTT] Connected: %s\n", mqttClient.connected() ? "YES" : "NO");
    LOGF("[MQTT] Publish result: %s\n", result ? "OK" : "FAILED");
}

void setupMqtt() {
    prefs.begin("mqtt", true);
    String server = prefs.getString("server", "");
    uint16_t port = prefs.getUInt("port", 0);
    String user = prefs.getString("user", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (server.isEmpty() || port == 0) {
        LOGF("[MQTT] No valid MQTT config found. Skipping setup.\nServer: %s\nPort: %d\nUser: %s\nPassword: %s\n", server, port, user, pass);
        return;
    }

    mqttClient.setServer(server.c_str(), port);
    mqttClient.setCallback(callback);
    mqtt_user = user.c_str();
    mqtt_pass = pass.c_str();

    reconnect();
}

void publishConfigsForHA() {
    // MQTT Status (optional, for Home Assistant reference)
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/status/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "MQTT Status";
        doc["unique_id"] = mqttIdentifier + "_status";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/status";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // CoffeeGrinder Weight
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/weight/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Current Weight";
        doc["unique_id"] = mqttIdentifier + "_current_weight";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/current_weight";
        doc["unit_of_measurement"] = "g";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Preset Left
    publishConfig(("homeassistant/number/" + mqttIdentifier + "/preset_left/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Coffee Small";
        doc["unique_id"] = mqttIdentifier + "_preset_left";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/preset_left/set";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/preset_left";
        doc["step"] = 0.1;
        doc["min"] = 0.1;
        doc["max"] = 30;
        doc["unit_of_measurement"] = "g";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Preset Right
    publishConfig(("homeassistant/number/" + mqttIdentifier + "/preset_right/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Coffee Large";
        doc["unique_id"] = mqttIdentifier + "_preset_right";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/preset_right/set";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/preset_right";
        doc["step"] = 0.1;
        doc["min"] = 0.1;
        doc["max"] = 30;
        doc["unit_of_measurement"] = "g";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Selected Preset
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/selected_preset/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Selected Preset";
        doc["unique_id"] = mqttIdentifier + "_selected_preset";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/selected_preset";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Current State
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/current_state/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Current State";
        doc["unique_id"] = mqttIdentifier + "_current_state";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/current_state";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Block Threshold
    publishConfig(("homeassistant/number/" + mqttIdentifier + "/block_threshold/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Block Threshold";
        doc["unique_id"] = mqttIdentifier + "_block_threshold";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/block_threshold/set";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/block_threshold";
        doc["step"] = 0.01;
        doc["unit_of_measurement"] = "g";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Scale Factor
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/scale_factor/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Scale Factor";
        doc["unique_id"] = mqttIdentifier + "_scale_factor";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/scale_factor";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Presets Runs Counter
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/presets_left_runs/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Coffee Small Count";
        doc["unique_id"] = mqttIdentifier + "_presets_left_runs";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/presets_left_runs";
        doc["state_class"] = "total_increasing";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/presets_right_runs/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Coffee Large Count";
        doc["unique_id"] = mqttIdentifier + "_presets_right_runs";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/presets_right_runs";
        doc["state_class"] = "total_increasing";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Total Weight Counter
    publishConfig(("homeassistant/sensor/" + mqttIdentifier + "/total_weight/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Total Weight";
        doc["unique_id"] = mqttIdentifier + "_total_weight";
        doc["state_topic"] = "coffeegrinder/" + mqttIdentifier + "/total_weight";
        doc["unit_of_measurement"] = "g";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Start
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/start/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Press Start";
        doc["unique_id"] = mqttIdentifier + "_cmd_start";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/start";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Start Left
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/start_left/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Press Start Small";
        doc["unique_id"] = mqttIdentifier + "_cmd_start_left";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/start_left";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Start Right
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/start_right/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Press Start Large";
        doc["unique_id"] = mqttIdentifier + "_cmd_start_right";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/start_right";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Calibrate
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/calibrate/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Calibrate";
        doc["unique_id"] = mqttIdentifier + "_cmd_calibrate";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/calibrate";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Tare scale
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/tare_scale/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Press Tare Scale";
        doc["unique_id"] = mqttIdentifier + "_cmd_tare_scale";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/tare_scale";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Preset Left
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/left/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Press Small";
        doc["unique_id"] = mqttIdentifier + "_cmd_press_left";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/left";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });

    // Button: Preset Right
    publishConfig(("homeassistant/button/" + mqttIdentifier + "/right/config").c_str(), [](JsonDocument& doc) {
        doc["name"] = "Press Large";
        doc["unique_id"] = mqttIdentifier + "_cmd_press_right";
        doc["command_topic"] = "coffeegrinder/" + mqttIdentifier + "/cmd/right";

        JsonObject device = doc["device"].to<JsonObject>();
        addDeviceBlock(device);
    });
}

void loopMqtt()
{
    if (!mqttClient.connected())
    {
        reconnect();
    }
    mqttClient.loop();
}

void mqttPublishState()
{
    static float lastWeight = -1;
    static uint16_t lastPresetSmall = 0;
    static uint16_t lastPresetLarge = 0;
    static float lastBlockThreshold = -1;
    static float lastScaleFactor = 0.0f;
    static unsigned long lastPresetSmallRuns = -1;
    static unsigned long lastPresetLargeRuns = -1;
    static float lastTotalWeight = -1;
    static PresetSelection lastSelectedPreset = SMALL;
    static State lastState = UNKNOWN;

    if (roundf(weight * 10.0f) != roundf(lastWeight * 10.0f)) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/current_weight").c_str(), String(weight, 1).c_str(), true);
        lastWeight = weight;
    }

    if (selectedPreset != lastSelectedPreset || presetSmall != lastPresetSmall || presetLarge != lastPresetLarge) {
        const char* preset = selectedPreset == SMALL ? "SMALL" : "LARGE";
        float timeValue = (selectedPreset == SMALL ? presetSmall : presetLarge) / 10.0f;
        String message = String(preset) + " (" + String(timeValue, 1) + "s)";
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/selected_preset").c_str(), message.c_str(), true);
        lastSelectedPreset = selectedPreset;
    }

    if (presetSmall != lastPresetSmall) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/preset_left").c_str(), String(presetSmall / 10.0f, 1).c_str(), true);
        lastPresetSmall = presetSmall;
    }

    if (presetLarge != lastPresetLarge) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/preset_right").c_str(), String(presetLarge / 10.0f, 1).c_str(), true);
        lastPresetLarge = presetLarge;
    }

    if (blockThreshold != lastBlockThreshold) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/block_threshold").c_str(), String(blockThreshold, 2).c_str(), true);
        lastBlockThreshold = blockThreshold;
    }

    if (scaleFactor != lastScaleFactor) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/scale_factor").c_str(), String(scaleFactor, 2).c_str(), true);
        lastScaleFactor = scaleFactor;
    }

    if (presetSmallRuns != lastPresetSmallRuns) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/presets_left_runs").c_str(), String(presetSmallRuns).c_str(), true);
        lastPresetSmallRuns = presetSmallRuns;
    }

    if (presetLargeRuns != lastPresetLargeRuns) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/presets_right_runs").c_str(), String(presetLargeRuns).c_str(), true);
        lastPresetLargeRuns = presetLargeRuns;
    }

    if (totalWeight != lastTotalWeight) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/total_weight").c_str(), String(totalWeight, 1).c_str(), true);
        lastTotalWeight = totalWeight;
    }

    if (state != lastState) {
        mqttClient.publish(("coffeegrinder/" + mqttIdentifier + "/current_state").c_str(), stateToString(state).c_str(), true);
        lastState = state;
    }
}