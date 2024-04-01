 /*
   -------------------------------------------------------------------------------------
   chicken-farm
   reads the load cells of the chicken farm and sends the value by mqtt
   by Stephan Stäheli, november 2023
   -------------------------------------------------------------------------------------
*/

#include <HX711_ADC.h>
#include <EEPROM.h>
#include "WiFiS3.h"
#include <ArduinoMqttClient.h>
#include "arduino-secrets.h" 

// wifi connection
char ssid[] = SECRET_SSID;    // your network SSID (name)
char pass[] = SECRET_PASS;    // your network password 
int reconnectWifiInterval = 30000; //interval when to send weight as message
int lastWifiConnectionState = -99;
unsigned long wifiConnectionSince = 0;
WiFiClient wifiClient;

// mqtt connection
char mqtt_user[] = SECRET_MQTT_USER;
char mqtt_pass[] = SECRET_MQTT_PASS;
const char broker[] = "huehnerstall"; //IP address of the EMQX broker.
const int  port = 1883;
const char willTopic[] = "/chicken-farm/lastwill";
const char inTopic[] = "/chicken-farm/commands";
const char outTopic[] = "/chicken-farm/replies";
const char stateTopic[] = "/chicken-farm/state";
const int QOS_AT_MOST_ONCE = 0;
const int QOS_AT_LEAST_ONCE = 1;
const int QOS_EXACTLY_ONCE = 2;
MqttClient mqttClient(wifiClient);

//HX711 connection
const int HX711_dout_1 = 4; //mcu > HX711 no 1 dout pin
const int HX711_sck_1 = 5; //mcu > HX711 no 1 sck pin
const int HX711_dout_2 = 6; //mcu > HX711 no 2 dout pin
const int HX711_sck_2 = 7; //mcu > HX711 no 2 sck pin
HX711_ADC LoadCell_1(HX711_dout_1, HX711_sck_1); //HX711 1
HX711_ADC LoadCell_2(HX711_dout_2, HX711_sck_2); //HX711 2

//EEPROM
const int calVal_eepromAdress_1 = 0; // eeprom adress for calibration value load cell 1 (4 bytes)
const int calVal_eepromAdress_2 = 4; // eeprom adress for calibration value load cell 2 (4 bytes)

unsigned long lastDataGrabbed = 0;
unsigned long lastMessageSent = 0;
unsigned long startUp = millis();

boolean executeTare1 = 0;
boolean executeTare2 = 0;

void setup() {
  delay(2000);

  // Create serial connection and wait for it to become available.
  Serial.begin(9600);
  while (!Serial) {
    ; 
  }

  connectToWifi();
  connectToMqttServer();

  float calibrationValue_1; // calibration value load cell 1
  float calibrationValue_2; // calibration value load cell 2

  EEPROM.get(calVal_eepromAdress_1, calibrationValue_1); // uncomment this if you want to fetch the value from eeprom
  EEPROM.get(calVal_eepromAdress_2, calibrationValue_2); // uncomment this if you want to fetch the value from eeprom

  LoadCell_1.begin();
  LoadCell_2.begin();
  //LoadCell_1.setReverseOutput();
  //LoadCell_2.setReverseOutput();
  unsigned long stabilizingtime = 2000; // tare preciscion can be improved by adding a few seconds of stabilizing time
  boolean _tare = true; //set this to false if you don't want tare to be performed in the next step
  byte loadcell_1_rdy = 0;
  byte loadcell_2_rdy = 0;
  while ((loadcell_1_rdy + loadcell_2_rdy) < 2) { //run startup, stabilization and tare, both modules simultaniously
    if (!loadcell_1_rdy) loadcell_1_rdy = LoadCell_1.startMultiple(stabilizingtime, _tare);
    if (!loadcell_2_rdy) loadcell_2_rdy = LoadCell_2.startMultiple(stabilizingtime, _tare);
  }
  if (LoadCell_1.getTareTimeoutFlag()) {
    Serial.println("Timeout, check MCU>HX711 no.1 wiring and pin designations");
  }
  if (LoadCell_2.getTareTimeoutFlag()) {
    Serial.println("Timeout, check MCU>HX711 no.2 wiring and pin designations");
  }
  LoadCell_1.setCalFactor(calibrationValue_1); // user set calibration value (float)
  LoadCell_2.setCalFactor(calibrationValue_2); // user set calibration value (float)
  Serial.println("Startup is complete");
}

void loop() {
  checkWifiAndReconnect();

  static boolean newDataReady = 0;
  const int serialPrintInterval = 1000; //increase value to slow down serial print activity
  const int sendMessageInterval = 20000; //interval when to send weight as message

  // check for new data/start next conversion:
  if (LoadCell_1.update()) newDataReady = true;
  LoadCell_2.update();

  //get smoothed value from data set
  if ((newDataReady)) {
    if (millis() > lastDataGrabbed + serialPrintInterval) {
      float a = LoadCell_1.getData();
      float b = LoadCell_2.getData();
      Serial.print("Load_cell 1 output val: ");
      Serial.print(a);
      Serial.print("    Load_cell 2 output val: ");
      Serial.println(b);
      newDataReady = 0;
      lastDataGrabbed = millis();
      if (millis() > lastMessageSent + sendMessageInterval) {
        lastMessageSent = millis();
        Serial.print("Sending message with weight 1: ");
        Serial.print(a);
        Serial.print(" and weight 2: ");
        Serial.println(b);
          mqttClient.beginMessage(outTopic);
          mqttClient.print("s1:");
          mqttClient.print(a);
          mqttClient.print(";s2:");
          mqttClient.print(b);
          mqttClient.endMessage();
      }
    }
  }

  if (executeTare1) {
      LoadCell_1.tareNoDelay();
      executeTare1 = 0;
  }

  if (executeTare2) {
      LoadCell_2.tareNoDelay();
      executeTare2 = 0;
  }

  //check if last tare operation is complete
  if (LoadCell_1.getTareStatus() == true) {
    mqttPrintState("Tare load cell 1 complete");
  }
  if (LoadCell_2.getTareStatus() == true) {
    mqttPrintState("Tare load cell 2 complete");
  }

  // call poll() regularly to allow the library to receive MQTT messages and
  // send MQTT keep alives which avoids being disconnected by the broker
  mqttClient.poll();
}

void onMqttMessage(int messageSize) {
  Serial.println("Received a message...");

  // we received a message, print out the topic and contents
  Serial.print("Received a message with topic '");
  Serial.print(mqttClient.messageTopic());
  Serial.print("', duplicate = ");
  Serial.print(mqttClient.messageDup() ? "true" : "false");
  Serial.print(", QoS = ");
  Serial.print(mqttClient.messageQoS());
  Serial.print(", retained = ");
  Serial.print(mqttClient.messageRetain() ? "true" : "false");
  Serial.print("', length ");
  Serial.print(messageSize);
  Serial.println(" bytes:");

  // use the Stream interface to print the contents
  char messageContent[messageSize + 1];
  for(int i = 0; mqttClient.available(); i++) {
    messageContent[i] = (char)mqttClient.read();
  }
  messageContent[messageSize] = '\0';
  Serial.print("received message is: ");
  Serial.println(messageContent);

  if(strcmp(messageContent, "tare:1") == 0) {
    executeTare1 = true;
  } else if(strcmp(messageContent, "tare:2") == 0) {
    executeTare2 = true;
  } else if(strcmp(messageContent, "ping") == 0) {
    mqttClient.beginMessage(outTopic);
    mqttClient.print("pong");
    mqttClient.endMessage();
  }
}

void connectToWifi() {
  // Connect to WiFi
  Serial.print("Attempting to (re-)connect to WPA SSID: ");
  Serial.println(ssid);
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    // failed, retry
    Serial.print(".");
    delay(2000);
  }

  wifiConnectionSince = millis();
  Serial.println("You're connected to the network");
  Serial.println();
}

void checkWifiAndReconnect() {
  int state = WiFi.status();
  if (lastWifiConnectionState != state) {
    Serial.print("Current WIFI state '");
    Serial.print(state);
    Serial.print("', WL_CONNECTED(3) = ");
    Serial.print(state == WL_CONNECTED ? "true" : "false");
    Serial.print("', WL_IDLE_STATUS(0) = ");
    Serial.print(state == WL_IDLE_STATUS ? "true" : "false");
    Serial.println("'.");
    lastWifiConnectionState = state;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectionSince = millis();
  } else if (millis() > wifiConnectionSince + reconnectWifiInterval) {
    Serial.print("WIFI connection not ready, current state is '");
    Serial.print(WiFi.status());
    Serial.println("', trying to reconnect.");
    WiFi.disconnect();
    connectToWifi();
    wifiConnectionSince = millis();
  }
}

void connectToMqttServer() {
//  mqttClient.setId("arduino-chicken-farm");
  mqttClient.setUsernamePassword(mqtt_user, mqtt_pass);
  mqttClient.setCleanSession(true);

  Serial.print("Attempting to connect to the MQTT broker: ");
  Serial.println(broker);

  while (!mqttClient.connect(broker, port)) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("");
  mqttPrintState("You're connected to the MQTT broker!");
  Serial.println();

  // set the message receive callback
  mqttClient.onMessage(onMqttMessage);

  Serial.print("Subscribing to topic: ");
  Serial.println(inTopic);
  Serial.println();
  mqttClient.subscribe(inTopic, QOS_AT_LEAST_ONCE);
  Serial.println("Waiting for messages...");

}

void mqttPrintState(char* message) {
  Serial.println(message);
  mqttClient.beginMessage(stateTopic);
  mqttClient.print(message);
  mqttClient.endMessage();
}

