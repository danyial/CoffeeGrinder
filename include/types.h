#pragma once

#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING true
#endif

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