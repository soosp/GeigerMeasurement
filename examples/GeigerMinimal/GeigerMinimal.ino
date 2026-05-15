/**
 * GeigerMeasurement.ino
 *
 * A near to minimal example for GeigerMeasurement.h on ESP32 / ESP8266.
 *
 * Output (Serial, 115200 baud):
 *   One line per second with CPM, µSv/h.
 */
#include "GeigerMeasurement.h"

constexpr int GEIGER_PIN = D7;  // NodeMCU D7 = GPIO13

// SBM-20 tube, background radiation, 60-second sliding window (default)
GeigerMeasurement geiger(TUBE_SBM20, SOURCE_BACKGROUND);

// ISR
void IRAM_ATTR geigerISR() { geiger.onPulse(); }

void setup() {
    Serial.begin(115200);
    // Use INPUT, not INPUT_PULLUP. The matching of the input signal depends
    // on the circuit design of the GM counter.
    pinMode(GEIGER_PIN, INPUT);
    // ISR detects the falling edge of pulse from the GM circuit.
    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);
    // Setting fieldFactor for the SBM-20 tube. See the documentation for
    // details.
    geiger.setFieldFactor(1.611f);
    Serial.println("Geiger counter starting...");
}

void loop() {
    GeigerReading r = geiger.getReading();
    if (r.valid) {
        Serial.printf("CPM: %.1f  µSv/h: %.4f  ±%.0f%%\n",
                      r.cpm, r.uSvH, r.confidenceHalf);
    }
    delay(1000);
}