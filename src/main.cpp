#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <Preferences.h>
#include <AccelStepper.h>
#include <Bounce2.h>
#include "HX711.h"
#include <esp_wifi.h>
#include <WebServer.h>
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

constexpr float MAX_STEPPER_SPEED = 6000.0;
constexpr float STEPPER_ACCELERATION = 400.0;
constexpr float STEPPER_RUN_SPEED = 5000.0;
constexpr unsigned long LOOP_INTERVAL_MS = 100;
constexpr unsigned long DISPLAY_REFRESH_MS = 50;
constexpr unsigned long SCALE_INTERVAL_MS = 500;
constexpr unsigned long DEBOUNCE_DELAY = 50;
// Maximum preset time in 0.1s units (60.0s)
constexpr uint16_t MAX_PRESET_WEIGHT = 300;
// Minimum preset time in 0.1s units (0.1s)
constexpr uint16_t MIN_PRESET_WEIGHT = 1;

volatile State state = IDLE;
static float speed = STEPPER_RUN_SPEED;

// Presets in 0.1 second steps
uint16_t presetLeft = 120 * 10;
uint16_t presetRight = 180 * 10;

// Currently selected preset
PresetSelection selectedPreset = LEFT;

// Preferences for NVS flash
Preferences prefs;

// Default calibration factor. Calibration is mandatory!
float scaleFactor = 1.0;

// Remaining time in 0.1s units
uint16_t remaining;
// Timestamp for run/pause timing
unsigned long lastMillis;
unsigned long lastScaleMillis = 0;

float weight = 0.0;
float lastWeight = 0.0;
unsigned long lastWeightChangeTime = 0;

uint8_t reverseAttempts = 0;

float blockThreshold = 0.07f;  // Initialwert für Blockiererkennungsschwelle (in Gramm)
unsigned long presetsLeftRun = 0;  // Anzahl durchgelaufener Presets
unsigned long presetsRightRun = 0;  // Anzahl durchgelaufener Presets
float totalWeight = 0.0f;      // Gesamtgewicht des gemahlenen Kaffees

// Long press threshold in milliseconds
const unsigned long LONGPRESS_MS = 3000;

bool webStart = false;

void displayTask(void *pvParameters);
void scaleTask(void *pvParameters);
void mqttTask(void *pvParameters);

void adjustSetting(State s, int8_t delta);
void calibrateScale();
void drawDisplay();
void enterSetting(PresetSelection selection);
void handleButton(Bounce2::Button button, PresetSelection selection);
void handleStartButton(Bounce2::Button button);
void loadPreferences();
void logState();
void savePreferences();
void setPreset(PresetSelection selection);
void setRemainingTime();
void setSelectedPreset(PresetSelection selection);
void setState(State s);
void setupButtons();
void setupStepper();
void startGrinding(bool tareScale);
void tareScale();

void calibrateScale()
{
    setState(CALIBRATE);

    Serial.println("== SCALE CALIBRATION ==");
    Serial.println("Remove all weight. Taring...");
    scale.tare();
    Serial.println("Place known weight (e.g. 100g) and press Start button.");
}

// Initialize serial, display, buttons, preferences and create display task
void setup()
{
    Serial.begin(115200);

    startWifi();

    setupOTA();

    setupMqtt();
    publishConfigsForHA();

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        Serial.println(F("SSD1306 allocation failed"));
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
        handleButton(btnL, LEFT);
        handleButton(btnR, RIGHT);

        break;
    case RUNNING:
        if (digitalRead(PIN_ENABLE) == HIGH)
        {
            digitalWrite(PIN_ENABLE, LOW);
        }

        // Check for weight change
        if (fabs(weight - lastWeight) > blockThreshold) {  // Schwelle: 0.1 g
            lastWeight = weight;
            lastWeightChangeTime = now;
            reverseAttempts = 0;
        }

        if (stepper.speed() > 0 && now - lastWeightChangeTime >= 3000) {
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
            setSelectedPreset(LEFT);
            savePreferences();
            setRemainingTime();
            setState(IDLE);
        }

        if (btnR.fell())
        {
            setSelectedPreset(RIGHT);
            savePreferences();
            setRemainingTime();
            setState(IDLE);
        }
        break;
    case FINISHED:
        if (selectedPreset == LEFT)
            presetsLeftRun++;
        else {
            presetsRightRun++;
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
            Serial.print("Raw reading: ");
            Serial.println(reading);

            float known_weight = 10.92;
            float factor = (float)reading / known_weight;

            Serial.print("Calibration factor set: ");
            Serial.println(factor, 2);

            scaleFactor = factor;
            scale.set_scale(scaleFactor);
            
            setState(SAVING);
        }
        break;
    }
}

// Set remaining time based on selected preset
void setRemainingTime()
{
    LOGF("[REMAINING] %d / %d g\n", presetLeft, presetRight);
    remaining = (selectedPreset == LEFT) ? presetLeft : presetRight;
}

void setPreset(PresetSelection selection) {
    setSelectedPreset(selection);
    savePreferences();
    setRemainingTime();
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

void tareScale() {
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
    setState(selection == LEFT ? SET_LEFT : SET_RIGHT);
}

// Adjust preset time in setting mode
void adjustSetting(State s, int8_t delta)
{
    if (s == SET_LEFT)
        presetLeft += delta;
    else
        presetRight += delta;
    presetLeft = constrain(presetLeft, MIN_PRESET_WEIGHT, MAX_PRESET_WEIGHT);
    presetRight = constrain(presetRight, MIN_PRESET_WEIGHT, MAX_PRESET_WEIGHT);
}

// Handle button press for short and long press actions
void handleStartButton(Bounce2::Button button)
{
    // Long Press
    // if (button.isPressed())
    // {
    //     if (button.currentDuration() >= LONGPRESS_MS)
    //     {
    //         if (state == IDLE) {
    //             setState(WEIGHING);
    //             tareScale();
    //         } else if (state == WEIGHING) {
    //             setState(IDLE);
    //         }
    //     }
    // }

    // Short Press
    if (button.pressed())
    {
        if (button.currentDuration() < LONGPRESS_MS)
        {
            if (state == IDLE) {
                startGrinding(true);
            } else if (state == WEIGHING) {
                tareScale();
            }
        }
    }
}

void handleButton(Bounce2::Button button, PresetSelection selection)
{
    // Long Press
    if (button.isPressed())
    {
        if (button.currentDuration() >= LONGPRESS_MS)
        {
            enterSetting(selection);
            logState();
        }
    }

    // Short Press
    if (button.pressed())
    {
        if (button.currentDuration() < LONGPRESS_MS)
        {
            setPreset(selection);
        }
    }
}

// Load presets and selected preset from non-volatile storage
void loadPreferences()
{
    prefs.begin("coffee", false);

    presetLeft = prefs.getUShort("pL", 120 * 10);
    presetRight = prefs.getUShort("pR", 180 * 10);

    LOGF("[PREFERENCES] %d / %d g\n", presetLeft, presetRight);

    uint32_t sel = prefs.getUInt("sel", static_cast<uint32_t>(selectedPreset));
    selectedPreset = (sel <= RIGHT) ? static_cast<PresetSelection>(sel) : LEFT;

    scaleFactor = prefs.getFloat("scale", 1.0);
    scale.set_scale(scaleFactor);

    totalWeight = prefs.getFloat("totalWeight", 0.0);
    presetsLeftRun = prefs.getULong("presetsLeftRun", 0);
    presetsRightRun = prefs.getULong("presetsRightRun", 2);

    prefs.end();
}

// Save presets and selected preset to non-volatile storage
void savePreferences()
{
    prefs.begin("coffee", false);

    prefs.putUShort("pL", presetLeft);
    prefs.putUShort("pR", presetRight);
    prefs.putUInt("sel", static_cast<uint32_t>(selectedPreset));
    prefs.putFloat("scale", scaleFactor);
    prefs.putFloat("totalWeight", totalWeight);
    prefs.putULong("presetsLeftRun", presetsLeftRun);
    prefs.putULong("presetsRightRun", presetsRightRun);

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
        sprintf(buf, "%4.1fg", (selectedPreset == LEFT ? presetLeft : presetRight) / 10.0);
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
        sprintf(buf, "Setze Kl.: %4.1f%s", presetLeft / 10.0, "g");
        break;
    case SET_RIGHT:
        sprintf(buf, "Setze Gr.: %4.1f%s", presetRight / 10.0, "g");
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

// Log current state, preset and remaining time via Serial
void logState()
{
    LOGF("[STATE] %s\n", stateToString(state));
    LOGF("[PRESET] %s\n", (selectedPreset == LEFT) ? "LEFT" : "RIGHT");
    LOGF("[REMAINING] %.1fs\n", remaining / 10.0);
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