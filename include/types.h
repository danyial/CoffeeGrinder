#pragma once

#include <Arduino.h>

#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING true
#endif

#if ENABLE_LOGGING
void logPrint();
void logPrint(const String &msg);
void logPrint(const __FlashStringHelper *msg);
void logPrint(const char *msg);
void logPrintf(const char *fmt, ...);

#define LOG(...) logPrint(__VA_ARGS__)
#define LOGF(fmt, ...) logPrintf((fmt), ##__VA_ARGS__)
#else
#define LOG(...)
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

enum Rotation {
    CW = 1,
    CCW = -1
};
