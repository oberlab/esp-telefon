#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

String wifiSSID;
String wifiPassword;

// ***** Pin-Konfiguration *****
// Für die Klingel (L298N)
const int in1Pin_h_bridge = 32;   // IN1 des L298N
const int in2Pin_h_bridge = 33;   // IN2 des L298N
// Wählscheibe
const int dialPin  = 12;   // Eingang für die Wählscheibe (mit internem Pullup, idle HIGH)
// Weitere Eingänge:
const int schwarzeTastePin = 14; // "Schwarze Taste" (active LOW) – dient hier auch als "Config-Taste" beim Powerup
const int gabelPin         = 27; // "Gabel" – normally closed (Logik wird invertiert)
const int resetPin         = 25; // "Reset" – dient hier als "Config-Taste" beim Powerup

// ***** Variablen für Wählscheibe & Klingel *****
volatile int pulseCount = 0;              // Zählt Impulse der Wählscheibe
volatile unsigned long lastPulseTime = 0; // Zeitpunkt des letzten Impulses
String dialNumber = "";                   // Sammelt die gewählten Ziffern

volatile bool klingelAktiv = false;       // Steuert den Klingelmodus

// ***** WiFi, Webserver & WebSocket *****
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ***** HTML-Weboberfläche für Telefoninterface *****
// Hier wurden die Anzeige der gewählten Nummer als History integriert.
// Die "Dial Number"-Anzeige wurde entfernt und stattdessen wird unter dem Klingel-Button eine History (max. 10 Einträge)
// angezeigt. Jeder Eintrag zeigt an, wie lange das Wählen her ist.
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP32 Telefon Interface</title>
  <style>
    body { font-family: Arial, sans-serif; background: #f0f0f0; margin: 0; padding: 0; }
    .container { max-width: 600px; margin: 50px auto; background: #fff; padding: 20px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }
    h1 { text-align: center; color: #333; }
    .status { margin: 10px 0; padding: 10px; background: #e0e0e0; border-radius: 5px; }
    .button { display: block; width: 100%; padding: 10px; background: #007BFF; color: #fff; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; margin-top: 20px; }
    .button:hover { background: #0056b3; }
    #dialHistory { margin-top: 20px; }
    #dialHistory h2 { margin: 0 0 10px 0; }
    #historyList { list-style-type: none; padding: 0; margin: 0; }
    #historyList li { padding: 5px; border-bottom: 1px solid #ccc; }
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
  </div>
  <script>
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    var historyList = []; // Array zur Speicherung der History-Einträge (max. 10 Einträge)

    function initWebSocket() {
      console.log('Connecting to WebSocket...');
      websocket = new WebSocket(gateway);
      websocket.onopen = function(event) { 
         console.log('WebSocket connected'); 
      };
      websocket.onclose = function(event) {
        console.log('WebSocket disconnected, retrying in 2 seconds...');
        setTimeout(initWebSocket, 2000);
      };
      websocket.onmessage = function(event) {
        var data = JSON.parse(event.data);
        document.getElementById("schwarzeTaste").innerText = data.schwarzeTaste ? "Pressed" : "Released";
        document.getElementById("gabel").innerText = data.gabel ? "Pressed" : "Released";
        document.getElementById("klingelAktiv").innerText = data.klingelAktiv ? "Active" : "Inactive";
        // Wenn ein dialNumber empfangen wird, füge diesen Eintrag zur History hinzu
        if (data.dialNumber !== undefined) {
          var entry = {
            number: data.dialNumber,
            timestamp: Date.now()
          };
          // Neuer Eintrag oben hinzufügen
          historyList.unshift(entry);
          // Falls mehr als 10 Einträge, entferne den ältesten
          if (historyList.length > 10) {
            historyList.pop();
          }
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
        li.textContent = "Number: " + entry.number + " ( " + elapsed + " s ago )";
        listEl.appendChild(li);
      });
    }

    // Aktualisiere die History alle Sekunde, damit die "elapsed time" aktualisiert wird
    setInterval(updateHistory, 1000);

    function toggleBell() {
      var current = document.getElementById("klingelAktiv").innerText;
      if (current === "Active") {
        websocket.send('{"klingelAktiv": false}');
      } else {
        websocket.send('{"klingelAktiv": true}');
      }
    }

    window.addEventListener('load', initWebSocket, false);
  </script>
</body>
</html>
)rawliteral";

// ***** HTML-Seite für WiFi-Konfiguration (AP-Modus) *****
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

// ***** Interrupt-Service-Routine für die Wählscheibe (Debounce 75ms) *****
void IRAM_ATTR dialISR() {
  static unsigned long lastDebounceTime = 0;
  unsigned long now = millis();
  if (now - lastDebounceTime > 75) {
    pulseCount++;
    lastPulseTime = now;
    lastDebounceTime = now;
  }
}

// ***** Hintergrundtask: Erzeugt 25-Hz-Signal durch alternierendes Schalten von in1 und in2 *****
void pulseTask(void * parameter) {
  for (;;) {
    if (klingelAktiv) {
      digitalWrite(in1Pin_h_bridge, HIGH);
      digitalWrite(in2Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(15));

      digitalWrite(in2Pin_h_bridge, LOW);
      digitalWrite(in1Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(15));

      digitalWrite(in2Pin_h_bridge, HIGH);
      digitalWrite(in1Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(15));

      digitalWrite(in2Pin_h_bridge, LOW);
      digitalWrite(in1Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(15));
    } else {
      digitalWrite(in2Pin_h_bridge, LOW);
      digitalWrite(in1Pin_h_bridge, LOW);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// ***** Task zur Auswertung der Wählscheibe *****
// Nach ca. 1.7 Sekunden Inaktivität werden die gezählten Impulse zu einer Ziffer verarbeitet,
// und die komplette Nummer wird per WebSocket versendet.
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
      Serial.print("Digit added: ");
      Serial.println(digit);
    }
    if (pulseCount == 0 && dialNumber.length() > 0 && (now - lastPulseTime > 1700)) {
      String json = "{ \"dialNumber\": \"" + dialNumber + "\" }";
      ws.textAll(json);
      Serial.print("Complete dial number sent: ");
      Serial.println(dialNumber);
      dialNumber = "";
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ***** WebSocket-Event-Handler *****
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client,
               AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u connected\n", client->id());
    String json = "{";
    json += "\"schwarzeTaste\":";
    json += (digitalRead(schwarzeTastePin) == LOW ? "true" : "false");
    json += ",\"gabel\":";
    json += (digitalRead(gabelPin) == LOW ? "false" : "true");
    json += ",\"klingelAktiv\":";
    json += (klingelAktiv ? "true" : "false");
    json += "}";
    client->text(json);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    String msg = "";
    for (size_t i = 0; i < len; i++) {
      msg += (char)data[i];
    }
    Serial.printf("Received WS message: %s\n", msg.c_str());
    if (msg.indexOf("klingelAktiv") >= 0) {
      if (msg.indexOf("true") >= 0) {
        klingelAktiv = true;
      } else if (msg.indexOf("false") >= 0) {
        klingelAktiv = false;
      }
      String json = "{ \"klingelAktiv\": " + String(klingelAktiv ? "true" : "false") + " }";
      ws.textAll(json);
    }
  }
}

// ***** Funktion: Sendet den Status der Tasten per WebSocket *****
void notifyClients() {
  String json = "{";
  json += "\"schwarzeTaste\":";
  json += (digitalRead(schwarzeTastePin) == LOW ? "true" : "false");
  json += ",\"gabel\":";
  json += (digitalRead(gabelPin) == LOW ? "false" : "true");
  json += ",\"klingelAktiv\":";
  json += (klingelAktiv ? "true" : "false");
  json += "}";
  ws.textAll(json);
}

// ***** WiFi-Konfigurationsmodus: AP-Modus, falls Konfigurationstaste beim Powerup gedrückt *****
void startConfigMode() {
  Serial.println("Entering configuration mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_Config");
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

void setup() {
  Serial.begin(115200);
  
  // ***** Pin-Konfiguration *****
  pinMode(in1Pin_h_bridge, OUTPUT);
  pinMode(in2Pin_h_bridge, OUTPUT);
  digitalWrite(in1Pin_h_bridge, LOW);
  digitalWrite(in2Pin_h_bridge, LOW);
  
  pinMode(dialPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(dialPin), dialISR, FALLING);
  
  pinMode(schwarzeTastePin, INPUT_PULLUP);
  pinMode(gabelPin, INPUT_PULLUP);
  pinMode(resetPin, INPUT_PULLUP);

  // ***** Prüfe, ob beim Powerup die Konfigurationstaste gedrückt ist *****
  delay(500);
  if (digitalRead(resetPin) == LOW) {
    startConfigMode();
  }
  
  // ***** Lade gespeicherte WLAN-Zugangsdaten (falls vorhanden) *****
  Preferences preferences;
  preferences.begin("wifi", true);
  String storedSSID = preferences.getString("ssid", "");
  String storedPassword = preferences.getString("password", "");
  preferences.end();
  if (storedSSID.length() > 0) {
    wifiSSID = storedSSID;
    wifiPassword = storedPassword;
    Serial.println("Loaded stored WiFi credentials:");
    Serial.println(wifiSSID);
  } else {
    Serial.println("Using default WiFi credentials.");
  }
  
  // ***** WLAN-Verbindung im Client-Modus *****
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());
  
  // ***** OTA-Setup *****
  ArduinoOTA.setHostname("ESP32_Telephone");
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Update Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\n", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if(error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if(error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if(error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if(error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if(error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  
  // ***** WebSocket und Webserver für das Telefoninterface einrichten *****
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  server.begin();
  Serial.println("HTTP server started");
  
  // ***** Tasks starten *****
  xTaskCreatePinnedToCore(pulseTask, "PulseTask", 1024, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(dialTask, "DialTask", 2048, NULL, 1, NULL, 1);
}

void loop() {
  ArduinoOTA.handle();
  
  bool currentSchwarzeTasteState = (digitalRead(schwarzeTastePin) == LOW);
  bool currentGabelState = (digitalRead(gabelPin) == LOW);
  
  static bool lastSchwarzeTasteState = currentSchwarzeTasteState;
  static bool lastGabelState = currentGabelState;
  
  if (currentSchwarzeTasteState != lastSchwarzeTasteState || currentGabelState != lastGabelState) {
    Serial.print("Schwarze Taste: ");
    Serial.print(currentSchwarzeTasteState ? "Pressed" : "Released");
    Serial.print(" | Gabel: ");
    Serial.println(currentGabelState ? "Released" : "Pressed");
    notifyClients();
    lastSchwarzeTasteState = currentSchwarzeTasteState;
    lastGabelState = currentGabelState;
  }
  
  delay(50);
}
