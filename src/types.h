#pragma once

#define ENABLE_LOGGING true

#if ENABLE_LOGGING
#define LOG(x) Serial.println(x)
#define LOGF(fmt, ...) Serial.printf((fmt), __VA_ARGS__)
#else
#define LOG(x)
#define LOGF(fmt, ...)
#endif

enum PresetSelection {
    SMALL,
    LARGE
};

enum PresetMode { 
    MODE_TIME, 
    MODE_WEIGHT 
};

enum State
{
    CALIBRATE,
    EMPTY,
    FINISHED,
    IDLE,
    MEASURING,
    PAUSED,
    RUNNING,
    SAVING,
    SET_LEFT,
    SET_RIGHT,
    UPDATING,
    UNKNOWN,
    WEIGHING
};