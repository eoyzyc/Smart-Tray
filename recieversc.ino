#include <WiFi.h>            // WiFi control for ESP32 
#include <WebServer.h>       // HTTP web server
#include <SPIFFS.h>          // Flash file system
#include <esp_now.h>         // ESP-NOW communication
#include <esp_wifi.h>        // Low-level WiFi functions
#include <SPI.h>             // SPI bus
#include <MFRC522.h>         // RC522 RFID reader
#include <vector>            // C++ vector container

#define AP_SSID    "ESP32-B-Receiver"   // Access Point SSID for ESP32-B
#define AP_PWD     ""                  // No password
#define AP_CHANNEL 1                   // Fixed WiFi channel for ESP-NOW

// RC522 pins (same as A)
#define RST_PIN 22                     // RC522 reset pin
#define SS_PIN  5                      // RC522 slave select pin
MFRC522 mfrc522(SS_PIN, RST_PIN);      // Create RC522 object

// Buzzer pin
#define BUZZER_PIN 15                  // GPIO for buzzer feedback (did not use)

WebServer server(80);                  // HTTP server on port 80

// ---------- Convert UID bytes to string ----------
String uidToString(byte *buffer, byte len){
  String s="";
  for(byte i=0;i<len;i++){
    if(buffer[i] < 0x10) s += "0";      // Pad hex values
    s += String(buffer[i], HEX);        // Convert byte to hex
    if(i+1 < len) s += ":";             // Colon-separated format
  }
  s.toUpperCase();                      // Standard UID format
  return s;
}

// ---------- Data structures ----------
struct Entry {
  String uid;                           // NFC UID
  String nric;                          // Patient NRIC
  String bedWard;                       // Bed/Ward info
  String starch;                        // Starch item
  String veg;                           // Vegetable item
  String meat;                          // Meat item
  String fruit;                         // Fruit item
  String drink;                         // Drink item
  String remarks;                       // Remarks
  unsigned long ts;                     // Timestamp received
};

std::vector<Entry>  entries;            // Parsed ESP-NOW entries
std::vector<String> rawPackets;         // Raw packets from ESP32-A

unsigned long lastPingSeenAt = 0;        // Last ping timestamp
String lastSenderMac = "";               // MAC address of last sender

// latest scanned tag (for patient matching)
String lastScanUIDB   = "";              // Last UID scanned on ESP32-B
String lastNRIC       = "";
String lastBedWard    = "";
String lastStarch     = "";
String lastVeg        = "";
String lastMeat       = "";
String lastFruit      = "";
String lastDrink      = "";
String lastRemarks    = "";

// last UID received from ESP-NOW (from A)
String lastEspNowUID  = "";              // UID received via ESP-NOW

// Debounce for RC522 on B
unsigned long lastScanTimeB = 0;         // Last scan time
const unsigned long B_SCAN_COOLDOWN_MS = 2000; // 2s scan cooldown

// Serving mode state
bool servingMode      = false;           // Serving UI active flag
int  servingItemSlot  = 0;               // Selected food slot (1–5)

String macToStr(const uint8_t *m){
  char b[18];
  sprintf(b,"%02X:%02X:%02X:%02X:%02X:%02X",
          m[0],m[1],m[2],m[3],m[4],m[5]); // Convert MAC bytes to string
  return String(b);
}

bool peerExists(const uint8_t *mac){
  return esp_now_is_peer_exist(mac);     // Check if ESP-NOW peer exists
}

void ensurePeer(const uint8_t *mac){
  if(peerExists(mac)) return;            // Skip if peer already exists
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);           // Copy MAC address
  p.channel = AP_CHANNEL;                // Set chan_
// ---------- Normal status JSON ----------
void handleStatus(){
  String json="{";                          // Begin JSON response

  json += "\"uid\":\""+lastScanUIDB+"\",";  // Last UID scanned on ESP32-B
  json += "\"nric\":\""+lastNRIC+"\",";     // Patient NRIC
  json += "\"bedWard\":\""+lastBedWard+"\","; // Bed/Ward
  json += "\"starch\":\""+lastStarch+"\","; // Starch item
  json += "\"veg\":\""+lastVeg+"\",";       // Vegetable item
  json += "\"meat\":\""+lastMeat+"\",";     // Meat item
  json += "\"fruit\":\""+lastFruit+"\",";   // Fruit item
  json += "\"drink\":\""+lastDrink+"\",";   // Drink item
  json += "\"remarks\":\""+lastRemarks+"\","; // Remarks
  json += "\"espNowUID\":\""+lastEspNowUID+"\","; // UID last received via ESP-NOW
  json += "\"pingMAC\":\""+lastSenderMac+"\",";   // MAC of last ping sender
  json += "\"pingTime\":" + String(lastPingSeenAt) + ","; // Ping timestamp

  json += "\"raw\":[";                      // Begin raw packet list
  for(size_t i=0;i<rawPackets.size();i++){
    String p = rawPackets[i];
    p.replace("\"","\\\"");                 // Escape quotes for JSON
    json += "\""+p+"\"";
    if(i+1<rawPackets.size()) json+=",";    // Comma separation
  }
  json += "]}";

  server.send(200,"application/json",json); // Send JSON to browser
}

// ---------- Serving Mode HTML ----------
void handleServe(){
  servingMode = true;                       // Enter serving mode
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
"let currentSlot = 0;"                      // Currently selected food slot
"async function selectSlot(slot){"
"  currentSlot = slot;"
"  await fetch('/serveSelect?slot=' + slot);" // Notify ESP32 of selected slot
"  refreshDisplay();"
"}"
"async function refreshDisplay(){"
"  if(currentSlot==0) return;"              // No slot selected
"  const r = await fetch('/serveStatus');"  // Fetch serving data
"  const j = await r.json();"
"  const d = document.getElementById('display');"
"  d.innerHTML = '';"                       // Clear display
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
"  itemLabel.textContent = j.fieldName;"    // Selected food type
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
"    rem.textContent = 'Remarks: ' + j.remarks;" // Show remarks if any
"    d.appendChild(rem);"
"  }"
"}"
"setInterval(refreshDisplay, 2000);"        // Auto-refresh serving display
"async function exitServe(){"
"  await fetch('/serveExit');"              // Exit serving mode
"  window.location = '/';"
"}"
"</script></body></html>";
  server.send(200,"text/html",page);        // Serve serving-mode UI
}

// ---------- Serving mode selection ----------
void handleServeSelect(){
  if(!server.hasArg(\"slot\")){             // Ensure slot parameter exists
    server.send(400,\"text/plain\",\"missing slot\");
    return;
  }
  int slot = server.arg(\"slot\").toInt();  // Parse slot value
  if(slot<1 || slot>5){
    server.send(400,\"text/plain\",\"bad slot\"); // Validate slot range
    return;
  }
  servingItemSlot = slot;                   // Store selected slot
  server.send(200,\"text/plain\",\"OK\");
}

// ---------- Serving mode status JSON ----------
void handleServeStatus(){
  String nric    = lastNRIC;                // Current patient NRIC
  String bedWard = lastBedWard;             // Current bed/ward
  String fieldName;
  String itemValue;
  String remarks = lastRemarks;

  switch(servingItemSlot){
    case 1: fieldName=\"Starch\"; itemValue=lastStarch; break;
    case 2: fieldName=\"Veg\";    itemValue=lastVeg;    break;
    case 3: fieldName=\"Meat\";   itemValue=lastMeat;   break;
    case 4: fieldName=\"Fruit\";  itemValue=lastFruit;  break;
    case 5: fieldName=\"Drink\";  itemValue=lastDrink;  break;
    default: fieldName=\"\";      itemValue=\"\";       break;
  }

  String json=\"{\";                        // Begin JSON
  json += \"\\\"nric\\\":\\\"\"+nric+\"\\\",\";
  json += \"\\\"bedWard\\\":\\\"\"+bedWard+\"\\\",\";
  json += \"\\\"fieldName\\\":\\\"\"+fieldName+\"\\\",\";
  json += \"\\\"itemValue\\\":\\\"\"+itemValue+\"\\\",\";
  json += \"\\\"remarks\\\":\\\"\"+remarks+\"\\\"\";
  json += \"}\";

  server.send(200,\"application/json\",json); // Send serving data
}

void handleServeExit(){
  servingMode = false;                     // Disable serving mode
  servingItemSlot = 0;                     // Reset slot
  server.send(200,\"text/plain\",\"OK\");
}
// ---------- ESP-NOW receive callback ----------
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len){
  if(!info || !data || len<=0) return;        // Validate incoming packet

  String s;
  s.reserve(len);                             // Pre-allocate buffer
  for(int i=0;i<len;i++) s += (char)data[i];  // Convert bytes to string

  // PING?
  if(s.startsWith("PING|")){
    lastSenderMac  = macToStr(info->src_addr); // Record sender MAC
    lastPingSeenAt = millis();                // Record ping timestamp
    sendPong(info->src_addr);                 // Respond with PONG
    return;
  }

  // Save raw packet from A exactly as received
  rawPackets.push_back(s);                    // Store raw ESP-NOW packet

  // Format from A: UID|NRIC,Bed.Ward,Starch,Veg,Meat,Fruit,Drink,Remarks
  int sep = s.indexOf('|');
  if(sep <= 0) return;                        // Invalid packet format

  String uid  = s.substring(0,sep);           // Extract UID
  String rest = s.substring(sep+1);           // Extract payload
  lastEspNowUID = uid;                        // Save last UID from A

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

  if(parts.size() < 2) return;                // Require at least NRIC + bed

  Entry e;
  e.uid      = uid;                           // Assign UID
  e.nric     = parts.size()>0? parts[0] : "";
  e.bedWard  = parts.size()>1? parts[1] : "";
  e.starch   = parts.size()>2? parts[2] : "";
  e.veg      = parts.size()>3? parts[3] : "";
  e.meat     = parts.size()>4? parts[4] : "";
  e.fruit    = parts.size()>5? parts[5] : "";
  e.drink    = parts.size()>6? parts[6] : "";
  e.remarks  = parts.size()>7? parts[7] : "";
  e.ts       = millis();                      // Timestamp reception

  entries.push_back(e);                       // Store parsed entry

  Serial.printf("RX from A: %s\n", s.c_str()); // Debug output
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);                       // Start serial debugging
  delay(200);
  SPIFFS.begin(true);                         // Mount SPIFFS

  pinMode(BUZZER_PIN, OUTPUT);                // Configure buzzer
  digitalWrite(BUZZER_PIN, LOW);              // Ensure buzzer off

  // RC522
  SPI.begin();                                // Start SPI bus
  mfrc522.PCD_Init();                         // Initialise RC522 reader

  WiFi.mode(WIFI_AP_STA);                     // Enable AP + STA
  WiFi.softAP(AP_SSID, AP_PWD, AP_CHANNEL);   // Start AP
  esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE); // Lock channel
  WiFi.setSleep(false);                       // Disable WiFi sleep

  Serial.printf("AP %s @ %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("ESP-B STA MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("ESP-B AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());

  if(esp_now_init()!=ESP_OK) Serial.println("esp_now init failed");
  esp_now_register_recv_cb(onReceive);        // Register ESP-NOW RX callback

  server.on("/",           HTTP_GET, handleRoot);
  server.on("/status",     HTTP_GET, handleStatus);
  server.on("/serve",      HTTP_GET, handleServe);
  server.on("/serveSelect",HTTP_GET, handleServeSelect);
  server.on("/serveStatus",HTTP_GET, handleServeStatus);
  server.on("/serveExit",  HTTP_GET, handleServeExit);
  server.begin();                             // Start HTTP server
  Serial.println("HTTP server started");
}

// ---------- Loop ----------
void loop(){
  server.handleClient();                      // Handle web requests

  // Always ready to scan on B, with 2s cooldown
  if(millis() - lastScanTimeB >= B_SCAN_COOLDOWN_MS){
    if(mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()){
      lastScanTimeB = millis();               // Update debounce timer

      String uid = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
      lastScanUIDB = uid;                     // Save scanned UID

      beepBuzzer();                           // Audible scan confirmation

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

      mfrc522.PICC_HaltA();                   // Stop card communication
    }
  }
}
