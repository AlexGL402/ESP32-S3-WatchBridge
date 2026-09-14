#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <vector>

static const char* AP_SSID = "WatchBridge-S3";
static const char* AP_PASS = "12345678";

static const char* WATCH_SERVICE = "6e400f01-b5a3-f393-e0a9-e50e24dcca9d";
static const char* WATCH_WRITE   = "6e400002-b5a3-f393-e0a9-e50e24dcca9d";
static const char* WATCH_NOTIFY  = "6e400003-b5a3-f393-e0a9-e50e24dcca9d";

static WebServer server(80);
static Preferences prefs;

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* writeChr = nullptr;
static NimBLERemoteCharacteristic* notifyChr = nullptr;

static bool connected = false;
static String boundMac;
static String lastRx;
static String lastStatus = "booting";

struct ScanItem {
    String mac;
    String name;
    int rssi;
};

static std::vector<ScanItem> scanItems;

static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

static void setStatus(const String& s) {
    lastStatus = s;
    Serial.println("[STATUS] " + s);
}

static void notifyCallback(
    NimBLERemoteCharacteristic* chr,
    uint8_t* data,
    size_t len,
    bool isNotify) {

    String s;
    s.reserve(len * 3 + 48);
    s += chr->getUUID().toString().c_str();
    s += " ";

    for (size_t i = 0; i < len; ++i) {
        char b[4];
        snprintf(b, sizeof(b), "%02X ", data[i]);
        s += b;
    }

    lastRx = s;

    Serial.printf("\n[WATCH RX] %s len=%u : ",
                  chr->getUUID().toString().c_str(),
                  (unsigned)len);

    for (size_t i = 0; i < len; ++i) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        connected = true;
        setStatus("watch connected");
    }

    void onDisconnect(NimBLEClient* c) override {
        connected = false;
        writeChr = nullptr;
        notifyChr = nullptr;
        setStatus("watch disconnected");
    }
};

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        ScanItem item;
        item.mac = dev->getAddress().toString().c_str();
        item.name = dev->haveName() ? dev->getName().c_str() : "";
        item.rssi = dev->getRSSI();

        for (auto& x : scanItems) {
            if (x.mac.equalsIgnoreCase(item.mac)) {
                if (item.rssi > x.rssi) x.rssi = item.rssi;
                if (x.name.length() == 0 && item.name.length()) x.name = item.name;
                return;
            }
        }

        scanItems.push_back(item);
    }
};

static ClientCallbacks clientCallbacks;
static ScanCallbacks scanCallbacks;

static void disconnectWatch() {
    if (client && client->isConnected()) {
        client->disconnect();
    }

    connected = false;
    writeChr = nullptr;
    notifyChr = nullptr;
}

static bool sendRaw(const uint8_t* data, size_t len) {
    if (!connected || !writeChr) {
        setStatus("TX failed: watch not connected");
        return false;
    }

    Serial.printf("[TX] %u bytes: ", (unsigned)len);
    for (size_t i = 0; i < len; ++i) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();

    bool ok = false;

    if (writeChr->canWriteNoResponse()) {
        ok = writeChr->writeValue(data, len, false);
    } else {
        ok = writeChr->writeValue(data, len, true);
    }

    setStatus(ok ? "TX OK" : "TX failed");
    return ok;
}

static bool sendFitPro(uint8_t command, const uint8_t* payload, size_t payloadLen) {
    const size_t totalLen = 8 + payloadLen;

    if (totalLen > 250) {
        setStatus("packet too large");
        return false;
    }

    uint8_t packet[250];
    const uint16_t protocolLen = (uint16_t)(totalLen - 3);

    packet[0] = 0xCD;
    packet[1] = (protocolLen >> 8) & 0xFF;
    packet[2] = protocolLen & 0xFF;
    packet[3] = 0x12;
    packet[4] = 0x01;
    packet[5] = command;
    packet[6] = (payloadLen >> 8) & 0xFF;
    packet[7] = payloadLen & 0xFF;

    if (payloadLen) {
        memcpy(&packet[8], payload, payloadLen);
    }

    return sendRaw(packet, totalLen);
}

static bool testFindWatch() {
    const uint8_t payload[] = {0x01};
    return sendFitPro(0x0B, payload, sizeof(payload));
}

static bool enableNotifications() {
    const uint8_t payload[] = {
        1,1,1,1,1,1,1,1,1,1,1,1
    };

    return sendFitPro(0x07, payload, sizeof(payload));
}

static bool sendNotification(const String& sender, const String& text) {
    String msg = sender + ":" + text;
    const size_t msgLen = msg.length();

    if (msgLen + 3 > 220) {
        setStatus("notification too long");
        return false;
    }

    uint8_t payload[220];
    payload[0] = 0x01;
    payload[1] = 0x00;
    payload[2] = 0x00;

    memcpy(&payload[3], msg.c_str(), msgLen);

    return sendFitPro(0x12, payload, msgLen + 3);
}

static bool connectWatch(const String& mac) {
    if (mac.length() == 0) {
        setStatus("no watch bound");
        return false;
    }

    disconnectWatch();

    if (client) {
        NimBLEDevice::deleteClient(client);
        client = nullptr;
    }

    NimBLEAddress address(mac.c_str());

    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);

    setStatus("connecting to " + mac);

    if (!client->connect(address)) {
        setStatus("connect failed");
        return false;
    }

    Serial.printf("[BLE] MTU: %u\n", client->getMTU());

    NimBLERemoteService* svc = client->getService(WATCH_SERVICE);

    if (!svc) {
        setStatus("connected, but watch service not found");
        disconnectWatch();
        return false;
    }

    writeChr = svc->getCharacteristic(WATCH_WRITE);
    notifyChr = svc->getCharacteristic(WATCH_NOTIFY);

    if (!writeChr) {
        setStatus("connected, but WRITE characteristic missing");
        disconnectWatch();
        return false;
    }

    if (notifyChr && notifyChr->canNotify()) {
        const bool ok = notifyChr->subscribe(true, notifyCallback);
        Serial.printf("[BLE] notify subscribe: %s\n", ok ? "OK" : "FAIL");
    }

    connected = true;
    setStatus("connected to " + mac);
    return true;
}

static void runScan() {
    setStatus("BLE scan running...");

    disconnectWatch();
    scanItems.clear();

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->clearResults();
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->start(6, false);

    setStatus("scan complete: " + String(scanItems.size()) + " devices");
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 WatchBridge</title>
<style>
body{font-family:Arial,sans-serif;max-width:780px;margin:20px auto;padding:0 14px;background:#111;color:#eee}
.card{background:#1d1d1d;border:1px solid #333;border-radius:12px;padding:14px;margin:12px 0}
button,input{font-size:16px;padding:9px;margin:4px;border-radius:7px;border:1px solid #555}
button{cursor:pointer}
.dev{display:flex;gap:8px;align-items:center;justify-content:space-between;border-top:1px solid #333;padding:8px 0}
small{color:#aaa}
#status,#rx{white-space:pre-wrap;overflow-wrap:anywhere}
input[type=text]{width:90%;background:#111;color:#eee}
.ok{color:#8f8}.bad{color:#f88}
</style>
</head>
<body>
<h2>ESP32-S3 WatchBridge</h2>

<div class="card">
<b>Status</b>
<div id="status">loading...</div>
<small>AP: WatchBridge-S3 / 12345678 · http://192.168.4.1</small>
</div>

<div class="card">
<b>1. BLE scanner</b><br>
<button onclick="scan()">Scan BLE 6s</button>
<div id="devices"></div>
</div>

<div class="card">
<b>2. Bound watch</b>
<div id="bound">—</div>
<button onclick="connectWatch()">Connect</button>
<button onclick="unbind()">Unbind</button>
</div>

<div class="card">
<b>3. Test commands</b><br>
<button onclick="cmd('find')">Find / Vibrate</button>
<button onclick="cmd('enable')">Enable notifications</button>
<button onclick="cmd('test')">Send TEST</button>
</div>

<div class="card">
<b>Custom notification</b><br>
<input id="sender" type="text" value="MeshCore" placeholder="Sender"><br>
<input id="msgtext" type="text" value="Hello from ESP32-S3" placeholder="Text"><br>
<button onclick="sendText()">Send</button>
</div>

<div class="card">
<b>Last packet from watch</b>
<div id="rx"><small>none</small></div>
</div>

<script>
async function j(url){
  let r=await fetch(url);
  return await r.json();
}

async function refresh(){
  try{
    let x=await j('/api/status');
    status.textContent=x.status+'\nconnected='+x.connected;
    status.className=x.connected?'ok':'';
    bound.textContent=x.bound||'not bound';
    rx.textContent=x.last_rx||'none';
  }catch(e){
    status.textContent='web/API error: '+e;
    status.className='bad';
  }
}

async function scan(){
  status.textContent='scanning...';
  let x=await j('/api/scan');
  devices.innerHTML='';
  x.devices.forEach(d=>{
    let e=document.createElement('div');
    e.className='dev';
    let t=document.createElement('span');
    t.textContent=(d.name||'(no name)')+'  '+d.mac+'  '+d.rssi+' dBm';
    let b=document.createElement('button');
    b.textContent='Bind';
    b.onclick=()=>bind(d.mac);
    e.append(t,b);
    devices.appendChild(e);
  });
  refresh();
}

async function bind(mac){
  await j('/api/bind?mac='+encodeURIComponent(mac));
  await refresh();
}

async function unbind(){
  await j('/api/unbind');
  await refresh();
}

async function connectWatch(){
  await j('/api/connect');
  await refresh();
}

async function cmd(c){
  await j('/api/cmd?name='+encodeURIComponent(c));
  setTimeout(refresh,200);
}

async function sendText(){
  let s=document.getElementById('sender').value;
  let t=document.getElementById('msgtext').value;
  await j('/api/send?sender='+encodeURIComponent(s)+'&text='+encodeURIComponent(t));
  setTimeout(refresh,200);
}

refresh();
setInterval(refresh,2500);
</script>
</body>
</html>
)HTML";

static void apiStatus() {
    String s = "{";
    s += "\"connected\":" + String(connected ? "true" : "false") + ",";
    s += "\"bound\":\"" + jsonEscape(boundMac) + "\",";
    s += "\"status\":\"" + jsonEscape(lastStatus) + "\",";
    s += "\"last_rx\":\"" + jsonEscape(lastRx) + "\"";
    s += "}";

    server.send(200, "application/json", s);
}

static void apiScan() {
    runScan();

    String s = "{\"devices\":[";

    for (size_t i = 0; i < scanItems.size(); ++i) {
        if (i) s += ',';

        s += "{\"mac\":\"" + jsonEscape(scanItems[i].mac) + "\",";
        s += "\"name\":\"" + jsonEscape(scanItems[i].name) + "\",";
        s += "\"rssi\":" + String(scanItems[i].rssi) + "}";
    }

    s += "]}";
    server.send(200, "application/json", s);
}

static void apiBind() {
    const String mac = server.arg("mac");

    if (mac.length() != 17) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad mac\"}");
        return;
    }

    boundMac = mac;
    prefs.putString("watch_mac", boundMac);

    const bool ok = connectWatch(boundMac);

    server.send(200, "application/json",
                String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void apiUnbind() {
    disconnectWatch();
    boundMac = "";
    prefs.remove("watch_mac");
    setStatus("watch unbound");

    server.send(200, "application/json", "{\"ok\":true}");
}

static void apiConnect() {
    const bool ok = connectWatch(boundMac);

    server.send(200, "application/json",
                String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void apiCmd() {
    const String name = server.arg("name");
    bool ok = false;

    if (name == "find") {
        ok = testFindWatch();
    } else if (name == "enable") {
        ok = enableNotifications();
    } else if (name == "test") {
        ok = sendNotification("MeshCore", "TEST FROM ESP32-S3");
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown command\"}");
        return;
    }

    server.send(200, "application/json",
                String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void apiSend() {
    const String sender = server.arg("sender").length() ? server.arg("sender") : "MeshCore";
    const String text = server.arg("text");

    const bool ok = sendNotification(sender, text);

    server.send(200, "application/json",
                String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void startWeb() {
    WiFi.mode(WIFI_AP);

    const bool apOk = WiFi.softAP(AP_SSID, AP_PASS);

    Serial.printf("[WEB] softAP: %s\n", apOk ? "OK" : "FAIL");
    Serial.print("[WEB] AP IP: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", INDEX_HTML);
    });

    server.on("/api/status", HTTP_GET, apiStatus);
    server.on("/api/scan", HTTP_GET, apiScan);
    server.on("/api/bind", HTTP_GET, apiBind);
    server.on("/api/unbind", HTTP_GET, apiUnbind);
    server.on("/api/connect", HTTP_GET, apiConnect);
    server.on("/api/cmd", HTTP_GET, apiCmd);
    server.on("/api/send", HTTP_GET, apiSend);

    server.onNotFound([]() {
        server.send(404, "application/json", "{\"error\":\"not found\"}");
    });

    server.begin();
    Serial.println("[WEB] server started");
}

static void printHelp() {
    Serial.println();
    Serial.println("Commands:");
    Serial.println("  F = Find/vibrate bound watch");
    Serial.println("  E = Enable notifications");
    Serial.println("  T = Send TEST notification");
    Serial.println("  S = BLE scan 6s");
    Serial.println("  C = Connect bound watch");
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("====================================");
    Serial.println(" ESP32-S3 WATCHBRIDGE");
    Serial.println("====================================");

    prefs.begin("watchbridge", false);
    boundMac = prefs.getString("watch_mac", "");

    NimBLEDevice::init("S3-WatchBridge");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    startWeb();

    if (boundMac.length()) {
        Serial.println("[NVS] bound watch: " + boundMac);
        connectWatch(boundMac);
    } else {
        setStatus("ready - no watch bound");
    }

    printHelp();
}

void loop() {
    server.handleClient();

    if (Serial.available()) {
        const char c = Serial.read();

        if (c == 'f' || c == 'F') {
            testFindWatch();
        } else if (c == 'e' || c == 'E') {
            enableNotifications();
        } else if (c == 't' || c == 'T') {
            sendNotification("MeshCore", "TEST FROM ESP32-S3");
        } else if (c == 's' || c == 'S') {
            runScan();
            for (const auto& d : scanItems) {
                Serial.printf("[SCAN] %s RSSI=%d NAME=\"%s\"\n",
                              d.mac.c_str(), d.rssi, d.name.c_str());
            }
        } else if (c == 'c' || c == 'C') {
            connectWatch(boundMac);
        }
    }

    delay(2);
}
