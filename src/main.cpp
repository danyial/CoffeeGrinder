#include <Arduino.h>
#include <Bounce2.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DShotRMT.h>
#include <StreamString.h>
#include <esp_wifi.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

#include "HX711.h"
#include "mqtt.h"
#include "ota.h"
#include "pins.h"
#include "types.h"
#include "webserver.h"

// -----------------------------------------------------------------------------
// Configuration constants
// -----------------------------------------------------------------------------

constexpr int8_t OLED_RESET = -1;
constexpr uint8_t SCREEN_ADDRESS = 0x3C;

constexpr unsigned long DISPLAY_REFRESH_MS = 50;
constexpr unsigned long SCALE_INTERVAL_MS = 500;
constexpr unsigned long DEBOUNCE_DELAY = 50;

constexpr uint16_t MIN_PRESET_WEIGHT = 1;
constexpr uint16_t MAX_PRESET_WEIGHT = 300;

constexpr uint16_t MOTOR_RUN_THROTTLE = 1200;
constexpr uint16_t MOTOR_SLOW_THROTTLE = MOTOR_RUN_THROTTLE / 2;
constexpr uint16_t MOTOR_RAMP_UP_STEP = 2;
constexpr uint16_t MOTOR_RAMP_UP_DELAY_MS = 4;
constexpr uint16_t MOTOR_RAMP_DOWN_STEP = 25;
constexpr uint16_t MOTOR_RAMP_DOWN_DELAY_MS = 4;
constexpr uint16_t MOTOR_RAMP_MIN_HOLD_MS = 200;
constexpr float MOTOR_SLOWDOWN_THRESHOLD_G = 0.7f;

const unsigned long LONGPRESS_MS = 2000;

// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------

WiFiServer logServer(23);
std::vector<WiFiClient> clients;

#if ENABLE_LOGGING
static void purgeClients()
{
    for (auto it = clients.begin(); it != clients.end();)
    {
        if (!it->connected())
        {
            it->stop();
            it = clients.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void logPrint()
{
    Serial.println();
    purgeClients();
    for (auto &client : clients)
    {
        client.println();
    }
}

void logPrint(const String &msg)
{
    Serial.println(msg);
    purgeClients();
    for (auto &client : clients)
    {
        client.println(msg);
    }
}

void logPrint(const __FlashStringHelper *msg)
{
    logPrint(String(msg));
}

void logPrint(const char *msg)
{
    logPrint(String(msg));
}

void logPrintf(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0)
    {
        return;
    }

    if (len >= static_cast<int>(sizeof(buffer)))
    {
        len = sizeof(buffer) - 1;
    }

    Serial.write(reinterpret_cast<const uint8_t *>(buffer), len);

    purgeClients();
    for (auto &client : clients)
    {
        client.write(reinterpret_cast<const uint8_t *>(buffer), len);
    }
}
#endif

// -----------------------------------------------------------------------------
// Hardware instances
// -----------------------------------------------------------------------------

Bounce2::Button btnStart, btnL, btnR;

Adafruit_SSD1306 display(128, 32, &Wire, OLED_RESET);

DShotRMT motor(PIN_ESC, dshot_mode_t::DSHOT300, false);
static volatile uint16_t throttleStream = DSHOT_THROTTLE_MIN;
static volatile bool stopPending = true;
static uint16_t motorCurrentThrottle = DSHOT_CMD_MOTOR_STOP;

HX711 scale;

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

volatile State state = IDLE;

uint16_t presetSmall = 8;
uint16_t presetLarge = 12;
PresetSelection selectedPreset = SMALL;

Preferences prefs;

float scaleFactor = 1.0;

uint16_t remaining = 0;
unsigned long lastMillis = 0;
unsigned long lastScaleMillis = 0;

float weight = 0.0;
float lastWeight = 0.0;
unsigned long lastWeightChangeTime = 0;
float blockThreshold = 0.03f;

unsigned long presetSmallRuns = 0;
unsigned long presetLargeRuns = 0;
float totalWeight = 0.0;

bool webStart = false;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

static void motorSendRaw(uint16_t value);
void motorRampTo(uint16_t targetThrottle, uint16_t stepSize, uint16_t delayMs);
inline void motorRampUp() { motorRampTo(MOTOR_RUN_THROTTLE, MOTOR_RAMP_UP_STEP, MOTOR_RAMP_UP_DELAY_MS); }
inline void motorRampDown() { motorRampTo(DSHOT_CMD_MOTOR_STOP, MOTOR_RAMP_DOWN_STEP, MOTOR_RAMP_DOWN_DELAY_MS); }

void setupButtons();
void setupMotor();

void setSelectedPreset(PresetSelection selection);
void setRemainingTime();
void setPreset(PresetSelection selection);
void setState(State s);
void tareScale();
void calibrateScale();
void startGrinding(bool tare);
void enterSetting(PresetSelection selection);
void adjustSetting(State s, int8_t delta);
void handleStartButton(Bounce2::Button button);
void handleButton(Bounce2::Button button, PresetSelection selection);

void loadPreferences();
void savePreferences();

void drawDisplay();
String stateToString(State s);
void logState();

void displayTask(void *pvParameters);
void scaleTask(void *pvParameters);
void mqttTask(void *pvParameters);
void throttleTask(void *pvParameters);

void setup();
void loop();

// -----------------------------------------------------------------------------
// Motor control
// -----------------------------------------------------------------------------

static void motorSendRaw(uint16_t value) {
    if (value == DSHOT_CMD_MOTOR_STOP) {
        motorCurrentThrottle = DSHOT_CMD_MOTOR_STOP;
        stopPending = true;
        throttleStream = DSHOT_THROTTLE_MIN; // Signal am Leben halten
        return;
    }
    uint16_t constrained = constrain(value, DSHOT_THROTTLE_MIN, DSHOT_THROTTLE_MAX);
    motorCurrentThrottle = constrained;
    throttleStream = constrained;
}

void motorRampTo(uint16_t targetThrottle, uint16_t stepSize, uint16_t delayMs)
{
    uint16_t currentThrottle = motorCurrentThrottle;
    uint16_t desired = targetThrottle;

    if (desired == DSHOT_CMD_MOTOR_STOP)
    {
        if (currentThrottle == DSHOT_CMD_MOTOR_STOP)
        {
            throttleStream = DSHOT_THROTTLE_MIN;
            return;
        }

        int32_t value = currentThrottle;
        int32_t step = stepSize ? stepSize : 1;
        while (value > DSHOT_THROTTLE_MIN)
        {
            value -= step;
            if (value < DSHOT_THROTTLE_MIN)
            {
                value = DSHOT_THROTTLE_MIN;
            }

            motorSendRaw(static_cast<uint16_t>(value));
            if (delayMs)
            {
                delay(delayMs);
            }
        }

        motorSendRaw(DSHOT_CMD_MOTOR_STOP);
        return;
    }

    if (desired < DSHOT_THROTTLE_MIN)
    {
        desired = DSHOT_THROTTLE_MIN;
    }

    if (currentThrottle == desired)
    {
        motorSendRaw(desired);
        return;
    }

    int32_t step = stepSize ? stepSize : 1;
    step = (desired > currentThrottle) ? step : -step;
    if (step == 0)
    {
        step = (desired > motorCurrentThrottle) ? 1 : -1;
    }

    int32_t value = currentThrottle;
    bool minHoldDone = false;

    while (value != desired)
    {
        value += step;

        if ((step > 0 && value > desired) || (step < 0 && value < desired))
        {
            value = desired;
        }

        if (value < DSHOT_THROTTLE_MIN)
        {
            value = DSHOT_THROTTLE_MIN;
        }

        motorSendRaw(static_cast<uint16_t>(value));

        if (!minHoldDone && value == DSHOT_THROTTLE_MIN && desired > DSHOT_THROTTLE_MIN && MOTOR_RAMP_MIN_HOLD_MS > 0)
        {
            delay(MOTOR_RAMP_MIN_HOLD_MS);
            minHoldDone = true;
        }

        if (delayMs)
        {
            delay(delayMs);
        }
    }
}

// -----------------------------------------------------------------------------
// Input hardware
// -----------------------------------------------------------------------------

// Configure button pins and debounce settings
void setupButtons()
{
    btnStart.attach(BTN_START, INPUT);
    btnStart.interval(DEBOUNCE_DELAY);
    btnStart.setPressedState(HIGH);

    btnR.attach(BTN_R, INPUT);
    btnR.interval(DEBOUNCE_DELAY);
    btnR.setPressedState(HIGH);

    btnL.attach(BTN_L, INPUT);
    btnL.interval(DEBOUNCE_DELAY);
    btnL.setPressedState(HIGH);
}

// Setup motor
void setupMotor()
{
    // Initialize the motor
    dshot_result_t result = motor.begin();
    if (result.success) {
        LOG("Motor initialized successfully");
    } else {
        printDShotResult(result);
    }
}

// -----------------------------------------------------------------------------
// Grinding workflow & state transitions
// -----------------------------------------------------------------------------

// Setter for selected preset without side effects
void setSelectedPreset(PresetSelection selection)
{
    selectedPreset = selection;
}

// Set remaining time based on selected preset
void setRemainingTime()
{
    LOGF("[REMAINING] %.1fg / %.1fg\n", presetSmall / 10.0, presetLarge / 10.0);
    remaining = (selectedPreset == SMALL) ? presetSmall : presetLarge;
}

void setPreset(PresetSelection selection)
{
    setSelectedPreset(selection);
    savePreferences();
    setState(IDLE);
    scale.tare();
}

// Setter for state variable with automatic logging
void setState(State s)
{
    state = s;
    logState();
}

void tareScale()
{
    LOG("Tare Scale");
    delay(500);
    scale.tare();
}

void calibrateScale()
{
    setState(CALIBRATE);

    LOG("== SCALE CALIBRATION ==");
    LOG("Remove all weight. Taring...");
    scale.tare();
    LOG("Place known weight (e.g. 100g) and press Start button.");
}

// Start grinding: reset timer, optionally tare scale and change state to RUNNING
void startGrinding(bool tare)
{
    if (state == IDLE && tare)
    {
        tareScale();
    }

    lastMillis = millis();
    setRemainingTime();
    setState(RUNNING);
}

// Enter setting mode for selected preset
void enterSetting(PresetSelection selection)
{
    setState(selection == SMALL ? SET_LEFT : SET_RIGHT);
}

// Adjust preset time in setting mode
void adjustSetting(State s, int8_t delta)
{
    if (s == SET_LEFT)
        presetSmall += delta;
    else
        presetLarge += delta;
    presetSmall = constrain(presetSmall, MIN_PRESET_WEIGHT, MAX_PRESET_WEIGHT);
    presetLarge = constrain(presetLarge, MIN_PRESET_WEIGHT, MAX_PRESET_WEIGHT);
}

// Handle button press for short and long press actions
void handleStartButton(Bounce2::Button button)
{
    static bool isSetWeighing = false;
    static bool isSetIdle = false;

    if (button.isPressed())
    {
        if (button.currentDuration() >= LONGPRESS_MS)
        {
            if (state == IDLE && !isSetIdle)
            {
                isSetWeighing = true;
                setState(WEIGHING);
                tareScale();
            }
            else if (state == WEIGHING && !isSetWeighing)
            {
                isSetIdle = true;
                setState(IDLE);
            }
        }
    }
    else if (button.released())
    {
        LOG("RELEASED");
        if (button.currentDuration() < LONGPRESS_MS)
        {
            LOG("SHORT");
            if (state == IDLE && !isSetIdle)
            {
                LOG("Start Grinding");
                startGrinding(true);
                lastWeight = weight;
            }
            else if (state == WEIGHING && !isSetWeighing)
            {
                tareScale();
            }
        }

        isSetIdle = false;
        isSetWeighing = false;
    }
}

void handleButton(Bounce2::Button button, PresetSelection selection)
{
    static bool isSetSettings = false;

    if (button.isPressed())
    {
        if (button.currentDuration() >= LONGPRESS_MS && !isSetSettings)
        {
            LOG("Enter Settings");
            isSetSettings = true;
            enterSetting(selection);
        }
    }

    if (button.released())
    {
        if (button.currentDuration() < LONGPRESS_MS)
        {
            LOG("Set Preset");
            setPreset(selection);
        }

        isSetSettings = false;
    }
}

// -----------------------------------------------------------------------------
// Preferences & persistence
// -----------------------------------------------------------------------------

// Load presets and selected preset from non-volatile storage
void loadPreferences()
{
    prefs.begin("coffee", false);

    presetSmall = prefs.getUShort("pL", 8 * 10);
    presetLarge = prefs.getUShort("pR", 12 * 10);

    LOGF("[PREFERENCES] %d / %d g\n", presetSmall, presetLarge);

    uint32_t sel = prefs.getUInt("sel", static_cast<uint32_t>(selectedPreset));
    selectedPreset = (sel <= LARGE) ? static_cast<PresetSelection>(sel) : SMALL;

    scaleFactor = prefs.getFloat("scale", 1.0);
    scale.set_scale(scaleFactor);

    totalWeight = prefs.getFloat("totalWeight", 0.0);
    presetSmallRuns = prefs.getULong("presetSmallRuns", 0);
    presetLargeRuns = prefs.getULong("presetLargeRuns", 0);
    blockThreshold = prefs.getFloat("blockThreshold", 0.3);

    prefs.end();
}

// Save presets and selected preset to non-volatile storage
void savePreferences()
{
    prefs.begin("coffee", false);

    prefs.putUShort("pL", presetSmall);
    prefs.putUShort("pR", presetLarge);
    prefs.putUInt("sel", static_cast<uint32_t>(selectedPreset));
    prefs.putFloat("scale", scaleFactor);
    prefs.putFloat("totalWeight", totalWeight);
    prefs.putULong("presetSmallRuns", presetSmallRuns);
    prefs.putULong("presetLargeRuns", presetLargeRuns);
    prefs.putFloat("blockThreshold", blockThreshold);

    prefs.end();

    setSelectedPreset(selectedPreset);
    setRemainingTime();
    drawDisplay();
}

// -----------------------------------------------------------------------------
// Display & logging
// -----------------------------------------------------------------------------

// Update OLED display based on current state
void drawDisplay()
{
    display.clearDisplay();
    char buf[32];
    switch (state)
    {
    case IDLE:
        sprintf(buf, "%4.1fg", (selectedPreset == SMALL ? presetSmall : presetLarge) / 10.0);
        break;
    case WEIGHING:
        sprintf(buf, "%4.1fg", weight);
        break;
    case RUNNING:
        // float weight = scale.get_units();
        sprintf(buf, "%4.1fg", (remaining / 10.0) - weight);
        break;
    case PAUSED:
        sprintf(buf, "%4.1fg", (remaining / 10.0) - weight);
        break;
    case MEASURING:
        sprintf(buf, "Warte...");
        break;
    case FINISHED:
        sprintf(buf, "Fertig!");
        break;
    case SAVING:
        sprintf(buf, "Gespeichert!");
        break;
    case SET_LEFT:
        sprintf(buf, "Setze Kl.: %4.1f%s", presetSmall / 10.0, "g");
        break;
    case SET_RIGHT:
        sprintf(buf, "Setze Gr.: %4.1f%s", presetLarge / 10.0, "g");
        break;
    case CALIBRATE:
        sprintf(buf, "Kalibrierung");
        break;
    case EMPTY:
        sprintf(buf, "Bohnen\nleer?");
        break;
    case UPDATING:
        sprintf(buf, "Update...");
        break;
    }

    int16_t x1, y1;
    uint16_t w, h;
    display.setTextSize(2);
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    int16_t x = (display.width() - w) / 2;
    int16_t y = (display.height() - h) / 2;
    display.setCursor(x, y);
    display.setTextColor(SSD1306_WHITE);
    display.println(buf);
    display.display();
}

// Convert state enum to human-readable string
String stateToString(State s)
{
    switch (s)
    {
    case IDLE: return "IDLE";
    case RUNNING: return "RUNNING";
    case PAUSED: return "PAUSED";
    case MEASURING: return "MEASURING";
    case FINISHED: return "FINISHED";
    case SAVING: return "SAVING";
    case SET_LEFT: return "SET_LEFT";
    case SET_RIGHT: return "SET_RIGHT";
    case CALIBRATE: return "CALIBRATE";
    case UPDATING: return "UDATING";
    case EMPTY: return "EMPTY";
    case WEIGHING: return "WEIGHING";
    default: return "UNKNOWN";
    }
}

// Log current state, preset and remaining time
void logState()
{
    LOGF("[STATE] %s\n", stateToString(state));
    LOGF("[PRESET] %s\n", (selectedPreset == SMALL) ? "SMALL" : "LARGE");
    LOGF("[REMAINING] %.1fg\n", remaining / 10.0);
}

// -----------------------------------------------------------------------------
// FreeRTOS tasks
// -----------------------------------------------------------------------------

// FreeRTOS task to refresh the display periodically
void displayTask(void *pvParameters)
{
    (void)pvParameters;
    while (true)
    {
        drawDisplay();
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));
    }
}

void scaleTask(void *pvParameters)
{
    (void)pvParameters;
    while (true)
    {
        weight = scale.get_units();
        // LOGF("[SCALE - Task] %.2f g\n", weight);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void mqttTask(void *pvParameters)
{
    (void)pvParameters;
    while (true)
    {
        loopMqtt();
        mqttPublishState();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void throttleTask(void *) {
    const TickType_t interval = pdMS_TO_TICKS(1);
    while (true) {
        if (stopPending) {
            motor.sendThrottle(DSHOT_CMD_MOTOR_STOP); // Befehl einmal
            stopPending = false;
        } else {
            motor.sendThrottle(throttleStream);       // Idle oder Run
        }
        vTaskDelay(interval);
    }
}

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------

// Initialize serial, networking, hardware, and background tasks
void setup()
{
    Serial.begin(115200);

    startWifi();
    logServer.begin();
    logServer.setNoDelay(true);
    setupWebServer();

    setupOTA();
    setupMqtt();

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        LOG(F("SSD1306 allocation failed"));
        for (;;)
        {
        }
    }
    display.clearDisplay();

    setupButtons();
    loadPreferences();
    setRemainingTime();
    setupMotor();

    scale.begin(HX_DT, HX_SCK);
    scale.set_scale(scaleFactor);
    scale.tare();

    logState();

    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(scaleTask, "ScaleTask", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(mqttTask, "MqttTask", 6144, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(throttleTask, "ThrottleTask", 1024, NULL, 1, NULL, 1);
}

// Main loop handling state machine and button updates
void loop()
{
    ArduinoOTA.handle();

    WiFiClient newClient = logServer.accept();
    if (newClient)
    {
        newClient.setNoDelay(true);
        clients.push_back(newClient);
    }

    unsigned long now = millis();

    btnStart.update();
    btnL.update();
    btnR.update();

    if (now - lastScaleMillis >= SCALE_INTERVAL_MS && state != CALIBRATE)
    {
        lastScaleMillis += SCALE_INTERVAL_MS;
        // LOGF("[SCALE - loop] %.2f g\n", weight);
    }

    switch (state)
    {
    case IDLE:
        motorRampDown();
        handleStartButton(btnStart);
        handleButton(btnL, SMALL);
        handleButton(btnR, LARGE);
        break;

    case RUNNING:
    {
        float gramsRemaining = (remaining / 10.0f) - weight;
        uint16_t targetThrottle = (gramsRemaining <= MOTOR_SLOWDOWN_THRESHOLD_G) ? MOTOR_SLOW_THROTTLE : MOTOR_RUN_THROTTLE;
        motorRampTo(targetThrottle, MOTOR_RAMP_UP_STEP, MOTOR_RAMP_UP_DELAY_MS);

        if (fabs(weight - lastWeight) > blockThreshold)
        {
            lastWeight = weight;
            lastWeightChangeTime = now;
        }

        if (now - lastWeightChangeTime >= 4000)
        {
            LOG("[EMPTY] Max time of weight not changing reached");
            setState(EMPTY);
        }
        else if (now - lastWeightChangeTime >= 500)
        {
            lastWeightChangeTime = now;
        }

        if (weight * 10 >= remaining)
        {
            motorRampDown();
            setState(MEASURING);
        }

        if (webStart)
        {
            webStart = false; // Skip first check after Web-Start
        }
        else if (btnStart.released())
        {
            setState(PAUSED);
        }
        break;
    }

    case MEASURING:
        delay(1000);
        if (weight * 10 >= remaining)
        {
            setState(FINISHED);
        }
        else
        {
            startGrinding(false);
        }
        break;

    case EMPTY:
    case PAUSED:
        motorRampDown();

        if (btnStart.fell())
        {
            startGrinding(false);
        }

        if (btnL.fell())
        {
            setSelectedPreset(SMALL);
            savePreferences();
            setRemainingTime();
            setState(IDLE);
        }

        if (btnR.fell())
        {
            setSelectedPreset(LARGE);
            savePreferences();
            setRemainingTime();
            setState(IDLE);
        }
        break;

    case FINISHED:
        motorRampDown();
        if (selectedPreset == SMALL)
        {
            presetSmallRuns++;
        }
        else
        {
            presetLargeRuns++;
        }
        totalWeight += weight;
        setState(SAVING);
        break;

    case SAVING:
        savePreferences();
        setRemainingTime();
        delay(1000);
        setState(IDLE);
        break;

    case SET_LEFT:
    case SET_RIGHT:
        if (btnL.fell())
        {
            adjustSetting(state, -1);
        }

        if (btnR.fell())
        {
            adjustSetting(state, 1);
        }

        if (btnStart.fell())
        {
            setState(SAVING);
        }
        break;

    case CALIBRATE:
        if (btnStart.fell())
        {
            long reading = scale.get_value(10);
            LOGF("Raw reading: %ld", reading);

            float known_weight = 10.92;
            float factor = static_cast<float>(reading) / known_weight;

            LOGF("Calibration factor set: %.2f\n", factor);

            scaleFactor = factor;
            scale.set_scale(scaleFactor);

            setState(SAVING);
        }
        break;

    case WEIGHING:
        handleStartButton(btnStart);
        break;
    }
}
