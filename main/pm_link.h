#pragma once
#include "pm_protocol.h"

bool initializePMLink(bool forcePair); // returns whether to show the pairing screen
void processPMLink();
void finishPMPairing();
bool pmLinkPaired();
bool pmLinkFullRefreshDue();
bool pmMeasurementDue();
void sendPMReading(const PMReading &reading);
