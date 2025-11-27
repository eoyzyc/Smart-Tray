#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <vector>

#define AP_SSID    "Smart Tray Uploader "
#define AP_PWD     ""
#define AP_CHANNEL 1

// RC522 pins
#define RST_PIN 22
#define SS_PIN  5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Buzzer pin
#define BUZZER_PIN 15

WebServer server(80);

const char *CSV_PATH     = "/uploaded.csv";
const char *MAPPING_PATH = "/mappings.txt";

#define MAX_ITEMS 4000
#define MAX_PEERS 5

// ---- Patient row structure (parsed from CSV) ----
// CSV format:
// Patient ,NRIC ,Bed.Ward,Starch,Veg,Meat ,Fruit,Drink,Remarks
struct PatientRow {
  String nric;      // col 2
  String bedWard;   // col 3 (e.g. "2-13")
  String starch;    // col 4
  String veg;       // col 5
  String meat;      // col 6
  String fruit;     // col 7
  String drink;     // col 8
  String remarks;   // col 9 (may be empty)
};

std::vector<PatientRow> patients;
std::vector<String>     rowUIDs;   // same size as patients
int curIndex = 0;                  // 0-based index

enum State { IDLE, READY, WAITING_FOR_TAG, TAG_READ, SENDING, SENT, ERROR_STATE };
volatile State stateVar = IDLE;
bool   waitingForTag    = false;
String nfcStatus        = "Idle";

// Hard-coded peers (STA MACs) – first is B, others can be added later
const char* HARD_CODED_MACS[MAX_PEERS] = {
  "38:18:2B:A7:7A:F4",  // ESP-B STA MAC
  "38:18:2B:A7:6D:6C",
  "38:18:2B:A7:9C:80",
  "38:18:2B:A7:9B:68",
  "38:18:2B:A7:86:4C"
};

struct Peer {
  uint8_t mac[6];
  bool valid = false;
  unsigned long lastAttempt = 0;
  unsigned long lastSuccess = 0;
  String macStr() const {
    char b[18];
    sprintf(b,"%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return String(b);
  }
};
Peer peers[MAX_PEERS];

volatile int   lastSendPeer = -1;
unsigned long  lastPingAt   = 0;

// ---------- Utilities ----------
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

bool macStringToBytes(const String &mac,uint8_t*out){
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
  return idx==6;
}

bool isZeroMac(const uint8_t*m){
  for(int i=0;i<6;i++) if(m[i]!=0) return false;
  return true;
}

void appendMapping(const String&uid,int index0,const PatientRow&row){
  File f=SPIFFS.open(MAPPING_PATH,FILE_APPEND);
  if(!f) return;
  String safe = row.nric + "," + row.bedWard + "," +
                row.starch + "," + row.veg + "," +
                row.meat + "," + row.fruit + "," +
                row.drink + "," + row.remarks;
  safe.replace("\n"," ");
  f.print(uid+"|"+String(index0)+"|"+safe+"\n");
  f.close();
}

void beepBuzzer(){
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}

// ---------- CSV parsing ----------
// Simple split by ',' and trim each column; ensure at least 9 columns
void splitLineSimple(const String &line, std::vector<String> &cols) {
  cols.clear();
  int start = 0;
  while (true) {
    int comma = line.indexOf(',', start);
    if (comma < 0) {
      String piece = line.substring(start);
      piece.trim();
      cols.push_back(piece);
      break;
    } else {
      String piece = line.substring(start, comma);
      piece.trim();
      cols.push_back(piece);
      start = comma + 1;
    }
  }
  // Ensure we always have at least 9 columns
  while (cols.size() < 9) cols.push_back("");
}

void parseCSV(){
  patients.clear();
  rowUIDs.clear();
  curIndex = 0;

  if(!SPIFFS.exists(CSV_PATH)){
    stateVar=IDLE;
    return;
  }

  File f=SPIFFS.open(CSV_PATH,FILE_READ);
  String line;
  int row=0;
  while(f.available()){
    line = f.readStringUntil('\n');
    line.trim();
    row++;
    if(row==1 || line=="") continue; // skip header

    std::vector<String> cols;
    splitLineSimple(line, cols);
    // expecting: Patient ,NRIC ,Bed.Ward,Starch,Veg,Meat ,Fruit,Drink,Remarks

    PatientRow pr;
    pr.nric    = cols[1];
    pr.bedWard = cols[2];
    pr.starch  = cols[3];
    pr.veg     = cols[4];
    pr.meat    = cols[5];
    pr.fruit   = cols[6];
    pr.drink   = cols[7];
    pr.remarks = cols[8];

    patients.push_back(pr);
    rowUIDs.push_back("");

    if(patients.size()>=MAX_ITEMS) break;
  }
  f.close();

  if(patients.empty()) stateVar=IDLE;
  else                 stateVar=READY;

  Serial.printf("Parsed %d rows from CSV\n",(int)patients.size());
}

// ---------- ESP-NOW callbacks ----------
void onSend(const wifi_tx_info_t *info, esp_now_send_status_t status){
  (void)info;
  if(lastSendPeer>=0 && lastSendPeer<MAX_PEERS && peers[lastSendPeer].valid){
    peers[lastSendPeer].lastAttempt = millis();
    if(status==ESP_NOW_SEND_SUCCESS){
      peers[lastSendPeer].lastSuccess = millis();
    }
  }
  lastSendPeer = -1;
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len){
  if(!info || !data || len<=0) return;
  String s; s.reserve(len);
  for(int i=0;i<len;i++) s += (char)data[i];

  // Handle PONG for connectivity
  if(s.startsWith("PONG|")){
    for(int i=0;i<MAX_PEERS;i++){
      if(!peers[i].valid) continue;
      bool match=true;
      for(int b=0;b<6;b++){
        if(peers[i].mac[b]!=info->src_addr[b]){ match=false; break; }
      }
      if(match){
        peers[i].lastSuccess = millis();
        break;
      }
    }
  }
}

// ---------- ESP-NOW send helpers ----------
void sendRawToPeer(int slot, const String &payload){
  if(slot<0||slot>=MAX_PEERS||!peers[slot].valid) return;
  String out = payload;
  if(out.length()>240) out = out.substring(0,240);
  lastSendPeer = slot;
  esp_err_t r = esp_now_send(peers[slot].mac, (const uint8_t*)out.c_str(), out.length());
  peers[slot].lastAttempt = millis();
  if(r!=ESP_OK){
    Serial.printf("Send fail %s [%d]\n", peers[slot].macStr().c_str(), r);
  }
}

// A sends: UID|NRIC,Bed.Ward,Starch,Veg,Meat,Fruit,Drink,Remarks
void sendDataToAll(const String &uid, const PatientRow &row){
  String payload = uid + "|" +
                   row.nric + "," + row.bedWard + "," +
                   row.starch + "," + row.veg + "," +
                   row.meat + "," + row.fruit + "," +
                   row.drink + "," + row.remarks;
  for(int i=0;i<MAX_PEERS;i++){
    if(peers[i].valid) sendRawToPeer(i,payload);
  }
}

void pingAllPeers(){
  String payload = "PING|" + String(millis());
  for(int i=0;i<MAX_PEERS;i++){
    if(peers[i].valid) sendRawToPeer(i,payload);
  }
}

// ---------- Web UI ----------
const char *INDEX_HTML = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><title>ESP32-A</title>
<style>
#list li { white-space: pre-wrap; margin-bottom: 0.75em; }
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
document.getElementById('uploadForm').onsubmit = async (e)=>{
  e.preventDefault();
  const f = document.getElementById('csvFile').files[0];
  if(!f){ alert('Select CSV'); return; }
  const fd = new FormData();
  fd.append('csvfile', f);
  document.getElementById('uploadResult').textContent='Uploading…';
  await fetch('/upload',{method:'POST',body:fd});
  document.getElementById('uploadResult').textContent='Done';
};

async function refresh(){
  const r = await fetch('/list');
  const j = await r.json();
  document.getElementById('nfcStatus').textContent = j.nfcStatus;
  document.getElementById('idx').textContent       = j.curIndexDisplay;
  document.getElementById('tot').textContent       = j.total;

  // peers
  const pl = document.getElementById('peerList');
  pl.innerHTML='';
  for(const p of j.peers){
    const li = document.createElement('li');
    li.textContent = p.mac + ' – ' + p.status;
    pl.appendChild(li);
  }

  // current row full details
  let curText = '';
  if(j.current){
    const c = j.current;
    curText += 'NRIC: '     + (c.nric     || '') + '\\n';
    curText += 'Bed.Ward: ' + (c.bedWard  || '') + '\\n';
    curText += 'Starch: '   + (c.starch   || '') + '\\n';
    curText += 'Veg: '      + (c.veg      || '') + '\\n';
    curText += 'Meat: '     + (c.meat     || '') + '\\n';
    curText += 'Fruit: '    + (c.fruit    || '') + '\\n';
    curText += 'Drink: '    + (c.drink    || '') + '\\n';
    curText += 'Remarks: '  + (c.remarks  || '') + '\\n';
    if(c.uid) curText      += 'UID: '     + c.uid;
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
    if(row.uid) line2 += ' | UID: ' + row.uid;

    const li = document.createElement('li');
    li.textContent = line1 + '\\n' + line2;
    ol.appendChild(li);
  }
}
setInterval(refresh,2000);
refresh();

async function startScan(){ await fetch('/startScan',{method:'POST'}); }
async function rescan(){ await fetch('/rescan'); await refresh(); }
</script></body></html>
)HTML";

void handleRoot(){
  server.send(200,"text/html",INDEX_HTML);
}

void handleUpload(){
  HTTPUpload &u = server.upload();
  static File f;
  if(u.status==UPLOAD_FILE_START){
    if(SPIFFS.exists(CSV_PATH))SPIFFS.remove(CSV_PATH);
    if(SPIFFS.exists(MAPPING_PATH))SPIFFS.remove(MAPPING_PATH);
    f = SPIFFS.open(CSV_PATH,FILE_WRITE);
  }else if(u.status==UPLOAD_FILE_WRITE){
    if(f) f.write(u.buf,u.currentSize);
  }else if(u.status==UPLOAD_FILE_END){
    if(f) f.close();
    parseCSV();
  }
  server.send(200,"text/plain","OK");
}

void handleStartScan(){
  if(patients.empty()){
    nfcStatus="ERROR: No CSV uploaded";
    stateVar=ERROR_STATE;
    waitingForTag=false;
    server.send(400,"text/plain","No CSV");
    return;
  }
  if(curIndex >= (int)patients.size()){
    nfcStatus="ERROR: No more rows to tag";
    stateVar=ERROR_STATE;
    waitingForTag=false;
    server.send(400,"text/plain","No more rows");
    return;
  }
  waitingForTag=true;
  nfcStatus="Waiting for tag…";
  stateVar=WAITING_FOR_TAG;
  server.send(200,"text/plain","OK");
}

void handleRescan(){
  esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
  for(int i=0;i<MAX_PEERS;i++){
    if(peers[i].valid && !esp_now_is_peer_exist(peers[i].mac)){
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, peers[i].mac, 6);
      p.channel=AP_CHANNEL;
      p.ifidx=WIFI_IF_STA;
      p.encrypt=0;
      esp_now_add_peer(&p);
    }
  }
  pingAllPeers();
  server.send(200,"text/plain","rescanned & pinged");
}

void handleList(){
  unsigned long now = millis();
  String j="{";
  j += "\"nfcStatus\":\""+nfcStatus+"\",";
  j += "\"curIndexDisplay\":" + (String)(patients.empty()?0:curIndex+1) + ",";
  j += "\"total\":" + (String)patients.size() + ",";

  // current row, full details
  String cnric, cbed, cstarch, cveg, cmeat, cfruit, cdrink, cremarks, cuid;
  if(curIndex<(int)patients.size()){
    cnric    = patients[curIndex].nric;
    cbed     = patients[curIndex].bedWard;
    cstarch  = patients[curIndex].starch;
    cveg     = patients[curIndex].veg;
    cmeat    = patients[curIndex].meat;
    cfruit   = patients[curIndex].fruit;
    cdrink   = patients[curIndex].drink;
    cremarks = patients[curIndex].remarks;
  }
  if(curIndex<(int)rowUIDs.size()){
    cuid = rowUIDs[curIndex];
  }

  cnric.replace("\"","\\\"");
  cbed.replace("\"","\\\"");
  cstarch.replace("\"","\\\"");
  cveg.replace("\"","\\\"");
  cmeat.replace("\"","\\\"");
  cfruit.replace("\"","\\\"");
  cdrink.replace("\"","\\\"");
  cremarks.replace("\"","\\\"");
  cuid.replace("\"","\\\"");

  j += "\"current\":{";
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
  int cnt = min((size_t)200, patients.size());
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
    j += "\"index\":" + String(i+1) + ",";
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
    if(i+1<cnt) j+=",";
  }
  j += "],";

  // peers (status)
  j += "\"peers\":[";
  for(int i=0;i<MAX_PEERS;i++){
    String status;
    if(!peers[i].valid){
      status = "Not set";
    }else if(peers[i].lastSuccess>0 && (now - peers[i].lastSuccess) < 10000){
      status = "Connected";
    }else if(peers[i].lastAttempt>0){
      status = "Unreachable";
    }else{
      status = "Unknown";
    }

    j += "{\"mac\":\""+peers[i].macStr()+"\",\"valid\":"+(String)(peers[i].valid?1:0)
      +",\"lastAttempt\":"+(String)peers[i].lastAttempt
      +",\"lastSuccess\":"+(String)peers[i].lastSuccess
      +",\"status\":\""+status+"\"}";
    if(i<MAX_PEERS-1) j+=",";
  }
  j += "]}";

  server.send(200,"application/json",j);
}

// ---------- Peers ----------
void initPeers(){
  for(int i=0;i<MAX_PEERS;i++){
    uint8_t macb[6]={0};
    String s=String(HARD_CODED_MACS[i]); s.trim(); s.toUpperCase();
    bool ok=macStringToBytes(s,macb);
    peers[i].valid = ok && !isZeroMac(macb);
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
  Serial.begin(115200);
  SPIFFS.begin(true);

  // Start fresh each boot
  if(SPIFFS.exists(CSV_PATH))SPIFFS.remove(CSV_PATH);
  if(SPIFFS.exists(MAPPING_PATH))SPIFFS.remove(MAPPING_PATH);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  SPI.begin();
  mfrc522.PCD_Init();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PWD, AP_CHANNEL);
  esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
  WiFi.setSleep(false);

  Serial.printf("AP %s @ %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("ESP-A STA MAC: %s\n", WiFi.macAddress().c_str());

  if(esp_now_init()!=ESP_OK) Serial.println("ESP-NOW init failed");
  esp_now_register_send_cb(onSend);
  esp_now_register_recv_cb(onRecv);

  initPeers();

  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/upload",    HTTP_POST, [](){ server.send(200); }, handleUpload);
  server.on("/list",      HTTP_GET,  handleList);
  server.on("/startScan", HTTP_POST, handleStartScan);
  server.on("/rescan",    HTTP_GET,  handleRescan);
  server.begin();
}

unsigned long tick=0;

void loop(){
  server.handleClient();

  if(millis()-lastPingAt > 5000){
    lastPingAt=millis();
    pingAllPeers();
  }

  if(millis()-tick>2000){
    tick=millis();
    Serial.printf("State=%d cur=%d tot=%d\n",(int)stateVar,curIndex,(int)patients.size());
  }

  // NFC scanning state machine
  if(waitingForTag){
    if(curIndex >= (int)patients.size()){
      nfcStatus="ERROR: No more rows to tag";
      stateVar=ERROR_STATE;
      waitingForTag=false;
      return;
    }

    if(mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()){
      String uid = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
      Serial.println("Tag: "+uid);

      beepBuzzer();

      if(curIndex < (int)rowUIDs.size()){
        rowUIDs[curIndex] = uid;
        appendMapping(uid, curIndex, patients[curIndex]);
      }

      // send full data
      sendDataToAll(uid, patients[curIndex]);

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();

      nfcStatus = "Tag read: "+uid;
      stateVar = TAG_READ;

      curIndex++;

      if(curIndex < (int)patients.size()){
        delay(2000); // 2s cooldown
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

