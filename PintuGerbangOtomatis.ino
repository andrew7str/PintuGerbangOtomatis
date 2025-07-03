/**********************************
 * ESP32 Gate Control System with:
 * - WiFi STA/AP Mode
 * - Blynk Remote Control
 * - RFID Registration/Management
 * - Ultrasonic Sensor
 * - Web Interface
**********************************/

#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <BlynkSimpleEsp32.h>

// Pin Definitions
#define IN1 26#define IN2 27
#define IN3 14
#define IN4 12
#define RST_PIN 22
#define SS_PIN 21
#define TRIG_PIN 18
#define ECHO_PIN 19

// Blynk Configuration
#define BLYNK_TEMPLATE_ID "TMPL6VGJRzU9o"
#define BLYNK_TEMPLATE_NAME "pintu gerbang"
#define BLYNK_AUTH_TOKEN "oRDrgpn1rcvkF0_tyPzb-x0B9neGiuFK"
char bluetoothName[] = "PintuGerbang";

// WiFi Configuration
const char* router_ssid = "ZERO";
const char* router_password = "tidakada";
const char* ap_ssid = "PintuGerbang";
const char* ap_password = "tidakada";

// Web Server
WebServer server(80);

// RFID
MFRC522 mfrc522(SS_PIN, RST_PIN);
String lastRFID = "";

// Ultrasonic Sensor
long duration;
int distance;
bool vehicleDetected = false;
unsigned long vehicleExitTime = 0;
const int CLOSE_DELAY = 5000; // 5 seconds

// NTP Client for time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// User credentials for web interface
const char* webUsername = "admin";
const char* webPassword = "root";

// EEPROM for storage
#define EEPROM_SIZE 4096
#define MAX_RFID 20  // Maximum number of registered RFID cards
#define RFID_START_ADDR 1000
#define HISTORY_START_ADDR 0

struct HistoryEntry {
  String rfid;
  String timestamp;
  String action;
};

HistoryEntry history[50]; // Store last 50 entries
String registeredRFIDs[MAX_RFID]; // Store registered RFID cards
int registeredCount = 0;
int historyIndex = 0;

// Stepper Motor Variables
int Steps = 0;
boolean Direction = true;
unsigned long last_time;
unsigned long currentMillis;
int steps_left = 0;
long timer;

// Login status
bool isAuthenticated = false;
bool isAPMode = false;

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize RFID
  SPI.begin();
  mfrc522.PCD_Init();
  
  // Load registered RFIDs
  loadRFIDs();
  
  // Connect to WiFi (try router first, fallback to AP)
  connectToWiFi();
  
  // Initialize Blynk
  Blynk.setDeviceName(bluetoothName);
  Blynk.begin(BLYNK_AUTH_TOKEN);
  
  // Initialize NTP client (only if connected to router)
  if (!isAPMode) {
    timeClient.begin();
    timeClient.setTimeOffset(25200); // Adjust for your timezone
  }
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/logout", handleLogout);
  server.on("/open", handleOpenGate);
  server.on("/history", handleHistory);
  server.on("/wifi", handleWiFiConfig);
  server.on("/updatewifi", handleUpdateWiFi);
  server.on("/rfid", handleRFIDManagement);
  server.on("/addrfid", handleAddRFID);
  server.on("/deleterfid", HTTP_POST, handleDeleteRFID);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  Blynk.run();
  server.handleClient();
  
  // Handle RFID reading
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String rfid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      rfid += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
      rfid += String(mfrc522.uid.uidByte[i], HEX);
    }
    rfid.toUpperCase();
    lastRFID = rfid;
    
    Serial.print("RFID detected: ");
    Serial.println(rfid);
    
    if(isRFIDRegistered(rfid)) {
      openGate();
      addHistoryEntry(rfid, "Open via RFID");
    } else {
      addHistoryEntry(rfid, "Unauthorized RFID");
      Serial.println("Unauthorized RFID card");
    }
    
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
  
  // Ultrasonic sensor reading
  checkVehiclePresence();
  
  // Close gate if no vehicle is detected for 5 seconds
  if (!vehicleDetected && vehicleExitTime != 0 && millis() - vehicleExitTime >= CLOSE_DELAY) {
    closeGate();
    vehicleExitTime = 0;
    if (!lastRFID.isEmpty()) {
      addHistoryEntry(lastRFID, "Auto Close");
      lastRFID = "";
    }
  }
  
  // Handle stepper motor movement
  if (steps_left > 0) {
    currentMillis = micros();
    if (currentMillis - last_time >= 1000) {
      stepper(1);
      timer = timer + micros() - last_time;
      last_time = micros();
      steps_left--;
    }
  }
}

// Blynk Virtual Pin Handlers
BLYNK_WRITE(V0) { // Button to open gate
  if (param.asInt() == 1) {
    openGate();
    addHistoryEntry("Blynk", "Open via Bluetooth");
  }
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V1); // Sync the gate status
}

void connectToWiFi() {
  Serial.println("Connecting to WiFi router...");
  WiFi.begin(router_ssid, router_password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("Connected to router!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    isAPMode = false;
  } else {
    // Fallback to AP mode
    Serial.println("");
    Serial.println("Failed to connect to router, starting AP mode");
    startAPMode();
  }
}

void startAPMode() {
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("AP Mode Activated");
  Serial.print("AP SSID: ");
  Serial.println(ap_ssid);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  isAPMode = true;
}

void openGate() {
  if (steps_left > 0) return;
  
  Serial.println("Opening gate");
  Direction = true;
  steps_left = 5000;
  Blynk.virtualWrite(V1, 1); // Update Blynk status
}

void closeGate() {
  if (steps_left > 0) return;
  
  Serial.println("Closing gate");
  Direction = false;
  steps_left = 5000;
  Blynk.virtualWrite(V1, 0); // Update Blynk status
}

void checkVehiclePresence() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  duration = pulseIn(ECHO_PIN, HIGH);
  distance = duration * 0.034 / 2;
  
  if (distance < 50) {
    if (!vehicleDetected) {
      Serial.println("Vehicle detected");
      vehicleDetected = true;
    }
    vehicleExitTime = 0;
  } else {
    if (vehicleDetected) {
      Serial.println("Vehicle no longer detected");
      vehicleDetected = false;
      vehicleExitTime = millis();
    }
  }
}

void addHistoryEntry(String rfid, String action) {
  String timestamp;
  if (!isAPMode) {
    timeClient.update();
    timestamp = timeClient.getFormattedTime();
  } else {
    timestamp = String(millis() / 1000) + "s";
  }
  
  history[historyIndex].rfid = rfid;
  history[historyIndex].timestamp = timestamp;
  history[historyIndex].action = action;
  
  // Save to EEPROM
  String entry = timestamp + "," + rfid + "," + action + "\n";
  int addr = HISTORY_START_ADDR + (historyIndex * 60); // Allow 60 bytes per entry
  for (int i = 0; i < entry.length(); i++) {
    EEPROM.write(addr + i, entry[i]);
  }
  EEPROM.commit();
  
  historyIndex = (historyIndex + 1) % 50;
}

void loadRFIDs() {
  registeredCount = 0;
  for (int i = 0; i < MAX_RFID; i++) {
    String rfid = "";
    for (int j = 0; j < 8; j++) {
      char c = EEPROM.read(RFID_START_ADDR + (i * 9) + j);
      if (c == 0 || c == 255) break;
      rfid += c;
    }
    if (rfid.length() == 8) {
      registeredRFIDs[registeredCount++] = rfid;
    }
  }
  Serial.print("Loaded ");
  Serial.print(registeredCount);
  Serial.println(" registered RFIDs");
}

void saveRFID(String rfid) {
  if (registeredCount >= MAX_RFID) {
    Serial.println("RFID storage full");
    return;
  }
  
  if (isRFIDRegistered(rfid)) {
    Serial.println("RFID already registered");
    return;
  }
  
  for (int i = 0; i < 8; i++) {
    EEPROM.write(RFID_START_ADDR + (registeredCount * 9) + i, rfid[i]);
  }
  EEPROM.write(RFID_START_ADDR + (registeredCount * 9) + 8, '\0');
  EEPROM.commit();
  
  registeredRFIDs[registeredCount++] = rfid;
  Serial.println("RFID saved: " + rfid);
}

void deleteRFID(int index) {
  if (index < 0 || index >= registeredCount) return;
  
  // Shift all entries after the deleted one
  for (int i = index; i < registeredCount - 1; i++) {
    registeredRFIDs[i] = registeredRFIDs[i + 1];
    for (int j = 0; j < 9; j++) {
      EEPROM.write(RFID_START_ADDR + (i * 9) + j, 
                  EEPROM.read(RFID_START_ADDR + ((i + 1) * 9) + j));
    }
  }
  
  // Clear the last entry
  for (int j = 0; j < 9; j++) {
    EEPROM.write(RFID_START_ADDR + ((registeredCount - 1) * 9 + j, 0);
  }
  EEPROM.commit();
  
  registeredCount--;
  Serial.println("RFID deleted at index: " + String(index));
}

bool isRFIDRegistered(String rfid) {
  for (int i = 0; i < registeredCount; i++) {
    if (registeredRFIDs[i] == rfid) {
      return true;
    }
  }
  return false;
}

void handleRoot() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  String html = "<html><head><title>Gate Control</title><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; }";
  html += "button, input[type='submit'] { padding: 10px 20px; font-size: 16px; margin: 5px; }";
  html += "table { border-collapse: collapse; width: 100%; margin-bottom: 20px; }";
  html += "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }";
  html += "th { background-color: #f2f2f2; }";
  html += ".success { color: green; }";
  html += ".error { color: red; }";
  html += "</style></head><body>";
  
  html += "<h1>Gate Control System</h1>";
  
  if (isAPMode) {
    html += "<p style='color:red;'>Mode: Access Point (Direct Connect)</p>";
    html += "<p>SSID: " + String(ap_ssid) + "</p>";
    html += "<p>Password: " + String(ap_password) + "</p>";
  } else {
    html += "<p style='color:green;'>Mode: Connected to Router</p>";
    html += "<p>Connected to: " + String(router_ssid) + "</p>";
  }
  
  html += "<p>IP: " + WiFi.localIP().toString() + "</p>";
  
  html += "<p><a href='/open'><button>Open Gate</button></a></p>";
  html += "<p><a href='/history'><button>View History</button></a></p>";
  html += "<p><a href='/rfid'><button>Manage RFID Cards</button></a></p>";
  html += "<p><a href='/wifi'><button>WiFi Settings</button></a></p>";
  html += "<p><a href='/logout'><button>Logout</button></a></p>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleLogin() {
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    if (cookie.indexOf("ESPSESSIONID=1") != -1) {
      isAuthenticated = true;
      server.sendHeader("Location", "/");
      server.send(301);
      return;
    }
  }
  
  if (server.method() == HTTP_POST) {
    String username = server.arg("username");
    String password = server.arg("password");
    
    if (username == webUsername && password == webPassword) {
      isAuthenticated = true;
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
      server.sendHeader("Location", "/");
      server.send(301);
      return;
    }
  }
  
  String html = "<html><head><title>Login</title><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; }";
  html += "input { margin: 5px; padding: 8px; }";
  html += "</style></head><body>";
  html += "<h1>Login</h1>";
  html += "<form method='POST' action='/login'>";
  html += "Username: <input type='text' name='username'><br>";
  html += "Password: <input type='password' name='password'><br>";
  html += "<input type='submit' value='Login'>";
  html += "</form></body></html>";
  
  server.send(200, "text/html", html);
}

void handleLogout() {
  isAuthenticated = false;
  server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
  server.sendHeader("Location", "/login");
  server.send(301);
}

void handleOpenGate() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  openGate();
  addHistoryEntry("WEB", "Open via Web");
  
  server.sendHeader("Location", "/");
  server.send(301);
}

void handleHistory() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  String html = "<html><head><title>Gate History</title><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; }";
  html += "table { border-collapse: collapse; width: 100%; }";
  html += "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }";
  html += "th { background-color: #f2f2f2; }";
  html += "</style></head><body>";
  html += "<h1>Gate History</h1>";
  html += "<table><tr><th>Timestamp</th><th>RFID</th><th>Action</th></tr>";
  
  for (int i = 0; i < 50; i++) {
    int index = (historyIndex - 1 - i + 50) % 50;
    if (!history[index].timestamp.isEmpty()) {
      html += "<tr>";
      html += "<td>" + history[index].timestamp + "</td>";
      html += "<td>" + history[index].rfid + "</td>";
      html += "<td>" + history[index].action + "</td>";
      html += "</tr>";
    }
  }
  
  html += "</table>";
  html += "<p><a href='/'><button>Back to Main</button></a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleRFIDManagement() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  String message = "";
  if (server.hasArg("success")) {
    message = "<p class='success'>RFID successfully registered!</p>";
  } else if (server.hasArg("error")) {
    int error = server.arg("error").toInt();
    if (error == 1) {
      message = "<p class='error'>RFID already registered!</p>";
    } else if (error == 2) {
      message = "<p class='error'>No RFID card detected!</p>";
    }
  }
  
  String html = "<html><head><title>RFID Management</title><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; }";
  html += "button, input[type='submit'] { padding: 10px 20px; font-size: 16px; margin: 5px; }";
  html += "table { border-collapse: collapse; width: 100%; margin-bottom: 20px; }";
  html += "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }";
  html += "th { background-color: #f2f2f2; }";
  html += ".success { color: green; }";
  html += ".error { color: red; }";
  html += "</style></head><body>";
  
  html += "<h1>RFID Card Management</h1>";
  html += message;
  
  // Form to add new RFID
  html += "<h2>Register New RFID</h2>";
  html += "<form method='POST' action='/addrfid'>";
  html += "<p>Scan RFID card and click below to register current card</p>";
  html += "<input type='submit' value='Register Current RFID'>";
  html += "</form>";
  
  // List of registered RFIDs
  html += "<h2>Registered RFID Cards</h2>";
  if (registeredCount == 0) {
    html += "<p>No RFID cards registered</p>";
  } else {
    html += "<table><tr><th>No</th><th>RFID</th><th>Action</th></tr>";
    for (int i = 0; i < registeredCount; i++) {
      html += "<tr>";
      html += "<td>" + String(i + 1) + "</td>";
      html += "<td>" + registeredRFIDs[i] + "</td>";
      html += "<td>";
      html += "<form method='POST' action='/deleterfid' style='display: inline;'>";
      html += "<input type='hidden' name='index' value='" + String(i) + "'>";
      html += "<input type='submit' value='Delete' onclick='return confirm(\"Are you sure you want to delete this RFID?\");'>";
      html += "</form>";
      html += "</td>";
      html += "</tr>";
    }
    html += "</table>";
  }
  
  html += "<p><a href='/'><button>Back to Main</button></a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleAddRFID() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  if (lastRFID.length() > 0) {
    if (!isRFIDRegistered(lastRFID)) {
      saveRFID(lastRFID);
      server.sendHeader("Location", "/rfid?success=1");
    } else {
      server.sendHeader("Location", "/rfid?error=1");
    }
  } else {
    server.sendHeader("Location", "/rfid?error=2");
  }
  server.send(301);
}

void handleDeleteRFID() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    deleteRFID(index);
  }
  
  server.sendHeader("Location", "/rfid");
  server.send(301);
}

void handleWiFiConfig() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  String html = "<html><head><title>WiFi Configuration</title><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; }";
  html += "input { margin: 5px; padding: 8px; width: 200px; }";
  html += "</style></head><body>";
  html += "<h1>WiFi Configuration</h1>";
  html += "<form method='POST' action='/updatewifi'>";
  html += "<p>Router SSID: <input type='text' name='ssid' value='" + String(router_ssid) + "'></p>";
  html += "<p>Router Password: <input type='password' name='password' value='" + String(router_password) + "'></p>";
  html += "<p><input type='submit' value='Update WiFi Settings'></p>";
  html += "</form>";
  html += "<p><a href='/'><button>Back to Main</button></a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleUpdateWiFi() {
  if (!isAuthenticated) {
    server.sendHeader("Location", "/login");
    server.send(301);
    return;
  }
  
  if (server.method() == HTTP_POST) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    
    // Update the WiFi credentials
    router_ssid = newSSID.c_str();
    router_password = newPassword.c_str();
    
    // Try to reconnect
    connectToWiFi();
  }
  
  server.sendHeader("Location", "/");
  server.send(301);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  
  server.send(404, "text/plain", message);
}

void stepper(int xw) {
  for (int x = 0; x < xw; x++) {
    switch (Steps) {
      case 0:
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, LOW);
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, HIGH);
        break;
      case 1:
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, LOW);
        digitalWrite(IN3, HIGH);
        digitalWrite(IN4, HIGH);
        break;
      case 2:
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, LOW);
        digitalWrite(IN3, HIGH);
        digitalWrite(IN4, LOW);
        break;
      case 3:
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, HIGH);
        digitalWrite(IN3, HIGH);
        digitalWrite(IN4, LOW);
        break;
      case 4:
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, HIGH);
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, LOW);
        break;
      case 5:
        digitalWrite(IN1, HIGH);
        digitalWrite(IN2, HIGH);
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, LOW);
        break;
      case 6:
        digitalWrite(IN1, HIGH);
        digitalWrite(IN2, LOW);
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, LOW);
        break;
      case 7:
        digitalWrite(IN1, HIGH);
        digitalWrite(IN2, LOW);
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, HIGH);
        break;
      default:
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, LOW);
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, LOW);
        break;
    }
    SetDirection();
  }
}

void SetDirection() {
  if (Direction == 1) {
    Steps++;
  }
  if (Direction == 0) {
    Steps--;
  }
  if (Steps > 7) {
    Steps = 0;
  }
  if (Steps < 0) {
    Steps = 7;
  }
}
