#include <Arduino.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Update.h>
#include "mqtt.h"
#include "pins.h"
#include "types.h"
#include "webserver.h"

extern void calibrateScale();
extern float scaleFactor;
extern uint16_t remaining;
extern PresetSelection selectedPreset;
extern uint16_t presetLeft;
extern uint16_t presetRight;
extern void savePreferences();
extern void startGrinding(bool tareScale);
extern void setPreset(PresetSelection selection);
extern bool webStart;
extern void setState(State s);
extern volatile State state;

AsyncWebServer server(80);

void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("mqtt", true);
        String mqttServer = prefs.getString("server", "");
        uint16_t mqttPort = prefs.getInt("port", 1883);
        String mqttUser = prefs.getString("user", "");
        String mqttPass = prefs.getString("pass", "");
        prefs.end();

        String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Settings - CoffeeGrinder</title><style>"
        "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f4; }"
        "h1 { text-align: center; }"
        "form { max-width: 400px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }"
        "label { display: block; margin-top: 12px; }"
        "input[type='text'], input[type='number'], input[type='submit'] {"
        "width: 100%; padding: 10px; margin-top: 6px; box-sizing: border-box; border-radius: 4px;"
        "border: 1px solid #ccc; font-size: 1em; }"
        "input[type='submit'], button { background-color: #4CAF50; color: white; border: none; cursor: pointer; margin-top: 20px; }"
        "input[type='submit']:hover, button:hover { background-color: #45a049; }"
        ".segmented-control {"
        "  display: flex;"
        "  overflow: hidden;"
        "  border-radius: 4px;"
        "  border: 1px solid #ccc;"
        "  margin-top: 6px;"
        "}"
        ".segmented-control input[type='radio'] {"
        "  display: none;"
        "}"
        ".segmented-control label {"
        "  flex: 1;"
        "  text-align: center;"
        "  padding: 10px;"
        "  cursor: pointer;"
        "  background-color: #f4f4f4;"
        "  border-right: 1px solid #ccc;"
        "  margin: 0;"
        "}"
        ".segmented-control label:last-child {"
        "  border-right: none;"
        "}"
        ".segmented-control input[type='radio']:checked + label {"
        "  background-color: #4CAF50;"
        "  color: white;"
        "}"
        "</style></head><body><h1>CoffeeGrinder</h1>"
        "<form id='controlForm' style='max-width: 400px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.2);'>"
        "<h3>Control</h3>"
        "<div style='display: flex; gap: 6px;'>"
        "<button type='button' style='flex:1; padding:10px; border-radius:4px; background-color:#4CAF50; color:white; border:none; cursor:pointer; font-size:1em;' onclick=\"sendAction('left')\">Left</button>"
        "<button type='button' style='flex:1; padding:10px; border-radius:4px; background-color:#4CAF50; color:white; border:none; cursor:pointer; font-size:1em;' onclick=\"sendAction('start')\">Start</button>"
        "<button type='button' style='flex:1; padding:10px; border-radius:4px; background-color:#4CAF50; color:white; border:none; cursor:pointer; font-size:1em;' onclick=\"sendAction('right')\">Right</button>"
        "</div>"
        "</form>"
        "<div style='height:20px;'></div>"
        "<form id='settingsForm'>"
        "<h3>Settings</h3>"
        "<label id='labelLeft' for='left'>Left Preset (Grams)</label>"
        "<input type='number' step='0.1' id='left' name='left' value='" + String(presetLeft / 10.0f, 1) + "'>"
        "<label id='labelRight' for='right'>Right Preset (Grams)</label>"
        "<input type='number' step='0.1' id='right' name='right' value='" + String(presetRight / 10.0f, 1) + "'>"
        "<input type='submit' value='Save Settings'>"
        "</form>"
        "<div style='height:20px;'></div>"
        "<form id='mqttForm' style='max-width: 400px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.2);'>"
        "<h3>MQTT Settings</h3>"
        "<label for='mqtt_server'>Server</label>"
        "<input type='text' id='mqtt_server' name='mqtt_server' value='" + mqttServer + "'>"
        "<label for='mqtt_port'>Port</label>"
        "<input type='number' id='mqtt_port' name='mqtt_port' value='" + String(mqttPort) + "'>"
        "<label for='mqtt_user'>Username</label>"
        "<input type='text' id='mqtt_user' name='mqtt_user' value='" + mqttUser + "'>"
        "<label for='mqtt_pass'>Password</label>"
        "<input type='password' id='mqtt_pass' name='mqtt_pass' style='width: 100%; padding: 10px; margin-top: 6px; box-sizing: border-box; border-radius: 4px; border: 1px solid #ccc; font-size: 1em;' value='" + mqttPass + "'>"
        "<input type='submit' value='Save MQTT Settings'>"
        "</form>"
        "<div style='height:20px;'></div>"
        "<form id='calibrationForm' style='max-width: 400px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.2);'>"
        "<h3>Calibration</h3>"
        "<p>Current factor: <span style='float: right;'>" + String(scaleFactor, 2) + "</span></p>"
        "<input type='submit' value='Start Calibration'>"
        "</form>"
        "<div style='height:20px;'></div>"
        "<form id='restartForm' style='max-width: 400px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.2);'>"
        "<h3>Restart</h3>"
        "<input type='submit' value='Restart ESP'>"
        "</form>"
        "<div id='toast' style='display:none; position:fixed; bottom:20px; left:50%; transform:translateX(-50%); background:#4CAF50; color:white; padding:10px 20px; border-radius:4px;'>Settings saved!</div>"
        "<script>"
        "function updateUnits() {"
        "  document.getElementById('labelLeft').innerText = 'Left Preset (Grams)';"
        "  document.getElementById('labelRight').innerText = 'Right Preset (Grams)';"
        "}"
        "document.getElementById('settingsForm').addEventListener('submit', function(e) {"
        "  e.preventDefault();"
        "  const left = document.getElementById('left').value;"
        "  const right = document.getElementById('right').value;"
        "  fetch(`/saveSettings?right=${right}&left=${left}`)"
        "    .then(() => showToast('Settings saved!'));"
        "});"
        "document.getElementById('calibrationForm').addEventListener('submit', function(e) {"
        "  e.preventDefault();"
        "  fetch('/calibrate')"
        "    .then(() => showToast('Calibration complete!'));"
        "});"
        "document.getElementById('restartForm').addEventListener('submit', function(e) {"
        "  e.preventDefault();"
        "  fetch('/restart')"
        "    .then(() => showToast('Calibration complete!'));"
        "});"
        "document.getElementById('mqttForm').addEventListener('submit', function(e) {"
        "  e.preventDefault();"
        "  const server = document.getElementById('mqtt_server').value;"
        "  const port = document.getElementById('mqtt_port').value;"
        "  const user = document.getElementById('mqtt_user').value;"
        "  const pass = document.getElementById('mqtt_pass').value;"
        "  fetch(`/mqtt?server=${server}&port=${port}&user=${user}&pass=${pass}`)"
        "    .then(() => showToast('MQTT settings saved!'));"
        "});"
        "function showToast(message) {"
        "  const toast = document.getElementById('toast');"
        "  toast.innerText = message;"
        "  toast.style.display = 'block';"
        "  setTimeout(() => { toast.style.display = 'none'; }, 2000);"
        "}"
        "function sendAction(cmd) {"
        "  fetch(`/action?cmd=${cmd}`)"
        "    .then(() => showToast(`Action '${cmd}' sent!`));"
        "}"
        "</script>"
        "<div style='text-align:center; color:#888; font-size:0.9em; margin-top:40px;'>Build: " __DATE__ " - " __TIME__ "</div>"
        "</body></html>";
        request->send(200, "text/html", html);
    });

    // Kalibrierung starten
    server.on("/calibrate", HTTP_GET, [](AsyncWebServerRequest *request){
        calibrateScale();
        request->send(200, "text/plain", "Calibration done!");
    });

    server.on("/saveSettings", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("left")) {
            String rawLeft = request->getParam("left")->value();
            LOGF("[WEB] Raw left preset: %s\n", rawLeft.c_str());
            float val = rawLeft.toFloat();
            LOGF("[WEB] Parsed left preset: %.2f\n", val);
            if (val >= 0.1 && val <= 180.0) {
                presetLeft = static_cast<uint16_t>(val * 10.0f);
                LOGF("[WEB] Stored presetLeft: %d\n", presetLeft);
            }
        }
        if (request->hasParam("right")) {
            String rawRight = request->getParam("right")->value();
            LOGF("[WEB] Raw right preset: %s\n", rawRight.c_str());
            float val = rawRight.toFloat();
            LOGF("[WEB] Parsed right preset: %.2f\n", val);
            if (val >= 0.1 && val <= 180.0) {
                presetRight = static_cast<uint16_t>(val * 10.0f);
                LOGF("[WEB] Stored presetRight: %d\n", presetRight);
            }
        }
        savePreferences();
        request->send(200, "text/plain", "Settings saved. <a href='/'>Back</a>");
    });

    // Presets einstellen (left/right)
    server.on("/setPreset", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("left")) {
            presetLeft = request->getParam("left")->value().toFloat();
        }
        if (request->hasParam("right")) {
            presetRight = request->getParam("right")->value().toFloat();
        }
        savePreferences();
        request->send(200, "text/plain", "Presets updated");
    });

    server.on("/action", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("cmd")) {
            String cmd = request->getParam("cmd")->value();
            if (cmd == "start") {
                webStart = true;
                if (state == IDLE)
                    startGrinding(true);
                else
                    startGrinding(true);
            } else if (cmd == "left") {
                setPreset(LEFT);
            } else if (cmd == "right") {
                setPreset(RIGHT);
            }
        }
        request->send(200, "text/plain", "Action executed");
    });

    server.on("/mqtt", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("mqtt", false);
        if (request->hasParam("server")) prefs.putString("server", request->getParam("server")->value());
        if (request->hasParam("port")) prefs.putUInt("port", request->getParam("port")->value().toInt());
        if (request->hasParam("user")) prefs.putString("user", request->getParam("user")->value());
        if (request->hasParam("pass")) prefs.putString("pass", request->getParam("pass")->value());
        prefs.end();
        request->send(200, "text/plain", "MQTT settings saved.");

        setupMqtt();
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<form method='POST' action='/update' enctype='multipart/form-data'>"
                    "<input type='file' name='update'>"
                    "<input type='submit' value='Update'>"
                    "</form>";
        request->send(200, "text/html", html);
    });

    server.on("/update", HTTP_POST, 
        [](AsyncWebServerRequest *request) {
            bool success = !Update.hasError();
            request->send(200, "text/plain", success ? "Update OK. Rebooting..." : "Update Failed!");
            if (success) {
                delay(3000);
                ESP.restart();
            }
        }, 
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                setState(UPDATING);
                Serial.printf("Update Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start update with max available size
                    Update.printError(Serial);
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
                if (Update.end(true)) { // true = successful
                    Serial.printf("Update Success: %u bytes\n", index+len);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );

    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
    });

    server.begin();
}

void startConfigPortal() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("CoffeeGrinder");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        int n = WiFi.scanNetworks();

        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WiFi Setup</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f4; }
    h1 { font-size: 1.5em; text-align: center; }
    label { display: block; margin-top: 12px; }
    input[type="text"], input[type="password"], input[type="submit"], select {
      width: 100%; padding: 12px; margin-top: 6px; box-sizing: border-box; border-radius: 4px;
      border: 1px solid #ccc; font-size: 1em;
    }
    input[type="submit"] {
      background-color: #4CAF50; color: white; border: none; margin-top: 20px; cursor: pointer;
    }
    input[type="submit"]:hover {
      background-color: #45a049;
    }
    .container {
      max-width: 400px; margin: auto; background-color: #fff; padding: 20px; border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.2);
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Select WiFi</h1>
    <form action="/save" method="post">
      <label for="ssid">Network</label>
      <select name="ssid" id="ssid">
)rawliteral";

        for (int i = 0; i < n; ++i) {
            html += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + "</option>";
        }

        html += R"rawliteral(
      </select>
      <label for="p">Password</label>
      <input id="p" name="p" length=64 type="password" placeholder="Password">
      <input type="submit" value="Save">
    </form>
  </div>
</body>
</html>
)rawliteral";
        request->send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        String ssid = request->arg("ssid");
        String pass = request->arg("p");
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        request->send(200, "text/plain", "WiFi settings saved. Restarting...");
        delay(1000);
        ESP.restart();
    });

    server.begin();
}

void startWifi() {
    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.length() > 0) {
        // Wir haben eine gespeicherte SSID → direkt verbinden
        WiFi.begin(ssid.c_str(), pass.c_str());
        setupWebServer();
    } else {
        // Keine gespeicherte SSID → Config Portal starten
        startConfigPortal();
    }
}