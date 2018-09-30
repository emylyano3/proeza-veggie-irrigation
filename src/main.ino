#include <FS.h>
#include <MQTTModule.h>
#include <ArduinoJson.h>

MQTTModule _mqttModule;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Starting module");
  _mqttModule.init();
}

void loop() {
  Serial.println("Main loop");
  _mqttModule.loop();
  delay(3000);
}

void subscribeToMqttBroker () {
  Serial.println("subscribeToMqttBroker");
}

void receiveMqttMessage(char* topic, unsigned char* payload, unsigned int length) {
  Serial.print("receiveMqttMessage: ");
  Serial.println(topic);
}