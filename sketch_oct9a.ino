#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPSPlus.h>
#include <math.h>

// SD Card SPI Pin Definitions for Seeed Studio XIAO ESP32-C3
#define MISO_PIN 20  // D7 (SD Card MISO)
#define SCK_PIN  8   // D8 (SD Card SCK)
#define MOSI_PIN 9   // D9 (SD Card MOSI)
#define CS_PIN   10  // D10 (SD Card CS)

// Dedicated GPS UART Pins (Avoiding SPI conflicts)
#define GPS_RX_PIN 7   // D4 (Connected to GPS TX)
#define GPS_TX_PIN 6   // D5 (Connected to GPS RX - optional)

// Battery ADC Measurement Pin (D1 / GPIO 2 on XIAO ESP32-C3)
#define BATTERY_ADC_PIN 2

TinyGPSPlus gps;

// Wi-Fi Access Point Credentials
const char* ssid = "SDragonFly";
const char* password = "";

WebServer server(80);

// System & SD Card Status Variables
bool sdAvailable = false;
uint64_t sdSizeMB = 0;
uint64_t sdFreeMB = 0;
String currentLogFileName = ""; 

// Telemetry Variables
float speed_kmh = 24.9;
float lipo_voltage = 4.12;
int battery_pct = 82;
bool is_charging = false;
String assist_level = "HIGH";
int cadence_rpm = 75;
float motor_temp_c = 28.4;

// GPS Position Variables (Default fallback: Schwabach)
double base_lat = 49.3245;  
double base_lng = 11.0234;
double gps_lat = 49.3245;  
double gps_lng = 11.0234;
float gps_alt = 320.0;
int gps_satellites = 8;
bool gps_has_fix = false;

// GPS Date & Time Variables
int gps_year = 2026;
byte gps_month = 10;
byte gps_day = 13;
byte gps_hour = 0;
byte gps_minute = 0;
byte gps_second = 0;

// NMEA Parsing Buffers
String latestNmeaSentence = "$GPRMC,000000.00,V,0000.0000,N,00000.0000E,0.0,0.0,000000,,,N*70";
String nmeaBuffer = "";

// Logging Timing
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 2000; // Log every 2 seconds

// Fallback HTML page served if index.html is missing from the SD card
const char fallback_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta charset="UTF-8">
  <title>Dragonfly - SD Uploader</title>
  <style>
    body { font-family: sans-serif; background: #0f172a; color: #fff; text-align: center; padding: 50px; }
    .card { background: #1e293b; max-width: 400px; margin: 0 auto; padding: 30px; border-radius: 16px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
    h2 { color: #38bdf8; }
    input { width: 100%; margin: 20px 0; background: #0f172a; color: #94a3b8; border: 1px dashed #334155; padding: 10px; border-radius: 8px; box-sizing: border-box; }
    button { background: #16a34a; color: #fff; border: none; padding: 12px 20px; font-weight: bold; border-radius: 8px; cursor: pointer; width: 100%; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Dragonfly Telemetry</h2>
    <p>There is no <b>index.html</b> file on the SD card yet.<br>Please upload it!</p>
    <input type="file" id="f"><br>
    <button onclick="up()">Upload index.html</button>
  </div>
<script>
  function up() {
    let file = document.getElementById('f').files[0];
    if(!file) { alert('Select a file first!'); return; }
    let fd = new FormData();
    fd.append("file", file, "/index.html");
    alert('Upload starting...');
    fetch('/upload', { method: 'POST', body: fd }).then(r => r.text()).then(msg => {
      alert(msg);
      location.reload();
    });
  }
</script>
</body>
</html>
)rawliteral";

// Initialize SD Card and create a new CSV log file
void initSD() {
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  if (SD.begin(CS_PIN, SPI, 4000000)) {
    sdAvailable = true;
    sdSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    sdFreeMB = (SD.cardSize() - SD.usedBytes()) / (1024ULL * 1024ULL);
    Serial.println("SD Card Initialized Successfully!");

    currentLogFileName = "/log_" + String(millis()) + ".csv";
    File logFile = SD.open(currentLogFileName, FILE_WRITE);
    if (logFile) {
      logFile.println("Timestamp_ms,GPS_Date,GPS_Time,Speed_kmh,Battery_V,Battery_pct,Charging,Assist_level,Cadence_rpm,Motor_temp_C,Latitude,Longitude,Satellites,Raw_NMEA");
      logFile.close();
    }
  } else {
    sdAvailable = false;
    sdSizeMB = 0;
    sdFreeMB = 0;
    Serial.println("SD Card Initialization Failed!");
  }
}

// Read LiPo Battery Voltage via ADC with multi-sampling noise reduction & 1:2 voltage divider
void updateLiPoMetrics() {
  uint32_t adc_sum = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    adc_sum += analogRead(BATTERY_ADC_PIN);
    delay(2);
  }
  float raw_adc = (float)adc_sum / samples;

  // Calculate voltage with 1:2 resistor divider scaling factor (multiplied by 2.0)
  float measured_v = (raw_adc / 4095.0) * 3.3 * 2.0; 
  
  if (measured_v > 0.5) {
    lipo_voltage = measured_v;
  }

  // Detect charging state based on voltage threshold
  if (lipo_voltage >= 4.3) is_charging = true;
  else is_charging = false;

  // Battery percentage mapping (Max: 4.0V, Min: 3.3V)
  if (lipo_voltage >= 4.0) {
    battery_pct = 100;
  } else if (lipo_voltage <= 3.3) {
    battery_pct = 0;
  } else {
    battery_pct = (int)((lipo_voltage - 3.3) / (4.0 - 3.3) * 100);
  }
}

// Optional GPS simulation fallback function for testing
void updateSimulatedGPS() {
  float t = millis() / 3000.0; 
  float scale = 0.003; 
  float denominator = 1.0 + sin(t) * sin(t);
  float dx = scale * cos(t) / denominator;
  float dy = scale * sin(t) * cos(t) / denominator;

  gps_lat = base_lat + dx;
  gps_lng = base_lng + dy;
  gps_satellites = 9;
}

// Read and parse incoming NMEA sentences from the GPS module
void readGPS() { 
  int maxIterations = 256; // Safety limit to prevent blocking
  while (Serial1.available() > 0 && maxIterations > 0) {
    char c = Serial1.read();
    gps.encode(c);
    maxIterations--;

    if (c == '\n') {
      nmeaBuffer.trim(); 
      // Accept only valid, complete NMEA sentences starting with '$' containing a checksum '*'
      if (nmeaBuffer.startsWith("$") && nmeaBuffer.indexOf('*') != -1) {
        latestNmeaSentence = nmeaBuffer;
      }
      nmeaBuffer = ""; 
    } else {
      if (c != '\r') {
        nmeaBuffer += c;
      }
      // Prevent buffer overflow from malformed data streams
      if (nmeaBuffer.length() > 120) {
        nmeaBuffer = ""; 
      }
    }
  }

  // Update telemetry metrics from TinyGPSPlus parser
  if (gps.location.isUpdated() && gps.location.isValid()) {
    gps_lat = gps.location.lat();
    gps_lng = gps.location.lng();
    gps_has_fix = true;
  }
  if (gps.altitude.isUpdated() && gps.altitude.isValid()) {
    gps_alt = gps.altitude.meters();
  }
  if (gps.speed.isUpdated() && gps.speed.isValid()) {
    speed_kmh = gps.speed.kmph();
  }
  if (gps.satellites.isUpdated()) {
    gps_satellites = gps.satellites.value();
  }
  // Update Date and Time from GPS
  if (gps.time.isUpdated() && gps.time.isValid()) {
    gps_hour = gps.time.hour();
    gps_minute = gps.time.minute();
    gps_second = gps.time.second();
  }
  if (gps.date.isUpdated() && gps.date.isValid()) {
    gps_year = gps.date.year();
    gps_month = gps.date.month();
    gps_day = gps.date.day();
  }
}

// Append current telemetry record to the active CSV log file on the SD card
void logDataToSD() {
  if (!sdAvailable || currentLogFileName == "") return;
  sdFreeMB = (SD.cardSize() - SD.usedBytes()) / (1024ULL * 1024ULL);

  File logFile = SD.open(currentLogFileName, FILE_APPEND);
  if (logFile) {
    logFile.print(millis()); logFile.print(",");

    // Format Date (YYYY-MM-DD)
    char dateStr[12];
    sprintf(dateStr, "%04d-%02d-%02d", gps_year, gps_month, gps_day);
    logFile.print(dateStr); logFile.print(",");

    // Format Time (HH:MM:SS)
    char timeStr[10];
    sprintf(timeStr, "%02d:%02d:%02d", gps_hour, gps_minute, gps_second);
    logFile.print(timeStr); logFile.print(",");

    logFile.print(speed_kmh, 1); logFile.print(",");
    logFile.print(lipo_voltage, 2); logFile.print(",");
    logFile.print(battery_pct); logFile.print(",");
    logFile.print(is_charging ? "1" : "0"); logFile.print(",");
    logFile.print(assist_level); logFile.print(",");
    logFile.print(cadence_rpm); logFile.print(",");
    logFile.print(motor_temp_c, 1); logFile.print(",");
    logFile.print(gps_lat, 6); logFile.print(",");
    logFile.print(gps_lng, 6); logFile.print(",");
    logFile.print(gps_satellites); logFile.print(",");
    logFile.println(latestNmeaSentence);
    logFile.close();
  }
}

// Serve root URL (index.html from SD card or fallback uploader page)
void handleRoot() {
  if (sdAvailable && SD.exists("/index.html")) {
    File htmlFile = SD.open("/index.html", FILE_READ);
    server.streamFile(htmlFile, "text/html");
    htmlFile.close();
  } else {
    server.send_P(200, "text/html", fallback_html);
  }
}

// REST API endpoint to provide real-time telemetry JSON payload
void handleApiData() {
  updateLiPoMetrics();
  readGPS();

  if (sdAvailable) {
    sdFreeMB = (SD.cardSize() - SD.usedBytes()) / (1024ULL * 1024ULL);
  }

  motor_temp_c = 28.0 + (random(0, 15) / 10.0);
  
  char timeBuffer[10];
  sprintf(timeBuffer, "%02d:%02d:%02d", gps_hour, gps_minute, gps_second);
  
  char dateBuffer[12];
  sprintf(dateBuffer, "%04d-%02d-%02d", gps_year, gps_month, gps_day);

  String json = "{";
  json += "\"time\":\"" + String(timeBuffer) + "\",";
  json += "\"date\":\"" + String(dateBuffer) + "\",";
  json += "\"speed\":" + String(speed_kmh, 1) + ",";
  json += "\"battery_pct\":" + String(battery_pct) + ",";
  json += "\"battery_v\":" + String(lipo_voltage, 2) + ",";
  json += "\"is_charging\":" + String(is_charging ? "true" : "false") + ",";
  json += "\"assist\":\"" + assist_level + "\",";
  json += "\"cadence\":" + String(cadence_rpm) + ",";
  json += "\"temp\":" + String(motor_temp_c, 1) + ",";
  json += "\"lat\":" + String(gps_lat, 6) + ",";
  json += "\"lng\":" + String(gps_lng, 6) + ",";
  json += "\"alt\":" + String(gps_alt, 1) + ",";
  json += "\"gps_sats\":" + String(gps_satellites) + ",";
  json += "\"sd_ok\":" + String(sdAvailable ? "true" : "false") + ",";
  json += "\"sd_size\":" + String(sdSizeMB) + ",";
  json += "\"sd_free\":" + String(sdFreeMB);
  json += "}";
  server.send(200, "application/json", json);
}

// API endpoint to list files stored on the SD card
void handleApiFiles() {
  if (!sdAvailable) { server.send(200, "application/json", "[]"); return; }
  File root = SD.open("/");
  String json = "[";
  bool first = true;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fname = String(file.name());
      if (fname != "index.html" && fname != "/index.html") {
        if (!first) json += ",";
        first = false;
        size_t bytes = file.size();
        String sizeStr = bytes < 1024 ? String(bytes) + " B" : (bytes < 1048576 ? String(bytes / 1024.0, 1) + " KB" : String(bytes / 1048576.0, 1) + " MB");
        json += "{\"name\":\"" + fname + "\",\"size\":\"" + sizeStr + "\"}";
      }
    }
    file = root.openNextFile();
  }
  json += "]";
  server.send(200, "application/json", json);
}

// API endpoint to retry SD card connection
void handleRetrySD() {
  initSD();
  server.send(200, "text/plain", sdAvailable ? "SD Card Connected!" : "SD Card Error.");
}

// API endpoint to download the active log file
void handleDownloadLog() {
  if (sdAvailable && currentLogFileName != "" && SD.exists(currentLogFileName)) {
    File downloadFile = SD.open(currentLogFileName, FILE_READ);
    server.streamFile(downloadFile, "text/csv");
    downloadFile.close();
  } else {
    server.send(404, "text/plain", "Log file not found.");
  }
}

// API endpoint to download arbitrary files from SD card
void handleDownloadFile() {
  if (!sdAvailable || !server.hasArg("name")) { server.send(400, "text/plain", "Bad Request."); return; }
  String fileName = server.arg("name");
  if (!fileName.startsWith("/")) fileName = "/" + fileName;
  if (SD.exists(fileName)) {
    File file = SD.open(fileName, FILE_READ);
    server.streamFile(file, "application/octet-stream");
    file.close();
  } else { server.send(404, "text/plain", "File not found."); }
}

// API endpoint to delete files from SD card (protecting index.html)
void handleDeleteFile() {
  if (!sdAvailable || !server.hasArg("name")) { server.send(400, "text/plain", "Bad Request."); return; }
  String fileName = server.arg("name");
  if (!fileName.startsWith("/")) fileName = "/" + fileName;
  if (fileName == "/index.html" || fileName == "index.html") {
    server.send(403, "text/plain", "index.html cannot be deleted!");
    return;
  }
  if (SD.exists(fileName)) {
    SD.remove(fileName);
    server.send(200, "text/plain", "Deleted: " + fileName);
  } else { server.send(404, "text/plain", "File not found."); }
}

File uploadFileObj;
// File upload handler for uploading index.html via Web UI
void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (SD.exists(filename)) SD.remove(filename);
    uploadFileObj = SD.open(filename, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFileObj) uploadFileObj.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFileObj) uploadFileObj.close();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configure ADC pin for battery monitoring
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogSetAttenuation(ADC_11db); // Set full 0-3.3V ADC range

  // Initialize GPS Serial communication
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  initSD();

  // Start Wi-Fi Access Point
  WiFi.softAP(ssid, password);

  // Register Web Server Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/files", HTTP_GET, handleApiFiles);
  server.on("/api/retry-sd", HTTP_GET, handleRetrySD);
  server.on("/download-log", HTTP_GET, handleDownloadLog);
  server.on("/download-file", HTTP_GET, handleDownloadFile);
  server.on("/delete-file", HTTP_GET, handleDeleteFile);
  server.on("/upload", HTTP_POST, []() { server.send(200, "text/plain", "Uploaded!"); }, handleFileUpload);

  server.begin();
}

void loop() {
  server.handleClient();

  updateLiPoMetrics();
  readGPS();

  // Periodic data logging to SD card based on LOG_INTERVAL
  if (millis() - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = millis();
    logDataToSD();
  }
}