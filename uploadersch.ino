#include <WiFi.h>          // WiFi control for ESP32 (AP + STA modes)
#include <WebServer.h>     // HTTP web server for browser UI
#include <SPIFFS.h>        // SPI Flash File System for CSV storage
#include <esp_now.h>       // ESP-NOW peer-to-peer communication
#include <esp_wifi.h>      // Low-level WiFi configuration (channel control)
#include <SPI.h>           // SPI bus (used by RC522)
#include <MFRC522.h>       // RFID/NFC reader library
#include <vector>          // C++ vector container

#define AP_SSID    "Smart Tray Uploader "   // WiFi access point SSID
#define AP_PWD     ""                       // No password
#define AP_CHANNEL 1                        // Fixed WiFi channel for ESP-NOW

// RC522 pins
#define RST_PIN 22                          // RC522 reset pin
#define SS_PIN  5                           // RC522 slave select pin
MFRC522 mfrc522(SS_PIN, RST_PIN);           // Create RC522 object

// Buzzer pin
#define BUZZER_PIN 15                       // GPIO pin for buzzer sound

WebServer server(80);                       // HTTP server running on port 80

const char *CSV_PATH     = "/uploaded.csv"; // Uploade CSV file path
const char *MAPPING_PATH = "/mappings.txt"; // UID → patient mapping log

#define MAX_ITEMS 4000                      // Maximum CSV rows supported
#define MAX_PEERS 5                         // Maximum ESP-NOW peers

// ---- Patient row structure (parsed from CSV) ----
// CSV format:
// Patient ,NRIC ,Bed.Ward,Starch,Veg,Meat ,Fruit,Drink,Remarks
struct PatientRow {
  String nric;      // col 2: patient NRIC
  String bedWard;   // col 3: bed and ward (e.g. "2-13")
  String starch;    // col 4: starch portion
  String veg;       // col 5: vegetable portion
  String meat;      // col 6: meat portion
  String fruit;     // col 7: fruit portion
  String drink;     // col 8: drink
  String remarks;   // col 9: remarks (optional)
};

std::vector<PatientRow> patients;           // Parsed patient rows
std::vector<String>     rowUIDs;             // NFC UID mapped to each row
int curIndex = 0;                            // Current patient index 

enum State { IDLE, READY, WAITING_FOR_TAG, TAG_READ, SENDING, SENT, ERROR_STATE }; // Scanner states
volatile State stateVar = IDLE;             
bool   waitingForTag    = false;             // Flag indicating NFC scan active
String nfcStatus        = "Idle";             // Nicer wording for NFC status

// Hard-coded peers (STA MACs)
const char* HARD_CODED_MACS[MAX_PEERS] = {
  "38:18:2B:A7:7A:F4",
  "38:18:2B:A7:6D:6C",
  "38:18:2B:A7:9C:80",
  "38:18:2B:A7:9B:68",
  "38:18:2B:A7:86:4C"
};

struct Peer {
  uint8_t mac[6];                        // MAC address bytes
  bool valid = false;                    // Whether peer slot is valid
  unsigned long lastAttempt = 0;         // Last ESP-NOW send attempt
  unsigned long lastSuccess = 0;         // Last successful communication
  String macStr() const {                // Convert MAC bytes to readable string
    char b[18];
    sprintf(b,"%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return String(b);
  }
};
Peer peers[MAX_PEERS];                   // Peer table

volatile int   lastSendPeer = -1;         // Index of last peer sent to
unsigned long  lastPingAt   = 0;          // Timestamp of last ping


String uidToString(byte *buffer, byte len){  // Convert UID bytes to colon-separated hex
  String s="";
  for(byte i=0;i<len;i++){
    if(buffer[i] < 0x10) s += "0";        
    s += String(buffer[i], HEX);
    if(i+1 < len) s += ":";
  }
  s.toUpperCase();                           // Standardise UID format
  return s;
}

bool macStringToBytes(const String &mac,uint8_t*out){ // Convert "AA:BB:CC:DD:EE:FF" → bytes
  int idx=0,start=0;
  for(int i=0;i<=mac.length();i++){
    if(i==mac.length()||mac[i]==':'){
      String p=mac.substring(start,i);
      p.trim();
      out[idx++]=(uint8_t)strtoul(p.c_str(),NULL,16);
      start=i+1;
      if(idx>=6)break;
    }
  }
  return idx==6;                              // Valid only if exactly 6 bytes
}


void appendMapping(const String&uid,int index0,const PatientRow&row){ // Save UID → patient mapping
  File f=SPIFFS.open(MAPPING_PATH,FILE_APPEND);
  if(!f) return;
  String safe = row.nric + "," + row.bedWard + "," +
                row.starch + "," + row.veg + "," +
                row.meat + "," + row.fruit + "," +
                row.drink + "," + row.remarks;
  safe.replace("\n"," ");                     // Prevent line breaks
  f.print(uid+"|"+String(index0)+"|"+safe+"\n");
  f.close();
}

void beepBuzzer(){                            // Will beep when scanning tag
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}
// ---------- CSV parsing ----------
// Simple split by ',' and trim each column; ensure at least 9 columns
void splitLineSimple(const String &line, std::vector<String> &cols) {
  cols.clear();                              // Clear any previous contents
  int start = 0;                             // Start index for substring
  while (true) {
    int comma = line.indexOf(',', start);    // Find next comma
    if (comma < 0) {                         // No more commas found
      String piece = line.substring(start);  // Take remaining substring
      cols.push_back(piece);                 // Store final column
      break;
    } else {
      String piece = line.substring(start, comma); // Extract column
      cols.push_back(piece);                 // Store column
      start = comma + 1;                     // Move past comma
    }
  }
  // Ensure we always have at least 9 columns
  while (cols.size() < 9) cols.push_back(""); // Pad missing columns
}

void parseCSV(){
  patients.clear();                          // Reset patient list
  rowUIDs.clear();                           // Reset UID mapping list
  curIndex = 0;                              // Reset current index

  if(!SPIFFS.exists(CSV_PATH)){              // Check if CSV exists
    stateVar=IDLE;                           // No data = idle state
    return;
  }

  File f=SPIFFS.open(CSV_PATH,FILE_READ);    // Open CSV for reading
  String line;
  int row=0;
  while(f.available()){
    line = f.readStringUntil('\n');          // Read line until newline
    row++;
    if(row==1 || line=="") continue;         // Skip header or empty lines

    std::vector<String> cols;
    splitLineSimple(line, cols);             // Split CSV row into columns
    // expecting: Patient ,NRIC ,Bed.Ward,Starch,Veg,Meat ,Fruit,Drink,Remarks

    PatientRow pr;
    pr.nric    = cols[1];                    // Assign NRIC
    pr.bedWard = cols[2];                    // Assign bed/ward
    pr.starch  = cols[3];                    // Assign starch
    pr.veg     = cols[4];                    // Assign vegetables
    pr.meat    = cols[5];                    // Assign meat
    pr.fruit   = cols[6];                    // Assign fruit
    pr.drink   = cols[7];                    // Assign drink
    pr.remarks = cols[8];                    // Assign remarks

    patients.push_back(pr);                  // Store patient row
    rowUIDs.push_back("");                   // Initialise empty UID

    if(patients.size()>=MAX_ITEMS) break;    // Enforce max row limit
  }
  f.close();                                 // Close CSV file

  if(patients.empty()) stateVar=IDLE;        // No rows parsed
  else                 stateVar=READY;       // Ready for scanning

  Serial.printf("Parsed %d rows from CSV\n",(int)patients.size()); // Debug output
}

// ---------- ESP-NOW callbacks ----------
void onSend(const wifi_tx_info_t *info, esp_now_send_status_t status){
  (void)info;                                // Suppress unused warning
  if(lastSendPeer>=0 && lastSendPeer<MAX_PEERS && peers[lastSendPeer].valid){
    peers[lastSendPeer].lastAttempt = millis(); // Record send attempt time
    if(status==ESP_NOW_SEND_SUCCESS){
      peers[lastSendPeer].lastSuccess = millis(); // Record success time
    }
  }
  lastSendPeer = -1;                         // Reset send tracker
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len){
  if(!info || !data || len<=0) return;       // Validate incoming packet
  String s; s.reserve(len);                  // Pre-allocate string buffer
  for(int i=0;i<len;i++) s += (char)data[i]; // Convert bytes to string

  // Handle PONG for connectivity
  if(s.startsWith("PONG|")){
    for(int i=0;i<MAX_PEERS;i++){
      if(!peers[i].valid) continue;          // Skip invalid peers
      bool match=true;
      for(int b=0;b<6;b++){
        if(peers[i].mac[b]!=info->src_addr[b]){ // Compare MAC addresses
          match=false; 
          break;
        }
      }
      if(match){
        peers[i].lastSuccess = millis();     // Update peer heartbeat
        break;
      }
    }
  }
}

// ---------- ESP-NOW send helpers ----------
void sendRawToPeer(int slot, const String &payload){
  if(slot<0||slot>=MAX_PEERS||!peers[slot].valid) return; // Validate peer slot
  String out = payload;                      // Copy payload
  if(out.length()>240) out = out.substring(0,240); // ESP-NOW payload size limit
  lastSendPeer = slot;                       // Track sending peer
  esp_err_t r = esp_now_send(peers[slot].mac, (const uint8_t*)out.c_str(), out.length()); // Send packet
  peers[slot].lastAttempt = millis();        // Record attempt time
  if(r!=ESP_OK){
    Serial.printf("Send fail %s [%d]\n", peers[slot].macStr().c_str(), r); // Error logging
  }
}

// A sends: UID|NRIC,Bed.Ward,Starch,Veg,Meat,Fruit,Drink,Remarks
void sendDataToAll(const String &uid, const PatientRow &row){
  String payload = uid + "|" +               // Construct payload with UID
                   row.nric + "," + row.bedWard + "," +
                   row.starch + "," + row.veg + "," +
                   row.meat + "," + row.fruit + "," +
                   row.drink + "," + row.remarks;
  for(int i=0;i<MAX_PEERS;i++){
    if(peers[i].valid) sendRawToPeer(i,payload); // Broadcast to all peers
  }
}

void pingAllPeers(){
  String payload = "PING|" + String(millis()); // Timestamped ping message
  for(int i=0;i<MAX_PEERS;i++){
    if(peers[i].valid) sendRawToPeer(i,payload); // Ping each valid peer
  }
}
// ---------- Web UI ----------
const char *INDEX_HTML = R"HTML(              // HTML UI served to browser
<!doctype html><html><head><meta charset="utf-8"><title>ESP32-A</title>
<style>
#list li { white-space: pre-wrap; margin-bottom: 0.75em; }  /* Preserve formatting */
</style>
</head><body>
<h2>ESP32-A CSV + NFC</h2>
<form id="uploadForm"><input type="file" id="csvFile"><button type="submit">Upload</button></form>
<div id="uploadResult"></div>
<button onclick="startScan()">Start NFC Scanning</button>
<button onclick="rescan()">Rescan / Refresh ESP-NOW</button>
<p>NFC Status: <span id="nfcStatus">Idle</span></p>
<ul id="peerList"></ul>
<p>Current Index: <span id="idx">0</span>/<span id="tot">0</span></p>

<h3>Current Row (Full Details)</h3>
<pre id="curRow"></pre>

<h3>Patients (Full List)</h3>
<ol id="list"></ol>

<script>
document.getElementById('uploadForm').onsubmit = async (e)=>{ // CSV upload handler
  e.preventDefault();                                       // Prevent page reload
  const f = document.getElementById('csvFile').files[0];    // Get selected file
  if(!f){ alert('Select CSV'); return; }                    // Guard clause
  const fd = new FormData();
  fd.append('csvfile', f);                                  // Attach file
  document.getElementById('uploadResult').textContent='Uploading…';
  await fetch('/upload',{method:'POST',body:fd});           // Send to ESP32
  document.getElementById('uploadResult').textContent='Done';
};

async function refresh(){                                   // Periodic UI refresh
  const r = await fetch('/list');                           // Fetch system state
  const j = await r.json();
  document.getElementById('nfcStatus').textContent = j.nfcStatus;
  document.getElementById('idx').textContent       = j.curIndexDisplay;
  document.getElementById('tot').textContent       = j.total;

  // peers
  const pl = document.getElementById('peerList');
  pl.innerHTML='';
  for(const p of j.peers){
    const li = document.createElement('li');
    li.textContent = p.mac + ' – ' + p.status;              // Show peer MAC + status
    pl.appendChild(li);
  }

  // current row full details
  let curText = '';
  if(j.current){
    const c = j.current;
    curText += 'NRIC: '     + (c.nric     || '') + '\n';
    curText += 'Bed.Ward: ' + (c.bedWard  || '') + '\n';
    curText += 'Starch: '   + (c.starch   || '') + '\n';
    curText += 'Veg: '      + (c.veg      || '') + '\n';
    curText += 'Meat: '     + (c.meat     || '') + '\n';
    curText += 'Fruit: '    + (c.fruit    || '') + '\n';
    curText += 'Drink: '    + (c.drink    || '') + '\n';
    curText += 'Remarks: '  + (c.remarks  || '') + '\n';
    if(c.uid) curText      += 'UID: '     + c.uid;          // Display NFC UID
  }
  document.getElementById('curRow').textContent = curText;

  // full preview list
  const ol = document.getElementById('list');
  ol.innerHTML='';
  for(const row of j.preview){
    const line1 = '[' + row.index + '] ' + (row.nric || '') + ' (' + (row.bedWard || '') + ')';
    const parts = [];
    if(row.starch)  parts.push(row.starch);
    if(row.veg)     parts.push(row.veg);
    if(row.meat)    parts.push(row.meat);
    if(row.fruit)   parts.push(row.fruit);
    if(row.drink)   parts.push(row.drink);
    if(row.remarks) parts.push(row.remarks);
    let line2 = parts.join(' | ');
    if(row.uid) line2 += ' | UID: ' + row.uid;              // Show UID if tagged

    const li = document.createElement('li');
    li.textContent = line1 + '\n' + line2;
    ol.appendChild(li);
  }
}
setInterval(refresh,2000);                                  // Auto-refresh every 2s
refresh();

async function startScan(){ await fetch('/startScan',{method:'POST'}); } // Trigger scan
async function rescan(){ await fetch('/rescan'); await refresh(); }       // Re-add peers
</script></body></html>
)HTML";

void handleRoot(){
  server.send(200,"text/html",INDEX_HTML);                  // Serve web UI
}

void handleUpload(){
  HTTPUpload &u = server.upload();                          // Handle multipart upload
  static File f;
  if(u.status==UPLOAD_FILE_START){
    if(SPIFFS.exists(CSV_PATH))SPIFFS.remove(CSV_PATH);     // Clear old CSV
    if(SPIFFS.exists(MAPPING_PATH))SPIFFS.remove(MAPPING_PATH); // Clear mappings
    f = SPIFFS.open(CSV_PATH,FILE_WRITE);                   // Create new CSV
  }else if(u.status==UPLOAD_FILE_WRITE){
    if(f) f.write(u.buf,u.currentSize);                     // Write incoming chunk
  }else if(u.status==UPLOAD_FILE_END){
    if(f) f.close();                                        // Close file
    parseCSV();                                             // Parse uploaded CSV
  }
  server.send(200,"text/plain","OK");                       // Respond success
}

void handleStartScan(){
  if(patients.empty()){
    nfcStatus="ERROR: No CSV uploaded";                     // No data error
    stateVar=ERROR_STATE;
    waitingForTag=false;
    server.send(400,"text/plain","No CSV");
    return;
  }
  if(curIndex >= (int)patients.size()){
    nfcStatus="ERROR: No more rows to tag";                 // End of CSV
    stateVar=ERROR_STATE;
    waitingForTag=false;
    server.send(400,"text/plain","No more rows");
    return;
  }
  waitingForTag=true;                                      // Enable NFC scanning
  nfcStatus="Waiting for tag…";
  stateVar=WAITING_FOR_TAG;
  server.send(200,"text/plain","OK");
}

void handleRescan(){
  esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE);  // Force WiFi channel
  for(int i=0;i<MAX_PEERS;i++){
    if(peers[i].valid && !esp_now_is_peer_exist(peers[i].mac)){
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, peers[i].mac, 6);
      p.channel=AP_CHANNEL;
      p.ifidx=WIFI_IF_STA;
      p.encrypt=0;
      esp_now_add_peer(&p);                                 // Re-add missing peer
    }
  }
  pingAllPeers();                                          // Test connectivity
  server.send(200,"text/plain","rescanned & pinged");
}
void handleList(){
  unsigned long now = millis();                 // Current time for peer status checks
  String j="{";                                 // Begin JSON object
  j += "\"nfcStatus\":\""+nfcStatus+"\",";      // NFC state string
  j += "\"curIndexDisplay\":" + (String)(patients.empty()?0:curIndex+1) + ","; // 1-based index
  j += "\"total\":" + (String)patients.size() + ","; // Total rows

  // current row, full details
  String cnric, cbed, cstarch, cveg, cmeat, cfruit, cdrink, cremarks, cuid;
  if(curIndex<(int)patients.size()){
    cnric    = patients[curIndex].nric;         // Current NRIC
    cbed     = patients[curIndex].bedWard;      // Current bed/ward
    cstarch  = patients[curIndex].starch;       // Current starch
    cveg     = patients[curIndex].veg;          // Current veg
    cmeat    = patients[curIndex].meat;         // Current meat
    cfruit   = patients[curIndex].fruit;        // Current fruit
    cdrink   = patients[curIndex].drink;        // Current drink
    cremarks = patients[curIndex].remarks;      // Current remarks
  }
  if(curIndex<(int)rowUIDs.size()){
    cuid = rowUIDs[curIndex];                   // UID if already tagged
  }

  cnric.replace("\"","\\\"");                   // Escape JSON quotes
  cbed.replace("\"","\\\"");
  cstarch.replace("\"","\\\"");
  cveg.replace("\"","\\\"");
  cmeat.replace("\"","\\\"");
  cfruit.replace("\"","\\\"");
  cdrink.replace("\"","\\\"");
  cremarks.replace("\"","\\\"");
  cuid.replace("\"","\\\"");

  j += "\"current\":{";                         // Current-row JSON object
  j += "\"nric\":\""+cnric+"\",";
  j += "\"bedWard\":\""+cbed+"\",";
  j += "\"starch\":\""+cstarch+"\",";
  j += "\"veg\":\""+cveg+"\",";
  j += "\"meat\":\""+cmeat+"\",";
  j += "\"fruit\":\""+cfruit+"\",";
  j += "\"drink\":\""+cdrink+"\",";
  j += "\"remarks\":\""+cremarks+"\",";
  j += "\"uid\":\""+cuid+"\"";
  j += "},";

  // preview list (first 200)
  j += "\"preview\":[";
  int cnt = min((size_t)200, patients.size());  // Limit preview size
  for(int i=0;i<cnt;i++){
    String n  = patients[i].nric;     n.replace("\"","\\\"");
    String b  = patients[i].bedWard;  b.replace("\"","\\\"");
    String st = patients[i].starch;   st.replace("\"","\\\"");
    String vg = patients[i].veg;      vg.replace("\"","\\\"");
    String mt = patients[i].meat;     mt.replace("\"","\\\"");
    String fr = patients[i].fruit;    fr.replace("\"","\\\"");
    String dr = patients[i].drink;    dr.replace("\"","\\\"");
    String rm = patients[i].remarks;  rm.replace("\"","\\\"");
    String u  = (i<(int)rowUIDs.size())? rowUIDs[i] : "";
    u.replace("\"","\\\"");

    j += "{";
    j += "\"index\":" + String(i+1) + ",";       // 1-based row index
    j += "\"nric\":\""+n+"\",";
    j += "\"bedWard\":\""+b+"\",";
    j += "\"starch\":\""+st+"\",";
    j += "\"veg\":\""+vg+"\",";
    j += "\"meat\":\""+mt+"\",";
    j += "\"fruit\":\""+fr+"\",";
    j += "\"drink\":\""+dr+"\",";
    j += "\"remarks\":\""+rm+"\",";
    j += "\"uid\":\""+u+"\"";
    j += "}";
    if(i+1<cnt) j+=",";                          // JSON comma separator
  }
  j += "],";

  // peers (status)
  j += "\"peers\":[";
  for(int i=0;i<MAX_PEERS;i++){
    String status;
    if(!peers[i].valid){
      status = "Not set";                        // Slot unused
    }else if(peers[i].lastSuccess>0 && (now - peers[i].lastSuccess) < 10000){
      status = "Connected";                     // Recent response
    }else if(peers[i].lastAttempt>0){
      status = "Unreachable";                   // Attempted but no reply
    }else{
      status = "Unknown";                       // No activity yet
    }

    j += "{\"mac\":\""+peers[i].macStr()+"\",\"valid\":"+(String)(peers[i].valid?1:0)
      +",\"lastAttempt\":"+(String)peers[i].lastAttempt
      +",\"lastSuccess\":"+(String)peers[i].lastSuccess
      +",\"status\":\""+status+"\"}";
    if(i<MAX_PEERS-1) j+=",";                   // JSON separator
  }
  j += "]}";

  server.send(200,"application/json",j);        // Send JSON response
}

// ---------- Peers ----------
void initPeers(){
  for(int i=0;i<MAX_PEERS;i++){
    uint8_t macb[6]={0};                        // MAC byte buffer
    String s=String(HARD_CODED_MACS[i]); s.trim(); s.toUpperCase();
    bool ok=macStringToBytes(s,macb);           // Parse MAC string
    peers[i].valid = ok && !isZeroMac(macb);    // Validate MAC
    memcpy(peers[i].mac,macb,6);
    if(peers[i].valid){
      esp_now_peer_info_t p={};
      memcpy(p.peer_addr, peers[i].mac, 6);
      p.channel=AP_CHANNEL;
      p.ifidx=WIFI_IF_STA;
      p.encrypt=0;
      if(!esp_now_is_peer_exist(peers[i].mac)){
        if(esp_now_add_peer(&p)==ESP_OK){
          Serial.printf("Peer added: %s\n", peers[i].macStr().c_str());
        }
      }
    }else{
      Serial.printf("Peer slot %d not set\n", i+1);
    }
  }
}

// ---------- Setup & Loop ----------
void setup(){
  Serial.begin(115200);                         // Start serial debugging
  SPIFFS.begin(true);                          // Start SPIFFS

  // Start fresh each boot
  if(SPIFFS.exists(CSV_PATH))SPIFFS.remove(CSV_PATH);        // Remove old CSV
  if(SPIFFS.exists(MAPPING_PATH))SPIFFS.remove(MAPPING_PATH);// Remove old mappings

  pinMode(BUZZER_PIN, OUTPUT);                 // Configure buzzer pin
  digitalWrite(BUZZER_PIN, LOW);               // Ensure buzzer off

  SPI.begin();                                 // Start SPI bus
  mfrc522.PCD_Init();                          // Initialise RC522 reader

  WiFi.mode(WIFI_AP_STA);                      // Enable AP + STA
  WiFi.softAP(AP_SSID, AP_PWD, AP_CHANNEL);    // Start access point
  esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE); // Lock channel
  WiFi.setSleep(false);                        // Disable WiFi power saving

  Serial.printf("AP %s @ %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("ESP-A STA MAC: %s\n", WiFi.macAddress().c_str());

  if(esp_now_init()!=ESP_OK) Serial.println("ESP-NOW init failed");
  esp_now_register_send_cb(onSend);             // Register send callback
  esp_now_register_recv_cb(onRecv);             // Register receive callback

  initPeers();                                  // Add ESP-NOW peers

  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/upload",    HTTP_POST, [](){ server.send(200); }, handleUpload);
  server.on("/list",      HTTP_GET,  handleList);
  server.on("/startScan", HTTP_POST, handleStartScan);
  server.on("/rescan",    HTTP_GET,  handleRescan);
  server.begin();                               // Start web server
}

unsigned long tick=0;                           // Debug timer

void loop(){
  server.handleClient();                        // Process HTTP requests

  if(millis()-lastPingAt > 5000){
    lastPingAt=millis();
    pingAllPeers();                             // Periodic peer heartbeat
  }

  if(millis()-tick>2000){
    tick=millis();
    Serial.printf("State=%d cur=%d tot=%d\n",(int)stateVar,curIndex,(int)patients.size());
  }

  // NFC scanning state machine
  if(waitingForTag){
    if(curIndex >= (int)patients.size()){
      nfcStatus="ERROR: No more rows to tag";   // End-of-data guard
      stateVar=ERROR_STATE;
      waitingForTag=false;
      return;
    }

    if(mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()){
      String uid = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size); // Read UID
      Serial.println("Tag: "+uid);

      beepBuzzer();                             // Audible confirmation

      if(curIndex < (int)rowUIDs.size()){
        rowUIDs[curIndex] = uid;                // Save UID to row
        appendMapping(uid, curIndex, patients[curIndex]); // Persist mapping
      }

      // send full data
      sendDataToAll(uid, patients[curIndex]);   // Broadcast patient data

      mfrc522.PICC_HaltA();                     // Stop card communication
      mfrc522.PCD_StopCrypto1();                // Stop encryption

      nfcStatus = "Tag read: "+uid;
      stateVar = TAG_READ;

      curIndex++;                               // Move to next patient

      if(curIndex < (int)patients.size()){
        delay(2000);                            // Cooldown between scans
        waitingForTag=true;
        nfcStatus="Waiting for tag…";
        stateVar=WAITING_FOR_TAG;
      }else{
        nfcStatus="ERROR: No more rows to tag";
        stateVar=ERROR_STATE;
        waitingForTag=false;
      }
    }
  }
}
