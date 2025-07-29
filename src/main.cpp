#include <AccelStepper.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Bounce2.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include "HX711.h"
#include "mqtt.h"
#include "ota.h"
#include "pins.h"
#include "types.h"
#include "webserver.h"

// Reset pin for OLED (or -1 if sharing Arduino reset pin)
#define OLED_RESET -1
// I2C address for OLED display (128x32)
#define SCREEN_ADDRESS 0x3C

Bounce2::Button btnStart, btnL, btnR;

// Display object for OLED
Adafruit_SSD1306 display(128, 32, &Wire, OLED_RESET);

// Stepper object (driver mode)
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);

// HX711 object
HX711 scale;


// Maximum speed of the stepper motor in steps per second
constexpr float MAX_STEPPER_SPEED = 10000.0;

// Acceleration of the stepper motor in steps per second squared
constexpr float STEPPER_ACCELERATION = 400.0;

// Default speed used during grinding
constexpr float STEPPER_RUN_SPEED = 8000.0;

// Interval for the main control loop in milliseconds
constexpr unsigned long LOOP_INTERVAL_MS = 100;

// Interval for refreshing the OLED display in milliseconds
constexpr unsigned long DISPLAY_REFRESH_MS = 50;

// Interval for updating the weight value from the scale in milliseconds
constexpr unsigned long SCALE_INTERVAL_MS = 500;

// Debounce delay for touch buttons in milliseconds
constexpr unsigned long DEBOUNCE_DELAY = 50;

// Maximum allowed preset value (in 0.1g units) – corresponds to 30.0g
constexpr uint16_t MAX_PRESET_WEIGHT = 300;

// Minimum allowed preset value (in 0.1g units) – corresponds to 0.1g
constexpr uint16_t MIN_PRESET_WEIGHT = 1;

volatile State state = IDLE;
static float speed = STEPPER_RUN_SPEED;

// Presets in grams
uint16_t presetSmall = 8;
uint16_t presetLarge = 12;

// Currently selected preset
PresetSelection selectedPreset = SMALL;

// Preferences for NVS flash
Preferences prefs;

// Default calibration factor. Calibration is mandatory!
float scaleFactor = 1.0;

// Remaining time in 0.1s units
uint16_t remaining;

// Timestamp for run/pause timing
unsigned long lastMillis;

// Timestamp of the last scale read interval check in loop()
unsigned long lastScaleMillis = 0;

// Current weight in grams measured from the load cell
float weight = 0.0;

// Last weight value to detect changes for block detection
float lastWeight = 0.0;

// Timestamp of the last significant weight change
unsigned long lastWeightChangeTime = 0;

// Number of attempts to reverse the stepper motor during a blockage
uint8_t reverseAttempts = 0;

// Threshold to detect if grinding is blocked (weight not changing)
float blockThreshold = 0.03;

// Number of times the left preset was run
unsigned long presetSmallRuns = 0;

// Number of times the right preset was run
unsigned long presetLargeRuns = 0;

// Accumulated total weight of ground coffee
float totalWeight = 0.0;

// Indicates whether grinding was started from the web interface
bool webStart = false;

// Long press threshold in milliseconds
const unsigned long LONGPRESS_MS = 2000;


// FreeRTOS task that periodically updates the OLED display with the current status
void displayTask(void *pvParameters);

// FreeRTOS task that reads the weight from the load cell at regular intervals
void scaleTask(void *pvParameters);

// FreeRTOS task that handles MQTT communication and state publishing
void mqttTask(void *pvParameters);


// Adjust the preset value (left or right) by a given delta in setting mode
void adjustSetting(State s, int8_t delta);

// Start the calibration process for the scale
void calibrateScale();

// Update the OLED display with the current state and values
void drawDisplay();

// Enter preset adjustment mode for the selected preset (left or right)
void enterSetting(PresetSelection selection);

// Handle press and long press events for preset selection buttons
void handleButton(Bounce2::Button button, PresetSelection selection);

// Handle press and long press events for the start button
void handleStartButton(Bounce2::Button button);

// Load configuration values from non-volatile storage (NVS)
void loadPreferences();

// Print the current state and settings to the Serial monitor
void logState();

// Save current configuration values to non-volatile storage (NVS)
void savePreferences();

// Set the active preset, store the preference, and prepare for grinding
void setPreset(PresetSelection selection);

// Update the remaining grind time based on the active preset
void setRemainingTime();

// Set the selected preset without changing state or storing preferences
void setSelectedPreset(PresetSelection selection);

// Change the machine's operating state and log the new state
void setState(State s);

// Initialize touch buttons with debounce configuration
void setupButtons();

// Initialize the stepper motor configuration and speed settings
void setupStepper();

// Begin the grinding process, optionally taring the scale
void startGrinding(bool tareScale);

// Reset the scale to zero
void tareScale();


// Initialize serial, display, buttons, preferences and create display task
void setup()
{
    Serial.begin(115200);

    startWifi();
    setupWebServer();

    setupOTA();

    setupMqtt();

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        LOG(F("SSD1306 allocation failed"));
        for (;;);
    }
    display.clearDisplay();

    setupButtons();
    loadPreferences();
    setRemainingTime();
    setupStepper();

    scale.begin(HX_DT, HX_SCK);
    scale.set_scale(scaleFactor); // Calibration factor to be determined
    scale.tare();      // Reset scale to 0

    logState();

    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(scaleTask, "ScaleTask", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(mqttTask, "MqttTask", 2048, NULL, 1, NULL, 1);
}

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

// Configure stepper motor parameters and enable pin
void setupStepper()
{
    pinMode(PIN_ENABLE, OUTPUT);
    digitalWrite(PIN_ENABLE, HIGH);
    stepper.setMaxSpeed(MAX_STEPPER_SPEED);
    stepper.setAcceleration(STEPPER_ACCELERATION);
    stepper.setSpeed(speed);
}

// Main loop handling state machine and button updates
void loop()
{   
    ArduinoOTA.handle();

    unsigned long now = millis();    

    btnStart.update();
    btnL.update();
    btnR.update();

    if (now - lastScaleMillis >= SCALE_INTERVAL_MS && state != CALIBRATE) {
        lastScaleMillis += SCALE_INTERVAL_MS;
        // LOGF("[SCALE - loop] %.2f g\n", weight);
    }

    switch (state)
    {
    case IDLE:
        if (digitalRead(PIN_ENABLE) == LOW)
        {
            digitalWrite(PIN_ENABLE, HIGH);
        }

        handleStartButton(btnStart);
        handleButton(btnL, SMALL);
        handleButton(btnR, LARGE);

        break;
    case RUNNING:
        if (digitalRead(PIN_ENABLE) == HIGH)
        {
            digitalWrite(PIN_ENABLE, LOW);
        }

        // Check for weight change
        if (fabs(weight - lastWeight) > blockThreshold) {
            lastWeight = weight;
            lastWeightChangeTime = now;
            reverseAttempts = 0;
        }

        if (stepper.speed() > 0 && now - lastWeightChangeTime >= 2000) {
            if (reverseAttempts < 3) {
                stepper.setSpeed(-speed);
                lastWeightChangeTime = now;
                reverseAttempts++;
            } else {
                LOGF("[BLOCKED] Max reverse attempts (%dx) reached!\n", reverseAttempts);
                reverseAttempts = 0;
                setState(EMPTY);
            }
        } else if (stepper.speed() < 0 && now - lastWeightChangeTime >= 500) {
            stepper.setSpeed(speed);
            lastWeightChangeTime = now;
        }

        stepper.runSpeed();

        if (weight * 10 >= remaining) {
            setState(MEASURING);
        }

        if (webStart)
        {
            webStart = false;  // Skip first check after Web-Start
        }
        else if (btnStart.fell())
        {
            setState(PAUSED);
        }

        break;
    case MEASURING:
        if (weight * 10 >= remaining) {
            setState(FINISHED);
        } else {
            startGrinding(false);
        }
        break;
    case EMPTY:
    case PAUSED:
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
        if (selectedPreset == SMALL)
            presetSmallRuns++;
        else {
            presetLargeRuns++;
        }
        totalWeight += weight;
        setState(SAVING);
        break;
    case SAVING:
        savePreferences();
        setRemainingTime();
        delay(2000);
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
            // scale.tare();

            long reading = scale.get_value(10);
            LOGF("Raw reading: %ld", reading);

            float known_weight = 10.92;
            float factor = (float)reading / known_weight;

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

// Set remaining time based on selected preset
void setRemainingTime()
{
    LOGF("[REMAINING] %.1fg / %.1fg\n", presetSmall / 10.0, presetLarge / 10.0);
    remaining = (selectedPreset == SMALL) ? presetSmall : presetLarge;
}

void setPreset(PresetSelection selection) {
    setSelectedPreset(selection);
    savePreferences();
    setState(IDLE);
    scale.tare();
}

// Setter for selectedPreset variable with automatic logging
void setSelectedPreset(PresetSelection selection)
{
    selectedPreset = selection;
}

// Setter for state variable with automatic logging
void setState(State s)
{
    state = s;
    logState();
}

void calibrateScale()
{
    setState(CALIBRATE);

    LOG("== SCALE CALIBRATION ==");
    LOG("Remove all weight. Taring...");
    scale.tare();
    LOG("Place known weight (e.g. 100g) and press Start button.");
}

void tareScale() {
    LOG("Tare Scale");
    scale.tare();
}

// Start grinding: reset timer, tare scale if state is IDLE and change state to RUNNING
void startGrinding(bool tareScale)
{
    if (state == IDLE && tareScale == true) {
        scale.tare();
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

    // Long Press
    if (button.isPressed())
    {
        if (button.currentDuration() >= LONGPRESS_MS)
        {
            if (state == IDLE && !isSetIdle) {
                isSetWeighing = true;
                setState(WEIGHING);
                tareScale();
            } else if (state == WEIGHING && !isSetWeighing) {
                isSetIdle = true;
                setState(IDLE);
            }
        }
    }

    // Short Press
    else if (button.released())
    {
        LOG("RELEASED");
        if (button.currentDuration() < LONGPRESS_MS)
        {
            LOG("SHORT");
            if (state == IDLE && !isSetIdle) {
                LOG("Start Grinding");
                startGrinding(true);
            } else if (state == WEIGHING && !isSetWeighing) {
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

    // Long Press
    if (button.isPressed())
    {
        if (button.currentDuration() >= LONGPRESS_MS && !isSetSettings)
        {
            LOG("Enter Settings");
            isSetSettings = true;
            enterSetting(selection);
        }
    }

    // Short Press
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

    // presetSmallRuns = 1;
    // presetLargeRuns = 5;

    // setState(SAVING);

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
    case RUNNING:
        // float weight = scale.get_units();
        sprintf(buf, "%4.1fg", weight);
        break;
    case PAUSED:
        sprintf(buf, "%4.1fg", remaining / 10.0);
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