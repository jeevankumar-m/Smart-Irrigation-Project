// ===== 1. Blynk + Wi-Fi settings =====
#define BLYNK_TEMPLATE_ID "TMPL3ncFfeyfc"
#define BLYNK_TEMPLATE_NAME "Smart Irrigation"
#define BLYNK_AUTH_TOKEN "mhhqEZzocKQ3sohxmTLqaBpBbUUw4-uM"

const char* WIFI_SSID = "MADHAN";       // 2.4 GHz only
const char* WIFI_PASS = "Chennai35";

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <BlynkSimpleEsp8266.h>
#include <DHT.h>

// ===== 2. Pins =====
#define DHT_PIN     D5
#define RAIN_PIN    D6
#define LIGHT_PIN   D7
#define RELAY_PIN   D1
// Soil moisture AO -> A0

// ===== 3. Calibration / settings =====
#define SOIL_DRY_RAW   800      // probe in dry air   (use your own reading)
#define SOIL_WET_RAW   350      // probe in water     (use your own reading)
#define RAIN_WET_LEVEL LOW      // rain board DO level when wet
#define DARK_LEVEL     LOW      // LDR divider: dark = LOW (LDR module DO: usually HIGH)
#define TEMP_MIN       19.0     // temperature factor 0 % at this temp
#define TEMP_MAX       35.0     // temperature factor 100 % at this temp

#define RELAY_ACTIVE_LOW  true  // flip to false if your relay works the other way round
#define PUMP_MAX_RUN_MS   (2UL * 60UL * 1000UL)   // pump auto-stops after 2 min
#define WIFI_GRACE_MS     10000UL                 // pump stops if Wi-Fi is gone this long
#define READ_INTERVAL_MS  2000UL
#define HEARTBEAT_MS      60000UL

// ===== Objects =====
DHT dht(DHT_PIN, DHT11);
ESP8266WebServer server(80);

// ===== Latest readings =====
float temp = 25.0, hum = 0;
bool  dhtOk = false;
int   soilRaw = 0, soilPct = 0;
int   rainState = 1, lightState = 0;
bool  raining = false, dark = true;
int   lightPct = 0;
float soilDryness = 0, tempFactor = 0, lightFactor = 0, rainFactor = 100;
int   wri = 0;

bool pumpOn = false;
unsigned long pumpStartMs = 0;
unsigned long wifiLostSince = 0;

// Blynk send-on-change bookkeeping (V0 temp, V1 soil%, V2 rain, V3 light, V4 WRI, V6 humidity, V7 soil raw)
const int   VPINS[7] = {0, 1, 2, 3, 4, 6, 7};
const float DELTA[7] = {0.5, 2, 1, 1, 2, 2, 10};
float lastSent[7];
unsigned long lastSentMs[7];

unsigned long lastReadMs = 0, lastBlynkTry = 0;
bool mdnsStarted = false;

// ===== Relay / pump =====
void setPump(bool on, bool notifyBlynk = true) {
  pumpOn = on;
  if (RELAY_ACTIVE_LOW) {
    if (on) { pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW); }
    else    { pinMode(RELAY_PIN, INPUT); }          // float = OFF, safe with 5V relay modules
  } else {
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  }
  if (on) pumpStartMs = millis();
  Serial.println(on ? ">>> PUMP ON" : ">>> PUMP OFF");
  if (notifyBlynk && Blynk.connected()) Blynk.virtualWrite(V5, on ? 1 : 0);
}

// Switch pressed in the Blynk app / web dashboard
BLYNK_WRITE(V5) {
  setPump(param.asInt() == 1, false);
}

// On every (re)connect, make the Blynk switch match the real pump state
BLYNK_CONNECTED() {
  Blynk.virtualWrite(V5, pumpOn ? 1 : 0);
  for (int i = 0; i < 7; i++) lastSent[i] = -999;     // resend all values
}

// ===== Sensors + formulas =====
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) { dhtOk = false; }
  else { dhtOk = true; temp = t; hum = h; }

  long sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogRead(A0); delay(2); }
  soilRaw = sum / 8;
  soilPct = constrain(map(soilRaw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100), 0, 100);

  rainState  = digitalRead(RAIN_PIN);
  raining    = (rainState == RAIN_WET_LEVEL);
  lightState = digitalRead(LIGHT_PIN);
  dark       = (lightState == DARK_LEVEL);
  lightPct   = dark ? 0 : 100;

  // Water Requirement Index (formula from the PPT)
  soilDryness = 100 - soilPct;
  tempFactor  = constrain((temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN) * 100.0, 0, 100);
  lightFactor = lightPct;
  rainFactor  = raining ? 0 : 100;
  wri = round(0.60 * soilDryness + 0.20 * tempFactor + 0.10 * lightFactor + 0.10 * rainFactor);
}

void printReadings() {
  Serial.println("---------------------------------");
  if (dhtOk) {
    Serial.print("Temperature : "); Serial.print(temp, 1); Serial.println(" C");
    Serial.print("Humidity    : "); Serial.print(hum, 0);  Serial.println(" %");
  } else {
    Serial.println("Temperature : READ FAILED (check DHT11 wiring)");
    Serial.println("Humidity    : READ FAILED (check DHT11 wiring)");
  }
  Serial.print("Soil raw    : "); Serial.println(soilRaw);
  Serial.print("Soil moist. : "); Serial.print(soilPct); Serial.println(" %");
  Serial.print("Rain        : "); Serial.print(raining ? "Yes" : "No");
  Serial.print("  (pin = "); Serial.print(rainState); Serial.println(")");
  Serial.print("Light       : "); Serial.print(dark ? "DARK" : "BRIGHT");
  Serial.print("  (pin = "); Serial.print(lightState); Serial.println(")");
  Serial.println("--- Water Requirement Index ---");
  Serial.print("Soil dryness  : "); Serial.print(soilDryness, 0); Serial.println(" %   (x 0.60)");
  Serial.print("Temp factor   : "); Serial.print(tempFactor, 0);  Serial.println(" %   (x 0.20)");
  Serial.print("Light factor  : "); Serial.print(lightFactor, 0); Serial.println(" %   (x 0.10)");
  Serial.print("Rain factor   : "); Serial.print(rainFactor, 0);  Serial.println(" %   (x 0.10)");
  Serial.print("WRI           : "); Serial.print(wri); Serial.println(" %");
  Serial.print("Pump        : "); Serial.println(pumpOn ? "ON" : "OFF");
  Serial.print("Wi-Fi       : ");
  if (WiFi.status() == WL_CONNECTED) { Serial.print("connected, IP "); Serial.println(WiFi.localIP()); }
  else Serial.println("not connected");
  Serial.print("Blynk       : "); Serial.println(Blynk.connected() ? "online" : "offline");
}

void sendToBlynk() {
  if (!Blynk.connected()) return;
  float v[7] = {temp, (float)soilPct, (float)(raining ? 1 : 0), (float)lightPct,
                (float)wri, hum, (float)soilRaw};
  for (int i = 0; i < 7; i++) {
    if (fabs(v[i] - lastSent[i]) >= DELTA[i] || millis() - lastSentMs[i] >= HEARTBEAT_MS) {
      if (VPINS[i] == 2) Blynk.virtualWrite(2, v[i] > 0.5 ? "Yes" : "No");   // V2 Rain is a String datastream
      else               Blynk.virtualWrite(VPINS[i], v[i]);
      lastSent[i] = v[i];
      lastSentMs[i] = millis();
    }
  }
}

// ===== Local web page =====
// NOTE: the script below uses arrow functions on purpose. The Arduino pre-processor
// mistakes the JavaScript word "function" inside this string for C functions.
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Irrigation</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Poppins:wght@400;500;600&display=swap" rel="stylesheet">
<style>
:root{
 --bg:#F4F6F5;--card:#FFFFFF;--line:#E3E8E5;--tx:#14201A;--mut:#6B7A72;
 --ac:#1F7A52;--chip:#EAF1ED;--amber:#C98A1B;--red:#C0392B;--off:#CBD5CF;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Poppins',system-ui,-apple-system,'Segoe UI',sans-serif;background:var(--bg);color:var(--tx);-webkit-font-smoothing:antialiased}
.wrap{max-width:560px;margin:0 auto;padding:20px 16px 40px}
svg.i{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex:none}

header{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}
.brand{display:flex;align-items:center;gap:12px}
.logo{width:42px;height:42px;border-radius:12px;background:var(--ac);color:#fff;display:flex;align-items:center;justify-content:center}
.logo svg{width:22px;height:22px}
h1{font-size:17px;font-weight:600;line-height:1.2}
.sub{font-size:12px;color:var(--mut);margin-top:2px}
.chip{display:flex;align-items:center;gap:7px;background:var(--card);border:1px solid var(--line);border-radius:20px;padding:6px 12px;font-size:12px;font-weight:500;color:var(--mut)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--red)}
.dot.ok{background:var(--ac)}

.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:18px}
.stack{display:flex;flex-direction:column;gap:12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.lbl{font-size:12px;color:var(--mut);font-weight:500}
.ico{width:38px;height:38px;border-radius:11px;background:var(--chip);color:var(--ac);display:flex;align-items:center;justify-content:center;margin-bottom:14px}
.val{font-size:28px;font-weight:600;line-height:1.1;margin-top:4px;font-variant-numeric:tabular-nums}
.val small{font-size:13px;font-weight:500;color:var(--mut);margin-left:3px}
.txt{font-size:22px}
.hide{display:none}

.hero{display:flex;align-items:center;gap:18px}
.ring{position:relative;width:124px;height:124px;flex:none}
.ring svg{width:124px;height:124px;transform:rotate(-90deg)}
.ring circle{fill:none;stroke-width:10;stroke-linecap:round}
.ring .bg{stroke:var(--chip)}
.ring .fg{stroke:var(--ac);stroke-dasharray:326.7;stroke-dashoffset:326.7;transition:stroke-dashoffset .6s,stroke .3s}
.ringtx{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:26px;font-weight:600}
.ringtx small{font-size:13px;color:var(--mut);margin-left:2px}
.state{font-size:19px;font-weight:600;margin:4px 0 4px}
.desc{font-size:12.5px;color:var(--mut);line-height:1.5}

.bar{height:6px;background:var(--chip);border-radius:4px;margin-top:12px;overflow:hidden}
.fill{height:100%;width:0;background:var(--ac);border-radius:4px;transition:width .5s}

.ph{display:flex;align-items:center;justify-content:space-between}
.pl{display:flex;align-items:center;gap:12px}
.pl .ico{margin:0}
.pt{font-size:15px;font-weight:600}
.ps{font-size:12px;color:var(--mut);margin-top:1px}
.sw{width:64px;height:36px;border-radius:18px;background:var(--off);border:0;position:relative;cursor:pointer;transition:background .2s;flex:none}
.sw::after{content:"";position:absolute;top:4px;left:4px;width:28px;height:28px;border-radius:50%;background:#fff;transition:transform .2s}
.sw.on{background:var(--ac)}
.sw.on::after{transform:translateX(28px)}
.sw:disabled{opacity:.5}
.safe{display:flex;align-items:center;gap:8px;margin-top:16px;padding-top:14px;border-top:1px solid var(--line);font-size:12px;color:var(--mut)}
.safe svg{width:16px;height:16px}

details{margin-top:2px}
summary{list-style:none;cursor:pointer;display:flex;align-items:center;justify-content:space-between;font-size:14px;font-weight:600}
summary::-webkit-details-marker{display:none}
summary svg{transition:transform .2s;color:var(--mut)}
details[open] summary svg{transform:rotate(180deg)}
.rows{margin-top:12px}
.row{display:flex;justify-content:space-between;padding:10px 0;border-top:1px solid var(--line);font-size:13px}
.row span{color:var(--mut)}
.row b{font-weight:500;font-variant-numeric:tabular-nums}
.foot{text-align:center;font-size:11.5px;color:var(--mut);margin-top:18px;line-height:1.5}
</style></head><body><div class="wrap">

<header>
 <div class="brand">
  <div class="logo"><svg class="i" viewBox="0 0 24 24"><path d="M11 20A7 7 0 0 1 9.8 6.1C15.5 5 17 4.48 19 2c1 2 2 4.18 2 8 0 5.5-4.78 10-10 10z"/><path d="M2 21c0-3 1.85-5.36 5.08-6C9.5 14.52 12 13 13 12"/></svg></div>
  <div><h1>Smart Irrigation</h1><div class="sub">Digital Twin for Precision Agriculture</div></div>
 </div>
 <div class="chip"><span class="dot" id="dot"></span><span id="conn">Connecting</span></div>
</header>

<div class="stack">

 <div class="card hero">
  <div class="ring">
   <svg viewBox="0 0 124 124"><circle class="bg" cx="62" cy="62" r="52"/><circle class="fg" id="ring" cx="62" cy="62" r="52"/></svg>
   <div class="ringtx"><span id="wri">--</span><small>%</small></div>
  </div>
  <div>
   <div class="lbl">Water Requirement Index</div>
   <div class="state" id="wl">--</div>
   <div class="desc">Estimated need based on soil dryness, temperature, light and rain.</div>
  </div>
 </div>

 <div class="grid">
  <div class="card">
   <div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z"/></svg></div>
   <div class="lbl">Temperature</div>
   <div class="val"><span id="t">--</span><small>&deg;C</small></div>
  </div>
  <div class="card">
   <div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z"/></svg></div>
   <div class="lbl">Humidity</div>
   <div class="val"><span id="h">--</span><small>%</small></div>
  </div>
  <div class="card">
   <div class="ico"><svg class="i" viewBox="0 0 24 24"><polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/></svg></div>
   <div class="lbl">Soil moisture</div>
   <div class="val"><span id="sp">--</span><small>%</small></div>
   <div class="bar"><div class="fill" id="spb"></div></div>
  </div>
  <div class="card">
   <div class="ico">
    <svg class="i" id="isun" viewBox="0 0 24 24"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>
    <svg class="i hide" id="imoon" viewBox="0 0 24 24"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>
   </div>
   <div class="lbl">Light</div>
   <div class="val txt" id="lt">--</div>
  </div>
  <div class="card" style="grid-column:1/-1">
   <div style="display:flex;align-items:center;gap:14px">
    <div class="ico" style="margin:0">
     <svg class="i" id="irain" viewBox="0 0 24 24"><line x1="16" y1="13" x2="16" y2="21"/><line x1="8" y1="13" x2="8" y2="21"/><line x1="12" y1="15" x2="12" y2="23"/><path d="M20 16.58A5 5 0 0 0 18 7h-1.26A8 8 0 1 0 4 15.25"/></svg>
     <svg class="i hide" id="inorain" viewBox="0 0 24 24"><path d="M18 10h-1.26A8 8 0 1 0 9 20h9a5 5 0 0 0 0-10z"/></svg>
    </div>
    <div><div class="lbl">Rain</div><div class="val txt" id="rn" style="margin-top:2px">--</div></div>
   </div>
  </div>
 </div>

 <div class="card">
  <div class="ph">
   <div class="pl">
    <div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M18.36 6.64a9 9 0 1 1-12.73 0"/><line x1="12" y1="2" x2="12" y2="12"/></svg></div>
    <div><div class="pt">Irrigation pump</div><div class="ps" id="ps">Off</div></div>
   </div>
   <button class="sw" id="sw" role="switch" aria-label="Pump" onclick="tog()"></button>
  </div>
  <div class="safe">
   <svg class="i" viewBox="0 0 24 24"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
   Safety cut-off stops the pump automatically
  </div>
 </div>

 <div class="card">
  <details>
   <summary>Sensor details <svg class="i" viewBox="0 0 24 24"><polyline points="6 9 12 15 18 9"/></svg></summary>
   <div class="rows">
    <div class="row"><span>Soil raw</span><b id="r1">--</b></div>
    <div class="row"><span>Rain pin</span><b id="r2">--</b></div>
    <div class="row"><span>Light pin</span><b id="r3">--</b></div>
    <div class="row"><span>Soil dryness (x 0.60)</span><b id="r4">--</b></div>
    <div class="row"><span>Temperature factor (x 0.20)</span><b id="r5">--</b></div>
    <div class="row"><span>Light factor (x 0.10)</span><b id="r6">--</b></div>
    <div class="row"><span>Rain factor (x 0.10)</span><b id="r7">--</b></div>
    <div class="row"><span>Blynk cloud</span><b id="r8">--</b></div>
   </div>
  </details>
 </div>

</div>
<div class="foot">Prototype estimate. The index is not an exact water volume and needs calibration.</div>

<script>
var pumpOn=false;
const g=i=>document.getElementById(i);
const set=(i,v)=>{g(i).textContent=v};
const show=(i,on)=>{g(i).classList.toggle('hide',!on)};
const tog=()=>{
 g('sw').disabled=true;
 fetch('/pump?state='+(pumpOn?0:1)).then(()=>load()).catch(()=>{}).then(()=>{g('sw').disabled=false});
};
const load=()=>fetch('/data').then(r=>r.json()).then(j=>{
 g('dot').className='dot ok'; set('conn','Online');
 set('t',j.t); set('h',j.h); set('sp',j.sp); set('wri',j.wri);
 g('spb').style.width=j.sp+'%';
 set('lt',j.dark?'Dark':'Bright'); show('isun',!j.dark); show('imoon',!!j.dark);
 set('rn',j.rain?'Yes':'No'); show('irain',!!j.rain); show('inorain',!j.rain);
 var c=j.wri<34?'#1F7A52':(j.wri<67?'#C98A1B':'#C0392B');
 g('ring').style.stroke=c;
 g('ring').style.strokeDashoffset=326.7*(1-j.wri/100);
 set('wl',j.wri<34?'Low water need':(j.wri<67?'Moderate water need':'High water need'));
 pumpOn=!!j.pump; g('sw').className='sw'+(pumpOn?' on':''); set('ps',pumpOn?'Running':'Off');
 set('r1',j.sr); set('r2',j.rs); set('r3',j.ls);
 set('r4',j.sd+' %'); set('r5',j.tf+' %'); set('r6',j.lf+' %'); set('r7',j.rf+' %');
 set('r8',j.blynk?'Online':'Offline');
}).catch(()=>{ g('dot').className='dot'; set('conn','Offline'); });
load(); setInterval(load,2000);
</script></div></body></html>
)rawliteral";

void handleData() {
  char buf[420];
  snprintf(buf, sizeof(buf),
    "{\"t\":%.1f,\"h\":%.0f,\"sr\":%d,\"sp\":%d,\"rain\":%d,\"rs\":%d,\"dark\":%d,\"ls\":%d,"
    "\"sd\":%.0f,\"tf\":%.0f,\"lf\":%.0f,\"rf\":%.0f,\"wri\":%d,\"pump\":%d,\"blynk\":%d}",
    temp, hum, soilRaw, soilPct, raining ? 1 : 0, rainState, dark ? 1 : 0, lightState,
    soilDryness, tempFactor, lightFactor, rainFactor, wri, pumpOn ? 1 : 0, Blynk.connected() ? 1 : 0);
  server.send(200, "application/json", buf);
}

void handlePump() {
  if (server.hasArg("state")) setPump(server.arg("state") == "1");
  server.send(200, "text/plain", "OK");
}

// ===== Setup / loop =====
void setup() {
  Serial.begin(115200);
  delay(500);
  setPump(false, false);                 // relay OFF before anything else
  dht.begin();
  pinMode(RAIN_PIN, INPUT);
  pinMode(LIGHT_PIN, INPUT);
  for (int i = 0; i < 7; i++) { lastSent[i] = -999; lastSentMs[i] = 0; }

  Serial.println("\nSmart Irrigation starting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Local control page: http://"); Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi not connected yet - will keep retrying");
  }

  Blynk.config(BLYNK_AUTH_TOKEN);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/data", HTTP_GET, handleData);
  server.on("/pump", HTTP_GET, handlePump);
  server.begin();
}

void loop() {
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mdnsStarted && MDNS.begin("irrigation")) {      // http://irrigation.local
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
    MDNS.update();
    if (Blynk.connected()) Blynk.run();
    else if (millis() - lastBlynkTry > 15000) { lastBlynkTry = millis(); Blynk.connect(3000); }
  }

  // Pump safety: stop if Wi-Fi is gone for WIFI_GRACE_MS, or max run time reached
  if (pumpOn) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiLostSince == 0) wifiLostSince = millis();
      if (millis() - wifiLostSince > WIFI_GRACE_MS) {
        setPump(false);
        Serial.println("Pump stopped: Wi-Fi lost");
      }
    } else {
      wifiLostSince = 0;
    }
    if (millis() - pumpStartMs > PUMP_MAX_RUN_MS) {
      setPump(false);
      Serial.println("Pump stopped: max run time reached");
    }
  } else {
    wifiLostSince = 0;
  }

  if (millis() - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = millis();
    readSensors();
    printReadings();
    sendToBlynk();
  }
}
