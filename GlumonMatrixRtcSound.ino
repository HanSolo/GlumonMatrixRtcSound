#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Adafruit_VS1053.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "RTClib.h"


// DotMatrix
#define HARDWARE_TYPE MD_MAX72XX::ICSTATION_HW
#define MAX_DEVICES   4
#define CLK_PIN      14  // or SCK
#define DATA_PIN     13  // or MOSI
#define CS_PIN       12  // or SS

// Realtime clock
RTC_DS3231 rtc;

// SPI hardware interface
MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// Text parameters
#define CHAR_SPACING 2 // pixels between characters

// Global message buffers shared by Serial and Scrolling functions
#define BUF_SIZE  75
char message[BUF_SIZE] = "   000";

// PiezoBuzzer
const int ALARM_BUZZER_PIN = 15;

// MuteButton
const int MUTE_BUTTON_PIN = 16;

// Wifi parameters
byte        mac[6];
const char* ssid     = "YOUR WIFI NAME";
const char* password = "YOUR WIFI PASSWORD";
WiFiClient wifiClient;

// Nightscout URL followed by /api/v1/entries/current.json
const String nightscoutUrl = "YOUR_NIGHTSCOUT_URL/api/v1/entries/current.json"; // Should be something like: https://glucose.herokuapp.com/api/v1/entries/current.json

// Variables
char daysOfTheWeek[7][12]      = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

const long UPDATE_INTERVAL     = 60000;
const long LOW_ALARM_INTERVAL  = 600000;
const long HIGH_ALARM_INTERVAL = 3600000;
const long OUTDATED_DURATION   = 4250;//650;
const int  URGENT_LOW_VALUE    = 55;
const int  LOW_VALUE           = 70;
const int  HIGH_VALUE_DAY      = 250;
const int  HIGH_VALUE_NIGHT    = 350;
const int  BRIGHTNESS_DAY      = 3;
const int  BRIGHTNESS_NIGHT    = 0;
const int  BRIGHTNESS_ALARM    = 15;
long       lastHttpRequest     = 60001;
long       lastAlarm           = 0;
bool       alarmOngoing        = false;
long       lastAlarmPlayed     = 1800001;
bool       displayOn           = true;
bool       tooHigh             = false;
bool       high                = false;
bool       low                 = false;
bool       tooLow              = false;
bool       urgentLow           = false;
bool       valueIsOld          = false;
int        brightness          = 0;


void setup() {
  Serial.begin(115200);
  delay(10);

  // Get MAC address
  WiFi.macAddress(mac);
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());

  // Connect to WiFi network
  WiFi.begin(ssid, password);
  Serial.print("\n\r \n\rWorking to connect");
 
  //Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  } 

  Serial.println("");
  Serial.println("ESP8266_2 GlumonMatrix");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
  Serial.println();
 
  // Matrix panel
  Serial.println("\nLED matrix modul");
  mx.begin();

  // Realtime clock
  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, lets set the time!");
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }

  // Mute Button
  pinMode(MUTE_BUTTON_PIN, INPUT);  
 
  delay(1000);
}



void loop() {
  DateTime now       = rtc.now();
  bool     isWeekend = now.dayOfTheWeek() == 0 || now.dayOfTheWeek() == 6;
  int      dayStart  = isWeekend ? 9 : 6;
  int      dayEnd    = isWeekend ? 22 : 21;
  bool     isDay     = now.hour() > dayStart && now.hour() < dayEnd;

  bool     mute      = digitalRead(MUTE_BUTTON_PIN) == HIGH;

  bool     rising    = false;
  bool     falling   = false;

 
  if (millis() - lastHttpRequest > UPDATE_INTERVAL) {
        HTTPClient http;
        if (http.begin(wifiClient, nightscoutUrl)) {  // HTTP

          // start connection and send HTTP header
          int httpCode = http.GET();

          // httpCode will be negative on error
          if (httpCode > 0) {
            // HTTP header has been send and Server response header has been handled
            //Serial.printf("[HTTP] GET... code: %d\n", httpCode);

            // file found at server
            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
              String payload = http.getString();
              //Serial.println("Payload: " + payload);

              DynamicJsonDocument doc(512);
              DeserializationError error = deserializeJson(doc, payload);
              if (error) {
                Serial.println("deserializationJson() failed");
                return;
              }
              JsonObject jsonObject    = doc[0];
              String     glucoseString = jsonObject["sgv"].as<String>();              
              int        glucose       = glucoseString.toInt();
              String     trend         = jsonObject["direction"].as<String>();
              String     dateString    = jsonObject["dateString"].as<String>();
              long long  date          = jsonObject["date"];
              int        utcOffset     = jsonObject["utcOffset"];             

              if (trend == "TripleUp" || trend == "DoubleUp" || trend == "SingleUp" || trend == "FortyFiveUp") {
                rising  = true;
                falling = false;
              } else if (trend == "FortyFiveDown" || trend == "SingleDown" || trend == "DoubleDown" || trend == "TripleDown") {
                rising  = false;
                falling = true;
              } else {
                rising  = false;
                falling = false;
              }
              
              date = date / 1000LL + (utcOffset * 60);
              long long  delta = ((long long) now.unixtime()) - date;

              valueIsOld = delta > OUTDATED_DURATION;
              tooHigh    = isDay ? glucose > HIGH_VALUE_DAY : glucose > HIGH_VALUE_NIGHT;
              high       = glucose > HIGH_VALUE_DAY && glucose <= HIGH_VALUE_NIGHT;
              tooLow     = glucose < LOW_VALUE;
              urgentLow  = glucose < URGENT_LOW_VALUE;
         
              if (tooHigh || tooLow) {
                lastAlarm    = millis();
                alarmOngoing = true;
              } else {
                alarmOngoing = false;
              }

              mx.clear(0, MAX_DEVICES-1);
                            
              int length = glucoseString.length();
              if (3 == length) {
                if (tooHigh) {                 
                  if (alarmOngoing && (millis() - lastAlarmPlayed) > HIGH_ALARM_INTERVAL) {
                    // Play high alarm
                    lastAlarmPlayed = millis();
                    brightness = falling ? (isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT) : BRIGHTNESS_ALARM;
                  } else {
                    brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;
                  }
                } else if (high) {
                  brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;
                } else {
                  brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;
                }
                mx.control(MD_MAX72XX::INTENSITY, brightness);
                String msg = "   " + glucoseString;
                msg.toCharArray(message, BUF_SIZE);
                printText(0, MAX_DEVICES-1, message, 2);
              } else if (2 == length) {
                if (tooLow) {
                  if (alarmOngoing && (millis() - lastAlarmPlayed) > LOW_ALARM_INTERVAL) {
                    if (urgentLow) { 
                      //Play low alarm
                      alarmBeep();
                    }
                    lastAlarmPlayed = millis();
                    brightness = rising ? (isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT) : BRIGHTNESS_ALARM;
                  } else {                    
                    brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;
                  }
                } else {
                  brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;
                }
                mx.control(MD_MAX72XX::INTENSITY, brightness);
                String msg = "     " + glucoseString;
                msg.toCharArray(message, BUF_SIZE);
                printText(0, MAX_DEVICES-1, message, 1);
              } else if (1 == length) {                
                brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;
                mx.control(MD_MAX72XX::INTENSITY, brightness);
                String msg = "     " + glucoseString;
                msg.toCharArray(message, BUF_SIZE);
                printText(0, MAX_DEVICES-1, message, 1);
              }
              
              if (valueIsOld) {
                oldValue();
              } else if (trend == "TripleUp") {
                doubleUp();
              } else if (trend == "DoubleUp") {
                doubleUp();
              } else if (trend == "SingleUp") {
                singleUp();
              } else if (trend == "FortyFiveUp") {
                fortyFiveUp();
              } else if (trend == "Flat") {
                flat();
              } else if (trend == "FortyFiveDown") {
                fortyFiveDown();
              } else if (trend == "SingleDown") {
                singleDown();
              } else if (trend == "DoubleDown") {
                doubleDown();
              } else if (trend == "TripleDown") {
                doubleDown();
              }
              delay(200);
            }
          } else {
            Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
          }
          http.end();
          lastHttpRequest = millis();
      }
    }
    
    if ((tooHigh && !falling) || (tooLow && !rising)) {
        displayOn = !displayOn;
    } else {
      displayOn = true;
      brightness = isDay ? BRIGHTNESS_DAY : BRIGHTNESS_NIGHT;      
    }
    mx.control(MD_MAX72XX::INTENSITY, displayOn ? brightness : 0);
    mx.control(MD_MAX72XX::SHUTDOWN, !displayOn);
    delay(1000);
}


void alarmBeep() {
  for (int i = 0 ; i < 5 ; i++) {
    tone(ALARM_BUZZER_PIN, 4000);   
    delay(500);
    noTone(ALARM_BUZZER_PIN);
    delay(500);
  }
}


// Print text
void printText(uint8_t modStart, uint8_t modEnd, char *pMsg, uint8_t offset) {
  uint8_t   state = 0;
  uint8_t   curLen;
  uint16_t  showLen;
  uint8_t   cBuf[8];
  int16_t   col = ((modEnd + 1) * COL_SIZE) - offset;

  mx.control(modStart, modEnd, MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);

  do {
    switch(state) {
      case 0: 
        // Load the next character from the font table
        // if we reached end of message, reset the message pointer
        if (*pMsg == '\0') {
          showLen = col - (modEnd * COL_SIZE);  // padding characters
          state = 2;
          break;
        }

        // retrieve the next character form the font file
        showLen = mx.getChar(*pMsg++, sizeof(cBuf)/sizeof(cBuf[0]), cBuf);
        curLen = 0;
        state++;
        // !! deliberately fall through to next state to start displaying

      case 1: 
        // display the next part of the character
        mx.setColumn(col--, cBuf[curLen++]);

        // done with font character, now display the space between chars
        if (curLen == showLen) {
          showLen = CHAR_SPACING;
          state = 2;
        }
        break;

      case 2: 
        // initialize state for displaying empty columns
        curLen = 0;
        state++;
        // fall through

      case 3:  
        // display inter-character spacing or end of message padding (blank columns)
        mx.setColumn(col--, 0);
        curLen++;
        if (curLen == showLen)
          state = 0;
        break;

      default:
        col = -1;   // this definitely ends the do loop
    }
  } while (col >= (modStart * COL_SIZE));

  mx.control(modStart, modEnd, MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

// Trend indicators
void doubleUp() {
    mx.setPoint(2, 31, true);
    mx.setPoint(1, 30, true);
    mx.setPoint(0, 29, true);
    mx.setPoint(1, 29, true);
    mx.setPoint(2, 29, true);
    mx.setPoint(3, 29, true);
    mx.setPoint(4, 29, true);
    mx.setPoint(5, 29, true);
    mx.setPoint(6, 29, true);
    mx.setPoint(1, 28, true);
    mx.setPoint(2, 27, true);

    mx.setPoint(2, 26, true);
    mx.setPoint(1, 25, true);
    mx.setPoint(0, 24, true);
    mx.setPoint(1, 24, true);
    mx.setPoint(2, 24, true);
    mx.setPoint(3, 24, true);
    mx.setPoint(4, 24, true);
    mx.setPoint(5, 24, true);
    mx.setPoint(6, 24, true);
    mx.setPoint(1, 23, true);
    mx.setPoint(2, 22, true);
}

void singleUp() {
    mx.setPoint(2, 31, true);
    mx.setPoint(1, 30, true);
    mx.setPoint(0, 29, true);
    mx.setPoint(1, 29, true);
    mx.setPoint(2, 29, true);
    mx.setPoint(3, 29, true);
    mx.setPoint(4, 29, true);
    mx.setPoint(5, 29, true);
    mx.setPoint(6, 29, true);
    mx.setPoint(1, 28, true);
    mx.setPoint(2, 27, true);
}

void fortyFiveUp() {
    mx.setPoint(6, 31, true);
    mx.setPoint(5, 30, true);
    mx.setPoint(4, 29, true);
    mx.setPoint(3, 28, true);
    mx.setPoint(2, 27, true);
    mx.setPoint(1, 26, true);
    mx.setPoint(0, 25, true);
    mx.setPoint(0, 26, true);
    mx.setPoint(0, 27, true);
    mx.setPoint(1, 25, true);
    mx.setPoint(2, 25, true);
}

void flat() {
    mx.setPoint(3, 31, true);
    mx.setPoint(3, 30, true);
    mx.setPoint(3, 29, true);
    mx.setPoint(3, 28, true);
    mx.setPoint(3, 27, true);
    mx.setPoint(3, 26, true);
    mx.setPoint(3, 25, true);
    mx.setPoint(2, 26, true);
    mx.setPoint(1, 27, true);
    mx.setPoint(4, 26, true);
    mx.setPoint(5, 27, true);
}

void fortyFiveDown() {
    mx.setPoint(0, 31, true);
    mx.setPoint(1, 30, true);
    mx.setPoint(2, 29, true);
    mx.setPoint(3, 28, true);
    mx.setPoint(4, 27, true);
    mx.setPoint(5, 26, true);
    mx.setPoint(6, 25, true);
    mx.setPoint(6, 26, true);
    mx.setPoint(6, 27, true);
    mx.setPoint(5, 25, true);
    mx.setPoint(4, 25, true);
}

void singleDown() {
    mx.setPoint(4, 31, true);
    mx.setPoint(5, 30, true);
    mx.setPoint(0, 29, true);
    mx.setPoint(1, 29, true);
    mx.setPoint(2, 29, true);
    mx.setPoint(3, 29, true);
    mx.setPoint(4, 29, true);
    mx.setPoint(5, 29, true);
    mx.setPoint(6, 29, true);
    mx.setPoint(5, 28, true);
    mx.setPoint(4, 27, true);
}

void doubleDown() {
    mx.setPoint(4, 31, true);
    mx.setPoint(5, 30, true);
    mx.setPoint(0, 29, true);
    mx.setPoint(1, 29, true);
    mx.setPoint(2, 29, true);
    mx.setPoint(3, 29, true);
    mx.setPoint(4, 29, true);
    mx.setPoint(5, 29, true);
    mx.setPoint(6, 29, true);
    mx.setPoint(5, 28, true);
    mx.setPoint(4, 27, true);

    mx.setPoint(4, 26, true);
    mx.setPoint(5, 25, true);
    mx.setPoint(0, 24, true);
    mx.setPoint(1, 24, true);
    mx.setPoint(2, 24, true);
    mx.setPoint(3, 24, true);
    mx.setPoint(4, 24, true);
    mx.setPoint(5, 24, true);
    mx.setPoint(6, 24, true);
    mx.setPoint(5, 23, true);
    mx.setPoint(4, 22, true);
}

void oldValue() {
    mx.setPoint(0, 28, true);
    mx.setPoint(1, 28, true);
    mx.setPoint(2, 28, true);
    mx.setPoint(3, 28, true);
    mx.setPoint(4, 28, true);
    mx.setPoint(6, 28, true);
}
