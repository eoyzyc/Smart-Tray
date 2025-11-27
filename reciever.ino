// ========== ESP32-B: Receiver + RC522 + raw list + Serving Mode + buzzer ==========

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <vector>

#define AP_SSID    "ESP32-B-Receiver"
#define AP_PWD     ""
#define AP_CHANNEL 1

// RC522 pins (same as A)
#define RST_PIN 22
#define SS_PIN  5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Buzzer pin
#define BUZZER_PIN 15

WebServer server(80);

// ---------- Convert UID bytes to string ----------
String uidToString(byte *buffer, byte len){
  String s="";
  for(byte i=0;i<len;i++){
    if(buffer[i] < 0x10) s += "0";
    s += String(buffer[i], HEX);
    if(i+1 < len) s += ":";
  }
  s.toUpperCase();
  return s;
}

// ---------- Data structures ----------
struct Entry {
  String uid;
  String nric;
  String bedWard;
  String starch;
  String veg;
  String meat;
  String fruit;
  String drink;
  String remarks;
  unsigned long ts;
};

std::vector<Entry>  entries;         // parsed entries
std::vector<String> rawPackets;      // raw strings from A

unsigned long lastPingSeenAt = 0;
String lastSenderMac = "";

// latest scanned tag (for patient matching)
String lastScanUIDB   = "";
String lastNRIC       = "";
String lastBedWard    = "";
String lastStarch     = "";
String lastVeg        = "";
String lastMeat       = "";
String lastFruit      = "";
String lastDrink      = "";
String lastRemarks    = "";

// last UID received from ESP-NOW (from A)
String lastEspNowUID  = "";

// Debounce for RC522 on B
unsigned long lastScanTimeB = 0;
const unsigned long B_SCAN_COOLDOWN_MS = 2000;

// Serving mode state
bool servingMode      = false;
int  servingItemSlot  = 0;  // 1..5 (1=Starch,2=Veg,3=Meat,4=Fruit,5=Drink)

String macToStr(const uint8_t *m){
  char b[18];
  sprintf(b,"%02X:%02X:%02X:%02X:%02X:%02X",
          m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(b);
}

bool peerExists(const uint8_t *mac){
  return esp_now_is_peer_exist(mac);
}

void ensurePeer(const uint8_t *mac){
  if(peerExists(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = AP_CHANNEL;
  p.ifidx   = WIFI_IF_STA;
  p.encrypt = 0;
  esp_now_add_peer(&p);
}

void sendPong(const uint8_t *dest){
  ensurePeer(dest);
  String payload = "PONG|" + String(millis());
  esp_now_send(dest, (const uint8_t*)payload.c_str(), payload.length());
}

void beepBuzzer(){
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}

// ---------- Normal Web page (status + raw list) ----------
void handleRoot(){
  String page =
"<!doctype html><html><head><meta charset='utf-8'><title>ESP32-B Receiver</title></head><body>"
"<h2>ESP32-B Receiver</h2>"
"<p>AP SSID: <b>" AP_SSID "</b></p>"
"<p>STA MAC: " + WiFi.macAddress() + "</p>"
"<p>AP  MAC: " + WiFi.softAPmacAddress() + "</p>"

"<button onclick=\"location.href='/serve'\">Serving Mode</button>"

"<h3>Last Scanned Tag on B (RC522)</h3>"
"<p>UID: <span id='uid'></span></p>"
"<p>NRIC: <span id='nric'></span></p>"
"<p>Bed/Ward: <span id='bed'></span></p>"

"<h4>Food</h4>"
"<ul>"
"<li>Starch: <span id='starch'></span></li>"
"<li>Veg:    <span id='veg'></span></li>"
"<li>Meat:   <span id='meat'></span></li>"
"<li>Fruit:  <span id='fruit'></span></li>"
"<li>Drink:  <span id='drink'></span></li>"
"</ul>"
"<p>Remarks: <span id='remarks'></span></p>"

"<h3>Last UID received from ESP-NOW (from A)</h3>"
"<p><span id='espuid'></span></p>"

"<h3>Connectivity</h3>"
"<p>Last PING from A: <span id='pingmac'></span></p>"
"<p>Last PING time: <span id='pingtime'></span> ms</p>"

"<h3>All Incoming Packets from A (raw)</h3>"
"<ol id='rawlist'></ol>"

"<script>"
"async function refresh(){"
"  const r = await fetch('/status');"
"  const j = await r.json();"
"  document.getElementById('uid').textContent     = j.uid;"
"  document.getElementById('nric').textContent    = j.nric;"
"  document.getElementById('bed').textContent     = j.bedWard;"
"  document.getElementById('starch').textContent  = j.starch;"
"  document.getElementById('veg').textContent     = j.veg;"
"  document.getElementById('meat').textContent    = j.meat;"
"  document.getElementById('fruit').textContent   = j.fruit;"
"  document.getElementById('drink').textContent   = j.drink;"
"  document.getElementById('remarks').textContent = j.remarks;"
"  document.getElementById('espuid').textContent  = j.espNowUID;"
"  document.getElementById('pingmac').textContent = j.pingMAC;"
"  document.getElementById('pingtime').textContent= j.pingTime;"
"  const rl = document.getElementById('rawlist');"
"  rl.innerHTML = '';"
"  for(const p of j.raw){"
"    const li = document.createElement('li');"
"    li.textContent = p;"
"    rl.appendChild(li);"
"  }"
"}"
"setInterval(refresh,2000);"
"refresh();"
"</script></body></html>";

  server.send(200,"text/html",page);
}

// ---------- Normal status JSON ----------
void handleStatus(){
  String json="{";

  json += "\"uid\":\""+lastScanUIDB+"\",";
  json += "\"nric\":\""+lastNRIC+"\",";
  json += "\"bedWard\":\""+lastBedWard+"\",";
  json += "\"starch\":\""+lastStarch+"\",";
  json += "\"veg\":\""+lastVeg+"\",";
  json += "\"meat\":\""+lastMeat+"\",";
  json += "\"fruit\":\""+lastFruit+"\",";
  json += "\"drink\":\""+lastDrink+"\",";
  json += "\"remarks\":\""+lastRemarks+"\",";
  json += "\"espNowUID\":\""+lastEspNowUID+"\",";
  json += "\"pingMAC\":\""+lastSenderMac+"\",";
  json += "\"pingTime\":" + String(lastPingSeenAt) + ",";

  json += "\"raw\":[";
  for(size_t i=0;i<rawPackets.size();i++){
    String p = rawPackets[i];
    p.replace("\"","\\\"");
    json += "\""+p+"\"";
    if(i+1<rawPackets.size()) json+=",";
  }
  json += "]}";

  server.send(200,"application/json",json);
}

// ---------- Serving Mode HTML ----------
void handleServe(){
  servingMode = true;
  String page =
"<!doctype html><html><head><meta charset='utf-8'><title>Serving Mode</title></head>"
"<body style='margin:0;padding:0;background:#000;color:#fff;font-family:sans-serif;'>"
"<div style='text-align:center;padding:20px;'>"
"<h1 style='font-size:70px;margin-bottom:20px;'>Serving Mode</h1>"
"<div style='margin-bottom:40px;'>"
"<button style='font-size:40px;padding:20px 40px;margin:10px;' onclick='selectSlot(1)'>Starch</button>"
"<button style='font-size:40px;padding:20px 40px;margin:10px;' onclick='selectSlot(2)'>Veg</button>"
"<button style='font-size:40px;padding:20px 40px;margin:10px;' onclick='selectSlot(3)'>Meat</button><br>"
"<button style='font-size:40px;padding:20px 40px;margin:10px;' onclick='selectSlot(4)'>Fruit</button>"
"<button style='font-size:40px;padding:20px 40px;margin:10px;' onclick='selectSlot(5)'>Drink</button>"
"</div>"
"<button style='font-size:40px;padding:20px 40px;margin:10px;background:#900;color:#fff;' onclick='exitServe()'>Exit Serving Mode</button>"
"<div id='display' style='margin-top:40px;'></div>"
"</div>"
"<script>"
"let currentSlot = 0;"
"async function selectSlot(slot){"
"  currentSlot = slot;"
"  await fetch('/serveSelect?slot=' + slot);"
"  refreshDisplay();"
"}"
"async function refreshDisplay(){"
"  if(currentSlot==0) return;"
"  const r = await fetch('/serveStatus');"
"  const j = await r.json();"
"  const d = document.getElementById('display');"
"  d.innerHTML = '';"
"  const nric = document.createElement('div');"
"  nric.style.fontSize = '60px';"
"  nric.style.marginBottom = '20px';"
"  nric.textContent = 'NRIC: ' + j.nric;"
"  const bed = document.createElement('div');"
"  bed.style.fontSize = '50px';"
"  bed.style.marginBottom = '40px';"
"  bed.textContent = 'Bed/Ward: ' + j.bedWard;"
"  const itemLabel = document.createElement('div');"
"  itemLabel.style.fontSize = '50px';"
"  itemLabel.textContent = j.fieldName;"
"  const item = document.createElement('div');"
"  item.style.fontSize = '80px';"
"  item.style.fontWeight = 'bold';"
"  item.style.marginTop = '10px';"
"  item.textContent = j.itemValue && j.itemValue.length>0 ? j.itemValue : 'No item for this slot';"
"  d.appendChild(nric);"
"  d.appendChild(bed);"
"  d.appendChild(itemLabel);"
"  d.appendChild(item);"
"  if(j.remarks && j.remarks.length>0){"
"    const rem = document.createElement('div');"
"    rem.style.fontSize = '40px';"
"    rem.style.marginTop = '30px';"
"    rem.textContent = 'Remarks: ' + j.remarks;"
"    d.appendChild(rem);"
"  }"
"}"
"setInterval(refreshDisplay, 2000);"
"async function exitServe(){"
"  await fetch('/serveExit');"
"  window.location = '/';"
"}"
"</script></body></html>";
  server.send(200,"text/html",page);
}

// ---------- Serving mode selection ----------
void handleServeSelect(){
  if(!server.hasArg("slot")){
    server.send(400,"text/plain","missing slot");
    return;
  }
  int slot = server.arg("slot").toInt();
  if(slot<1 || slot>5){
    server.send(400,"text/plain","bad slot");
    return;
  }
  servingItemSlot = slot;
  server.send(200,"text/plain","OK");
}

// ---------- Serving mode status JSON ----------
void handleServeStatus(){
  String nric    = lastNRIC;
  String bedWard = lastBedWard;
  String fieldName;
  String itemValue;
  String remarks = lastRemarks;

  switch(servingItemSlot){
    case 1: fieldName="Starch"; itemValue=lastStarch; break;
    case 2: fieldName="Veg";    itemValue=lastVeg;    break;
    case 3: fieldName="Meat";   itemValue=lastMeat;   break;
    case 4: fieldName="Fruit";  itemValue=lastFruit;  break;
    case 5: fieldName="Drink";  itemValue=lastDrink;  break;
    default: fieldName="";      itemValue="";         break;
  }

  String json="{";
  json += "\"nric\":\""+nric+"\",";
  json += "\"bedWard\":\""+bedWard+"\",";
  json += "\"fieldName\":\""+fieldName+"\",";
  json += "\"itemValue\":\""+itemValue+"\",";
  json += "\"remarks\":\""+remarks+"\"";
  json += "}";

  server.send(200,"application/json",json);
}

void handleServeExit(){
  servingMode = false;
  servingItemSlot = 0;
  server.send(200,"text/plain","OK");
}

// ---------- ESP-NOW receive callback ----------
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len){
  if(!info || !data || len<=0) return;

  String s;
  s.reserve(len);
  for(int i=0;i<len;i++) s += (char)data[i];

  // PING?
  if(s.startsWith("PING|")){
    lastSenderMac  = macToStr(info->src_addr);
    lastPingSeenAt = millis();
    sendPong(info->src_addr);
    return;
  }

  // Save raw packet from A exactly as received
  rawPackets.push_back(s);

  // Format from A: UID|NRIC,Bed.Ward,Starch,Veg,Meat,Fruit,Drink,Remarks
  int sep = s.indexOf('|');
  if(sep <= 0) return;

  String uid  = s.substring(0,sep);
  String rest = s.substring(sep+1);
  lastEspNowUID = uid;

  // Split rest by commas
  std::vector<String> parts;
  int start=0;
  while(true){
    int comma = rest.indexOf(',', start);
    if(comma<0){
      String piece = rest.substring(start);
      piece.trim();
      if(piece.length()>0 || start<(int)rest.length()) parts.push_back(piece);
      break;
    }else{
      String piece = rest.substring(start, comma);
      piece.trim();
      parts.push_back(piece);
      start = comma+1;
    }
  }

  if(parts.size() < 2) return;

  Entry e;
  e.uid      = uid;
  e.nric     = parts.size()>0? parts[0] : "";
  e.bedWard  = parts.size()>1? parts[1] : "";
  e.starch   = parts.size()>2? parts[2] : "";
  e.veg      = parts.size()>3? parts[3] : "";
  e.meat     = parts.size()>4? parts[4] : "";
  e.fruit    = parts.size()>5? parts[5] : "";
  e.drink    = parts.size()>6? parts[6] : "";
  e.remarks  = parts.size()>7? parts[7] : "";
  e.ts       = millis();

  entries.push_back(e);

  Serial.printf("RX from A: %s\n", s.c_str());
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);
  delay(200);
  SPIFFS.begin(true);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // RC522
  SPI.begin();
  mfrc522.PCD_Init();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PWD, AP_CHANNEL);
  esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
  WiFi.setSleep(false);

  Serial.printf("AP %s @ %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("ESP-B STA MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("ESP-B AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());

  if(esp_now_init()!=ESP_OK) Serial.println("esp_now init failed");
  esp_now_register_recv_cb(onReceive);

  server.on("/",           HTTP_GET, handleRoot);
  server.on("/status",     HTTP_GET, handleStatus);
  server.on("/serve",      HTTP_GET, handleServe);
  server.on("/serveSelect",HTTP_GET, handleServeSelect);
  server.on("/serveStatus",HTTP_GET, handleServeStatus);
  server.on("/serveExit",  HTTP_GET, handleServeExit);
  server.begin();
  Serial.println("HTTP server started");
}

// ---------- Loop ----------
void loop(){
  server.handleClient();

  // Always ready to scan on B, with 2s cooldown
  if(millis() - lastScanTimeB >= B_SCAN_COOLDOWN_MS){
    if(mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()){
      lastScanTimeB = millis();

      String uid = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
      lastScanUIDB = uid;

      beepBuzzer();

      // Find latest matching entry by UID
      lastNRIC    = "";
      lastBedWard = "";
      lastStarch  = "";
      lastVeg     = "";
      lastMeat    = "";
      lastFruit   = "";
      lastDrink   = "";
      lastRemarks = "";

      for(int i=(int)entries.size()-1; i>=0; i--){
        if(entries[i].uid == uid){
          lastNRIC    = entries[i].nric;
          lastBedWard = entries[i].bedWard;
          lastStarch  = entries[i].starch;
          lastVeg     = entries[i].veg;
          lastMeat    = entries[i].meat;
          lastFruit   = entries[i].fruit;
          lastDrink   = entries[i].drink;
          lastRemarks = entries[i].remarks;
          break;
        }
      }

      if(lastNRIC==""){
        lastNRIC    = "No matching patient record for this UID.";
        lastBedWard = "";
        lastStarch  = "";
        lastVeg     = "";
        lastMeat    = "";
        lastFruit   = "";
        lastDrink   = "";
        lastRemarks = "";
      }

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }
  }
}
