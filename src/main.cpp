#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <Preferences.h>
#include <AccelStepper.h>
#include <Bounce2.h>

// Pins für Stepper
#define PIN_STEP 19
#define PIN_DIR 18
#define PIN_ENABLE 14 // Enable-Pin für Treiber (LOW = enabled)

// Enum oder Konstanten für die Preset-Seite
#define LEFT true
#define RIGHT false

Bounce2::Button btnStart, btnL, btnR;

// Pins für Taster
#define BTN_L 4     // Preset links auswählen
#define BTN_R 0     // Preset rechts auswählen
#define BTN_START 2 // Start-Schalter

#define OLED_RESET -1       // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C // 0x3C für 128x32

// Display-Objekt erstellen
Adafruit_SSD1306 display(128, 32, &Wire, OLED_RESET);

// Stepper-Objekt (Driver-Modus)
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);

// Zustände
enum State
{
    IDLE,
    RUNNING,
    PAUSED,
    FINISHED,
    SAVING,
    SET_LEFT,
    SET_RIGHT
};

static volatile State state = IDLE;

// Debounce-Einstellungen
const unsigned long DEBOUNCE_DELAY = 50; // ms

// Presets in 0,1-s-Schritten und gewünschte Geschwindigkeiten
static uint16_t presetLeft = 120 * 10;   //  8.0 s
static uint16_t presetRight = 180 * 10; // 12.0 s

// Speed in Steps/Second
static float speed = 9000.0;

// Laufzeit-Variablen
uint16_t remaining;       // verbleibende 0,1 s
unsigned long lastMillis; // für run-/pause-Timing

// Longpress-Erkennung
const unsigned long LONGPRESS_MS = 800;

// Welches Preset ist aktiv?
bool selectedPreset = LEFT;

// Preferences für NVS-Flash
Preferences prefs;

// Task-Prototypen
void displayTask(void *pvParameters);
void stepperTask(void *pvParameters);

void setupStepper();
void setRemainingTime();
void startGrinding();
void enterSetting(bool isLeft);
void adjustSetting(State s, int8_t delta);
void handleButton(Bounce2::Button button, bool isLeft);
void saveSetting(State s);
void drawDisplay();
void displayTask(void *pvParameters);
void stepperTask(void *pvParameters);

void setup()
{
    Serial.begin(115200);

    // Display
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        Serial.println(F("SSD1306 allocation failed"));
        for (;;)
            ;
    }
    display.clearDisplay();

    // Buttons
    btnStart.attach(BTN_START, INPUT_PULLUP);
    btnStart.interval(DEBOUNCE_DELAY);
    btnStart.setPressedState(LOW);
    btnR.attach(BTN_R, INPUT_PULLUP);
    btnR.interval(DEBOUNCE_DELAY);
    btnR.setPressedState(LOW);
    btnL.attach(BTN_L, INPUT_PULLUP);
    btnL.interval(DEBOUNCE_DELAY);
    btnL.setPressedState(LOW);

    // Load saved preferences
    prefs.begin("coffee", false);
    if (!prefs.isKey("pL"))
        prefs.putUShort("pL", presetLeft);
    if (!prefs.isKey("pR"))
        prefs.putUShort("pR", presetRight);
    presetLeft = prefs.getUShort("pL", presetLeft);
    presetRight = prefs.getUShort("pR", presetRight);

    if (prefs.isKey("sel"))
        selectedPreset = prefs.getUInt("sel", selectedPreset);
    else
        prefs.putUInt("sel", selectedPreset);

    setRemainingTime();

    // Stepper
    setupStepper();

    // Display-Task auf Core 1
    //xTaskCreatePinnedToCore(displayTask, "DisplayTask", 2048, NULL, 1, NULL, 1);
    // Stepper-Task auf Core 0
    //xTaskCreatePinnedToCore(stepperTask, "StepperTask", 2048, NULL, 2, NULL, 0);
}

void loop()
{
    unsigned long now = millis();

    btnStart.update();
    btnL.update();
    btnR.update();

    switch (state)
    {
    case IDLE:
        digitalWrite(PIN_ENABLE, HIGH);

        // Start Grinding
        if (btnStart.fell())
        {
            startGrinding();
        }

        handleButton(btnL, LEFT);
        handleButton(btnR, RIGHT);

        break;
    case RUNNING:
        if (digitalRead(PIN_ENABLE) == HIGH)
        {
            digitalWrite(PIN_ENABLE, LOW);
        }

        if (now - lastMillis >= 100)
        {
            lastMillis += 100;
            remaining--;
            if (remaining == 0)
            {
                state = FINISHED;
            }
        }

        stepper.runSpeed();

        if (btnStart.fell())
        {
            state = PAUSED;
        }

        break;
    case PAUSED:
        if (btnStart.fell())
        {
            startGrinding();
        }

        if (btnL.fell())
        {
            selectedPreset = LEFT;
            prefs.putUInt("sel", selectedPreset);
            setRemainingTime();
            state = IDLE;
        }

        if (btnR.fell())
        {
            selectedPreset = RIGHT;
            prefs.putUInt("sel", selectedPreset);
            setRemainingTime();
            state = IDLE;
        }
        break;
    case FINISHED:
        setRemainingTime();
        state = IDLE;
        break;
    case SAVING:
        saveSetting(state);
        prefs.putUInt("sel", selectedPreset);
        setRemainingTime();
        delay(2000);
        state = IDLE;
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
            state = SAVING;
        }
        break;
    }
}

void setupStepper()
{
    pinMode(PIN_ENABLE, OUTPUT);
    digitalWrite(PIN_ENABLE, HIGH);
    stepper.setMaxSpeed(20000);
    stepper.setAcceleration(400.0);
    stepper.setSpeed(speed);
}

void setRemainingTime()
{
    remaining = selectedPreset ? presetLeft : presetRight;
}

void startGrinding()
{
    lastMillis = millis();
    state = RUNNING;
}

void enterSetting(bool isLeft)
{
    state = isLeft ? SET_LEFT : SET_RIGHT;
}

void adjustSetting(State s, int8_t delta)
{
    if (s == SET_LEFT)
        presetLeft += delta;
    else
        presetRight += delta;
    presetLeft = constrain(presetLeft, 1, 600);
    presetRight = constrain(presetRight, 1, 600);
}

void handleButton(Bounce2::Button button, bool isLeft)
{
    // Long Press
    if (button.isPressed())
    {
        if (button.currentDuration() >= LONGPRESS_MS)
        {
            enterSetting(isLeft);
        }
    }

    // Short Press
    if (button.pressed())
    {
        if (button.currentDuration() < LONGPRESS_MS)
        {
            selectedPreset = isLeft;
            prefs.putUInt("sel", selectedPreset);
            setRemainingTime();
        }
    }
}

void saveSetting(State s)
{
    prefs.putUShort("pL", presetLeft);
    prefs.putUShort("pR", presetRight);
}

void drawDisplay()
{
    display.clearDisplay();
    char buf[32];
    switch (state)
    {
    case IDLE:
        sprintf(buf, "%s %4.1fs", selectedPreset ? "Kl." : "Gr.", remaining / 10.0);
        break;
    case RUNNING:
        sprintf(buf, "%4.1fs", remaining / 10.0);
        break;
    case PAUSED:
        sprintf(buf, "%4.1fs", remaining / 10.0);
        break;
    case FINISHED:
        sprintf(buf, "Fertig!");
        break;
    case SAVING:
        sprintf(buf, "Zeit gespeichert!");
        break;
    case SET_LEFT:
        sprintf(buf, "Setze Kl.: %4.1fs", presetLeft / 10.0);
        break;
    case SET_RIGHT:
        sprintf(buf, "Setze Gr.: %4.1fs", presetRight / 10.0);
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

void displayTask(void *pvParameters)
{
    (void)pvParameters;
    while (true)
    {
        drawDisplay();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void stepperTask(void *pvParameters)
{
    (void)pvParameters;
    for (;;)
    {
        if (state == RUNNING)
            stepper.setSpeed(speed);
            stepper.runSpeed();
        // Minimal yield, sonst blockiert
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}