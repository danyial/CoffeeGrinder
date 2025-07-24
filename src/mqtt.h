#pragma once

#include <Arduino.h>

void setupMqtt();
void publishConfigsForHA();
void loopMqtt();

void mqttPublishState();