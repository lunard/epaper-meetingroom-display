#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include "time.h"

#include <SPIFFS.h>
#include <ArduinoJson.h>

// e-Ink resolution 960x540 pixel

String baseAPIUrl = "https://meetingroominfo.testingmachine.eu/";
DynamicJsonDocument doc(1024);
char prettyJsonSensorData[512];

class CalendarEvent {
public:
  String Title;
  String Organizer;
  String StartAt;
  String EndAt;
  bool bookedByLabel;
  String ToString() {
    return Title + Organizer + StartAt + EndAt;
  }
};

class SensorData {
public:
  int CO2;
  float temperature;
  float humidity;
};

class Room {
public:
  String email;
  String displayName;
  String location;
};

int timeToNextEvent = 0;
bool isFree = true;
bool wasFree = false;
bool nextEventFound = false;

bool roomDataUpdated = false;
bool roomStatusChanged = true;
bool isFirstRoomDataUpdate = true;

String wifiSSID = "openAiR";

CalendarEvent *currentEvent = new CalendarEvent();
CalendarEvent *nextEvent = new CalendarEvent();
SensorData *sensorData = new SensorData();
Room *associatedRoom = new Room();

// Humidity and temperature
char temStr[10];
char humStr[10];
float tem;
float hum;

WiFiUDP ntpUDP;
#define NTP_OFFSET 60 * 60 * 2  // In seconds
NTPClient timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET);
rtc_time_t RTCtime;

M5EPD_Canvas centerCanvas(&M5.EPD);
M5EPD_Canvas sensorsCanvas(&M5.EPD);
M5EPD_Canvas buttonsCanvas(&M5.EPD);

bool detectTouch = true;
tp_finger_t lastTouch;

bool bookingRoomAreaShown = false;
long bookingRoomAreaShownElapsedSeconds = 0;

bool button30MinEnabled = true;
bool button45MinEnabled = true;

long lastReboot = -1;
int rebootTimeoutInHours = 2;

// Function to read status from URL
void QueryRoomStatusTask(void *parameter) {
  for (;;)  // infinite loop
  {
    QueryRoomStatus();
    QuerySensorData();
    vTaskDelay(2000 / portTICK_PERIOD_MS);  // delay for 30 sec
  }
}

void QueryRoomStatus() {
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/status");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println(payload);
    deserializeJson(doc, payload);

    String currentStatus = isFree ? "yes" : "no" + currentEvent->ToString() + nextEvent->ToString();

    isFree = doc["isFree"];
    timeToNextEvent = doc["timeToNextEvent"];

    JsonVariant ce = doc["currentEvent"];
    if (ce.isNull()) {
      isFree = true;
    } else {
      isFree = false;
      currentEvent->Title = ce["title"].as<String>();
      currentEvent->Organizer = ce["organizer"].as<String>();
      currentEvent->StartAt = ce["startAt"].as<String>();
      currentEvent->EndAt = ce["endAt"].as<String>();
      currentEvent->bookedByLabel = ce["bookedByLabel"].as<bool>();
    }

    JsonVariant ne = doc["nextEvent"];
    if (ne.isNull()) {
      nextEventFound = false;
    } else {
      nextEventFound = true;
      nextEvent->Title = ne["title"].as<String>();
      nextEvent->Organizer = ne["organizer"].as<String>();
      nextEvent->StartAt = ne["startAt"].as<String>();
      nextEvent->EndAt = ne["endAt"].as<String>();
      // nextEvent->bookedByLabel = ne["bookedByLabel"].as<bool>();
    }

    String newStatus = isFree ? "yes" : "no" + currentEvent->ToString() + nextEvent->ToString();

    roomStatusChanged = isFirstRoomDataUpdate || currentStatus != newStatus;
    roomDataUpdated = true;
    isFirstRoomDataUpdate = false;
  } else {
    String payload = http.getString();
    Serial.println("QueryRoomStatus error:" + String(httpCode) + ", " + payload);
  }
  http.end();
}

void SetupWiFiTask() {
  // Create a task that will be executed in the QueryRoomStatus function, with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
    QueryRoomStatusTask,   /* Function to implement the task */
    "QueryRoomStatusTask", /* Name of the task */
    10000,                 /* Stack size in words */
    NULL,                  /* Task input parameter */
    1,                     /* Priority of the task */
    NULL,                  /* Task handle. */
    1);                    /* Core where the task should run */
}

void RefreshSensorArea() {
  sensorsCanvas.setTextColor(BLACK);
  sensorsCanvas.setTextSize(3);

  sensorsCanvas.clear();
  sensorsCanvas.fillRect(0, 0, 200, 540, WHITE);

  // Refresh Time
  sensorsCanvas.drawString(String("00" + String(RTCtime.hour)).substring(String(RTCtime.hour).length()) + ":" + String("00" + String(RTCtime.min)).substring(String(RTCtime.min).length()), 50, 20);


  if (sensorData->CO2 > 1200) {
    sensorsCanvas.drawPngFile(SPIFFS, "/CO2-warning-reverse-64.png", 25, 90);
  } else {
    sensorsCanvas.drawPngFile(SPIFFS, "/CO2-reverse-64.png", 25, 90);
  }
  sensorsCanvas.drawString(String(sensorData->CO2), 100, 110);

  sensorsCanvas.drawPngFile(SPIFFS, "/temperature-reverse-64.png", 25, 230);
  sensorsCanvas.drawString(String((int)sensorData->temperature), 95, 250);

  sensorsCanvas.drawPngFile(SPIFFS, "/humidity-reverse-64.png", 25, 380);
  sensorsCanvas.drawString(String((int)sensorData->humidity), 95, 400);

  sensorsCanvas.pushCanvas(0, 0, UPDATE_MODE_DU);
  Serial.println("DrawSensorArea done");
}

void DrawButtons() {
  buttonsCanvas.fillRect(0, 0, 160, 540, WHITE);

  if (isFree) {
    if (timeToNextEvent > 15)
      buttonsCanvas.drawPngFile(SPIFFS, "/add-event-reverse-64.png", 40, 170);
  } else {
    if (currentEvent->bookedByLabel)
      buttonsCanvas.drawPngFile(SPIFFS, "/delete-event-reverse-64.png", 40, 170);
  }
  buttonsCanvas.pushCanvas(800, 0, UPDATE_MODE_DU);
  Serial.println("DrawButtons done");
}

void CleanCenterCanvas() {
  centerCanvas.fillRect(0, 0, 600, 540, WHITE);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);
}

void DrawSeparatorLines() {
  centerCanvas.drawLine(0, 0, 0, 540, BLACK);      // left vertical separator line
  centerCanvas.drawLine(599, 0, 599, 540, BLACK);  // right vertical separator line
  centerCanvas.drawLine(0, 340, 599, 340, BLACK);  // horizontal separator line
}

void DrawRoomTitle() {
  centerCanvas.setTextSize(2);
  centerCanvas.setTextColor(BLACK);
  centerCanvas.drawString(associatedRoom->displayName, 30, 20);
}
void DrawRoomData() {

  ReadRoomInfo();

  if (associatedRoom != NULL) {
    DrawRoomTitle();
    // Serial.println("Update room name with: " + associatedRoom->displayName);
  } else {
    Serial.println("Associated room is NULL");
  }

  Serial.println("DrawRoomData done");
}

void ReadRoomInfo() {
  // TODO: get the info to the backend

  HTTPClient http;
  http.begin(baseAPIUrl + "api/room");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println(payload);
    deserializeJson(doc, payload);

    associatedRoom->email = doc["email"].as<String>();
    associatedRoom->displayName = doc["displayName"].as<String>();
    associatedRoom->location = doc["location"].as<String>();
  } else {
    String payload = http.getString();
    Serial.println("ReadRoomInfo error:" + String(httpCode) + ", " + payload);
  }
  http.end();

  Serial.println("ReadRoomInfo done");
}

void ReDrawCenterCanvas(bool strong = false) {

  const m5epd_update_mode_t refreshMode = strong ? UPDATE_MODE_GL16 : UPDATE_MODE_DU;
  DrawRoomTitle();
  DrawSeparatorLines();
  centerCanvas.pushCanvas(200, 0, refreshMode);
  //M5.EPD.UpdateArea(200, 0, 600, 540, UPDATE_MODE_GL16);
}

void QuerySensorData() {
  Serial.println("-- QuerySensorData");
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/airquality");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    deserializeJson(doc, payload);

    sensorData->CO2 = doc["cO2"].as<int>();
    sensorData->temperature = doc["temperature"].as<float>();
    sensorData->humidity = doc["humidity"].as<float>();

    serializeJsonPretty(doc, prettyJsonSensorData);
    Serial.println(prettyJsonSensorData);
  } else {
    String payload = http.getString();
    Serial.println("QuerySensorData error:" + String(httpCode) + ", " + payload);
  }
  http.end();
}

void RefreshCurrentEvent() {
  if (!roomDataUpdated) {
    Serial.println("RefreshCurrentEvent: data not changed");
    return;
  }

  if (isFree) {
    if (!wasFree) {
      // already drew
      buttonsCanvas.fillRect(0, 0, 160, 340, WHITE);  // cleanup upper button area
      centerCanvas.fillRect(0, 80, 600, 260, WHITE);  // cleanup 'current event' area

      centerCanvas.setTextColor(BLACK);
      centerCanvas.setTextSize(5);

      centerCanvas.setFreeFont(&FreeSansBold12pt7b);
      centerCanvas.drawString("Free", 180, 140);
      centerCanvas.setFreeFont(&FreeSerif12pt7b);
    }

    wasFree = true;
  } else {
    if (wasFree) {
      // cleanup
      buttonsCanvas.fillRect(0, 0, 160, 340, WHITE);  // cleanup upper button area
      centerCanvas.fillRect(0, 80, 600, 260, WHITE);  // cleanup 'current event' area
    }

    centerCanvas.setTextColor(BLACK);
    centerCanvas.setTextSize(2);
    if (currentEvent->Title.length() > 20)
      centerCanvas.drawString(currentEvent->Title.substring(0, 20) + "..", 30, 130);
    else
      centerCanvas.drawString(currentEvent->Title, 30, 130);

    centerCanvas.setTextSize(1);
    centerCanvas.drawString(currentEvent->Organizer, 30, 200);

    Serial.println("RefreshCurrentEvent: updated current event");

    DrawCurrenEventTime();

    wasFree = false;
  }

  ReDrawCenterCanvas();
  buttonsCanvas.pushCanvas(800, 0, UPDATE_MODE_DU);
  Serial.println("RefreshCurrentEvent done");
}

void RefreshNextEvent() {
  if (bookingRoomAreaShown)  // this area is used for booking the room
    return;

  centerCanvas.setTextColor(BLACK);
  centerCanvas.setTextSize(1);
  // cleanup
  centerCanvas.fillRect(0, 340, 600, 200, WHITE);
  if (nextEventFound) {
    if (nextEvent->Title.length() > 20)
      centerCanvas.drawString(nextEvent->Title.substring(0, 20) + "..", 30, 370);
    else
      centerCanvas.drawString(nextEvent->Title, 30, 370);
    centerCanvas.drawString(nextEvent->Organizer, 30, 440);
    centerCanvas.drawString(nextEvent->StartAt + " - " + nextEvent->EndAt, 30, 480);
  }
  ReDrawCenterCanvas();
}

void DrawTimeToNextEvent() {
  if (!isFree)
    return;
  // cleanup string
  centerCanvas.fillRect(110, 280, 399, 40, WHITE);

  centerCanvas.setTextColor(BLACK);
  centerCanvas.setTextSize(2);

  centerCanvas.drawString("Free for " + String(timeToNextEvent) + " minutes", 110, 280);
  centerCanvas.drawPngFile(SPIFFS, "/free-for-clock-reverse-64.png", 15, 265);
  Serial.println("DrawTimeToNextEvent done: " + String(timeToNextEvent) + " minutes");

  ReDrawCenterCanvas();
}

void DrawCurrenEventTime() {
  // cleanup string
  centerCanvas.fillRect(5, 280, 600, 40, WHITE);

  centerCanvas.setTextColor(BLACK);
  centerCanvas.setTextSize(2);

  centerCanvas.drawPngFile(SPIFFS, "/starts-at-reverse-64.png", 15, 265);
  centerCanvas.drawPngFile(SPIFFS, "/ends-at-reverse-64.png", 400, 265);
  centerCanvas.drawString(currentEvent->StartAt, 100, 280);
  centerCanvas.drawString(currentEvent->EndAt, 475, 280);
  Serial.println("DrawCurrenEventTime done: " + currentEvent->StartAt + " - " + currentEvent->EndAt);
}

void InizializeLabel() {

  // See Font list here https://learn.adafruit.com/adafruit-gfx-graphics-library/using-fonts
  centerCanvas.setFreeFont(&FreeSerif12pt7b);

  CleanCenterCanvas();

  DrawRoomData();
  RefreshSensorArea();

  ReDrawCenterCanvas();

  Serial.println("InizializeLabel done");
}

void DrawBookRoomArea() {
  if (!isFree || timeToNextEvent < 15)
    return;

  centerCanvas.fillRect(0, 340, 600, 200, WHITE);

  centerCanvas.setTextColor(BLACK);
  centerCanvas.setTextSize(3);

  centerCanvas.drawRect(40, 370, 130, 130, BLACK);
  centerCanvas.drawString("15", 55, 390);

  centerCanvas.drawRect(230, 370, 130, 130, BLACK);
  centerCanvas.drawString("30", 255, 390);
  if (timeToNextEvent < 30) {
    button30MinEnabled = false;
    centerCanvas.drawLine(230, 370, 360, 500, BLACK);
  } else
    button30MinEnabled = true;

  centerCanvas.drawRect(420, 370, 130, 130, BLACK);
  centerCanvas.drawString("45", 445, 390);
  if (timeToNextEvent < 45) {
    button45MinEnabled = false;
    centerCanvas.drawLine(420, 370, 550, 500, BLACK);
  } else
    button45MinEnabled = true;

  centerCanvas.setTextSize(2);
  centerCanvas.drawString("min", 65, 455);
  centerCanvas.drawString("min", 255, 455);
  centerCanvas.drawString("min", 445, 455);

  ReDrawCenterCanvas();
}

void DisplayOperationMessage(String operationMessage) {
  // already drew
  centerCanvas.fillRect(0, 0, 600, 540, WHITE);  // cleanup

  centerCanvas.setTextColor(BLACK);
  centerCanvas.setTextSize(2);

  centerCanvas.drawString(operationMessage, 30, 140);

  ReDrawCenterCanvas();
}

void HideBookRoomArea() {
  centerCanvas.fillRect(1, 341, 599, 539, WHITE);
  RefreshNextEvent();

  ReDrawCenterCanvas(true);
  Serial.println("HideBookRoomArea done");
}

void SaveS3IconOnSPIFFS(String s3Filename) {
  HTTPClient http;
  http.begin("https://codethecat-public.s3.eu-west-1.amazonaws.com/door-signage/" + s3Filename);
  http.setUserAgent("ESP32");                                              // Set user agent
  http.addHeader("Host", "codethecat-public.s3.eu-west-1.amazonaws.com");  // Add host header

  int httpCode = http.GET();

  if (httpCode == 200) {
    File file = SPIFFS.open("/" + s3Filename, FILE_WRITE);
    if (!file) {
      Serial.println("Failed to open file for writing");
      return;
    }

    WiFiClient *stream = http.getStreamPtr();
    int count = 0;
    if (!http.connected()) {
      Serial.println("Not connected!");
    }

    while (stream->available() == 0) {
      Serial.println("Zero bytes availables");
      delay(100);
    }

    while (http.connected() && (stream->available() > 0)) {
      char c = stream->read();
      file.write(c);
      count++;
    }

    file.close();
    Serial.println("downloadImage: saved image '" + String(s3Filename) + "' (" + String(count) + " bytes)");
  } else {
    String payload = http.getString();
    Serial.println("SaveS3IconOnSPIFFS error:" + String(httpCode) + ", " + payload);
  }

  http.end();
}

void Refresh() {
  QuerySensorData();
  RefreshCurrentEvent();
  DrawButtons();
  RefreshNextEvent();
  RefreshSensorArea();
}

void StartDemo() {

  Serial.println("Start demo");

  isFree = true;
  timeToNextEvent = 5;
  roomDataUpdated = true;

  sensorData->CO2 = 450;

  // Simulate the loop logic
  Refresh();

  // ----
  for (int i = timeToNextEvent; i > 0; i--) {
    delay(1000);

    // Simulate the loop logic
    timeToNextEvent--;
    roomDataUpdated = true;

    sensorData->CO2 = 450 + random(1, 30);

    Refresh();
  }

  // ----
  isFree = false;
  currentEvent->Title = "Test this beautiful label";
  currentEvent->Organizer = "lunard@gmai.com";
  currentEvent->StartAt = "10:15";
  currentEvent->EndAt = "10:30";

  sensorData->CO2 = 450 + random(1, 30);

  Refresh();
  delay(1000);

  // ----
  nextEventFound = true;
  nextEvent->Title = "Check the sensor's data";
  nextEvent->Organizer = "mario.rossi@gmai.com";
  nextEvent->StartAt = "12:25";
  nextEvent->EndAt = "13:10";

  sensorData->CO2 = 450 + random(1, 30);

  Refresh();
  delay(1000);

  // ----
  nextEvent->Title = "And now a a drink !!";
  nextEvent->Organizer = "mario.rossi@gmai.com";
  nextEvent->StartAt = "19:00";
  nextEvent->EndAt = "19:30";

  sensorData->CO2 = 450 + random(1, 30);

  Refresh();
  delay(1000);

  sensorData->CO2 = 1124;

  Refresh();
  delay(1000);

  // ----
  sensorData->CO2 = 915;

  Refresh();
  delay(1000);

  roomDataUpdated = false;
  Serial.println("Demo ended");
}

bool IsAddOrDeleteCalendarEventButtonClicked(tp_finger_t fingerItem) {
  // Button Area: 802, 0, 158, 340
  return fingerItem.x >= 802 && fingerItem.x < 960 && fingerItem.y >= 0 && fingerItem.y < 340;
}

bool Is15MinButtonClicked(tp_finger_t fingerItem) {
  // Button Area: 240, 370, 370, 500
  return fingerItem.x >= 240 && fingerItem.x < 370 && fingerItem.y >= 370 && fingerItem.y < 500;
}

bool Is30MinButtonClicked(tp_finger_t fingerItem) {
  // Button Area: 430, 370, 560, 500
  return fingerItem.x >= 430 && fingerItem.x < 560 && fingerItem.y >= 370 && fingerItem.y < 500;
}

bool Is45MinButtonClicked(tp_finger_t fingerItem) {
  // Button Area: 620, 370, 750, 500
  return fingerItem.x >= 620 && fingerItem.x < 750 && fingerItem.y >= 370 && fingerItem.y < 500;
}

void SetupRTC() {

  M5.RTC.begin();

  timeClient.begin();
  timeClient.update();

  //Get a time structure
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime((time_t *)&epochTime);
  int currentMonth = ptm->tm_mon + 1;
  int monthDay = ptm->tm_mday;

  if (
    (currentMonth >= 10 && monthDay >= 27 && timeClient.getHours() >= 3)       // on 31 march 2024, at 02:00 clocks were turned forward 1 hour
    || (currentMonth <= 3 && monthDay <= 31 && timeClient.getHours() <= 2)) {  // on 27 October at 03:00 clocks are turned backward 1 hour
    timeClient.setTimeOffset(NTP_OFFSET / 2);
    timeClient.update();
  }

  RTCtime.hour = timeClient.getHours();
  RTCtime.min = timeClient.getMinutes();
  RTCtime.sec = timeClient.getSeconds();

  M5.RTC.setTime(&RTCtime);
  Serial.println("SetupRTC done");
}

long GetRTCTimeAsTotalSeconds() {
  return RTCtime.hour * 60 * 60 + RTCtime.min * 60 + RTCtime.sec;
}

void RebootedIfNeeded() {

  long elapsed = GetRTCTimeAsTotalSeconds() - lastReboot;

  if (lastReboot > 0 && elapsed > 60 * 60 * rebootTimeoutInHours) {
    lastReboot = GetRTCTimeAsTotalSeconds();
    DisplayOperationMessage("Rebooting..");
    delay(1000);
    ESP.restart();
  }
}

bool BookTheRoom(int duration) {
  Serial.println("Book the room for " + String(duration) + "minutes");
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/book/" + String(duration));
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  return httpCode == 200;
}

bool DeleteRoomBooking() {
  Serial.println("Delete Room booking");
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/book");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.sendRequest("DELETE");
  return httpCode == 200;
}

void setup() {
  M5.begin();

  M5.EPD.SetRotation(0);
  M5.TP.SetRotation(0);
  M5.EPD.Clear(true);
  M5.EPD.SetColorReverse(true);
  M5.EPD.UpdateArea(0, 0, 960, 540, UPDATE_MODE_DU);


  // Setup Canvas!
  // ---------------------
  // |  |             |  |
  // |  |             |  |
  // ---------------------
  // 0  200         800  960

  sensorsCanvas.createCanvas(200, 540);
  sensorsCanvas.fillCanvas(WHITE);
  sensorsCanvas.pushCanvas(0, 0, UPDATE_MODE_DU);

  centerCanvas.createCanvas(600, 540);
  centerCanvas.fillCanvas(WHITE);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  buttonsCanvas.createCanvas(160, 540);
  buttonsCanvas.fillCanvas(WHITE);
  buttonsCanvas.pushCanvas(800, 0, UPDATE_MODE_DU);

  randomSeed(analogRead(0));

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  centerCanvas.setTextColor(BLACK);
  centerCanvas.setTextSize(2);
  centerCanvas.drawString("NOI Tech Park - IoT Door Signage", 10, 20);
  centerCanvas.drawString("Connect to the WiFi '" + wifiSSID + "' ..", 10, 100);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  WiFi.begin(wifiSSID, "");
  Serial.print("Connect to the WiFi '" + wifiSSID + "'");
  int wifiCount = 0;

  while (WiFi.status() != WL_CONNECTED && wifiCount < 15) {
    delay(500);
    Serial.print(".");
    wifiCount++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Reboot the device
    centerCanvas.drawString("cannot connect..rebooting!", 600, 100);
    centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);
    Serial.println("cannot connect..rebooting!");
    ESP.restart();
  }

  Serial.println("connected!");

  centerCanvas.drawString("connected!", 400, 100);
  centerCanvas.drawString("Downloading icons on SPIFFS", 10, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  int x = 380;
  SaveS3IconOnSPIFFS("starts-at-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("ends-at-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("CO2-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("CO2-warning-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("temperature-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("humidity-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("add-event-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("delete-event-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  x = x + 10;
  SaveS3IconOnSPIFFS("free-for-clock-reverse-64.png");
  centerCanvas.drawString(".", x, 150);
  centerCanvas.pushCanvas(200, 0, UPDATE_MODE_DU);

  SetupWiFiTask();

  SetupRTC();

  InizializeLabel();

  // canvas.drawString("Ready .. let's start a quick demo!", 10, 200);
  // canvas.pushCanvas(200, 0, UPDATE_MODE_DU);
  // StartDemo();
}

void loop() {

  M5.update();
  if (M5.BtnP.wasPressed()) {
    ESP.restart();
  }

  M5.RTC.getTime(&RTCtime);

  RebootedIfNeeded();

  if (M5.TP.available()) {
    if (!M5.TP.isFingerUp()) {
      M5.TP.update();
      tp_finger_t fingerItem = M5.TP.readFinger(0);

      if (lastTouch.x != fingerItem.x || lastTouch.y != fingerItem.y) {

        // Update the last finger position
        lastTouch.x = fingerItem.x;
        lastTouch.y = fingerItem.y;

        if (bookingRoomAreaShown) {
          bool booked = false;
          int duration = 0;
          if (Is15MinButtonClicked(fingerItem)) {
            duration = 15;
          } else if (button30MinEnabled && Is30MinButtonClicked(fingerItem)) {
            duration = 30;
          } else if (button45MinEnabled && Is45MinButtonClicked(fingerItem)) {
            duration = 45;
          }

          if (duration > 0) {
            DisplayOperationMessage("Booking the room..");
            HideBookRoomArea();
            booked = BookTheRoom(duration);
            if (booked) {
              QueryRoomStatus();

              roomDataUpdated = true;
              bookingRoomAreaShown = false;

              RefreshCurrentEvent();
              RefreshNextEvent();
              DrawButtons();
              // reset
              roomDataUpdated = false;
            }
          }
        } else {

          if (IsAddOrDeleteCalendarEventButtonClicked(fingerItem)) {

            if (!isFree) {
              Serial.println("Delete Calendar Event button clicked!");
              DisplayOperationMessage("Freeup the room..");
              DeleteRoomBooking();
              QueryRoomStatus();

              roomDataUpdated = true;
              RefreshCurrentEvent();
              DrawButtons();
              roomDataUpdated = false;
            } else if (timeToNextEvent > 15) {

              Serial.println("Add Calendar Event button clicked!");

              DrawBookRoomArea();
              bookingRoomAreaShownElapsedSeconds = GetRTCTimeAsTotalSeconds();
              bookingRoomAreaShown = true;
            }
          }
        }
      }
    } else {
      // Serial.println("Touch point has not been changed!");
    }
  }

  if (roomDataUpdated) {

    if (roomStatusChanged)
      Refresh();

    DrawTimeToNextEvent();
    // Reset
    roomDataUpdated = false;
    roomStatusChanged = false;
  }

  long elapsed = GetRTCTimeAsTotalSeconds() - bookingRoomAreaShownElapsedSeconds;
  if (bookingRoomAreaShown && (!isFree || elapsed >= 6)) {
    bookingRoomAreaShown = false;
    HideBookRoomArea();
  }

  if (RTCtime.sec % 15 == 0)
    RefreshSensorArea();

  delay(20);
}