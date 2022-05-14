/*
  This is the code for the AirGradient DIY Air Quality Sensor with an ESP8266 Microcontroller.

  It is a high quality sensor showing PM2.5, CO2, Temperature and Humidity on a small display and can send data over WiFi.

  For build instructions please visit https://www.airgradient.com/diy/

  Compatible with the following sensors:

  Plantower PMS5003 (Fine Particle Sensor)
  SenseAir S8 (CO2 Sensor)
  SHT30/31 (Temperature/Humidity Sensor)

  Dependent Libraries:

  The codes needs the following libraries installed:
  ESP8266 board with standard libraries
  WifiManager by tzar, tablatronix tested with Version 2.0.3-alpha
  ESP8266 and ESP32 OLED driver for SSD1306 displays by ThingPulse, Fabrice Weinberg tested with Version 4.1.0
  PubSubClient

  Configuration:
  Please set in the code below which sensor you are using and if you want to connect it to WiFi.

  If you are a school or university contact us for a free trial on the AirGradient platform.
  https://www.airgradient.com/schools/

  MIT License
*/

#include <AirGradient.h>
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SSD1306Wire.h>
#include <Settings.h>
#include <WiFiManager.h>
#include <Wire.h>

WiFiClient espClient;
PubSubClient client(espClient);
AirGradient ag = AirGradient();
SSD1306Wire display(0x3c, SDA, SCL);

// Set sensors that you do not use to false
boolean hasCO2 = true;
boolean hasPM = true;
boolean hasSHT = true;

// Enable WiFi and MQTT
boolean connectWIFI = true;
boolean sendMQTT = true;

char mqtt_payload[128];
char mqtt_topic[128];
char deviceid[32];

const uint32_t msSAMPLE_INTERVAL = 2500;
const uint32_t msPUBLISH_INTERVAL = 30000;

// Pass sensor values around more easily
float Temperature;

int AQI;
int CO2;
int Humidity;
int PM2;

String ln1;
String ln2;
String ln3;
String ln4;
String ln5;
String topic_prefix;

uint32_t msLAST_METRIC;
uint32_t msLAST_SAMPLE;

// Setup the oled display
void lcd(String ln1, String ln2 = "", String ln3 = "", String ln4 = "", String ln5 = "")
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(32, 0, ln1);
  display.drawString(32, 9, ln2);
  display.drawString(32, 18, ln3);
  display.drawString(32, 27, ln4);
  display.drawString(32, 36, ln5);
  display.display();
}

// Setup WiFi
void setupWiFi()
{
  delay(3000);

  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to " + String(wifi_ssid));

  lcd("Initializing", "WiFi:", wifi_ssid);

  WiFi.begin(wifi_ssid, wifi_password);
  WiFi.mode(WIFI_STA); // Turn off the soft ap (enabled by default)

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  randomSeed(micros());

  IPAddress ip = WiFi.localIP();

  Serial.println("");
  Serial.print("WiFi connected, IP Address: ");
  Serial.println(ip);

  lcd(String(ip[0]) + ".", String(ip[1]) + ".", String(ip[2]) + ".", String(ip[3]));
  delay(1000);
}

void reconnect()
{
  // Loop until we're reconnected
  while (!client.connected())
  {
    Serial.print("Attempting MQTT connection...");

    // Create a random client ID
    String clientId = "airgradient-";
    clientId += String(random(0xffff), HEX);

    // Attempt to connect
    if (client.connect(clientId.c_str(), mqtt_username, mqtt_password))
    {
      Serial.println("Connected to MQTT broker.");
    }
    else
    {
      Serial.print("Failed to connect to MQTT broker: rc=");
      Serial.print(client.state());
      Serial.println("Retrying in 5 seconds.");
      delay(5000);
    }
  }
}

int linear(int AQIhigh, int AQIlow, float Conchigh, float Conclow, float Concentration)
{
  float a = ((Concentration - Conclow) / (Conchigh - Conclow)) * (AQIhigh - AQIlow) + AQIlow;

  return round(a);
}

// This is the same AQI logic that is in the Purple Air javascript, so if it seems weird/incorrect, that's why.
// We use Purple Air as the local source of truth for PM2.5 so this enables to use the same logic with the same Plantower sensor.
int pm25toAQI(int pm25)
{
  int AQI;
  float c = (floor(10 * float(pm25))) / 10;

  if (c >= 0 && c < 12.1)
  {
    AQI = linear(50, 0, 12, 0, c);
  }
  else if (c >= 12.1 && c < 35.5)
  {
    AQI = linear(100, 51, 35.4, 12.1, c);
  }
  else if (c >= 35.5 && c < 55.5)
  {
    AQI = linear(150, 101, 55.4, 35.5, c);
  }
  else if (c >= 55.5 && c < 150.5)
  {
    AQI = linear(200, 151, 150.4, 55.5, c);
  }
  else if (c >= 150.5 && c < 250.5)
  {
    AQI = linear(300, 201, 250.4, 150.5, c);
  }
  else if (c >= 250.5 && c < 350.5)
  {
    AQI = linear(400, 301, 350.4, 250.5, c);
  }
  else if (c >= 350.5 && c < 500.5)
  {
    AQI = linear(500, 401, 500.4, 350.5, c);
  }
  else
  {
    AQI = linear(1000, 501, 1000.4, 500.5, c);
  }

  return AQI;
}

void setup()
{
  Serial.begin(9600);

  delay(2000);

  String(ESP.getChipId(), HEX).toCharArray(deviceid, 32);

  Serial.print("DeviceId: ");
  Serial.println(deviceid);

  display.init();

  lcd("Initializing", "Device:", deviceid);

  if (hasCO2)
    ag.CO2_Init();
  if (hasPM)
    ag.PMS_Init();
  if (hasSHT)
    ag.TMP_RH_Init(0x44);
  if (connectWIFI)
    setupWiFi();
  if (sendMQTT)
    client.setServer(mqtt_host, 1883);

  delay(2000);
}

void loop()
{
  if (connectWIFI && sendMQTT)
  {
    if (!client.connected())
    {
      reconnect();
    }
    client.loop();
  }

  // Sample data
  if (millis() - msLAST_SAMPLE >= msSAMPLE_INTERVAL)
  {
    msLAST_SAMPLE = millis();
    ln1 = ln2 = ln3 = ln4 = ln5 = "";
    topic_prefix = String(mqtt_topic_root) + "/" + String(deviceid) + "/";

    if (hasCO2)
    {
      CO2 = ag.getCO2_Raw();
      ln5 = "CO2: " + String(CO2);

      Serial.print("CO2: " + String(CO2));
    }

    if (hasPM)
    {
      PM2 = ag.getPM2_Raw();
      AQI = pm25toAQI(PM2);
      ln3 = "AQI:  " + String(AQI);
      ln4 = "PM2: " + String(PM2);

      Serial.print(" PM2: " + String(PM2));
      Serial.print(" AQI: " + String(AQI));
    }

    if (hasSHT)
    {
      TMP_RH result = ag.periodicFetchData();
      Humidity = result.rh;
      Temperature = (result.t * 1.8) + 32;
      ln1 = "TMP: " + String((int)Temperature) + "°F";
      ln2 = "HMD: " + String(Humidity) + "\%";

      Serial.print(" T: " + String(Temperature));
      Serial.print(" H: " + String(Humidity));
    }

    Serial.println();
    lcd(ln1, ln2, ln3, ln4, ln5);
  }

  // Send MQTT payload(s)
  if (connectWIFI && sendMQTT && millis() - msLAST_METRIC >= msPUBLISH_INTERVAL)
  {
    msLAST_METRIC = millis();

    if (hasCO2)
    {
      String(CO2).toCharArray(mqtt_payload, 64);
      String(topic_prefix + "co2").toCharArray(mqtt_topic, 64);
      client.publish(mqtt_topic, mqtt_payload, true);
    }

    if (hasPM)
    {
      String(AQI).toCharArray(mqtt_payload, 64);
      String(topic_prefix + "aqi").toCharArray(mqtt_topic, 64);
      client.publish(mqtt_topic, mqtt_payload, true);

      String(PM2).toCharArray(mqtt_payload, 64);
      String(topic_prefix + "pm25").toCharArray(mqtt_topic, 64);
      client.publish(mqtt_topic, mqtt_payload, true);
    }

    if (hasSHT)
    {
      String(Humidity).toCharArray(mqtt_payload, 64);
      String(topic_prefix + "humidity").toCharArray(mqtt_topic, 64);
      client.publish(mqtt_topic, mqtt_payload, true);

      String(Temperature).toCharArray(mqtt_payload, 64);
      String(topic_prefix + "temperature").toCharArray(mqtt_topic, 64);
      client.publish(mqtt_topic, mqtt_payload, true);
    }
  }
}