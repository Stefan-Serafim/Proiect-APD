#include <Arduino.h>
#include <LittleFS.h>

void setup() {
    Serial.begin(115200);
    Serial.println(APD_NODE_ID == 1 ? "Node A" : "Node B");

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        return;
    }

    File status = LittleFS.open("/www/status.txt", "r");
    if (!status) {
        Serial.println("Missing /www/status.txt");
        return;
    }

    while (status.available()) {
        Serial.write(status.read());
    }
}

void loop() {
}
