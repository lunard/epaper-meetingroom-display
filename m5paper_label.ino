#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

String baseAPIUrl = "https://meetingroominfo.testingmachine.eu/";
DynamicJsonDocument doc(1024);
char prettyJsonSensorData[512];

class CalendarEvent
{
public:
  String Title;
  String Organizer;
  String StartAt;
  String EndAt;
  bool bookedByLabel;
  String ToString()
  {
    return Title + Organizer + StartAt + EndAt;
  }
};

class SensorData
{
public:
  int CO2;
  float temperature;
  float humidity;
};

class Room
{
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
#define NTP_OFFSET 60 * 60 // In seconds
NTPClient timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET);
rtc_time_t RTCtime;

M5EPD_Canvas canvas(&M5.EPD);

bool detectTouch = true;
tp_finger_t lastTouch;

bool bookingRoomAreaShown = false;
long bookingRoomAreaShownElapsedSeconds = 0;

bool button30MinEnabled = true;
bool button45MinEnabled = true;

long lastReboot = -1;
int rebootTimeoutInHours = 2;

// Function to read status from URL
void QueryRoomStatusTask(void *parameter)
{
  for (;;) // infinite loop
  {
    QueryRoomStatus();
    QuerySensorData();
    vTaskDelay(2000 / portTICK_PERIOD_MS); // delay for 30 sec
  }
}

void QueryRoomStatus()
{
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/status");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  if (httpCode == 200)
  {
    String payload = http.getString();
    Serial.println(payload);
    deserializeJson(doc, payload);

    String currentStatus = isFree ? "yes" : "no" + currentEvent->ToString() + nextEvent->ToString();

    isFree = doc["isFree"];
    timeToNextEvent = doc["timeToNextEvent"];

    JsonVariant ce = doc["currentEvent"];
    if (ce.isNull())
    {
      isFree = true;
    }
    else
    {
      isFree = false;
      currentEvent->Title = ce["title"].as<String>();
      currentEvent->Organizer = ce["organizer"].as<String>();
      currentEvent->StartAt = ce["startAt"].as<String>();
      currentEvent->EndAt = ce["endAt"].as<String>();
      currentEvent->bookedByLabel = ce["bookedByLabel"].as<bool>();
    }

    JsonVariant ne = doc["nextEvent"];
    if (ne.isNull())
    {
      nextEventFound = false;
    }
    else
    {
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
  }
  else
  {
    Serial.println("Error on HTTP request: " + String(httpCode));
  }
  http.end();
}

void SetupWiFiTask()
{
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

void DrawSensorAndClockArea()
{
  canvas.fillRect(0, 0, 200, 540, WHITE);
}

void RefreshSensorArea()
{
  canvas.setTextColor(BLACK);
  canvas.setTextSize(2);

  // M5.SHT30.UpdateData();
  // tem = M5.SHT30.GetTemperature();
  // hum = M5.SHT30.GetRelHumidity();
  // Serial.printf("Temperature: %2.2f*C  Humidity: %0.2f%%\r\n", tem, hum);
  // dtostrf(tem, 2, 1, temStr);
  // dtostrf(hum, 2, 1, humStr);

  canvas.fillRect(0, 60, 200, 540, WHITE);
  if (sensorData->CO2 > 1200)
  {
    canvas.drawPngFile(SPIFFS, "/CO2-warning-reverse-64.png", 25, 90);
  }
  else
  {
    canvas.drawPngFile(SPIFFS, "/CO2-reverse-64.png", 25, 90);
  }
  canvas.drawString(String(sensorData->CO2), 100, 105);

  canvas.drawPngFile(SPIFFS, "/temperature-reverse-64.png", 25, 230);
  canvas.drawString(String((int)sensorData->temperature), 95, 245);

  canvas.drawPngFile(SPIFFS, "/humidity-reverse-64.png", 25, 380);
  canvas.drawString(String((int)sensorData->humidity), 95, 395);

  Serial.println("DrawSensorArea done");
}

void RefreshTime()
{
  canvas.setTextColor(BLACK);
  canvas.setTextSize(2);

  canvas.fillRect(0, 0, 200, 60, WHITE);
  canvas.drawString(String("00" + String(RTCtime.hour)).substring(String(RTCtime.hour).length()) + ":" + String("00" + String(RTCtime.min)).substring(String(RTCtime.min).length()), 50, 20);

  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
}

void DrawButtonArea()
{
  canvas.fillRect(802, 0, 158, 340, WHITE);
  canvas.fillRect(802, 340, 158, 200, BLACK);
}

void DrawButtons()
{
  if (isFree)
  {
    if (timeToNextEvent > 15)
      canvas.drawPngFile(SPIFFS, "/add-event-reverse-64.png", 840, 170);
  }
  else
  {
    if (currentEvent->bookedByLabel)
      canvas.drawPngFile(SPIFFS, "/delete-event-reverse-64.png", 840, 170);
  }
  Serial.println("DrawButtons done");
}

void DrawCalendarEventArea()
{
  canvas.fillRect(200, 0, 600, 340, WHITE);
  canvas.fillRect(200, 340, 600, 200, BLACK);
}

void DrawSeparatorLines()
{
  canvas.fillRect(200, 0, 2, 540, BLACK);   // left vertical separator line
  canvas.fillRect(800, 0, 2, 340, BLACK);   // right vertical separator line - up
  canvas.fillRect(800, 340, 2, 240, WHITE); // right vertical separator line - down
  canvas.fillRect(202, 340, 758, 2, BLACK); // right horizontal separator line
}

void DrawRoomData()
{

  ReadRoomInfo();

  if (associatedRoom != NULL)
  {
    canvas.setTextSize(2);
    canvas.setTextColor(BLACK);
    canvas.drawString(associatedRoom->displayName, 230, 20);
    // Serial.println("Update room name with: " + associatedRoom->displayName);
  }
  else
  {
    Serial.println("Associated room is NULL");
  }

  Serial.println("DrawRoomData done");
}

void ReadRoomInfo()
{
  // TODO: get the info to the backend

  HTTPClient http;
  http.begin(baseAPIUrl + "api/room");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  if (httpCode == 200)
  {
    String payload = http.getString();
    Serial.println(payload);
    deserializeJson(doc, payload);

    associatedRoom->email = doc["email"].as<String>();
    associatedRoom->displayName = doc["displayName"].as<String>();
    associatedRoom->location = doc["location"].as<String>();
  }
  else
  {
    Serial.println("Error on HTTP request: " + String(httpCode));
  }
  http.end();

  Serial.println("ReadRoomInfo done");
}

void QuerySensorData()
{
  Serial.println("-- QuerySensorData");
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/airquality");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  if (httpCode == 200)
  {
    String payload = http.getString();
    deserializeJson(doc, payload);

    sensorData->CO2 = doc["cO2"].as<int>();
    sensorData->temperature = doc["temperature"].as<float>();
    sensorData->humidity = doc["humidity"].as<float>();

    serializeJsonPretty(doc, prettyJsonSensorData);
    Serial.println(prettyJsonSensorData);
  }
  else
  {
    Serial.println("Error on HTTP request: " + String(httpCode));
  }
  http.end();
}

void RefreshCurrentEvent()
{
  if (!roomDataUpdated)
  {
    Serial.println("RefreshCurrentEvent: data not changed");
    return;
  }

  if (isFree)
  {
    if (!wasFree)
    {
      // already drew
      canvas.fillRect(802, 0, 158, 340, WHITE);  // cleanup right upper button area
      canvas.fillRect(202, 80, 590, 260, WHITE); // cleanup current event Area

      canvas.setTextColor(BLACK);
      canvas.setTextSize(5);

      canvas.setFreeFont(&FreeSansBold12pt7b);
      canvas.drawString("Free", 380, 140);
      canvas.setFreeFont(&FreeSerif12pt7b);
    }

    wasFree = true;
  }
  else
  {
    if (wasFree)
    {
      // cleanup
      canvas.fillRect(802, 0, 158, 340, WHITE);  // cleanup right upper button area
      canvas.fillRect(202, 80, 590, 260, WHITE); // cleanup current event Area
    }
    canvas.setTextColor(BLACK);
    canvas.setTextSize(2);
    canvas.drawString(currentEvent->Title, 230, 130);

    canvas.setTextSize(1);
    canvas.drawString(currentEvent->Organizer, 230, 200);

    Serial.println("RefreshCurrentEvent: updated current event");

    DrawCurrenEventTime();

    wasFree = false;
  }

  DrawButtons();
  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
  Serial.println("RefreshCurrentEvent done");
}

void RefreshNextEvent()
{
  if (bookingRoomAreaShown) // this area is used for booking the room
    return;

  canvas.setTextColor(WHITE);
  canvas.setTextSize(1);
  // cleanup
  canvas.fillRect(200, 340, 600, 200, BLACK);
  if (nextEventFound)
  {
    canvas.drawString(nextEvent->Title, 230, 370);
    canvas.drawString(nextEvent->Organizer, 230, 440);
    canvas.drawString(nextEvent->StartAt + " - " + nextEvent->EndAt, 230, 480);
  }
  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
}

void DrawTimeToNextEvent()
{
  if (!isFree)
    return;
  // cleanup string
  canvas.fillRect(310, 280, 400, 40, WHITE);

  canvas.setTextColor(BLACK);
  canvas.setTextSize(2);

  canvas.drawString("Free for " + String(timeToNextEvent) + " minutes", 310, 280);
  canvas.drawPngFile(SPIFFS, "/free-for-clock-reverse-64.png", 215, 265);
  Serial.println("DrawTimeToNextEvent done: " + String(timeToNextEvent) + " minutes");

  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
}

void DrawCurrenEventTime()
{
  // cleanup string
  canvas.fillRect(205, 280, 595, 40, WHITE);

  canvas.setTextColor(BLACK);
  canvas.setTextSize(2);

  canvas.drawPngFile(SPIFFS, "/starts-at-reverse-64.png", 215, 265);
  canvas.drawPngFile(SPIFFS, "/ends-at-reverse-64.png", 600, 265);
  canvas.drawString(currentEvent->StartAt, 300, 280);
  canvas.drawString(currentEvent->EndAt, 675, 280);
  Serial.println("DrawCurrenEventTime done: " + currentEvent->StartAt + " - " + currentEvent->EndAt);
}

void InizializeLabel()
{

  // See Font list here https://learn.adafruit.com/adafruit-gfx-graphics-library/using-fonts
  canvas.setFreeFont(&FreeSerif12pt7b);

  canvas.clear();
  canvas.fillCanvas(BLACK);

  DrawCalendarEventArea();
  DrawButtonArea();
  DrawSeparatorLines();
  DrawRoomData();
  DrawSensorAndClockArea();

  RefreshSensorArea();

  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
  Serial.println("InizializeLabel done");
}

void DrawBookRoomArea()
{
  if (!isFree || timeToNextEvent < 15)
    return;

  canvas.fillRect(200, 340, 600, 200, BLACK);

  canvas.setTextColor(BLACK);
  canvas.setTextSize(3);

  canvas.fillRect(240, 370, 130, 130, WHITE);
  canvas.drawString("15", 255, 390);

  canvas.fillRect(430, 370, 130, 130, WHITE);
  canvas.drawString("30", 455, 390);
  if (timeToNextEvent < 30)
  {
    button30MinEnabled = false;
    canvas.drawLine(430, 370, 560, 500, BLACK);
  }
  else
    button30MinEnabled = true;

  canvas.fillRect(620, 370, 130, 130, WHITE);
  canvas.drawString("45", 645, 390);
  if (timeToNextEvent < 45)
  {
    button45MinEnabled = false;
    canvas.drawLine(620, 370, 750, 500, BLACK);
  }
  else
    button45MinEnabled = true;

  canvas.setTextSize(2);
  canvas.drawString("min", 265, 455);
  canvas.drawString("min", 455, 455);
  canvas.drawString("min", 645, 455);

  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
}

void DisplayOperationMessage(String operationMessage)
{
  // already drew
  canvas.fillRect(802, 0, 158, 340, WHITE);  // cleanup right upper button area
  canvas.fillRect(202, 80, 590, 260, WHITE); // cleanup current event Area

  canvas.setTextColor(BLACK);
  canvas.setTextSize(2);

  canvas.drawString(operationMessage, 230, 140);

  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
}

void HideBookRoomArea()
{
  canvas.fillRect(200, 340, 600, 200, BLACK);
  canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);

  RefreshNextEvent();
  Serial.println("HideBookRoomArea done");
}

void SaveS3IconOnSPIFFS(String s3Filename)
{
  HTTPClient http;
  http.begin("https://codethecat-public.s3.eu-west-1.amazonaws.com/door-signage/" + s3Filename);
  http.setUserAgent("ESP32");                                             // Set user agent
  http.addHeader("Host", "codethecat-public.s3.eu-west-1.amazonaws.com"); // Add host header

  int httpCode = http.GET();

  if (httpCode == 200)
  {
    File file = SPIFFS.open("/" + s3Filename, FILE_WRITE);
    if (!file)
    {
      Serial.println("Failed to open file for writing");
      return;
    }

    WiFiClient *stream = http.getStreamPtr();
    int count = 0;
    if (!http.connected())
    {
      Serial.println("Not connected!");
    }

    while (stream->available() == 0)
    {
      Serial.println("Zero bytes availables");
      delay(100);
    }

    while (http.connected() && (stream->available() > 0))
    {
      char c = stream->read();
      file.write(c);
      count++;
    }

    file.close();
    Serial.println("downloadImage: saved image '" + String(s3Filename) + "' (" + String(count) + " bytes)");
  }
  else
  {
    Serial.println("downloadImage error: " + String(httpCode));
  }

  http.end();
}

void Refresh()
{
  QuerySensorData();
  RefreshCurrentEvent();
  RefreshNextEvent();
  RefreshSensorArea();
}

void StartDemo()
{

  Serial.println("Start demo");

  isFree = true;
  timeToNextEvent = 5;
  roomDataUpdated = true;

  sensorData->CO2 = 450;

  // Simulate the loop logic
  Refresh();

  // ----
  for (int i = timeToNextEvent; i > 0; i--)
  {
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

bool IsAddOrDeleteCalendarEventButtonClicked(tp_finger_t fingerItem)
{
  // Button Area: 802, 0, 158, 340
  return fingerItem.x >= 802 && fingerItem.x < 960 && fingerItem.y >= 0 && fingerItem.y < 340;
}

bool Is15MinButtonClicked(tp_finger_t fingerItem)
{
  // Button Area: 240, 370, 370, 500
  return fingerItem.x >= 240 && fingerItem.x < 370 && fingerItem.y >= 370 && fingerItem.y < 500;
}

bool Is30MinButtonClicked(tp_finger_t fingerItem)
{
  // Button Area: 430, 370, 560, 500
  return fingerItem.x >= 430 && fingerItem.x < 560 && fingerItem.y >= 370 && fingerItem.y < 500;
}

bool Is45MinButtonClicked(tp_finger_t fingerItem)
{
  // Button Area: 620, 370, 750, 500
  return fingerItem.x >= 620 && fingerItem.x < 750 && fingerItem.y >= 370 && fingerItem.y < 500;
}

void SetupRTC()
{

  M5.RTC.begin();

  timeClient.begin();
  timeClient.update();

  RTCtime.hour = timeClient.getHours();
  RTCtime.min = timeClient.getMinutes();
  RTCtime.sec = timeClient.getSeconds();

  M5.RTC.setTime(&RTCtime);
  Serial.println("SetupRTC done");
}

long GetRTCTimeAsTotalSeconds()
{
  return RTCtime.hour * 60 * 60 + RTCtime.min * 60 + RTCtime.sec;
}

void RebootedIfNeeded()
{

  long elapsed = GetRTCTimeAsTotalSeconds() - lastReboot;

  if (lastReboot > 0 && elapsed > 60 * 60 * rebootTimeoutInHours)
  {
    lastReboot = GetRTCTimeAsTotalSeconds();
    DisplayOperationMessage("Rebooting..");
    delay(1000);
    ESP.restart();
  }
}

bool BookTheRoom(int duration)
{
  Serial.println("Book the room for " + String(duration) + "minutes");
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/book/" + String(duration));
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.GET();
  return httpCode == 200;
}

bool DeleteRoomBooking()
{
  Serial.println("Delete Room booking");
  HTTPClient http;
  http.begin(baseAPIUrl + "api/room/book");
  http.addHeader("label-id", WiFi.macAddress());

  int httpCode = http.sendRequest("DELETE");
  return httpCode == 200;
}

void setup()
{
  M5.begin();

  M5.EPD.SetRotation(0);
  M5.TP.SetRotation(0);
  M5.EPD.Clear(true);
  M5.EPD.SetColorReverse(true);
  canvas.createCanvas(960, 540);
  canvas.fillCanvas(WHITE);
  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);

  randomSeed(analogRead(0));

  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  canvas.setTextColor(BLACK);
  canvas.setTextSize(3);
  canvas.drawString("NOI Tech Park - IoT Door Signage", 10, 20);
  canvas.drawString("Connect to the WiFi '" + wifiSSID + "' ..", 10, 100);
  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);

  WiFi.begin(wifiSSID, "");
  Serial.print("Connect to the WiFi '" + wifiSSID + "'");
  int wifiCount = 0;

  while (WiFi.status() != WL_CONNECTED && wifiCount < 15)
  {
    delay(500);
    Serial.print(".");
    wifiCount++;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    // Reboot the device
    canvas.drawString("cannot connect..rebooting!", 600, 100);
    canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
    Serial.println("cannot connect..rebooting!");
    ESP.restart();
  }

  Serial.println("connected!");

  canvas.drawString("connected!", 700, 100);
  canvas.drawString("Downloading icons on SPIFFS", 10, 150);
  canvas.pushCanvas(0, 0, UPDATE_MODE_DU);

  SaveS3IconOnSPIFFS("starts-at-reverse-64.png");
  SaveS3IconOnSPIFFS("ends-at-reverse-64.png");
  SaveS3IconOnSPIFFS("CO2-reverse-64.png");
  SaveS3IconOnSPIFFS("CO2-warning-reverse-64.png");
  SaveS3IconOnSPIFFS("temperature-reverse-64.png");
  SaveS3IconOnSPIFFS("humidity-reverse-64.png");
  SaveS3IconOnSPIFFS("add-event-reverse-64.png");
  SaveS3IconOnSPIFFS("delete-event-reverse-64.png");
  SaveS3IconOnSPIFFS("free-for-clock-reverse-64.png");

  SetupWiFiTask();

  SetupRTC();

  InizializeLabel();

  // canvas.drawString("Ready .. let's start a quick demo!", 10, 200);
  // canvas.pushCanvas(0, 0, UPDATE_MODE_DU);
  // StartDemo();
}

void loop()
{

  M5.RTC.getTime(&RTCtime);

  RebootedIfNeeded();

  if (M5.TP.available())
  {
    if (!M5.TP.isFingerUp())
    {
      M5.TP.update();
      tp_finger_t fingerItem = M5.TP.readFinger(0);

      if (lastTouch.x != fingerItem.x || lastTouch.y != fingerItem.y)
      {

        // Update the last finger position
        lastTouch.x = fingerItem.x;
        lastTouch.y = fingerItem.y;

        if (bookingRoomAreaShown)
        {
          bool booked = false;
          int duration = 0;
          if (Is15MinButtonClicked(fingerItem))
          {
            duration = 15;
          }
          else if (button30MinEnabled && Is30MinButtonClicked(fingerItem))
          {
            duration = 30;
          }
          else if (button45MinEnabled && Is45MinButtonClicked(fingerItem))
          {
            duration = 45;
          }

          if (duration > 0)
          {
            DisplayOperationMessage("Booking the room..");
            HideBookRoomArea();
            booked = BookTheRoom(duration);
            if (booked)
            {
              QueryRoomStatus();

              roomDataUpdated = true;
              bookingRoomAreaShown = false;

              RefreshCurrentEvent();
              RefreshNextEvent();
              // reset
              roomDataUpdated = false;
            }
          }
        }
        else
        {

          if (IsAddOrDeleteCalendarEventButtonClicked(fingerItem))
          {

            if (!isFree)
            {
              Serial.println("Delete Calendar Event button clicked!");
              DisplayOperationMessage("Freeup the room..");
              DeleteRoomBooking();
              QueryRoomStatus();

              roomDataUpdated = true;
              RefreshCurrentEvent();
              roomDataUpdated = false;
            }
            else if (timeToNextEvent > 15)
            {

              Serial.println("Add Calendar Event button clicked!");

              DrawBookRoomArea();
              bookingRoomAreaShownElapsedSeconds = GetRTCTimeAsTotalSeconds();
              bookingRoomAreaShown = true;
            }
          }
        }
      }
    }
    else
    {
      // Serial.println("Touch point has not been changed!");
    }
  }

  if (roomDataUpdated)
  {

    if (roomStatusChanged)
      Refresh();

    DrawTimeToNextEvent();
    RefreshTime();
    // Reset
    roomDataUpdated = false;
    roomStatusChanged = false;
  }

  long elapsed = GetRTCTimeAsTotalSeconds() - bookingRoomAreaShownElapsedSeconds;
  if (bookingRoomAreaShown && (!isFree || elapsed >= 6))
  {
    bookingRoomAreaShown = false;
    HideBookRoomArea();
  }

  if (RTCtime.sec % 15 == 0)
    RefreshSensorArea();

  delay(20);
}