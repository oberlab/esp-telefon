#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <HTTPClient.h>

String wifiSSID;
String wifiPassword;

// ***** Configurable URLs and number mappings *****
String urlBellOn;
String urlBellOff;
String urlOHookRing;
String urlOHookNoRing;
String urlOnHook;
String urlDialStarted;
String urlOnDialed;
String urlUnknownNumber;
String urlBlackButton;
String mapKey[10];       // 1-based: configured dial strings
String mapURL[10];       // corresponding URLs

// ***** Pin-Konfiguration *****
const int in1Pin_h_bridge  = 32;  // IN1 des L298N
const int in2Pin_h_bridge  = 33;  // IN2 des L298N
const int dialPin          = 12;  // Wählscheibe Eingang (mit internem Pullup)
const int schwarzeTastePin = 14;  // "Schwarze Taste" (active LOW)
const int gabelPin         = 27;  // "Gabel" – normally closed (invertierte Logik)
const int resetPin         = 25;  // "Reset" – Config-Taste beim Powerup

// ***** Variablen für Wählscheibe & Klingel *****
volatile int pulseCount = 0;              // Zählt Impulse der Wählscheibe
volatile unsigned long lastPulseTime = 0; // Zeitpunkt des letzten Impulses
String dialNumber = "";                   // Sammelt gewählte Ziffern
volatile bool klingelAktiv = false;       // Klingel-Modus steuern
volatile bool klingel_not_paused = false;
volatile bool url_after_lift = false;
volatile bool url_after_hang_up = false;
bool currentGabel = LOW;
bool numberDialInProgress = false;        // wird true, wenn eine nummer gewählt wird

#define GABEL_ABGENOMMEN HIGH
#define GABEL_AUFGELEGT LOW

// ***** WiFi, Webserver & WebSocket *****
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Helper: HTTP GET ausführen
void sendURL(const String &url) {
  if (url.length() > 0) {
    HTTPClient http;
    http.begin(url);
    http.GET();
    http.end();
  }
}

// ***** HTML-Weboberfläche für Telefoninterface mit Konfigurationsmenü *****
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP32 Telefon Interface</title>
  <style>
    body { font-family: Arial, sans-serif; background: #f0f0f0; margin: 0; padding: 0; }
    .container { max-width: 600px; margin: 50px auto; background: #fff; padding: 20px;
                 box-shadow: 0 0 10px rgba(0,0,0,0.1); }
    h1 { text-align: center; color: #333; }
    .status { margin: 10px 0; padding: 10px; background: #e0e0e0; border-radius: 5px; }
    .button { display: block; width: 100%; padding: 10px; background: #007BFF;
              color: #fff; border: none; border-radius: 5px; cursor: pointer;
              font-size: 16px; margin-top: 20px; }
    .button:hover { background: #0056b3; }
    #dialHistory { margin-top: 20px; }
    #historyList { list-style: none; padding: 0; margin: 0; }
    #historyList li { padding: 5px; border-bottom: 1px solid #ccc; }
    details { margin-top: 20px; padding: 10px; background: #fafafa;
               border: 1px solid #ccc; border-radius: 5px; }
    summary { font-weight: bold; cursor: pointer; }
    label { display: block; margin-top: 8px; }
    input[type="text"] { width: 100%; padding: 6px; margin-top: 2px; }
    input[type="submit"] { margin-top: 12px; padding: 8px 16px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>ESP32 Telefon Interface</h1>
    <div class="status">Schwarze Taste: <span id="schwarzeTaste">-</span></div>
    <div class="status">Gabel: <span id="gabel">-</span></div>
    <div class="status">Klingel Status: <span id="klingelAktiv">Inactive</span></div>
    <button class="button" onclick="toggleBell()">Toggle Klingel</button>
    <div id="dialHistory">
      <h2>Dial History</h2>
      <ul id="historyList"></ul>
    </div>
    <details>
      <summary>Konfiguration</summary>
      <form method="POST" action="/saveConfig">
        <label>URL Klingel aktivieren:</label>
        <input type="text" name="urlBellOn" value="%URL_BELL_ON%">
        <label>URL Klingel deaktivieren:</label>
        <input type="text" name="urlBellOff" value="%URL_BELL_OFF%">
        <label>URL Hörer abgehoben bei Klingeln:</label>
        <input type="text" name="urlOHookRing" value="%URL_OFF_HOOK_RING%">
        <label>URL Hörer abgehoben ohne Klingeln:</label>
        <input type="text" name="urlOHookNoRing" value="%URL_OFF_HOOK_NO_RING%">
        <label>URL Hörer aufgelegt:</label>
        <input type="text" name="urlOnHook" value="%URL_ON_HOOK%">
        <label>URL Anfang wahl:</label>
        <input type="text" name="urlDialStarted" value="%URL_NUMBER_DIAL_STARTED%">
        <label>URL gewählt:</label>
        <input type="text" name="urlOnDialed" value="%URL_ON_DIALED%">
        <label>URL unbekannte Nummer:</label>
        <input type="text" name="urlUnknownNumber" value="%URL_UNKNOWN%">
        <label>URL Schwarze Taste:</label>
        <input type="text" name="urlBlackButton" value="%URL_BLACK%">
        <h3>Nummer-Mappings</h3>
        %NUM_CONFIG_FIELDS%
        <input type="submit" value="Speichern">
      </form>
    </details>
  </div>
  <script>
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    var historyList = [];

    function initWebSocket() {
      websocket = new WebSocket(gateway);
      websocket.onopen = function() { console.log('WebSocket connected'); };
      websocket.onclose = function() { console.log('WebSocket disconnected'); setTimeout(initWebSocket, 2000); };
      websocket.onmessage = function(event) {
        var data = JSON.parse(event.data);
        document.getElementById("schwarzeTaste").innerText = data.schwarzeTaste ? "Gedrückt" : "Losgelassen";
        document.getElementById("gabel").innerText = data.gabel ? "Abgehoben" : "Aufgelegt";
        document.getElementById("klingelAktiv").innerText = data.klingelAktiv ? "Active" : "Inactive";
        if (data.dialNumber !== undefined) {
          historyList.unshift({ number: data.dialNumber, timestamp: Date.now() });
          if (historyList.length > 10) historyList.pop();
          updateHistory();
        }
      };
    }

    function updateHistory() {
      var listEl = document.getElementById("historyList");
      listEl.innerHTML = "";
      historyList.forEach(function(entry) {
        var elapsed = Math.floor((Date.now() - entry.timestamp) / 1000);
        var li = document.createElement("li");
        li.textContent = "Number: " + entry.number + " (" + elapsed + " s ago)";
        listEl.appendChild(li);
      });
    }

    setInterval(updateHistory, 1000);

    function toggleBell() {
      var current = document.getElementById("klingelAktiv").innerText;
      websocket.send(JSON.stringify({ klingelAktiv: current !== "Active" }));
    }

    window.addEventListener('load', initWebSocket, false);
  </script>
</body>
</html>
)rawliteral";

const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>WiFi Configuration</title>
  <style>
    body { font-family: Arial, sans-serif; background: #fafafa; padding: 20px; }
    h1 { color: #333; }
    form { margin-top: 20px; }
    input[type="text"] { padding: 10px; width: 100%; margin: 5px 0; }
    input[type="submit"] { padding: 10px 20px; background: #007BFF; border: none; color: #fff; cursor: pointer; }
    input[type="submit"]:hover { background: #0056b3; }
  </style>
</head>
<body>
  <h1>WiFi Configuration</h1>
  <form method="POST" action="/save">
    <label>SSID:</label><br>
    <input type="text" name="ssid"><br>
    <label>Password:</label><br>
    <input type="text" name="password"><br><br>
    <input type="submit" value="Save">
  </form>
</body>
</html>
)rawliteral";



// ***** Konfigurationsmodus (AP + Webinterface) *****
void startConfigMode() {
  Serial.println("Entering configuration mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Oberlab_Telefon");
  
  server.on("/", HTTP_GET, [] (AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", config_html);
  });

  server.on("/save", HTTP_POST, [] (AsyncWebServerRequest *request) {
    String newSSID = "";
    String newPassword = "";
    if(request->hasParam("ssid", true)) {
      newSSID = request->getParam("ssid", true)->value();
    }
    if(request->hasParam("password", true)) {
      newPassword = request->getParam("password", true)->value();
    }
    Preferences preferences;
    preferences.begin("wifi", false);
    preferences.putString("ssid", newSSID);
    preferences.putString("password", newPassword);
    preferences.end();
    request->send(200, "text/html", "<html><body><h1>Credentials saved. Restarting...</h1></body></html>");
    delay(2000);
    ESP.restart();
  });
  server.begin();
  Serial.println("Configuration portal started.");
  ArduinoOTA.begin();
  while(true) {
    ArduinoOTA.handle();
    delay(10);
  }
}



// ***** Interrupt-Service-Routine für die Wählscheibe (Entprellung) *****
void IRAM_ATTR dialISR() {
  static unsigned long lastDebounceTime = 0;
  unsigned long now = millis();
  if (now - lastDebounceTime > 75) {
    numberDialInProgress = true;
    pulseCount++;
    lastPulseTime = now;
    lastDebounceTime = now;
  }
}

// ***** Hintergrundtask: Erzeugt Klingelimpulse bei aktivem Klingelmodus *****
void pulseTask(void * parameter) {
  for (;;) {
    if (klingelAktiv && klingel_not_paused) {
      
      digitalWrite(in1Pin_h_bridge, HIGH);
      digitalWrite(in2Pin_h_bridge, LOW);

      vTaskDelay(pdMS_TO_TICKS(10));
      digitalWrite(in1Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(5));
      digitalWrite(in2Pin_h_bridge, HIGH);
      vTaskDelay(pdMS_TO_TICKS(10));
      digitalWrite(in2Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(5));
    } else {
      digitalWrite(in1Pin_h_bridge, LOW);
      digitalWrite(in2Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}



// ***** Task zur Auswertung der Wählscheibe und URL-Aufrufe *****
void dialTask(void * parameter) {
  for (;;) {
    unsigned long now = millis();
    if (pulseCount > 0 && (now - lastPulseTime > 200)) {
      noInterrupts();
      int count = pulseCount;
      pulseCount = 0;
      interrupts();
      int digit = (count == 10) ? 0 : count;
      dialNumber += String(digit);
      Serial.printf("Digit added: %d\n", digit);
    }

    // zuende gewählt, nach 3 Sekunden Stillstand
    if (pulseCount == 0 && dialNumber.length() > 0 && (now - lastPulseTime > 3000)) {
      numberDialInProgress = false;
      // Nummer gesamt senden
      ws.textAll(String("{ \"dialNumber\": \"") + dialNumber + "\" }");
      // passende URL auswählen oder unbekannt
      String callURL = urlUnknownNumber;
      int idx = dialNumber.toInt();
      if (millis() < 30*1000) {
        if (dialNumber == "1234")
          startConfigMode();
      }
      sendURL(urlOnDialed);
      for (int i = 1; i <= 5; i++) {
        if (dialNumber == mapKey[i]) {
          callURL = mapURL[i];
          break;
        }
      }
      if(currentGabel == GABEL_ABGENOMMEN)
        sendURL(callURL);
      Serial.printf("Called URL for number %s: %s\n", dialNumber.c_str(), callURL.c_str());
      dialNumber = "";
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ***** WebSocket-Event-Handler *****
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client,
               AwsEventType type, void * arg, uint8_t * data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u connected\n", client->id());
    String json = "{";
    json += "\"schwarzeTaste\":" + String(digitalRead(schwarzeTastePin)==LOW?"true":"false");
    json += ",\"gabel\":" + String(digitalRead(gabelPin)==LOW?"true":"false");
    json += ",\"klingelAktiv\":" + String(klingelAktiv?"true":"false");
    json += "}";
    client->text(json);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    String msg;
    for (size_t i = 0; i < len; i++) msg += (char)data[i];
    Serial.printf("Received WS message: %s\n", msg.c_str());
    if (msg.indexOf("klingelAktiv") >= 0) {
      klingelAktiv = (msg.indexOf("true") >= 0);
      String resp = String("{ \"klingelAktiv\": ") + (klingelAktiv?"true":"false") + " }";
      ws.textAll(resp);
      // Klingel-URLs aufrufen
      sendURL(klingelAktiv ? urlBellOn : urlBellOff);
    }
  }
}

// ***** Status an alle WebSocket-Clients senden *****
void notifyClients() {
  String json = "{";
  json += "\"schwarzeTaste\":" + String(digitalRead(schwarzeTastePin)==LOW?"true":"false");
  json += ",\"gabel\":" + String(digitalRead(gabelPin)==LOW?"true":"false");
  json += ",\"klingelAktiv\":" + String(klingelAktiv?"true":"false");
  json += "}";
  ws.textAll(json);
}

void setup() {
  Serial.begin(115200);
  // Pin-Setup
  pinMode(in1Pin_h_bridge, OUTPUT);
  pinMode(in2Pin_h_bridge, OUTPUT);
  pinMode(dialPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(dialPin), dialISR, FALLING);
  pinMode(schwarzeTastePin, INPUT_PULLUP);
  pinMode(gabelPin, INPUT_PULLUP);
  pinMode(resetPin, INPUT_PULLUP);

  // Config-Modus prüfen
  delay(50);
  if (digitalRead(resetPin) == LOW) {
    startConfigMode();
  }

  // WLAN-Credentials laden
  Preferences wifiPrefs;
  wifiPrefs.begin("wifi", true);
  wifiSSID = wifiPrefs.getString("ssid", "");
  wifiPassword = wifiPrefs.getString("password", "");
  wifiPrefs.end();

  // WLAN verbinden
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.print("Connecting to WiFi: ");
  Serial.println(wifiSSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print('.');
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());

  // URL-Konfiguration laden
  Preferences cfg;
  cfg.begin("config", true);
  urlBellOn            = cfg.getString("urlBellOn", "");
  urlBellOff           = cfg.getString("urlBellOff", "");
  urlOHookRing         = cfg.getString("urlOHookRing", "");
  urlOHookNoRing       = cfg.getString("urlOHookNoRing", "");
  urlOnHook            = cfg.getString("urlOnHook", "");
  urlDialStarted       = cfg.getString("urlDialStarted", "");
  urlOnDialed          = cfg.getString("urlOnDialed", "");
  urlUnknownNumber     = cfg.getString("url_unknown", "");
  urlBlackButton       = cfg.getString("urlBlackButton", "");
  char keyBuf[16];
  for (int i = 1; i <= 5; i++) {
    snprintf(keyBuf, sizeof(keyBuf), "mapKey%d", i);
    mapKey[i] = cfg.getString(keyBuf, "");
    snprintf(keyBuf, sizeof(keyBuf), "mapURL%d", i);
    mapURL[i] = cfg.getString(keyBuf, "");
  }
  cfg.end();

  // OTA starten
  ArduinoOTA.begin();

  // WebSocket & Webserver
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * req) {
    String page = FPSTR(index_html);

    // Platzhalter ersetzen
    String fields;
    for (int i = 1; i <= 5; i++) {
      fields += String("<label>Nummer ") + i + ":</label>";
      fields += String("<input type=\"text\" name=\"mapKey") + i +
                String("\" value=\"") + mapKey[i] + String("\">");
      fields += String("<label>URL Nummer ") + i + ":</label>";
      fields += String("<input type=\"text\" name=\"mapURL") + i +
                String("\" value=\"") + mapURL[i] + String("\">");
    }
    page.replace("%NUM_CONFIG_FIELDS%", fields);
    page.replace("%URL_BELL_ON%", urlBellOn);
    page.replace("%URL_BELL_OFF%", urlBellOff);
    page.replace("%URL_OFF_HOOK_RING%", urlOHookRing);
    page.replace("%URL_OFF_HOOK_NO_RING%", urlOHookNoRing);
    page.replace("%URL_ON_HOOK%", urlOnHook);
    page.replace("%URL_NUMBER_DIAL_STARTED%", urlDialStarted);
    page.replace("%URL_ON_DIALED%", urlOnDialed);
    page.replace("%URL_UNKNOWN%", urlUnknownNumber);
    page.replace("%URL_BLACK%", urlBlackButton);
    req->send(200, "text/html", page);
  });


  server.on("/saveConfig", HTTP_POST, [](AsyncWebServerRequest * req) {
    req->send(200, "text/html", "<html><body><h1>Saving Config. Restarting...</h1></body></html>");
    Serial.println("save config!");
    Preferences prefs;
    prefs.begin("config", false);
    prefs.putString("urlBellOn", req->getParam("urlBellOn", true)->value());
    prefs.putString("urlBellOff", req->getParam("urlBellOff", true)->value());
    prefs.putString("urlOHookRing", req->getParam("urlOHookRing", true)->value());
    prefs.putString("urlOHookNoRing", req->getParam("urlOHookNoRing", true)->value());
    prefs.putString("urlOnHook", req->getParam("urlOnHook", true)->value());
    prefs.putString("urlDialStarted", req->getParam("urlDialStarted", true)->value());
    prefs.putString("urlOnDialed", req->getParam("urlOnDialed", true)->value());
    prefs.putString("url_unknown", req->getParam("urlUnknownNumber", true)->value());
    prefs.putString("urlBlackButton", req->getParam("urlBlackButton", true)->value());
    char key[16];
    for (int i = 1; i <= 5; i++) {
      snprintf(key, sizeof(key), "mapKey%d", i);
      prefs.putString(key, req->getParam(key, true)->value());
      snprintf(key, sizeof(key), "mapURL%d", i);
      prefs.putString(key, req->getParam(key, true)->value());
    }
    prefs.end();
    Serial.println("save config done!");
    req->send(200, "text/html", "<html><body><h1>Config saved. Restarting...</h1></body></html>");
    delay(20000);
    ESP.restart();
  });

  server.on("/url_after_hang_up", HTTP_ANY, [](AsyncWebServerRequest * req) {
    Serial.println("aktivate URL call after hang up");
    url_after_hang_up = true;
    req->send(200, "text/html", "<html><body>URL call after hang up</body></html>");
  });

  server.on("/url_after_lift", HTTP_ANY, [](AsyncWebServerRequest * req) {
    Serial.println("aktivate URL call after hang up");
    url_after_lift = true;
    req->send(200, "text/html", "<html><body>URL call after lift</body></html>");
  });

  server.on("/klingel_an", HTTP_ANY, [](AsyncWebServerRequest * req) {
    Serial.println("url call klingel an");
    klingelAktiv = true;
    req->send(200, "text/html", "<html><body>klingel an</body></html>");
  });

  server.on("/klingel_aus", HTTP_ANY, [](AsyncWebServerRequest * req) {
    Serial.println("url call klingel aus");
    klingelAktiv = false;
    req->send(200, "text/html", "<html><body>klingel aus</body></html>");
  });

  server.begin();

  // Tasks starten
  xTaskCreatePinnedToCore(pulseTask, "PulseTask", 1024, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(dialTask, "DialTask", 2048, NULL, 1, NULL, 1);
}

void loop() {
  ArduinoOTA.handle();

  // Tasten-Status überwachen
  bool currentSchwarz = (digitalRead(schwarzeTastePin) == LOW);
  currentGabel  = (digitalRead(gabelPin) == LOW);
  static bool lastSchwarz = currentSchwarz;
  static bool lastGabel   = currentGabel;
  static bool lastnumberDialInProgress = numberDialInProgress;

  // did the user do anything (press black button or pick up the receiver)
  if ((currentSchwarz != lastSchwarz) || (currentGabel != lastGabel)) {
    notifyClients();
    
    // Schwarze Taste gedrückt oder losgelassen
    if (currentSchwarz && !lastSchwarz) sendURL(urlBlackButton);

    // Gabel aufgenommen oder aufgelegt
    if (currentGabel != lastGabel) {

      // Es hat geklingelt und die Gabel wurde abgenommen = Ein Anruf kommt rein
      if (klingelAktiv && currentGabel == GABEL_ABGENOMMEN)
        klingelAktiv = false;
        sendURL(urlOHookRing);

      // Es hat nicht geklingelt und die Gabel wurde abgenommen = Es soll ein Anruf gemacht werden
      if (!klingelAktiv && currentGabel == GABEL_ABGENOMMEN)
        klingelAktiv = false;
        sendURL(urlOHookNoRing);

      // Legacy mode, Url senden wenn freigeschatet
      //if((currentGabel == GABEL_ABGENOMMEN) && url_after_lift){
      //  url_after_lift = false;
      //  sendURL(urlOffHook);
      //} 

      if((currentGabel  == GABEL_AUFGELEGT) && url_after_hang_up ){
          url_after_hang_up = false;
          sendURL(urlOnHook);
      }
    }
    lastSchwarz = currentSchwarz;
    lastGabel   = currentGabel;
  }

  // Ist der Hörer aufgenommen und wird angefangen zu wählen?
  if((currentGabel == GABEL_ABGENOMMEN) && numberDialInProgress == true && lastnumberDialInProgress == false){
    sendURL(urlDialStarted);
  }
  lastnumberDialInProgress = numberDialInProgress;
  
  delay(50);

  klingel_not_paused = (millis()/1000)%2 != 0;
}
