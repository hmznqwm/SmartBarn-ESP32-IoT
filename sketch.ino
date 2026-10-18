/*
  ================================================================================
  SMART AUTOMATED LIVESTOCK/POULTRY BARN SYSTEM (ESP32)
  SISTEM KANDANG OTOMATIS & MONITORING LINGKUNGAN PINTAR
  
  Komponen & Fungsi:
  - SERVO MOTOR (GPIO 13)    : Sistem Bagian Pemberi Makanan Otomatis (Feeder Gate)
  - ULTRASONIK HC-SR04       : Mengukur Tinggi / Kedalaman Air Minum Kandang (TRIG: 19, ECHO: 23)
  - DHT22 (GPIO 5)           : Monitoring Suhu & Kelembapan Ruang Kandang
  - MQ-135 (GPIO 34 ADC1)    : Monitoring Kualitas Udara, Bau & Gas Amonia Kotoran
  - RELAY 1-CHANNEL (GPIO 18): Pengendali Kipas Dinamo Exhaust / Pendingin Kandang
  
  Fitur Kontrol:
  - Pengisian Makanan Ternak Terjadwal & Tombol Cepat Dispenser (Servo 3s)
  - Monitoring Ketinggian Air Minum Realtime + Peringatan Air Habis
  - Histeresis Otomatis Kipas Pendingin Suhu & Pembuang Gas Amonia
  - Web Dashboard Interaktif (Port 80)
  - Bot Telegram Realtime Alert & Remote Control
  - Cloud MQTT Broker (broker.emqx.io)
  ================================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>

// ====== 1. PENGATURAN WIFI ======
#if __has_include(<wokwi_config.h>) || defined(WOKWI_SIMULATION)
  const char *WIFI_SSID = "Wokwi-GUEST";
  const char *WIFI_PASS = "";
#else
  const char *WIFI_SSID = "YOUR_WIFI_SSID";
  const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
#endif

// ====== 2. CLOUD MQTT BROKER (EMQX) ======
const char *MQTT_BROKER = "broker.emqx.io";
const int   MQTT_PORT   = 1883;
const char *TOPIC_DATA  = "hami_kandang/data";
const char *TOPIC_CMD   = "hami_kandang/cmd";

// ====== 3. BOT TELEGRAM ======
const char *BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN";
const char *CHAT_ID   = "YOUR_TELEGRAM_CHAT_ID";

// ====== 4. PIN HARDWARE ESP32 ======
#define DHT_PIN        5    // Data DHT22 -> GPIO 5
#define DHT_TYPE       DHT22
#define MQ135_PIN      34   // Analog Out MQ-135 -> GPIO 34 (ADC1)
#define RELAY_PIN      18   // Sinyal IN Relay Kipas -> GPIO 18
#define TRIG_PIN       19   // Ultrasonic Trigger -> GPIO 19
#define ECHO_PIN       23   // Ultrasonic Echo -> GPIO 23
#define SERVO_PIN      13   // PWM Servo Pemberi Makan -> GPIO 13

// ====== 5. PARAMETER AIR MINUM & MAKANAN ======
// Parameter Wadah/Tandon Air Minum Kandang (Ultrasonik membaca dari atas ke bawah)
const float TINGGI_WADAH_AIR    = 30.0; // Tinggi total bibir wadah ke dasar (cm)
const float JARAK_SENSOR_PENUH  = 5.0;  // Jarak ke air saat wadah penuh (cm)
const float KEDALAMAN_MAX_AIR   = 25.0; // Kedalaman air maksimal (30 - 5 = 25 cm)
const float BATAS_AIR_KRITIS_CM = 6.0;  // Air <= 6 cm -> Peringatan Air Habis!

// Parameter Servo Pemberi Makanan Otomatis
const int SERVO_TUTUP_MAKAN = 0;   // Katup makanan tertutup rapat
const int SERVO_BUKA_MAKAN  = 90;  // Katup makanan terbuka mengucurkan pakan
const unsigned long DURASI_BERI_MAKAN = 3000; // Buka selama 3 detik lalu otomatis tutup

// Parameter Suhu & Gas Kandang
const float SUHU_KIPAS_NYALA = 30.0; // Suhu >= 30.0 C -> Kipas ON
const float SUHU_KIPAS_MATI  = 28.0; // Suhu <= 28.0 C -> Kipas OFF
const int GAS_THRESHOLD_WARNING = 2400; // Gas amonia waspada
const int GAS_THRESHOLD_DANGER  = 3200; // Gas amonia bahaya

// Interval Timer
const unsigned long READ_MS = 1500;
const unsigned long POLL_MS = 8000;

// ====== 6. OBJEK GLOBAL ======
DHT dht(DHT_PIN, DHT_TYPE);
Servo feederServo;
WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
Preferences prefs;

// ====== 7. VARIABEL DATA & STATUS ======
float temp = 0.0, hum = 0.0;
int gasRaw = 0;
int gasPpmEstimate = 0;

// Variabel Air Minum (Sensor Ultrasonik)
float rawDistanceCm = 0.0;     // Jarak pantul sensor ke permukaan air
float waterDepthCm = 0.0;      // Kedalaman aktual air minum (cm)
int waterPercentage = 0;       // Persentase isi air minum (0 - 100%)

// Variabel Pemberi Makanan (Servo)
bool isDispensingFood = false;
unsigned long dispenseStartTime = 0;
bool feederGateOpen = false;

// Variabel Kontrol Sistem
bool relayState = false;       // Kipas Pendingin
bool autoMode = true;          // Mode Otomatis
bool hasData = false, sensorErr = false;
uint8_t errCount = 0;
String systemStatus = "Memulai sistem kandang otomatis...";

// Tracking Notifikasi
unsigned long lastRead = 0, lastPoll = 0, lastWifiCheck = 0, lastNotifyTry = 0;
unsigned long lastMqttRetry = 0;
bool prevRelayState = false;
bool prevWaterCritical = false;

// Forward Declaration
void publishMQTTData();
void evaluateSystemLogic();
void setRelay(bool turnOn);
void setFeederGate(bool open);
void triggerFeedDispense();

// ---------- KENDALI HARDWARE ----------
void setRelay(bool turnOn) {
  relayState = turnOn;
  if (turnOn) {
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW); // Active Low modul relay
  } else {
    pinMode(RELAY_PIN, INPUT);    // Floating / Hi-Z (Relay Mati)
  }
}

void setFeederGate(bool open) {
  feederGateOpen = open;
  int angle = open ? SERVO_BUKA_MAKAN : SERVO_TUTUP_MAKAN;
  feederServo.write(angle);
}

void triggerFeedDispense() {
  Serial.println("[Makanan] Membuka dispenser makanan (3 detik)...");
  setFeederGate(true);
  isDispensingFood = true;
  dispenseStartTime = millis();
}

// ---------- PEMBACAAN KEDALAMAN AIR MINUM (ULTRASONIK) ----------
float readUltrasonicRaw() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 26000); // Timeout ~4.5m
  if (duration <= 0) {
    return rawDistanceCm > 0 ? rawDistanceCm : 15.0; // Gunakan nilai sebelumnya jika timeout
  }
  float dist = (duration * 0.0343) / 2.0;
  return constrain(dist, 2.0, 400.0);
}

void calculateWaterLevel(float distCm) {
  rawDistanceCm = distCm;
  // Jika air semakin penuh, jarak sensor ke air semakin kecil (JARAK_SENSOR_PENUH = 5 cm)
  // Kedalaman air = TINGGI_WADAH_AIR - jarakSensor
  float depth = TINGGI_WADAH_AIR - rawDistanceCm;
  if (depth < 0.0) depth = 0.0;
  if (depth > KEDALAMAN_MAX_AIR) depth = KEDALAMAN_MAX_AIR;
  waterDepthCm = depth;

  // Konversi ke persentase (0 cm = 0%, KEDALAMAN_MAX_AIR = 100%)
  int pct = (int)((waterDepthCm / KEDALAMAN_MAX_AIR) * 100.0);
  waterPercentage = constrain(pct, 0, 100);
}

// ---------- FILTER ANALOG GAS MQ-135 ----------
int readFilteredMQ135() {
  long sum = 0;
  const int NUM_SAMPLES = 16;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogRead(MQ135_PIN);
    delayMicroseconds(100);
  }
  return (int)(sum / NUM_SAMPLES);
}

int calculateEstimatedPPM(int rawAdc) {
  if (rawAdc <= 1200) return 400;
  float ratio = (float)(rawAdc - 1200) / (4095.0 - 1200.0);
  int ppm = 400 + (int)(pow(ratio, 1.35) * 2600.0);
  return constrain(ppm, 400, 3000);
}

// ---------- EVALUASI LOGIKA SISTEM KANDANG ----------
void evaluateSystemLogic() {
  if (!hasData) return;

  bool gasHazard = (gasRaw >= GAS_THRESHOLD_DANGER);
  bool gasWarning = (gasRaw >= GAS_THRESHOLD_WARNING);
  bool waterCritical = (waterDepthCm <= BATAS_AIR_KRITIS_CM);

  // Jika mode manual:
  if (!autoMode) {
    if (gasHazard) {
      systemStatus = "Manual: Kipas " + String(relayState ? "ON" : "OFF") + " (BAHAYA GAS AMONIA!)";
    } else {
      systemStatus = "Mode Manual: Kipas " + String(relayState ? "ON" : "OFF") +
                     " | Air: " + String(waterPercentage) + "%";
    }
    return;
  }

  // JIKA MODE OTOMATIS:
  // 1. Gas Amonia Pekat (Prioritas Darurat)
  if (gasHazard) {
    systemStatus = "Otomatis: GAS AMONIA PEKAT! (Exhaust ON)";
    setRelay(true);
  } else if (gasWarning) {
    systemStatus = "Otomatis: Waspada Bau Kandang (Kipas ON)";
    setRelay(true);
  } else {
    // 2. Histeresis Suhu Ruang Kandang
    if (temp >= SUHU_KIPAS_NYALA) {
      systemStatus = "Otomatis: Suhu Panas (Kipas ON)";
      setRelay(true);
    } else if (temp <= SUHU_KIPAS_MATI) {
      systemStatus = "Otomatis: Suhu Sejuk Ideal (Kipas OFF)";
      setRelay(false);
    } else {
      systemStatus = relayState ? "Otomatis: Mendinginkan Kandang (Kipas ON)"
                                : "Otomatis: Suhu Kandang Nyaman (Kipas OFF)";
    }
  }

  if (waterCritical) {
    systemStatus += " | AIR MINUM KRITIS!";
  }
}

// ---------- PEMBACAAN SEMUA SENSOR ----------
void readSensors() {
  gasRaw = readFilteredMQ135();
  gasPpmEstimate = calculateEstimatedPPM(gasRaw);

  float currentDist = readUltrasonicRaw();
  calculateWaterLevel(currentDist);

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(t) || isnan(h)) {
    if (errCount < 255) errCount++;
    if (errCount >= 3) {
      sensorErr = true;
      hasData = true;
      if (autoMode) setRelay(true); // Safemode
    }
    evaluateSystemLogic();
    publishMQTTData();
    return;
  }

  errCount = 0;
  sensorErr = false;
  hasData = true;
  temp = t;
  hum = h;

  evaluateSystemLogic();
  publishMQTTData();

  Serial.printf("[Kandang] Suhu: %.1f C | Lembap: %.1f %% | Gas: %d ADC | Air Minum: %.1f cm (%d%%) | Makanan (Servo): %s | Kipas: %s\n",
                temp, hum, gasRaw, waterDepthCm, waterPercentage,
                feederGateOpen ? "TERBUKA (MEMBERI MAKAN)" : "TERTUTUP",
                relayState ? "ON" : "OFF");
}

// ---------- CLOUD MQTT PUBLISH ----------
void publishMQTTData() {
  if (!mqttClient.connected()) return;

  char json[450];
  snprintf(json, sizeof(json),
           "{\"valid\":%s,\"temp\":%.1f,\"hum\":%.1f,\"gas_raw\":%d,\"gas_ppm\":%d,\"water_cm\":%.1f,\"water_pct\":%d,\"relay\":%s,\"feeder_open\":%s,\"auto\":%s,\"status\":\"%s\"}",
           hasData ? "true" : "false", temp, hum, gasRaw, gasPpmEstimate,
           waterDepthCm, waterPercentage, relayState ? "true" : "false",
           feederGateOpen ? "true" : "false", autoMode ? "true" : "false",
           systemStatus.c_str());

  mqttClient.publish(TOPIC_DATA, json, false);
}

// ---------- EKSEKUSI PERINTAH (WEB / MQTT / TELEGRAM) ----------
void executeCommand(String action) {
  action.trim();
  Serial.println("[Perintah Masuk]: " + action);
  String actLower = action;
  actLower.toLowerCase();

  if (actLower == "/auto" || actLower == "auto" || actLower == "/auto_on" || actLower == "auto_on" || actLower == "/otomatis") {
    autoMode = true;
    evaluateSystemLogic();
    prefs.putBool("auto", true);
  } else if (actLower == "/auto_off" || actLower == "auto_off" || actLower == "/manual" || actLower == "manual") {
    autoMode = false;
    evaluateSystemLogic();
    prefs.putBool("auto", false);
  } else if (actLower == "/on" || actLower == "on") {
    autoMode = false;
    setRelay(true);
    evaluateSystemLogic();
    prefs.putBool("auto", false);
    prefs.putBool("relay", true);
  } else if (actLower == "/off" || actLower == "off") {
    autoMode = false;
    setRelay(false);
    evaluateSystemLogic();
    prefs.putBool("auto", false);
    prefs.putBool("relay", false);
  } else if (actLower == "/feed" || actLower == "feed" || actLower == "/makan" || actLower == "/pakan" || actLower == "pakan") {
    // Tombol Beri Makanan Otomatis via Servo
    triggerFeedDispense();
  } else if (actLower == "/servo_open" || actLower == "servo_open" || actLower == "/buka_pakan") {
    setFeederGate(true);
    isDispensingFood = false;
  } else if (actLower == "/servo_close" || actLower == "servo_close" || actLower == "/tutup_pakan") {
    setFeederGate(false);
    isDispensingFood = false;
  }

  publishMQTTData();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  executeCommand(message);
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttRetry < 4000) return;
  lastMqttRetry = millis();

  String clientId = "ESP32_Kandang_" + String(random(0xffff), HEX);
  if (mqttClient.connect(clientId.c_str(), TOPIC_DATA, 0, false, "{\"valid\":false,\"status\":\"Offline\"}")) {
    mqttClient.subscribe(TOPIC_CMD);
    publishMQTTData();
  }
}

// ---------- DASHBOARD WEB PORT 80 ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Kandang Otomatis (ESP32)</title>
<style>
:root{--bg:#0b1120;--card:#1e293b;--border:#334155;--text:#f8fafc;--muted:#94a3b8;--primary:#38bdf8;--water:#0284c7;--feed:#10b981;--warn:#f59e0b;--danger:#ef4444}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);padding:18px;text-align:center}
h1{font-size:1.6rem;color:var(--primary);margin-bottom:4px}
.sub{font-size:.85rem;color:var(--muted);margin-bottom:18px}
.container{max-width:760px;margin:0 auto;display:flex;flex-direction:column;gap:14px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:16px;text-align:left;position:relative}
.card .title{font-size:.72rem;color:var(--muted);text-transform:uppercase;font-weight:700;letter-spacing:1px}
.card .val{font-size:1.55rem;font-weight:800;margin-top:6px}
.card.full{grid-column:1/-1}
.progress-bar{width:100%;height:10px;background:#0f172a;border-radius:5px;overflow:hidden;margin-top:10px}
.progress-fill{height:100%;background:var(--water);width:0%;transition:width .4s}
.banner{padding:12px;border-radius:12px;font-size:.9rem;font-weight:600;display:flex;align-items:center;justify-content:center;gap:8px}
.banner.ok{background:#064e3b;border:1px solid #059669;color:#a7f3d0}
.banner.alert{background:#7f1d1d;border:1px solid #dc2626;color:#fee2e2}
.btn-group{display:flex;gap:8px;flex-wrap:wrap;justify-content:center;margin-top:12px}
button{background:#334155;border:1px solid var(--border);color:var(--text);padding:10px 16px;border-radius:10px;font-weight:600;cursor:pointer;font-size:.85rem;transition:all .2s ease}
button:hover{filter:brightness(1.15)}
button.primary{background:#0284c7;color:#fff;border:none}
button.feed{background:#059669;color:#fff;border:none}
button.danger{background:#e11d48;color:#fff;border:none}
button.auto-off{background:#475569;color:#cbd5e1}
</style></head><body>
<div class="container">
  <div>
    <h1>🐔 Smart Kandang Otomatis (ESP32)</h1>
    <div class="sub">Pemberi Makan (Servo) &bull; Kedalaman Air Minum (Ultrasonik) &bull; Suhu &bull; Gas Amonia &bull; Kipas</div>
  </div>
  <div id="status-banner" class="banner ok">Memeriksa kondisi kandang...</div>
  <div class="grid">
    <div class="card">
      <div class="title">🌡️ Suhu Kandang</div>
      <div class="val" id="temp">--</div>
    </div>
    <div class="card">
      <div class="title">💧 Kelembapan</div>
      <div class="val" id="hum">--</div>
    </div>
    <div class="card">
      <div class="title">💨 Gas Amonia (MQ-135)</div>
      <div class="val" id="gas">--</div>
    </div>
    <div class="card">
      <div class="title">🚰 Kedalaman Air Minum</div>
      <div class="val" id="water">--</div>
      <div class="progress-bar"><div id="water-bar" class="progress-fill"></div></div>
    </div>
    <div class="card">
      <div class="title">🥣 Pintu Makanan (Servo)</div>
      <div class="val" id="servo">--</div>
    </div>
    <div class="card">
      <div class="title">🌀 Kipas Pendingin</div>
      <div class="val" id="relay">--</div>
    </div>
  </div>
  <div class="card full">
    <div class="title">Panel Kendali Makanan & Aktuator Kandang</div>
    <div class="btn-group">
      <button class="feed" onclick="sendCmd('/feed')">🥣 Beri Makanan (Buka Servo 3s)</button>
      <button id="btn-auto" class="primary" onclick="toggleAuto()">🤖 Mode Otomatis</button>
      <button class="primary" onclick="sendCmd('/on')">⚡ Kipas Nyala (ON)</button>
      <button class="danger" onclick="sendCmd('/off')">🛑 Kipas Mati (OFF)</button>
    </div>
    <div class="btn-group" style="margin-top:8px">
      <button onclick="sendCmd('/servo_open')">🔓 Buka Katup Makanan Manual</button>
      <button onclick="sendCmd('/servo_close')">🔒 Tutup Katup Makanan Manual</button>
    </div>
  </div>
</div>
<script>
let isAutoActive = true;
function toggleAuto(){ sendCmd(isAutoActive ? '/auto_off' : '/auto_on'); }
function sendCmd(act){ fetch('/cmd?action='+encodeURIComponent(act)).then(updateData); }
function updateData(){
  fetch('/data').then(r=>r.json()).then(d=>{
    isAutoActive = d.auto;
    document.getElementById('temp').textContent = d.temp + ' °C';
    document.getElementById('hum').textContent = d.hum + ' %';
    document.getElementById('gas').textContent = d.gas_ppm + ' PPM';
    
    // Status Air Minum (Ultrasonik)
    document.getElementById('water').textContent = d.water_cm + ' cm (' + d.water_pct + '%)';
    const wBar = document.getElementById('water-bar');
    wBar.style.width = d.water_pct + '%';
    wBar.style.background = d.water_pct <= 20 ? 'var(--danger)' : (d.water_pct <= 50 ? 'var(--warn)' : 'var(--water)');

    // Status Servo Makanan
    document.getElementById('servo').innerHTML = d.feeder_open 
      ? '<span style="color:#38bdf8">MENGUCURKAN MAKANAN (BUKA)</span>' 
      : '<span style="color:#94a3b8">TERTUTUP (STANDBY)</span>';

    // Status Kipas Relay
    document.getElementById('relay').innerHTML = d.relay 
      ? '<span style="color:#34d399">MENYALA (ON)</span>' 
      : '<span style="color:#94a3b8">STANDBY (OFF)</span>';

    const btnAuto = document.getElementById('btn-auto');
    if (d.auto) {
      btnAuto.className = 'primary';
      btnAuto.innerHTML = '🤖 Mode Otomatis (AKTIF)';
    } else {
      btnAuto.className = 'auto-off';
      btnAuto.innerHTML = '🖐️ Mode Manual (AKTIF)';
    }

    const b = document.getElementById('status-banner');
    b.textContent = d.status;
    b.className = (d.gas_raw >= 3200 || d.water_pct <= 20) ? 'banner alert' : 'banner ok';
  }).catch(()=>{});
}
setInterval(updateData, 1500); updateData();
</script></body></html>
)rawliteral";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
  char json[450];
  snprintf(json, sizeof(json),
           "{\"valid\":%s,\"temp\":%.1f,\"hum\":%.1f,\"gas_raw\":%d,\"gas_ppm\":%d,\"water_cm\":%.1f,\"water_pct\":%d,\"relay\":%s,\"feeder_open\":%s,\"auto\":%s,\"status\":\"%s\"}",
           hasData ? "true" : "false", temp, hum, gasRaw, gasPpmEstimate,
           waterDepthCm, waterPercentage, relayState ? "true" : "false",
           feederGateOpen ? "true" : "false", autoMode ? "true" : "false",
           systemStatus.c_str());
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleCmd() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("action")) executeCommand(server.arg("action"));
  server.send(200, "text/plain", "OK");
}

// ---------- BOT TELEGRAM ----------
String buildTelegramReport() {
  if (!hasData) return "Belum ada data sensor.";
  String msg = "📊 *STATUS SMART KANDANG OTOMATIS (ESP32)*\n\n";
  msg += "🌡️ *Suhu Ruang:* " + String(temp, 1) + " °C\n";
  msg += "💧 *Kelembapan:* " + String(hum, 1) + " %\n";
  msg += "💨 *Gas Amonia (MQ-135):* " + String(gasRaw) + " ADC (~" + String(gasPpmEstimate) + " PPM)\n";
  msg += "🚰 *Kedalaman Air Minum (Ultrasonik):* " + String(waterDepthCm, 1) + " cm (" + String(waterPercentage) + "%)\n";
  msg += "🚨 *Kondisi Air Minum:* " + String(waterPercentage <= 20 ? "🔴 HABIS / KRITIS!" : (waterPercentage <= 50 ? "🟡 CUKUP" : "🟢 AMAN")) + "\n";
  msg += "🥣 *Katup Makanan (Servo):* " + String(feederGateOpen ? "🔓 TERBUKA (MEMBERI MAKAN)" : "🔒 TERTUTUP") + "\n";
  msg += "🌀 *Kipas Pendingin:* " + String(relayState ? "⚡ MENYALA (ON)" : "🛑 STANDBY (OFF)") + "\n";
  msg += "⚙️ *Mode:* " + String(autoMode ? "🤖 Otomatis" : "🖐️ Manual") + "\n";
  msg += "📌 *Keterangan:* " + systemStatus;
  return msg;
}

void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    text.trim();
    String textLower = text;
    textLower.toLowerCase();

    if (textLower == "/status" || textLower == "status" || textLower == "/air") {
      bot.sendMessage(chat_id, buildTelegramReport(), "Markdown");
    } else if (textLower == "/feed" || textLower == "/makan" || textLower == "/pakan") {
      executeCommand("/feed");
      bot.sendMessage(chat_id, "🥣 *Pemberi Makanan Diaktifkan!*\nKatup servo dibuka selama 3 detik untuk mengucurkan pakan.", "Markdown");
    } else if (textLower == "/auto" || textLower == "/auto_on" || textLower == "/otomatis") {
      executeCommand("/auto_on");
      bot.sendMessage(chat_id, "✅ *Mode Otomatis DIAKTIFKAN*", "Markdown");
    } else if (textLower == "/manual" || textLower == "/auto_off") {
      executeCommand("/auto_off");
      bot.sendMessage(chat_id, "🖐️ *Mode Otomatis DIMATIKAN (Manual Aktif)*", "Markdown");
    } else if (textLower == "/on") {
      executeCommand("/on");
      bot.sendMessage(chat_id, "⚡ *Kipas Pendingin Dinyalakan (ON)*", "Markdown");
    } else if (textLower == "/off") {
      executeCommand("/off");
      bot.sendMessage(chat_id, "🛑 *Kipas Pendingin Dimatikan (OFF)*", "Markdown");
    } else if (textLower == "/start" || textLower == "/help") {
      String help = "🐔 *DAFTAR PERINTAH SMART KANDANG ESP32*\n\n";
      help += "📊 `/status` - Cek suhu, gas amonia & kedalaman air minum\n";
      help += "🥣 `/feed` atau `/makan` - Beri makanan (buka servo 3 detik)\n";
      help += "🤖 `/auto` - Aktifkan mode otomatis kandang\n";
      help += "🖐️ `/manual` - Mode manual\n";
      help += "⚡ `/on` - Nyalakan kipas\n";
      help += "🛑 `/off` - Matikan kipas\n";
      bot.sendMessage(chat_id, help, "Markdown");
    }
  }
}

void checkSafetyNotifications() {
  if (millis() - lastNotifyTry < 25000) return;

  bool waterCritical = (waterPercentage <= 20);
  if (waterCritical && !prevWaterCritical) {
    lastNotifyTry = millis();
    prevWaterCritical = true;
    String alert = "⚠️ *[PERINGATAN AIR MINUM KANDANG HABIS]* ⚠️\n" +
                   "Kedalaman air minum kandang tersisa: *" + String(waterDepthCm, 1) + " cm (" + String(waterPercentage) + "%)*!\n" +
                   "Segera isi ulang tandon air minum kandang agar ternak tidak kehausan.";
    bot.sendMessage(CHAT_ID, alert, "Markdown");
    return;
  } else if (!waterCritical && prevWaterCritical) {
    prevWaterCritical = false;
  }

  if (autoMode && relayState != prevRelayState) {
    lastNotifyTry = millis();
    prevRelayState = relayState;
    String notif = "🌀 *Status Kipas Kandang: " + String(relayState ? "MENYALA (ON)*" : "STANDBY (OFF)*") + "\n" +
                   "Kondisi: " + systemStatus;
    bot.sendMessage(CHAT_ID, notif, "Markdown");
  }
}

// ---------- SETUP SISTEM ----------
void setup() {
  Serial.begin(115200);

  prefs.begin("kandang", false);
  autoMode = prefs.getBool("auto", true);
  relayState = prefs.getBool("relay", false);
  setRelay(relayState);

  // Inisialisasi Ultrasonik Pembaca Air Minum
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Inisialisasi Servo Pemberi Makanan Otomatis
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  feederServo.setPeriodHertz(50);
  feederServo.attach(SERVO_PIN, 500, 2400);
  setFeederGate(false); // Katup tertutup saat awal

  // Inisialisasi Sensor MQ-135 & DHT22
  pinMode(MQ135_PIN, INPUT);
  dht.begin();
  lastRead = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  Serial.printf("\n[WiFi] Menghubungkan ke %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long startW = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startW < 10000) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Offline/Simulasi. Sistem tetap berjalan lokal.");
  }

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  secured_client.setInsecure();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/cmd", handleCmd);
  server.begin();

  Serial.println("Sistem Smart Kandang Otomatis (ESP32) Siap Beroperasi!");
}

// ---------- LOOP UTAMA ----------
void loop() {
  // Non-blocking Timer: Kembalikan servo ke posisi tertutup setelah 3 detik
  if (isDispensingFood && (millis() - dispenseStartTime >= DURASI_BERI_MAKAN)) {
    Serial.println("[Makanan] Waktu 3 detik selesai -> Menutup katup makanan.");
    setFeederGate(false);
    isDispensingFood = false;
    publishMQTTData();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    } else {
      mqttClient.loop();
    }
  }

  if (millis() - lastRead >= READ_MS) {
    lastRead = millis();
    readSensors();
  }

  server.handleClient();

  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll >= POLL_MS) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleTelegramMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastPoll = millis();
  }

  if (WiFi.status() == WL_CONNECTED) {
    checkSafetyNotifications();
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiCheck > 15000) {
      lastWifiCheck = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}
