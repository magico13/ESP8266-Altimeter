#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085_U.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <LittleFS.h>
/*
Ideal plan:
- start up a wifi connection point
- start logging data when button clicked on website (or an api call?)
- log until told to stop. Or autostop when a condition is met (alt not changing for 3 seconds after launch?)
- Stop logging, output data as a csv
- Return to start

Nice to have: 
- live telemetry (should be able to get 100-200m range from esp8266-01)
  - Will have to see how fast we can collect data while doing telemetry, might be too slow. Could update telemetry slowly (1-2hz) but collect quickly (20-40hz)
  - Could require the server to exist on the computer and the esp sends udp packets
- OTA updates
*/
#define STATE_IDLE 0
#define STATE_FLIGHT 1
#define STATE_POSTFLIGHT 2

const int TEMP_POLL_INTERVAL = 1000; //ms between reading the temperature
const uint16_t ALLOWABLE_DATA_POINTS = 8000; //total data points to keep, after which we'll stop logging. This is to keep the file size within the flash limit (8k*30B ~= 234kB )

const char* IDLE_PAGE =  "State is IDLE </br><form action=\"/START\" method=\"POST\"><input type=\"submit\" value=\"Start Recording\"></form>";
const char* POST_FLIGHT_PAGE = "State is POSTFLIGHT </br><a href=\"/data\">Download Flight Data</a> </br></br> <a href=\"/reset\">RESET</a>";
const char* UPDATE_PAGE = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";


uint8_t currentState = STATE_IDLE;
uint8_t connectedStations = 0;
uint16_t totalDataPoints = 0;
unsigned long time_ms;
unsigned long last_temp_ms = 0;
unsigned long start_ms = 0;
float pressure;
float temperature;
float altitude;
File dataFile;

//vars for checking for transition to post-flight
unsigned long tsn_time_start = 0;
float tsn_alt = 0;

Adafruit_BMP085_Unified bmp = Adafruit_BMP085_Unified(10085);
ESP8266WebServer server(80);
void handleRoot();
void handleStart();
void handleNotFound();
void handleDataDownload();
void handleUpdateIndex(); //page you see when you go to /update
void handleUpdateResponse(); //response from update page
void handleUpdateLogic(); //actual update calls
void handleReset();

void setup() 
{
  // put your setup code here, to run once:
  Wire.pins(0, 2);
  Wire.begin(0, 2);

  Serial.begin(115200);

  //bmp180 setup
  if (!bmp.begin(BMP085_MODE_ULTRAHIGHRES))
  {
    Serial.println("No BMP180");
  }

  if (!LittleFS.begin())
  {
    Serial.println("Failed to start filesystem");
  }

  Serial.println("Starting AP");
  WiFi.softAP("ESP8266-Altimeter");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/START", HTTP_POST, handleStart);
  server.on("/STOP", HTTP_POST, handleStop);
  server.on("/data", HTTP_GET, handleDataDownload);
  server.on("/update", HTTP_GET, handleUpdateIndex);
  server.on("/update", HTTP_POST, handleUpdateResponse, handleUpdateLogic);
  server.on("/reset", HTTP_GET, handleReset);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() 
{
  check_connections();

  if (currentState == STATE_IDLE)
  {
    idle_loop();
  }
  else if (currentState == STATE_FLIGHT)
  {
    flight_loop();
  }
  else if (currentState == STATE_POSTFLIGHT)
  {
    post_flight_loop();
  }
}

void check_connections()
{
  uint8_t connected = WiFi.softAPgetStationNum();
  if (connected > connectedStations)
  {
    Serial.print("New connection(s). ");
  }
  else if (connected < connectedStations)
  {
    Serial.print("Lost connection(s). ");
  }

  if (connected != connectedStations)
  {
    Serial.print("Active: ");
    Serial.println(connected);
    connectedStations = connected;
  }
}

void idle_loop()
{
  if (connectedStations) //any clients connected?
  {
    server.handleClient();
  }
  else
  {
    //nobody connected, just idle
    delay(250);
  }
  update_sensors(); //periodicly read the sensor data
}

void flight_setup()
{
  Serial.println("Transition to FLIGHT");
  currentState = STATE_FLIGHT;
  dataFile = LittleFS.open("data.csv", "w");
  if (!dataFile)
  {
    Serial.println("Failed to open data file!");
  }
  dataFile.println("time (ms), pressure (hPa), temp (C), altitude (m)");
  start_ms = time_ms;
}

void flight_loop()
{
  if (connectedStations)
  {
    server.handleClient();
  }

  if (update_sensors())
  {
    dataFile.print(time_ms-start_ms);
    dataFile.print(", ");
    dataFile.print(pressure);
    dataFile.print(", ");
    dataFile.print(temperature);
    dataFile.print(", ");
    dataFile.println(altitude);

    totalDataPoints++;

    // if (tsn_time_start == 0) //disable auto stop data collection, must manually stop
    // {
    //   tsn_time_start = time_ms;
    //   tsn_alt = altitude;
    // }
    // else
    // {
    //   if (abs(altitude - tsn_alt) < POST_FLIGHT_TSN_ERROR)
    //   { //within error, check if been enough time
    //     if (time_ms - tsn_time_start > POST_FLIGHT_TSN_TIME)
    //     {
    //       // we're grounded, transition to post flight
    //       post_flight_setup();
    //     }
    //   }
    //   else
    //   { //out of error range, reset
    //     tsn_time_start = 0;
    //   }
    // }
  }
  //no delay for most readings
  //delay(10);
  if (totalDataPoints > ALLOWABLE_DATA_POINTS)
  {
    post_flight_setup();
  }
}

void post_flight_setup()
{
  Serial.println("Transition to POSTFLIGHT");
  currentState = STATE_POSTFLIGHT;
  dataFile.close();
}

void post_flight_loop()
{
  if (connectedStations)
  {
    server.handleClient();
  }
  else
  {
    //nobody connected, just idle
    delay(250);
  } 
}

bool update_sensors()
{
  /* Get a new sensor event */ 
  sensors_event_t event;
  bmp.getEvent(&event);
 
  /* Display the results (barometric pressure is measure in hPa) */
  if (event.pressure)
  {
    //current time
    time_ms = millis();
    //get pressure, temp, and altitude
    pressure = event.pressure;
    if ((time_ms - last_temp_ms) > TEMP_POLL_INTERVAL || !last_temp_ms) //update temperature once per second
    {
      last_temp_ms = time_ms;
      bmp.getTemperature(&temperature); //C
    }
    
    altitude = bmp.pressureToAltitude(1013.25f, pressure, temperature); //meters
    return true;
  }
  return false;
}

#pragma region WebServer
void handleRoot()
{
  String pageText;
  if (currentState == STATE_IDLE)
  {
    pageText = IDLE_PAGE;
    pageText += String(time_ms) + " ms</br>";
    pageText += String(pressure) + " hPa</br>";
    pageText += String(temperature) + " C</br>";
    pageText += String(altitude) + " meters</br>";
  }
  else if (currentState == STATE_FLIGHT)
  {
    pageText = "State is FLIGHT </br>";
    pageText += String(time_ms) + " ms</br>";
    pageText += String(time_ms - start_ms) + " ms MET</br>";
    pageText += String(pressure) + " hPa</br>";
    pageText += String(temperature) + " C</br>";
    pageText += String(altitude) + " meters</br>";
    pageText += "<form action=\"/STOP\" method=\"POST\"><input type=\"submit\" value=\"Stop Recording\"></form>";
  }
  else if (currentState == STATE_POSTFLIGHT)
  {
    pageText = POST_FLIGHT_PAGE;
  }

  server.send(200, "text/html", pageText);
}

void handleStart()
{
  //change to flight mode
  flight_setup();

  //redirect to root
  server.sendHeader("Location","/");
  server.send(303);
}

void handleStop()
{
  //change to post-flight mode
  post_flight_setup();

  //redirect to root
  server.sendHeader("Location","/");
  server.send(303);
}

void handleNotFound()
{
  server.send(404, "text/plain", "404: Not found");
}

void handleDataDownload()
{
  File f = LittleFS.open("data.csv", "r");
  if (f)
  {
    server.sendHeader("Content-Type", "text/text");
    server.sendHeader("Content-Disposition", "attachment; filename=data.csv");
    server.sendHeader("Connection", "close");
    server.streamFile(f, "application/octet-stream");
    f.close();
  }
  else
  {
    handleNotFound();
  }
  
}

void handleUpdateIndex()
{
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", UPDATE_PAGE);
}

void handleUpdateResponse()
{
  if (Update.hasError())
  {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "FAIL");
  }
  else
  {
    server.sendHeader("Location","/");
    server.send(303);
    ESP.restart();
  }
}

void handleUpdateLogic()
{
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.setDebugOutput(true);
    //WiFiUDP::stopAll();
    Serial.printf("Update: %s\n", upload.filename.c_str());
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) { //start with max available size
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { //true to set the size to the current progress
      Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
    Serial.setDebugOutput(false);
  }
  yield();
}

void handleReset()
{
  //redirect to root
  server.sendHeader("Connection", "close");
  server.sendHeader("Location","/");
  server.send(303);

  delay(10);
  //reset
  ESP.reset();
}
#pragma endregion WebServer
