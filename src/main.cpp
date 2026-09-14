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
static String serialLine;
static String gattDump;
static String gattDevice;
static bool gattFitProCompatible = false;

struct ScanItem {
    String mac;
    String name;
    int rssi;
};

struct WiFiScanItem {
    String ssid;
    int rssi;
    bool open;
};

static std::vector<ScanItem> scanItems;
static std::vector<WiFiScanItem> wifiItems;

static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if ((uint8_t)c < 0x20) {
            out += ' ';
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

static void printWebAddresses() {
    Serial.println();
    Serial.println("[WEB] Access:");
    Serial.printf("  AP  : http://%s  (%s / %s)\n",
                  WiFi.softAPIP().toString().c_str(), AP_SSID, AP_PASS);

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  LAN : http://%s  SSID=\"%s\" RSSI=%d dBm\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.SSID().c_str(),
                      WiFi.RSSI());
    } else {
        Serial.println("  LAN : not connected");
    }
    Serial.println();
}

static void scanWiFiConsole() {
    Serial.println();
    Serial.println("====================================");
    Serial.println(" WIFI SCAN");
    Serial.println("====================================");

    WiFi.mode(WIFI_AP_STA);
    WiFi.scanDelete();

    const int count = WiFi.scanNetworks(false, true);
    wifiItems.clear();

    if (count <= 0) {
        Serial.println("[WIFI] No networks found");
        return;
    }

    wifiItems.reserve(count);
    for (int i = 0; i < count; ++i) {
        WiFiScanItem item;
        item.ssid = WiFi.SSID(i);
        item.rssi = WiFi.RSSI(i);
        item.open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        wifiItems.push_back(item);

        Serial.printf("[%d] RSSI=%d  %s  SSID=\"%s\"\n",
                      i + 1,
                      item.rssi,
                      item.open ? "OPEN " : "LOCK ",
                      item.ssid.c_str());
    }

    WiFi.scanDelete();
    Serial.println();
    Serial.println("Connect by index:");
    Serial.println("  wifi <N> <password>");
    Serial.println("  wifi <N>              (open network)");
}

static bool connectWiFi(const String& ssid, const String& password, bool saveCreds) {
    if (ssid.length() == 0) {
        Serial.println("[WIFI] Empty SSID");
        return false;
    }

    Serial.printf("[WIFI] Connecting to \"%s\"", ssid.c_str());
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false, false);
    delay(150);

    if (password.length()) WiFi.begin(ssid.c_str(), password.c_str());
    else WiFi.begin(ssid.c_str());

    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
        Serial.print('.');
        server.handleClient();
        delay(250);
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] FAILED, status=%d\n", (int)WiFi.status());
        return false;
    }

    Serial.println("[WIFI] CONNECTED");
    Serial.printf("[WIFI] SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("[WIFI] IP  : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());

    if (saveCreds) {
        prefs.putString("wifi_ssid", ssid);
        prefs.putString("wifi_pass", password);
        Serial.println("[WIFI] Credentials saved");
    }

    printWebAddresses();
    return true;
}

static void connectSavedWiFi() {
    if (!prefs.isKey("wifi_ssid")) {
        Serial.println("[WIFI] No saved WiFi");
        return;
    }

    const String ssid = prefs.getString("wifi_ssid", "");
    const String pass = prefs.getString("wifi_pass", "");
    if (ssid.length()) {
        Serial.printf("[WIFI] Saved network: \"%s\"\n", ssid.c_str());
        connectWiFi(ssid, pass, false);
    }
}

static void forgetWiFi() {
    prefs.remove("wifi_ssid");
    prefs.remove("wifi_pass");
    WiFi.disconnect(false, true);
    Serial.println("[WIFI] Saved WiFi removed");
    printWebAddresses();
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
    for (size_t i = 0; i < len; ++i) Serial.printf("%02X ", data[i]);
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
    if (client && client->isConnected()) client->disconnect();
    connected = false;
    writeChr = nullptr;
    notifyChr = nullptr;
}

static void deleteClientSafe() {
    disconnectWatch();
    if (client) {
        NimBLEDevice::deleteClient(client);
        client = nullptr;
    }
}

static bool sendRaw(const uint8_t* data, size_t len) {
    if (!connected || !writeChr) {
        setStatus("TX failed: watch not connected");
        return false;
    }

    Serial.printf("[TX] %u bytes: ", (unsigned)len);
    for (size_t i = 0; i < len; ++i) Serial.printf("%02X ", data[i]);
    Serial.println();

    bool ok = false;
    if (writeChr->canWrite()) ok = writeChr->writeValue(data, len, true);
    else if (writeChr->canWriteNoResponse()) ok = writeChr->writeValue(data, len, false);

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
    if (payloadLen) memcpy(&packet[8], payload, payloadLen);

    return sendRaw(packet, totalLen);
}

static bool testFindWatch() {
    const uint8_t payload[] = {0x01};
    return sendFitPro(0x0B, payload, sizeof(payload));
}

static bool enableNotifications() {
    const uint8_t payload[] = {1,1,1,1,1,1,1,1,1,1,1,1};
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

    deleteClientSafe();
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
        setStatus("connected, but known watch service not found; use Probe");
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

static String propsToString(NimBLERemoteCharacteristic* chr) {
    String p;
    if (chr->canRead()) p += "READ ";
    if (chr->canWrite()) p += "WRITE ";
    if (chr->canWriteNoResponse()) p += "WRITE_NR ";
    if (chr->canNotify()) p += "NOTIFY ";
    if (chr->canIndicate()) p += "INDICATE ";
    if (!p.length()) p = "-";
    return p;
}

static bool probeGatt(const String& mac) {
    if (mac.length() != 17) {
        setStatus("probe: bad MAC");
        return false;
    }

    deleteClientSafe();
    gattDump = "";
    gattDevice = mac;
    gattFitProCompatible = false;

    setStatus("GATT probe connecting to " + mac);

    NimBLEAddress address(mac.c_str());
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);

    if (!client->connect(address)) {
        setStatus("GATT probe connect failed");
        deleteClientSafe();
        return false;
    }

    const uint16_t mtu = client->getMTU();
    gattDump += "Device: " + mac + "\n";
    gattDump += "MTU: " + String(mtu) + "\n\n";

    Serial.println();
    Serial.println("====================================");
    Serial.println(" GATT PROBE");
    Serial.println("====================================");
    Serial.printf("Device: %s\nMTU: %u\n", mac.c_str(), mtu);

    std::vector<NimBLERemoteService*>* services = client->getServices(true);
    if (!services) {
        gattDump += "No services discovered\n";
        setStatus("GATT probe: no services");
        deleteClientSafe();
        return false;
    }

    for (auto svc : *services) {
        const String suuid = svc->getUUID().toString().c_str();
        gattDump += "[SERVICE] " + suuid + "\n";
        Serial.printf("\n[SERVICE] %s\n", suuid.c_str());

        if (suuid.equalsIgnoreCase(WATCH_SERVICE) ||
            suuid.equalsIgnoreCase("0xfee7") ||
            suuid.equalsIgnoreCase("fee7")) {
            if (suuid.equalsIgnoreCase(WATCH_SERVICE)) gattFitProCompatible = true;
        }

        std::vector<NimBLERemoteCharacteristic*>* chars = svc->getCharacteristics(true);
        if (!chars) {
            gattDump += "  (no characteristics)\n";
            continue;
        }

        for (auto chr : *chars) {
            const String cuuid = chr->getUUID().toString().c_str();
            const String props = propsToString(chr);

            gattDump += "  [CHAR] " + cuuid + "  " + props + "\n";
            Serial.printf("  [CHAR] %s  %s\n", cuuid.c_str(), props.c_str());

            if (chr->canRead()) {
                std::string value = chr->readValue();
                if (value.length() > 0 && value.length() <= 64) {
                    String hex = "       READ: ";
                    for (size_t i = 0; i < value.length(); ++i) {
                        char b[4];
                        snprintf(b, sizeof(b), "%02X ", (uint8_t)value[i]);
                        hex += b;
                    }
                    gattDump += hex + "\n";
                    Serial.println(hex);
                }
            }
        }
    }

    gattDump += "\nProtocol: ";
    gattDump += gattFitProCompatible ? "FitPro/LJ compatible (known service found)\n" : "Unknown\n";

    Serial.println();
    Serial.printf("Protocol: %s\n", gattFitProCompatible ? "FitPro/LJ compatible" : "Unknown");
    Serial.println("====================================");

    setStatus(gattFitProCompatible ? "GATT probe complete: known FitPro/LJ protocol" : "GATT probe complete: unknown protocol");
    deleteClientSafe();
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
body{font-family:Arial,sans-serif;max-width:900px;margin:20px auto;padding:0 14px;background:#111;color:#eee}
.card{background:#1d1d1d;border:1px solid #333;border-radius:12px;padding:14px;margin:12px 0}
button,input{font-size:16px;padding:9px;margin:4px;border-radius:7px;border:1px solid #555}
button{cursor:pointer}.dev{display:flex;gap:8px;align-items:center;justify-content:space-between;border-top:1px solid #333;padding:8px 0}
.actions{white-space:nowrap}small{color:#aaa}#status,#rx,#net,#gatt{white-space:pre-wrap;overflow-wrap:anywhere}
#gatt{background:#0b0b0b;border-radius:8px;padding:10px;font-family:monospace;font-size:13px;max-height:480px;overflow:auto}
input[type=text]{width:90%;background:#111;color:#eee}.ok{color:#8f8}.bad{color:#f88}
</style>
</head>
<body>
<h2>ESP32-S3 WatchBridge</h2>
<div class="card"><b>Status</b><div id="status">loading...</div><div id="net">network...</div><small>Fallback AP: WatchBridge-S3 / 12345678 · http://192.168.4.1</small></div>
<div class="card"><b>1. BLE scanner</b><br><button onclick="scan()">Scan BLE 6s</button><div id="devices"></div></div>
<div class="card"><b>2. Bound watch</b><div id="bound">—</div><button onclick="connectWatch()">Connect</button><button onclick="unbind()">Unbind</button></div>
<div class="card"><b>3. Test commands (known FitPro/LJ watches)</b><br><button onclick="cmd('find')">Find / Vibrate</button><button onclick="cmd('enable')">Enable notifications</button><button onclick="cmd('test')">Send TEST</button></div>
<div class="card"><b>Custom notification</b><br><input id="sender" type="text" value="MeshCore" placeholder="Sender"><br><input id="msgtext" type="text" value="Hello from ESP32-S3" placeholder="Text"><br><button onclick="sendText()">Send</button></div>
<div class="card"><b>GATT Inspector</b><div><small>For unknown watches press Probe in the scanner. The watch is connected only long enough to enumerate services/characteristics.</small></div><div id="gatt">No probe yet</div></div>
<div class="card"><b>Last packet from watch</b><div id="rx"><small>none</small></div></div>
<script>
async function j(url){let r=await fetch(url);return await r.json()}
async function refresh(){try{let x=await j('/api/status');status.textContent=x.status+'\nwatch_connected='+x.connected;status.className=x.connected?'ok':'';bound.textContent=x.bound||'not bound';rx.textContent=x.last_rx||'none';gatt.textContent=x.gatt_dump||'No probe yet';net.textContent=x.wifi_connected?'WiFi: '+x.wifi_ssid+'  '+x.wifi_ip+'  '+x.wifi_rssi+' dBm':'WiFi: not connected; use serial console W / wifi <N> <password>'}catch(e){status.textContent='web/API error: '+e;status.className='bad'}}
async function scan(){status.textContent='scanning...';let x=await j('/api/scan');devices.innerHTML='';x.devices.forEach(d=>{let e=document.createElement('div');e.className='dev';let t=document.createElement('span');t.textContent=(d.name||'(no name)')+'  '+d.mac+'  '+d.rssi+' dBm';let a=document.createElement('span');a.className='actions';let p=document.createElement('button');p.textContent='Probe';p.onclick=()=>probe(d.mac);let b=document.createElement('button');b.textContent='Bind';b.onclick=()=>bind(d.mac);a.append(p,b);e.append(t,a);devices.appendChild(e)});refresh()}
async function probe(mac){status.textContent='probing '+mac+' ...';await j('/api/probe?mac='+encodeURIComponent(mac));await refresh()}
async function bind(mac){await j('/api/bind?mac='+encodeURIComponent(mac));await refresh()}
async function unbind(){await j('/api/unbind');await refresh()}
async function connectWatch(){await j('/api/connect');await refresh()}
async function cmd(c){await j('/api/cmd?name='+encodeURIComponent(c));setTimeout(refresh,200)}
async function sendText(){let s=document.getElementById('sender').value;let t=document.getElementById('msgtext').value;await j('/api/send?sender='+encodeURIComponent(s)+'&text='+encodeURIComponent(t));setTimeout(refresh,200)}
refresh();setInterval(refresh,2500)
</script>
</body>
</html>
)HTML";

static void apiStatus() {
    String s = "{";
    s += "\"connected\":" + String(connected ? "true" : "false") + ",";
    s += "\"bound\":\"" + jsonEscape(boundMac) + "\",";
    s += "\"status\":\"" + jsonEscape(lastStatus) + "\",";
    s += "\"last_rx\":\"" + jsonEscape(lastRx) + "\",";
    s += "\"gatt_device\":\"" + jsonEscape(gattDevice) + "\",";
    s += "\"gatt_fitpro\":" + String(gattFitProCompatible ? "true" : "false") + ",";
    s += "\"gatt_dump\":\"" + jsonEscape(gattDump) + "\",";
    s += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    s += "\"wifi_ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "") + "\",";
    s += "\"wifi_ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",";
    s += "\"wifi_rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
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

static void apiProbe() {
    const String mac = server.arg("mac");
    const bool ok = probeGatt(mac);
    String s = "{\"ok\":" + String(ok ? "true" : "false") + ",";
    s += "\"fitpro\":" + String(gattFitProCompatible ? "true" : "false") + ",";
    s += "\"dump\":\"" + jsonEscape(gattDump) + "\"}";
    server.send(ok ? 200 : 500, "application/json", s);
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
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void apiUnbind() {
    deleteClientSafe();
    boundMac = "";
    prefs.remove("watch_mac");
    setStatus("watch unbound");
    server.send(200, "application/json", "{\"ok\":true}");
}

static void apiConnect() {
    const bool ok = connectWatch(boundMac);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void apiCmd() {
    const String name = server.arg("name");
    bool ok = false;
    if (name == "find") ok = testFindWatch();
    else if (name == "enable") ok = enableNotifications();
    else if (name == "test") ok = sendNotification("MeshCore", "TEST FROM ESP32-S3");
    else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown command\"}");
        return;
    }
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void apiSend() {
    const String sender = server.arg("sender").length() ? server.arg("sender") : "MeshCore";
    const String text = server.arg("text");
    const bool ok = sendNotification(sender, text);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void startWeb() {
    WiFi.mode(WIFI_AP_STA);
    const bool apOk = WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WEB] softAP: %s\n", apOk ? "OK" : "FAIL");
    Serial.printf("[WEB] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    server.on("/", HTTP_GET, [](){ server.send_P(200, "text/html", INDEX_HTML); });
    server.on("/favicon.ico", HTTP_GET, [](){ server.send(204); });
    server.on("/api/status", HTTP_GET, apiStatus);
    server.on("/api/scan", HTTP_GET, apiScan);
    server.on("/api/probe", HTTP_GET, apiProbe);
    server.on("/api/bind", HTTP_GET, apiBind);
    server.on("/api/unbind", HTTP_GET, apiUnbind);
    server.on("/api/connect", HTTP_GET, apiConnect);
    server.on("/api/cmd", HTTP_GET, apiCmd);
    server.on("/api/send", HTTP_GET, apiSend);
    server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
    server.begin();
    Serial.println("[WEB] server started");
}

static void printBleScanToSerial() {
    for (const auto& x : scanItems) {
        Serial.printf("[SCAN] %s RSSI=%d NAME=\"%s\"\n", x.mac.c_str(), x.rssi, x.name.c_str());
    }
}

static void printCommands() {
    Serial.println();
    Serial.println("Commands:");
    Serial.println("  W = WiFi scan");
    Serial.println("      then: wifi <N> <password>");
    Serial.println("  wifi status / wifi forget");
    Serial.println("  S = BLE scan 6s");
    Serial.println("  probe <MAC> = universal GATT inspector");
    Serial.println("  C = Connect bound known watch");
    Serial.println("  F = Find/vibrate bound watch");
    Serial.println("  E = Enable notifications");
    Serial.println("  T = Send TEST notification");
    Serial.println("  help = full help");
}

static void handleConsoleLine(String line) {
    line.trim();
    if (!line.length()) return;

    if (line.equalsIgnoreCase("help") || line == "?") {
        printCommands();
        return;
    }
    if (line.equalsIgnoreCase("wifi scan")) { scanWiFiConsole(); return; }
    if (line.equalsIgnoreCase("wifi status")) { printWebAddresses(); return; }
    if (line.equalsIgnoreCase("wifi forget")) { forgetWiFi(); return; }

    if (line.startsWith("wifi ")) {
        String args = line.substring(5);
        args.trim();
        const int space = args.indexOf(' ');
        String indexText = space < 0 ? args : args.substring(0, space);
        String password = space < 0 ? "" : args.substring(space + 1);
        password.trim();

        const int index = indexText.toInt();
        if (index < 1 || index > (int)wifiItems.size()) {
            Serial.println("[WIFI] Bad index. Run 'W' or 'wifi scan' first.");
            return;
        }

        const WiFiScanItem& item = wifiItems[index - 1];
        if (!item.open && password.length() == 0) {
            Serial.printf("[WIFI] Password required. Use: wifi %d <password>\n", index);
            return;
        }
        connectWiFi(item.ssid, password, true);
        return;
    }

    if (line.startsWith("probe ")) {
        String mac = line.substring(6);
        mac.trim();
        probeGatt(mac);
        return;
    }

    Serial.println("[CONSOLE] Unknown command. Type: help");
}

void setup() {
    Serial.begin(115200);
    delay(1600);

    Serial.println();
    Serial.println("====================================");
    Serial.println(" ESP32-S3 WATCHBRIDGE");
    Serial.println("====================================");

    prefs.begin("watchbridge", false);
    boundMac = prefs.isKey("watch_mac") ? prefs.getString("watch_mac", "") : "";

    NimBLEDevice::init("ESP32-S3-WatchBridge");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    startWeb();
    connectSavedWiFi();
    printWebAddresses();
    printCommands();

    if (boundMac.length()) {
        Serial.println("[BLE] saved watch: " + boundMac);
        connectWatch(boundMac);
    } else {
        setStatus("ready - no watch bound");
    }
}

void loop() {
    server.handleClient();

    if (WiFi.status() != WL_CONNECTED && prefs.isKey("wifi_ssid")) {
        static uint32_t lastRetry = 0;
        if (millis() - lastRetry > 30000) {
            lastRetry = millis();
            const String ssid = prefs.getString("wifi_ssid", "");
            const String pass = prefs.getString("wifi_pass", "");
            if (ssid.length()) {
                Serial.println("[WIFI] reconnecting saved network...");
                connectWiFi(ssid, pass, false);
            }
        }
    }

    while (Serial.available()) {
        const char c = Serial.read();

        if (serialLine.length() == 0) {
            if (c == 'W') { scanWiFiConsole(); continue; }
            if (c == 'F') { testFindWatch(); continue; }
            if (c == 'E') { enableNotifications(); continue; }
            if (c == 'T') { sendNotification("MeshCore", "TEST FROM ESP32-S3"); continue; }
            if (c == 'S') { runScan(); printBleScanToSerial(); continue; }
            if (c == 'C') { connectWatch(boundMac); continue; }
        }

        if (c == '\r' || c == '\n') {
            if (serialLine.length()) {
                handleConsoleLine(serialLine);
                serialLine = "";
            }
        } else if (c == 8 || c == 127) {
            if (serialLine.length()) serialLine.remove(serialLine.length() - 1);
        } else if (c >= 32 && c <= 126) {
            if (serialLine.length() < 180) serialLine += c;
        }
    }

    delay(5);
}
