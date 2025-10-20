/*
 *  ASPetFeeder ESP32 Firmware
 *  VERSION 11.4.1
 *  - Changed: 'Manual' to 'manual'
 *  VERSION 11.4 (MERGED STABLE - CORRECTED)
 *  - Base: Version 11.3 (Stable setup, auth, and structure)
 *  - Merged: Schedule parsing and triggering logic from Version 11.2
 *  - Fixed: Success response in handleConfig() to include "OK" for app compatibility
 *  - Fixed: lastCheckedDay initialization, improved error handling
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h> 
#include <Firebase_ESP_Client.h>
#include "time.h"
#include <ESP32Time.h>
#include <ESP32Servo.h>

// ------------------- CONSTANTS -------------------
#define API_KEY       "AIzaSyCc7CfbiUP7ivEo4Vrgr-2Gq3i1xmaCrVE"       
#define DATABASE_URL  "https://aspetfeeder-default-rtdb.asia-southeast1.firebasedatabase.app/" 
#define MOTOR_PIN 23
#define MAX_SCHEDULES 10
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 28800
#define DAYLIGHT_OFFSET_SEC 0

// ------------------- OBJECTS -------------------
WebServer server(80);
Preferences preferences;
FirebaseData fbdo;
FirebaseData streamFeedNow;
FirebaseData streamSchedules;
FirebaseAuth auth;
FirebaseConfig config;
ESP32Time rtc; 
String uid; 
Servo feederServo;

// ------------------- STRUCT -------------------
struct Schedule {
  String id;
  int weight;
  bool isOn;
  int hour;
  int minute;
  bool triggeredToday;
};
Schedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
int lastCheckedDay = -1; // Initialize to -1 to ensure first day is properly set
bool isInSetupMode = false;
unsigned long lastStatusUpdate = 0;

// ------------------- FUNCTIONS -------------------
void tokenStatusCallback(TokenInfo info) {
  if (info.status == token_status_ready) {
    Serial.println("Firebase token ready.");
  } else if (info.status == token_status_error) {
    Serial.printf("Firebase token error: %s\n", info.error.message.c_str());
  }
}

void handleRoot() {
  String html = "<html><body><h1>Pet Feeder Setup</h1><p>Use the ASPetFeeder mobile app to configure.</p></body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  Serial.println("\nReceived /config request");

  if (!server.hasArg("ssid") || !server.hasArg("uid") || !server.hasArg("email") || !server.hasArg("user_pass")) {
    server.send(400, "text/plain", "Bad Request: Missing parameters.");
    return;
  }

  String home_ssid = server.arg("ssid");
  String home_pass = server.hasArg("password") ? server.arg("password") : "";
  String user_uid = server.arg("uid");
  String user_email = server.arg("email");
  String user_password = server.arg("user_pass");

  Serial.println("Connecting to home WiFi...");
  WiFi.begin(home_ssid.c_str(), home_pass.c_str());
  int retries = 25;
  while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed.");
    server.send(401, "text/plain", "WiFi Connection Failed. Check SSID and Password.");
    WiFi.disconnect(); 
    return;
  }
  Serial.println("\nWiFi connected successfully.");

  Serial.println("Testing Firebase authentication...");
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = user_email;
  auth.user.password = user_password;
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  unsigned long start = millis();
  bool authSuccess = false;
  while (millis() - start < 10000) {
    if (Firebase.ready()) {
      authSuccess = true;
      break;
    }
    delay(500);
  }

  if (!authSuccess) {
    Serial.println("Firebase authentication failed or timed out.");
    server.send(401, "text/plain", "Firebase Auth Failed. Check your credentials.");
    WiFi.disconnect();
    return;
  }
  Serial.println("Firebase authentication successful!");

  Serial.println("Saving configuration...");
  preferences.begin("feeder-config", false);
  preferences.putString("ssid", home_ssid);
  preferences.putString("pass", home_pass);
  preferences.putString("uid", user_uid);
  preferences.putString("email", user_email);
  preferences.putString("user_pass", user_password);
  preferences.putBool("configured", true);
  preferences.end();

  // Response includes "OK" to match app's check: responseText.includes('OK')
  server.send(200, "text/plain", "Configuration successful! OK. The feeder will now restart.");
  Serial.println("Configuration saved. Restarting...");
  delay(3000);
  ESP.restart();
}

void startSetupMode() {
  isInSetupMode = true;
  Serial.println("\nStarting Setup Mode...");
  WiFi.softAP("PetFeeder-Setup");
  delay(100);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  Serial.printf("Connect to WiFi: PetFeeder-Setup\nAP IP: %s\n", WiFi.softAPIP().toString().c_str());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_POST, handleConfig);
  server.begin();
  Serial.println("Web server started. Waiting for configuration...");
}

void performFeed(int amount, const String& mode) {
  Serial.printf("Feeding: %d grams (%s)\n", amount, mode.c_str());

  int openAngle = 90;
  int closeAngle = 0;
  int baseDelay = 1500;
  int extraTime = amount * 10;

  feederServo.write(openAngle);
  Serial.println("  Servo: Dispensing food...");
  delay(baseDelay + extraTime);
  feederServo.write(closeAngle);
  Serial.println("  Servo: Feeding complete.");
  delay(500);

  // Update Firebase feed status
  String statusPath = "users/" + uid + "/feederStatus";
  FirebaseJson statusUpdate;
  FirebaseJson sv_timestamp;
  sv_timestamp.set(".sv", "timestamp");
  statusUpdate.set("lastFeedTimestamp", sv_timestamp);
  statusUpdate.set("lastFeedAmount", String(amount));

  if (Firebase.RTDB.updateNode(&fbdo, statusPath.c_str(), &statusUpdate)) {
    Serial.println("  Feed status updated.");
  } else {
    Serial.println("  Failed to update feed status: " + fbdo.errorReason());
  }

  // Add to feeding history
  String historyPath = "users/" + uid + "/feedingHistory/" + String(rtc.getEpoch());
  FirebaseJson historyEntry;
  historyEntry.set("amount", String(amount));
  historyEntry.set("type", mode);
  historyEntry.set("timestamp", sv_timestamp);
  
  if (Firebase.RTDB.setJSON(&fbdo, historyPath.c_str(), &historyEntry)) {
    Serial.println("  Added to feeding history.");
  } else {
    Serial.println("  Failed to add to history: " + fbdo.errorReason());
  }
}

void feedNowStreamCallback(FirebaseStream data) {
  Serial.println("'Feed Now' stream data received.");
  if (data.dataTypeEnum() == firebase_rtdb_data_type_json) {
    FirebaseJson *json = data.jsonObjectPtr();
    FirebaseJsonData result;
    if (json && json->get(result, "amount") && result.success) {
      int feedAmount = result.to<int>();
      Serial.printf("  Manual feed requested: %d grams\n", feedAmount);
      performFeed(feedAmount, "manual");
      String commandPath = "users/" + uid + "/commands/feedNow";
      if (Firebase.RTDB.deleteNode(&fbdo, commandPath.c_str())) {
        Serial.println("  Feed command cleared.");
      }
    }
  }
}

void schedulesStreamCallback(FirebaseStream data) {
  Serial.println("Schedule data received. Parsing...");
  scheduleCount = 0; 
  
  if (data.dataTypeEnum() == firebase_rtdb_data_type_json) {
    FirebaseJson *json = data.jsonObjectPtr();
    if (!json) {
      Serial.println("  JSON object is null.");
      return;
    }
    
    size_t len = json->iteratorBegin();
    String key, value;
    int type;
    
    for (size_t i = 0; i < len && scheduleCount < MAX_SCHEDULES; i++) {
      json->iteratorGet(i, type, key, value);
      if (type == FirebaseJson::JSON_OBJECT) {
        FirebaseJson scheduleJson;
        scheduleJson.setJsonData(value);
        FirebaseJsonData s_id, s_time, s_weight, s_isOn;
        
        if (scheduleJson.get(s_id, "id") && scheduleJson.get(s_time, "time") &&
            scheduleJson.get(s_weight, "weight") && scheduleJson.get(s_isOn, "isOn")) {
          
          schedules[scheduleCount].id = s_id.to<String>();
          schedules[scheduleCount].weight = s_weight.to<int>();
          schedules[scheduleCount].isOn = s_isOn.to<bool>();
          
          // Parse time string (format: "HH:MM AM/PM")
          String timeStr = s_time.to<String>();
          int h = timeStr.substring(0, timeStr.indexOf(":")).toInt();
          int m = timeStr.substring(timeStr.indexOf(":") + 1, timeStr.lastIndexOf(" ")).toInt();
          
          // Convert to 24-hour format
          if (timeStr.indexOf("PM") > -1 && h != 12) h += 12;
          if (timeStr.indexOf("AM") > -1 && h == 12) h = 0;
          
          schedules[scheduleCount].hour = h;
          schedules[scheduleCount].minute = m;
          schedules[scheduleCount].triggeredToday = false;
          
          Serial.printf("  > Schedule %d: ID=%s, Time=%02d:%02d, Weight=%dg, Enabled=%s\n",
                        scheduleCount + 1, 
                        schedules[scheduleCount].id.c_str(),
                        h, m, 
                        schedules[scheduleCount].weight,
                        schedules[scheduleCount].isOn ? "Yes" : "No");
          scheduleCount++;
        }
      }
    }
    json->iteratorEnd();
    Serial.printf("  Total schedules loaded: %d\n", scheduleCount);
    
  } else if (data.dataTypeEnum() == firebase_rtdb_data_type_null) {
    Serial.println("  All schedules deleted from Firebase.");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout. Will resume automatically...");
  }
}

void checkSchedules() {
  if (scheduleCount == 0) return;
  
  // Check if day has changed - reset all triggers
  int currentDay = rtc.getDay();
  if (currentDay != lastCheckedDay) {
    Serial.printf("New day detected (was %d, now %d). Resetting triggers.\n", lastCheckedDay, currentDay);
    for (int i = 0; i < scheduleCount; i++) {
      schedules[i].triggeredToday = false;
    }
    lastCheckedDay = currentDay;
  }
  
  // Check each schedule
  int currentHour = rtc.getHour(true);  // 24-hour format
  int currentMinute = rtc.getMinute();
  
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].isOn && !schedules[i].triggeredToday) {
      if (currentHour == schedules[i].hour && currentMinute == schedules[i].minute) {
        Serial.printf("SCHEDULE TRIGGERED: ID=%s, Amount=%dg\n", 
                      schedules[i].id.c_str(), 
                      schedules[i].weight);
        performFeed(schedules[i].weight, "Scheduled");
        schedules[i].triggeredToday = true;
      }
    }
  }
}

void setupFirebaseListeners() {
  String feedNowPath = "users/" + uid + "/commands/feedNow";
  String schedulesPath = "users/" + uid + "/schedules";

  if (!Firebase.RTDB.beginStream(&streamFeedNow, feedNowPath.c_str())) {
    Serial.println("Could not begin 'feedNow' stream: " + streamFeedNow.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&streamFeedNow, feedNowStreamCallback, streamTimeoutCallback);
    Serial.println("Listening for 'Feed Now' commands.");
  }
  
  if (!Firebase.RTDB.beginStream(&streamSchedules, schedulesPath.c_str())) {
    Serial.println("Could not begin 'schedules' stream: " + streamSchedules.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&streamSchedules, schedulesStreamCallback, streamTimeoutCallback);
    Serial.println("Listening for schedule updates.");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nASPetFeeder Firmware v11.4 Booting...");

  // Initialize servo
  feederServo.attach(MOTOR_PIN);
  feederServo.write(0);
  Serial.println("  Servo initialized at position 0°");

  // Check if device is configured
  preferences.begin("feeder-config", true);
  bool configured = preferences.getBool("configured", false);
  preferences.end();

  if (!configured) {
    startSetupMode();
  } else {
    isInSetupMode = false;
    Serial.println("Configuration found. Starting Operational Mode...");
    
    // Load saved credentials
    preferences.begin("feeder-config", true);
    String ssid = preferences.getString("ssid", "");
    String pass = preferences.getString("pass", "");
    uid = preferences.getString("uid", "");
    String email = preferences.getString("email", "");
    String user_pass = preferences.getString("user_pass", "");
    preferences.end();

    // Connect to WiFi
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print(".");
      delay(500);
    }
    Serial.println(" Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Initialize Firebase
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    auth.user.email = email;
    auth.user.password = user_pass;
    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    // Synchronize time
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.setTimeStruct(timeinfo);
      lastCheckedDay = rtc.getDay(); // CRITICAL: Initialize lastCheckedDay
      Serial.println("Time synchronized: " + rtc.getTimeDate());
      Serial.printf("  Current day of month: %d\n", lastCheckedDay);
    } else {
      Serial.println("Failed to obtain time from NTP server.");
    }
  }
}

void loop() {
  if (isInSetupMode) {
    server.handleClient();
  } else {
    if (Firebase.ready() && WiFi.status() == WL_CONNECTED) {
      static bool listeners_setup = false;
      if (!listeners_setup) {
        setupFirebaseListeners();
        // Set initial online status
        String statusPath = "users/" + uid + "/feederStatus/isOnline";
        if (Firebase.RTDB.setBool(&fbdo, statusPath.c_str(), true)) {
          Serial.println("Feeder status set to ONLINE.");
        }
        listeners_setup = true;
      }

      // Check schedules every loop iteration
      checkSchedules(); 
      
      // Heartbeat: Update online status every 5 minutes
      if (millis() - lastStatusUpdate > 300000) {
        lastStatusUpdate = millis();
        Firebase.RTDB.setBool(&fbdo, ("users/" + uid + "/feederStatus/isOnline").c_str(), true);
        Serial.println("Heartbeat: online status refreshed.");
      }
    } else if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Reconnecting...");
      delay(5000);
    } else {
      Serial.println("Firebase not ready. Waiting...");
      delay(5000);
    }
  }
}