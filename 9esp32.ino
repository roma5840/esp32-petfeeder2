#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h> 
#include <Firebase_ESP_Client.h>
#include "time.h"
#include <ESP32Time.h>

#define API_KEY       "AIzaSyCc7CfbiUP7ivEo4Vrgr-2Gq3i1xmaCrVE"       
#define DATABASE_URL  "https://aspetfeeder-default-rtdb.asia-southeast1.firebasedatabase.app/" 

const char* SETUP_SSID = "PetFeeder-Setup"; 
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);
// edit motor pin/s as needed
const int MOTOR_PIN = 23; 
const int MAX_SCHEDULES = 10;
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 28800; 
const int   DAYLIGHT_OFFSET_SEC = 0; 

WebServer server(80);
Preferences preferences;
FirebaseData fbdo;
FirebaseData streamFeedNow;
FirebaseData streamSchedules;
FirebaseAuth auth;
FirebaseConfig config;
ESP32Time rtc; 
String uid; 

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
int lastCheckedDay = 0; 
bool isInSetupMode = false;
unsigned long lastStatusUpdate = 0;

void tokenStatusCallback(TokenInfo info) {
  if (info.status() == token_status_ready) {
    Serial.println("Firebase token is ready.");
  } else if (info.status() == token_status_error) {
    Serial.printf("Firebase token error: %s\n", info.error().message().c_str());
  }
}

void handleRoot() {
  String html = "<html><body><h1>Pet Feeder Setup</h1><p>Use the ASPetFeeder mobile app to configure.</p></body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  Serial.println("Received /config request.");
  if (!server.hasArg("ssid") || !server.hasArg("uid") || !server.hasArg("email") || !server.hasArg("user_pass")) {
    server.send(400, "text/plain", "Bad Request: Missing parameters.");
    return;
  }
  String home_ssid = server.arg("ssid");
  String home_pass = server.hasArg("password") ? server.arg("password") : "";
  String user_uid = server.arg("uid");
  String user_email = server.arg("email");
  String user_password = server.arg("user_pass");

  Serial.print("Testing new WiFi connection...");
  WiFi.begin(home_ssid.c_str(), home_pass.c_str());
  int retries = 20; 
  while (WiFi.status() != WL_CONNECTED && retries > 0) {
    delay(500);
    Serial.print(".");
    retries--;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed. Aborting.");
    server.send(401, "text/plain", "WiFi Connection Failed. Check SSID and Password.");
    WiFi.disconnect(); 
    return;
  }
  Serial.println("\nWiFi connection successful!");

  Serial.println("Testing Firebase authentication...");
  config.api_key = API_KEY;
  auth.user.email = user_email;
  auth.user.password = user_password;
  Firebase.begin(&config, &auth);
  
  unsigned long start_time = millis();
  while (Firebase.getToken() == "" && millis() - start_time < 15000) {
    delay(100);
  }
  if (Firebase.getToken() == "") {
    Serial.println("Firebase sign-in timed out or failed.");
    server.send(401, "text/plain", "Firebase Auth Failed. Check your app password.");
    WiFi.disconnect();
    return;
  }
  Serial.println("Firebase authentication successful!");

  Serial.println("Saving configuration to flash memory...");
  preferences.begin("feeder-config", false);
  preferences.putString("ssid", home_ssid);
  preferences.putString("pass", home_pass);
  preferences.putString("uid", user_uid);
  preferences.putString("email", user_email);
  preferences.putString("user_pass", user_password);
  preferences.putBool("configured", true);
  preferences.end();

  server.send(200, "text/plain", "Configuration successful! OK. The feeder will now restart.");
  Serial.println("Configuration successful! Restarting in 3 seconds...");
  delay(3000);
  ESP.restart();
}

void startSetupMode() {
  isInSetupMode = true;
  Serial.println("\nStarting Setup Mode...");
  Serial.printf("Connect to WiFi network: %s\n", SETUP_SSID);
  WiFi.softAP(SETUP_SSID);
  delay(100);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  Serial.printf("AP IP address: %s\n", WiFi.softAPIP().toString().c_str());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_POST, handleConfig);
  server.begin();
  Serial.println("Web server started. Waiting for configuration from the app.");
}

void performFeed(int amount, const String& mode) {
  Serial.printf("FEEDING: %d grams, Mode: %s\n", amount, mode.c_str());
  
  // --- MOTOR CONTROL LOGIC HERE ---
  // Example:
  // digitalWrite(MOTOR_PIN, HIGH);
  // delay(amount * 100); // Simple delay based on amount, needs calibration
  // digitalWrite(MOTOR_PIN, LOW);
  // in short, pakiadd nalang dito yung gumaganang code nung nacocontrol mo yung motor.

  String statusPath = "users/" + uid + "/feederStatus";
  FirebaseJson statusUpdate;

  // Create a separate JSON object for the server value timestamp
  FirebaseJson sv_timestamp;
  sv_timestamp.set(".sv", "timestamp");
  statusUpdate.set("lastFeedTimestamp", sv_timestamp);
  statusUpdate.set("lastFeedAmount", String(amount));

  if (Firebase.RTDB.updateNode(&fbdo, statusPath.c_str(), &statusUpdate)) {
    Serial.println("Successfully updated last feed status.");
  } else {
    Serial.println("Failed to update last feed status: " + fbdo.errorReason());
  }

  String historyPath = "users/" + uid + "/feedingHistory/" + String(rtc.getEpoch());
  FirebaseJson historyEntry;
  historyEntry.set("amount", String(amount));
  historyEntry.set("type", mode);
  historyEntry.set("timestamp", sv_timestamp);

  if (Firebase.RTDB.setJSON(&fbdo, historyPath.c_str(), &historyEntry)) {
      Serial.println("Successfully added entry to feeding history.");
  } else {
      Serial.println("Failed to add feeding history entry: " + fbdo.errorReason());
  }
}

void feedNowStreamCallback(FirebaseStream data) {
  Serial.println("Stream: 'feedNow' data received.");
  if (data.dataTypeEnum() == firebase_rtdb_data_type_json) {
    FirebaseJson *json = data.jsonObjectPtr();
    FirebaseJsonData result;
    if (json && json->get(result, "amount") && result.success) {
      int feedAmount = result.to<int>();
      Serial.printf("Received manual feed command for %d grams.\n", feedAmount);
      performFeed(feedAmount, "Manual");
      String commandPath = "users/" + uid + "/commands/feedNow";
      if (Firebase.RTDB.deleteNode(&fbdo, commandPath.c_str())) {
        Serial.println("Feed command node deleted.");
      } else {
        Serial.println("Failed to delete feed command node: " + fbdo.errorReason());
      }
    }
  }
}

void schedulesStreamCallback(FirebaseStream data) {
  Serial.println("Stream: Schedule data received.");
  scheduleCount = 0; 
  if (data.dataTypeEnum() == firebase_rtdb_data_type_json) {
    FirebaseJson *json = data.jsonObjectPtr();
    if (!json) return;
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
          String timeStr = s_time.to<String>();
          int h = timeStr.substring(0, timeStr.indexOf(":")).toInt();
          int m = timeStr.substring(timeStr.indexOf(":") + 1, timeStr.lastIndexOf(" ")).toInt();
          if (timeStr.indexOf("PM") > -1 && h != 12) h += 12;
          if (timeStr.indexOf("AM") > -1 && h == 12) h = 0;
          schedules[scheduleCount].hour = h;
          schedules[scheduleCount].minute = m;
          schedules[scheduleCount].triggeredToday = false;
          Serial.printf("Loaded Schedule %d: ID %s, Time %02d:%02d, Weight %dg, On: %s\n",
                        scheduleCount + 1, schedules[scheduleCount].id.c_str(),
                        h, m, schedules[scheduleCount].weight,
                        schedules[scheduleCount].isOn ? "Yes" : "No");
          scheduleCount++;
        }
      }
    }
    json->iteratorEnd();
  } else if (data.dataTypeEnum() == firebase_rtdb_data_type_null) {
    Serial.println("All schedules were deleted from Firebase.");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, will be resumed automatically.");
  }
}

void checkSchedules() {
  if (scheduleCount == 0) return;
  if (rtc.getDay() != lastCheckedDay) {
    Serial.println("New day detected. Resetting schedule triggers.");
    for (int i = 0; i < scheduleCount; i++) {
      schedules[i].triggeredToday = false;
    }
    lastCheckedDay = rtc.getDay();
  }
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].isOn && !schedules[i].triggeredToday) {
      if (rtc.getHour(true) == schedules[i].hour && rtc.getMinute() == schedules[i].minute) {
        Serial.printf("SCHEDULE TRIGGERED: ID %s, %dg\n", schedules[i].id.c_str(), schedules[i].weight);
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
  }
  if (!Firebase.RTDB.beginStream(&streamSchedules, schedulesPath.c_str())) {
    Serial.println("Could not begin 'schedules' stream: " + streamSchedules.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&streamSchedules, schedulesStreamCallback, streamTimeoutCallback);
  }

  String statusPath = "users/" + uid + "/feederStatus/isOnline";
  // Renamed function for new library version
  if (Firebase.RTDB.onDisconnectSet(&fbdo, statusPath.c_str(), false)) {
    Serial.println("onDisconnect handler set for isOnline status.");
  } else {
    Serial.println("Failed to set onDisconnect handler: " + fbdo.errorReason());
  }
  if (Firebase.RTDB.setBool(&fbdo, statusPath.c_str(), true)) {
    Serial.println("Feeder status set to ONLINE.");
  } else {
    Serial.println("Failed to set feeder status to online: " + fbdo.errorReason());
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nASPetFeeder Firmware Starting...");
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  preferences.begin("feeder-config", true); 
  bool configured = preferences.getBool("configured", false);
  preferences.end();
  if (!configured) {
    startSetupMode();
  } else {
    isInSetupMode = false;
    Serial.println("Configuration found. Starting Operational Mode...");
    preferences.begin("feeder-config", true);
    String ssid = preferences.getString("ssid", "");
    String pass = preferences.getString("pass", "");
    uid = preferences.getString("uid", "");
    String email = preferences.getString("email", "");
    String user_pass = preferences.getString("user_pass", "");
    preferences.end();
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print(".");
      delay(500);
    }
    Serial.println(" connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    auth.user.email = email;
    auth.user.password = user_pass;
    config.token_status_callback = tokenStatusCallback; 
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.setTimeStruct(timeinfo);
      Serial.println("Time synchronized: " + rtc.getTimeDate());
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
        listeners_setup = true;
      }
      checkSchedules();
      if (millis() - lastStatusUpdate > 300000) {
        lastStatusUpdate = millis();
        String statusPath = "users/" + uid + "/feederStatus/isOnline";
        Firebase.RTDB.setBool(&fbdo, statusPath.c_str(), true);
        Serial.println("Heartbeat: Refreshed online status.");
      }
    } else if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected. Attempting to reconnect...");
        delay(5000);
    } else { 
        Serial.println("WiFi OK, but Firebase not ready. Check credentials or network rules.");
        delay(5000);
    }
  }
}