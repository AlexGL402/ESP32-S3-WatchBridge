#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <vector>
#include <time.h>

static const char* AP_SSID = "WatchBridge-S3";
static const char* AP_PASS = "12345678";

// FitPro / LJ737
static const char* FITPRO_SERVICE = "6e400f01-b5a3-f393-e0a9-e50e24dcca9d";
static const char* FITPRO_WRITE   = "6e400002-b5a3-f393-e0a9-e50e24dcca9d";
static const char* FITPRO_NOTIFY  = "6e400003-b5a3-f393-e0a9-e50e24dcca9d";

// E91A / WearWatch family
static const char* E91A_SERVICE = "0000e91a-0000-1000-8000-00805f9b34fb";
static const char* E91A_NOTIFY  = "0000b001-0000-1000-8000-00805f9b34fb";
static const char* E91A_WRITE   = "0000b002-0000-1000-8000-00805f9b34fb";
static const char* E91A_CONFIG  = "0000b003-0000-1000-8000-00805f9b34fb";

// Xiaomi / Huami family
static const char* MIBAND_AUTH = "00000009-0000-3512-2118-0009af100700";
static const char* MIBAND_FEC1 = "0000fec1-0000-3512-2118-0009af100700";

static WebServer server(80);
static Preferences prefs;

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* writeChr = nullptr;
static NimBLERemoteCharacteristic* notifyChr = nullptr;
static NimBLERemoteCharacteristic* configChr = nullptr;

static bool connected = false;
static String boundMac;
static String lastRx;
static String lastStatus = "booting";
static String serialLine;
static String gattDump;
static String gattDevice;
static uint32_t txCount = 0;
static uint32_t rxCount = 0;
static uint8_t e91aPid = 0;
static bool e91aInitialized = false;

enum WatchProfile {
    PROFILE_NONE,
    PROFILE_FITPRO,
    PROFILE_E91A,
    PROFILE_MIBAND,
    PROFILE_GENERIC
};
static WatchProfile profile = PROFILE_NONE;

struct ScanItem { String mac; String name; int rssi; };
struct WiFiScanItem { String ssid; int rssi; bool open; };
static std::vector<ScanItem> scanItems;
static std::vector<WiFiScanItem> wifiItems;

static const char* profileName() {
    switch (profile) {
        case PROFILE_FITPRO: return "FitPro/LJ";
        case PROFILE_E91A: return "E91A/WearWatch";
        case PROFILE_MIBAND: return "Xiaomi/Huami Mi Band";
        case PROFILE_GENERIC: return "Generic GATT";
        default: return "None";
    }
}

static String jsonEscape(const String& in) {
    String out; out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if ((uint8_t)c < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

static void setStatus(const String& s) {
    lastStatus = s;
    Serial.println("[STATUS] " + s);
}

static String hexOf(const uint8_t* data, size_t len) {
    String s; s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        char b[4]; snprintf(b, sizeof(b), "%02X ", data[i]); s += b;
    }
    return s;
}

static bool parseHex(String text, std::vector<uint8_t>& out) {
    out.clear(); text.replace("0x", ""); text.replace("0X", "");
    String clean;
    for (size_t i = 0; i < text.length(); ++i) if (isxdigit((unsigned char)text[i])) clean += text[i];
    if (!clean.length() || (clean.length() & 1)) return false;
    for (size_t i = 0; i < clean.length(); i += 2) out.push_back((uint8_t)strtoul(clean.substring(i, i + 2).c_str(), nullptr, 16));
    return true;
}

static void printWebAddresses() {
    Serial.println("\n[WEB] Access:");
    Serial.printf("  AP  : http://%s  (%s / %s)\n", WiFi.softAPIP().toString().c_str(), AP_SSID, AP_PASS);
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("  LAN : http://%s  SSID=\"%s\" RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), WiFi.RSSI());
    else Serial.println("  LAN : not connected");
    Serial.println();
}

static void scanWiFiConsole() {
    Serial.println("\n====================================\n WIFI SCAN\n====================================");
    WiFi.mode(WIFI_AP_STA); WiFi.scanDelete();
    int count = WiFi.scanNetworks(false, true); wifiItems.clear();
    if (count <= 0) { Serial.println("[WIFI] No networks found"); return; }
    for (int i = 0; i < count; ++i) {
        WiFiScanItem item{WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN};
        wifiItems.push_back(item);
        Serial.printf("[%d] RSSI=%d  %s  SSID=\"%s\"\n", i + 1, item.rssi, item.open ? "OPEN " : "LOCK ", item.ssid.c_str());
    }
    WiFi.scanDelete();
    Serial.println("\nConnect: wifi <N> <password>");
}

static bool connectWiFi(const String& ssid, const String& password, bool saveCreds) {
    if (!ssid.length()) return false;
    Serial.printf("[WIFI] Connecting to \"%s\"", ssid.c_str());
    WiFi.mode(WIFI_AP_STA); WiFi.disconnect(false, false); delay(150);
    if (password.length()) WiFi.begin(ssid.c_str(), password.c_str()); else WiFi.begin(ssid.c_str());
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) { Serial.print('.'); server.handleClient(); delay(250); }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) { Serial.printf("[WIFI] FAILED status=%d\n", (int)WiFi.status()); return false; }
    Serial.printf("[WIFI] CONNECTED\n[WIFI] SSID: %s\n[WIFI] IP  : %s\n[WIFI] RSSI: %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    if (saveCreds) { prefs.putString("wifi_ssid", ssid); prefs.putString("wifi_pass", password); Serial.println("[WIFI] Credentials saved"); }
    printWebAddresses(); return true;
}

static void connectSavedWiFi() {
    if (!prefs.isKey("wifi_ssid")) { Serial.println("[WIFI] No saved WiFi"); return; }
    String ssid = prefs.getString("wifi_ssid", ""), pass = prefs.getString("wifi_pass", "");
    if (ssid.length()) connectWiFi(ssid, pass, false);
}

static void forgetWiFi() {
    prefs.remove("wifi_ssid"); prefs.remove("wifi_pass"); WiFi.disconnect(false, true); Serial.println("[WIFI] Saved WiFi removed");
}

static void notifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    ++rxCount;
    lastRx = String(chr->getUUID().toString().c_str()) + " " + hexOf(data, len);
    Serial.printf("\n[WATCH RX] %s len=%u : %s\n", chr->getUUID().toString().c_str(), (unsigned)len, hexOf(data, len).c_str());
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override { connected = true; setStatus("watch connected"); }
    void onDisconnect(NimBLEClient*) override {
        connected = false; writeChr = nullptr; notifyChr = nullptr; configChr = nullptr; e91aInitialized = false; setStatus("watch disconnected");
    }
};

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        ScanItem item{dev->getAddress().toString().c_str(), dev->haveName() ? dev->getName().c_str() : "", dev->getRSSI()};
        for (auto& x : scanItems) {
            if (x.mac.equalsIgnoreCase(item.mac)) { if (item.rssi > x.rssi) x.rssi = item.rssi; if (!x.name.length() && item.name.length()) x.name = item.name; return; }
        }
        scanItems.push_back(item);
    }
};
static ClientCallbacks clientCallbacks;
static ScanCallbacks scanCallbacks;

static void disconnectWatch() {
    if (client && client->isConnected()) client->disconnect();
    connected = false; writeChr = nullptr; notifyChr = nullptr; configChr = nullptr; profile = PROFILE_NONE; e91aInitialized = false;
}

static void deleteClientSafe() {
    disconnectWatch();
    if (client) { NimBLEDevice::deleteClient(client); client = nullptr; }
}

static NimBLERemoteCharacteristic* findCharacteristic(const char* uuid) {
    if (!client || !client->isConnected()) return nullptr;
    auto services = client->getServices(true); if (!services) return nullptr;
    for (auto svc : *services) {
        auto chars = svc->getCharacteristics(true); if (!chars) continue;
        for (auto chr : *chars) if (String(chr->getUUID().toString().c_str()).equalsIgnoreCase(uuid)) return chr;
    }
    return nullptr;
}

static bool subscribeChar(NimBLERemoteCharacteristic* chr) {
    if (!chr) return false;
    if (chr->canNotify()) return chr->subscribe(true, notifyCallback);
    if (chr->canIndicate()) return chr->subscribe(false, notifyCallback);
    return false;
}

static WatchProfile detectProfile() {
    if (!client || !client->isConnected()) return PROFILE_NONE;
    if (client->getService(FITPRO_SERVICE)) return PROFILE_FITPRO;
    if (client->getService(E91A_SERVICE)) return PROFILE_E91A;
    if (findCharacteristic(MIBAND_AUTH) || findCharacteristic(MIBAND_FEC1)) return PROFILE_MIBAND;
    return PROFILE_GENERIC;
}

static bool sendRawToChr(NimBLERemoteCharacteristic* chr, const uint8_t* data, size_t len) {
    if (!connected || !chr) { setStatus("RAW TX failed: no writable characteristic"); return false; }
    bool ok = false;
    if (chr->canWrite()) ok = chr->writeValue(data, len, true);
    else if (chr->canWriteNoResponse()) ok = chr->writeValue(data, len, false);
    ++txCount;
    Serial.printf("[RAW TX] %s len=%u : %s\n", chr->getUUID().toString().c_str(), (unsigned)len, hexOf(data, len).c_str());
    setStatus(ok ? "RAW TX OK" : "RAW TX failed"); return ok;
}

static bool sendE91AChunk(const uint8_t* data, size_t len) {
    if (profile != PROFILE_E91A || !connected || !writeChr) return false;
    bool ok = writeChr->writeValue(data, len, false);
    ++txCount;
    Serial.printf("[E91A TX] len=%u : %s\n", (unsigned)len, hexOf(data, len).c_str());
    delay(35);
    return ok;
}

static bool e91aBind() {
    if (profile != PROFILE_E91A || !connected || !writeChr) { setStatus("E91A init unavailable"); return false; }
    // Verified WearWatch/WTWD binding handshake, already split into 4 x 20-byte frames.
    static const uint8_t bindData[] = {
        0x00,0x00,0x03,0x00,0x01,0x6E,0x00,0x00,0x38,0x00,0x36,0x00,0x08,0x0C,0x00,0x66,0x90,0xFC,0x5F,0xB4,
        0x01,0x00,0x21,0xAA,0x3C,0x00,0x04,0x00,0x67,0x00,0x0C,0x00,0x68,0x48,0xFF,0x53,0x6A,0x20,0x1C,0x00,
        0x02,0x00,0x00,0x04,0x00,0x6D,0x01,0x04,0x00,0x7A,0x00,0x04,0x00,0x7B,0x00,0x08,0x00,0x7C,0x00,0x00,
        0x03,0x00,0x00,0x00,0x05,0x00,0x78,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    };
    bool ok = true;
    for (size_t off = 0; off < sizeof(bindData); off += 20) if (!sendE91AChunk(bindData + off, 20)) ok = false;
    e91aInitialized = ok;
    setStatus(ok ? "E91A init/bind sent" : "E91A init/bind failed");
    return ok;
}

static bool sendE91APacket(uint8_t cmdType, uint8_t sendType, uint8_t opcode, const uint8_t* payload, size_t payloadLen) {
    if (profile != PROFILE_E91A || !connected || !writeChr) { setStatus("E91A not connected"); return false; }
    uint8_t first[20] = {0};
    size_t remaining = payloadLen > 10 ? payloadLen - 10 : 0;
    uint8_t extra = remaining ? (uint8_t)((remaining + 18) / 19) : 0;
    first[0] = 0x00; first[1] = e91aPid++; first[2] = extra; first[3] = cmdType; first[4] = sendType; first[5] = opcode;
    first[8] = payloadLen & 0xFF; first[9] = (payloadLen >> 8) & 0xFF;
    size_t n0 = payloadLen < 10 ? payloadLen : 10;
    if (n0) memcpy(first + 10, payload, n0);
    bool ok = sendE91AChunk(first, sizeof(first));
    size_t pos = 10;
    for (uint8_t frag = 1; frag <= extra; ++frag) {
        uint8_t cont[20] = {0}; cont[0] = frag;
        size_t n = payloadLen - pos; if (n > 19) n = 19;
        if (n) memcpy(cont + 1, payload + pos, n);
        if (!sendE91AChunk(cont, sizeof(cont))) ok = false;
        pos += n;
    }
    return ok;
}

static uint32_t currentEpoch() {
    time_t now = time(nullptr);
    if (now < 1600000000) return 0;
    return (uint32_t)now;
}

static bool sendE91ANotification(uint8_t appId, const String& text) {
    if (!e91aInitialized) e91aBind();
    String t = text; if (t.length() > 60) t.remove(60);
    size_t textLen = t.length();
    std::vector<uint8_t> p(8 + textLen, 0);
    uint32_t ts = currentEpoch();
    p[0] = ts & 0xFF; p[1] = (ts >> 8) & 0xFF; p[2] = (ts >> 16) & 0xFF; p[3] = (ts >> 24) & 0xFF;
    p[4] = 0x01; p[5] = 0x01; p[6] = appId; p[7] = (uint8_t)textLen;
    if (textLen) memcpy(&p[8], t.c_str(), textLen);
    bool ok = sendE91APacket(0x00, 0x01, 0x6B, p.data(), p.size());
    setStatus(ok ? "E91A notification sent" : "E91A notification failed");
    return ok;
}

static bool setupProfile() {
    profile = detectProfile(); writeChr = nullptr; notifyChr = nullptr; configChr = nullptr; e91aInitialized = false;
    if (profile == PROFILE_FITPRO) {
        auto svc = client->getService(FITPRO_SERVICE); if (!svc) return false;
        writeChr = svc->getCharacteristic(FITPRO_WRITE); notifyChr = svc->getCharacteristic(FITPRO_NOTIFY); if (notifyChr) subscribeChar(notifyChr);
        return writeChr != nullptr;
    }
    if (profile == PROFILE_E91A) {
        auto svc = client->getService(E91A_SERVICE); if (!svc) return false;
        notifyChr = svc->getCharacteristic(E91A_NOTIFY); writeChr = svc->getCharacteristic(E91A_WRITE); configChr = svc->getCharacteristic(E91A_CONFIG);
        if (notifyChr) Serial.printf("[BLE] E91A notify subscribe: %s\n", subscribeChar(notifyChr) ? "OK" : "FAIL");
        return writeChr != nullptr;
    }
    if (profile == PROFILE_MIBAND) {
        auto auth = findCharacteristic(MIBAND_AUTH); auto fec1 = findCharacteristic(MIBAND_FEC1);
        if (auth) { configChr = auth; subscribeChar(auth); }
        if (fec1) { if (!configChr) configChr = fec1; subscribeChar(fec1); if (fec1->canWrite() || fec1->canWriteNoResponse()) writeChr = fec1; }
        return true;
    }
    auto services = client->getServices(true);
    if (services) for (auto svc : *services) {
        auto chars = svc->getCharacteristics(true); if (!chars) continue;
        for (auto chr : *chars) {
            if (!writeChr && (chr->canWrite() || chr->canWriteNoResponse())) writeChr = chr;
            if (!notifyChr && (chr->canNotify() || chr->canIndicate())) { notifyChr = chr; subscribeChar(chr); }
        }
    }
    return true;
}

static bool connectWatch(const String& mac) {
    if (mac.length() != 17) { setStatus("bad watch MAC"); return false; }
    deleteClientSafe(); client = NimBLEDevice::createClient(); client->setClientCallbacks(&clientCallbacks, false);
    setStatus("connecting to " + mac);
    if (!client->connect(NimBLEAddress(mac.c_str()))) { setStatus("connect failed"); return false; }
    Serial.printf("[BLE] MTU: %u\n", client->getMTU());
    if (!setupProfile()) { setStatus("connected but profile setup failed"); return false; }
    connected = true;
    setStatus(String("connected: ") + profileName() + " @ " + mac); Serial.printf("[BLE] profile: %s\n", profileName());
    if (profile == PROFILE_E91A) { delay(250); e91aBind(); }
    return true;
}

static bool sendRaw(const uint8_t* data, size_t len) { return sendRawToChr(writeChr, data, len); }

static bool sendFitPro(uint8_t command, const uint8_t* payload, size_t payloadLen) {
    if (profile != PROFILE_FITPRO) { setStatus("FitPro command unavailable for this profile"); return false; }
    size_t totalLen = 8 + payloadLen; if (totalLen > 250) return false;
    uint8_t packet[250]; uint16_t protocolLen = totalLen - 3;
    packet[0]=0xCD; packet[1]=(protocolLen>>8)&0xFF; packet[2]=protocolLen&0xFF; packet[3]=0x12; packet[4]=0x01; packet[5]=command; packet[6]=(payloadLen>>8)&0xFF; packet[7]=payloadLen&0xFF;
    if (payloadLen) memcpy(&packet[8], payload, payloadLen);
    return sendRaw(packet, totalLen);
}

static bool testFindWatch() {
    if (profile == PROFILE_E91A) { const uint8_t p[] = {0x01}; bool ok = sendE91APacket(0x00, 0x01, 0x0B, p, sizeof(p)); setStatus(ok ? "E91A find sent" : "E91A find failed"); return ok; }
    const uint8_t p[] = {0x01}; return sendFitPro(0x0B, p, sizeof(p));
}

static bool enableNotifications() {
    if (profile == PROFILE_E91A) return e91aBind();
    const uint8_t p[] = {1,1,1,1,1,1,1,1,1,1,1,1}; return sendFitPro(0x07, p, sizeof(p));
}

static bool sendNotification(const String& sender, const String& text) {
    String msg = sender + ":" + text;
    if (profile == PROFILE_E91A) return sendE91ANotification(0, msg);
    if (profile != PROFILE_FITPRO) { setStatus("Text push unsupported for this profile"); return false; }
    if (msg.length() + 3 > 220) return false;
    uint8_t p[220]; p[0]=0x01; p[1]=0; p[2]=0; memcpy(&p[3], msg.c_str(), msg.length());
    return sendFitPro(0x12, p, msg.length() + 3);
}

static String propsToString(NimBLERemoteCharacteristic* chr) {
    String p; if (chr->canRead()) p += "READ "; if (chr->canWrite()) p += "WRITE "; if (chr->canWriteNoResponse()) p += "WRITE_NR "; if (chr->canNotify()) p += "NOTIFY "; if (chr->canIndicate()) p += "INDICATE "; if (!p.length()) p = "-"; return p;
}

static bool probeGatt(const String& mac) {
    if (mac.length() != 17) return false;
    deleteClientSafe(); gattDump = ""; gattDevice = mac; setStatus("GATT probe connecting to " + mac);
    client = NimBLEDevice::createClient(); client->setClientCallbacks(&clientCallbacks, false);
    if (!client->connect(NimBLEAddress(mac.c_str()))) { setStatus("GATT probe connect failed"); deleteClientSafe(); return false; }
    uint16_t mtu = client->getMTU(); gattDump += "Device: " + mac + "\nMTU: " + String(mtu) + "\n\n";
    Serial.println("\n====================================\n GATT PROBE\n===================================="); Serial.printf("Device: %s\nMTU: %u\n", mac.c_str(), mtu);
    auto services = client->getServices(true);
    if (services) for (auto svc : *services) {
        String suuid = svc->getUUID().toString().c_str(); gattDump += "[SERVICE] " + suuid + "\n"; Serial.printf("\n[SERVICE] %s\n", suuid.c_str());
        auto chars = svc->getCharacteristics(true); if (!chars) continue;
        for (auto chr : *chars) {
            String cuuid = chr->getUUID().toString().c_str(), props = propsToString(chr); gattDump += "  [CHAR] " + cuuid + "  " + props + "\n"; Serial.printf("  [CHAR] %s  %s\n", cuuid.c_str(), props.c_str());
            if (chr->canRead()) { std::string v = chr->readValue(); if (v.length() && v.length() <= 64) { String h="       READ: "; for (size_t i=0;i<v.length();++i){char b[4];snprintf(b,sizeof(b),"%02X ",(uint8_t)v[i]);h+=b;} gattDump += h + "\n"; Serial.println(h); } }
        }
    }
    WatchProfile d = detectProfile();
    const char* dn = d==PROFILE_FITPRO?"FitPro/LJ":d==PROFILE_E91A?"E91A/WearWatch":d==PROFILE_MIBAND?"Xiaomi/Huami Mi Band":"Unknown / Generic";
    gattDump += "\nProtocol: " + String(dn) + "\n"; Serial.printf("\nProtocol: %s\n====================================\n", dn);
    deleteClientSafe(); setStatus("GATT probe complete"); return true;
}

static void runScan() {
    setStatus("BLE scan running..."); disconnectWatch(); scanItems.clear();
    NimBLEScan* scan = NimBLEDevice::getScan(); scan->clearResults(); scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true); scan->setActiveScan(true); scan->setInterval(45); scan->setWindow(30); scan->start(6, false);
    setStatus("scan complete: " + String(scanItems.size()) + " devices");
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-S3 WatchBridge</title><style>
body{font-family:Arial,sans-serif;max-width:950px;margin:20px auto;padding:0 14px;background:#111;color:#eee}.card{background:#1d1d1d;border:1px solid #333;border-radius:12px;padding:14px;margin:12px 0}button,input{font-size:16px;padding:9px;margin:4px;border-radius:7px;border:1px solid #555;background:#111;color:#eee}button{cursor:pointer}.dev{display:flex;gap:8px;align-items:center;justify-content:space-between;border-top:1px solid #333;padding:8px 0}.actions{white-space:nowrap}small{color:#aaa}#status,#rx,#net,#gatt{white-space:pre-wrap;overflow-wrap:anywhere}#gatt{background:#0b0b0b;border-radius:8px;padding:10px;font-family:monospace;font-size:13px;max-height:460px;overflow:auto}input[type=text]{width:90%}
</style></head><body><h2>ESP32-S3 WatchBridge</h2>
<div class="card"><b>Status</b><div id="status">loading...</div><div id="profile">profile...</div><div id="net">network...</div></div>
<div class="card"><b>1. BLE scanner</b><br><button onclick="scan()">Scan BLE 6s</button><div id="devices"></div></div>
<div class="card"><b>2. Bound watch</b><div id="bound">—</div><button onclick="connectWatch()">Connect</button><button onclick="unbind()">Unbind</button></div>
<div class="card"><b>3. Watch commands</b><br><button onclick="cmd('init')">E91A Init / Bind</button><button onclick="cmd('find')">Find / Vibrate</button><button onclick="cmd('enable')">Enable / Init</button><button onclick="cmd('test')">Send TEST</button></div>
<div class="card"><b>Custom notification</b><br><input id="sender" type="text" value="MeshCore"><br><input id="msgtext" type="text" value="Hello from ESP32-S3"><br><button onclick="sendText()">Send notification</button></div>
<div class="card"><b>Raw GATT console</b><br><input id="rawhex" type="text" value="01"><br><button onclick="rawSend('write')">RAW -> WRITE</button><button onclick="rawSend('config')">RAW -> CONFIG</button><button onclick="readConfig()">Read CONFIG</button></div>
<div class="card"><b>GATT Inspector</b><div id="gatt">No probe yet</div></div><div class="card"><b>Last packet from watch</b><div id="rx">none</div></div>
<script>
async function j(u){let r=await fetch(u);return await r.json()}async function refresh(){try{let x=await j('/api/status');status.textContent=x.status+'\nconnected='+x.connected+'  TX='+x.tx_count+' RX='+x.rx_count+'  E91A_init='+x.e91a_init;profile.textContent='Profile: '+x.profile;bound.textContent=x.bound||'not bound';rx.textContent=x.last_rx||'none';gatt.textContent=x.gatt_dump||'No probe yet';net.textContent=x.wifi_connected?'WiFi: '+x.wifi_ssid+' '+x.wifi_ip+' '+x.wifi_rssi+' dBm':'WiFi: not connected'}catch(e){status.textContent='API error: '+e}}
async function scan(){let x=await j('/api/scan');devices.innerHTML='';x.devices.forEach(d=>{let e=document.createElement('div');e.className='dev';let t=document.createElement('span');t.textContent=(d.name||'(no name)')+' '+d.mac+' '+d.rssi+' dBm';let a=document.createElement('span');a.className='actions';let p=document.createElement('button');p.textContent='Probe';p.onclick=()=>probe(d.mac);let b=document.createElement('button');b.textContent='Bind';b.onclick=()=>bind(d.mac);a.append(p,b);e.append(t,a);devices.appendChild(e)});refresh()}async function probe(m){await j('/api/probe?mac='+encodeURIComponent(m));refresh()}async function bind(m){await j('/api/bind?mac='+encodeURIComponent(m));refresh()}async function unbind(){await j('/api/unbind');refresh()}async function connectWatch(){await j('/api/connect');refresh()}async function cmd(c){await j('/api/cmd?name='+c);setTimeout(refresh,250)}async function sendText(){await j('/api/send?sender='+encodeURIComponent(sender.value)+'&text='+encodeURIComponent(msgtext.value));setTimeout(refresh,250)}async function rawSend(t){await j('/api/raw?target='+t+'&hex='+encodeURIComponent(rawhex.value));setTimeout(refresh,250)}async function readConfig(){await j('/api/readconfig');setTimeout(refresh,250)}refresh();setInterval(refresh,2000)
</script></body></html>)HTML";

static void apiStatus() {
    String s="{";
    s += "\"connected\":" + String(connected?"true":"false") + ",\"profile\":\"" + String(profileName()) + "\",\"bound\":\"" + jsonEscape(boundMac) + "\",\"status\":\"" + jsonEscape(lastStatus) + "\",\"last_rx\":\"" + jsonEscape(lastRx) + "\",\"gatt_dump\":\"" + jsonEscape(gattDump) + "\",";
    s += "\"tx_count\":" + String(txCount) + ",\"rx_count\":" + String(rxCount) + ",\"e91a_init\":" + String(e91aInitialized?"true":"false") + ",";
    s += "\"wifi_connected\":" + String(WiFi.status()==WL_CONNECTED?"true":"false") + ",\"wifi_ssid\":\"" + jsonEscape(WiFi.status()==WL_CONNECTED?WiFi.SSID():"") + "\",\"wifi_ip\":\"" + String(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"") + "\",\"wifi_rssi\":" + String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0) + "}";
    server.send(200,"application/json",s);
}

static void apiScan(){runScan();String s="{\"devices\":[";for(size_t i=0;i<scanItems.size();++i){if(i)s+=',';s+="{\"mac\":\""+jsonEscape(scanItems[i].mac)+"\",\"name\":\""+jsonEscape(scanItems[i].name)+"\",\"rssi\":"+String(scanItems[i].rssi)+"}";}s+="]}";server.send(200,"application/json",s);}
static void apiProbe(){bool ok=probeGatt(server.arg("mac"));server.send(200,"application/json",String("{\"ok\":")+(ok?"true":"false")+"}");}
static void apiBind(){String mac=server.arg("mac");if(mac.length()!=17){server.send(400,"application/json","{\"ok\":false}");return;}boundMac=mac;prefs.putString("watch_mac",boundMac);bool ok=connectWatch(boundMac);server.send(200,"application/json",String("{\"ok\":")+(ok?"true":"false")+"}");}
static void apiUnbind(){deleteClientSafe();boundMac="";prefs.remove("watch_mac");setStatus("watch unbound");server.send(200,"application/json","{\"ok\":true}");}
static void apiConnect(){bool ok=connectWatch(boundMac);server.send(200,"application/json",String("{\"ok\":")+(ok?"true":"false")+"}");}
static void apiCmd(){String n=server.arg("name");bool ok=false;if(n=="init")ok=e91aBind();else if(n=="find")ok=testFindWatch();else if(n=="enable")ok=enableNotifications();else if(n=="test")ok=sendNotification("MeshCore","TEST FROM ESP32-S3");server.send(200,"application/json",String("{\"ok\":")+(ok?"true":"false")+"}");}
static void apiSend(){String sender=server.arg("sender").length()?server.arg("sender"):"MeshCore";bool ok=sendNotification(sender,server.arg("text"));server.send(200,"application/json",String("{\"ok\":")+(ok?"true":"false")+"}");}
static void apiRaw(){std::vector<uint8_t>d;if(!parseHex(server.arg("hex"),d)){server.send(400,"application/json","{\"ok\":false,\"error\":\"bad hex\"}");return;}auto target=server.arg("target")=="config"?configChr:writeChr;bool ok=sendRawToChr(target,d.data(),d.size());server.send(200,"application/json",String("{\"ok\":")+(ok?"true":"false")+"}");}
static void apiReadConfig(){if(!connected||!configChr||!configChr->canRead()){server.send(200,"application/json","{\"ok\":false}");return;}std::string v=configChr->readValue();lastRx=String(configChr->getUUID().toString().c_str())+" READ "+hexOf((const uint8_t*)v.data(),v.size());server.send(200,"application/json","{\"ok\":true}");}

static void startWeb(){WiFi.mode(WIFI_AP_STA);WiFi.softAP(AP_SSID,AP_PASS);Serial.printf("[WEB] AP IP: %s\n",WiFi.softAPIP().toString().c_str());server.on("/",HTTP_GET,[](){server.send_P(200,"text/html",INDEX_HTML);});server.on("/favicon.ico",HTTP_GET,[](){server.send(204,"text/plain","");});server.on("/api/status",HTTP_GET,apiStatus);server.on("/api/scan",HTTP_GET,apiScan);server.on("/api/probe",HTTP_GET,apiProbe);server.on("/api/bind",HTTP_GET,apiBind);server.on("/api/unbind",HTTP_GET,apiUnbind);server.on("/api/connect",HTTP_GET,apiConnect);server.on("/api/cmd",HTTP_GET,apiCmd);server.on("/api/send",HTTP_GET,apiSend);server.on("/api/raw",HTTP_GET,apiRaw);server.on("/api/readconfig",HTTP_GET,apiReadConfig);server.onNotFound([](){server.send(404,"text/plain","Not found");});server.begin();Serial.println("[WEB] server started");}
static void printBleScanToSerial(){for(const auto&x:scanItems)Serial.printf("[SCAN] %s RSSI=%d NAME=\"%s\"\n",x.mac.c_str(),x.rssi,x.name.c_str());}

static void printCommands(){Serial.println("\nCommands:\n  W = WiFi scan\n  S = BLE scan\n  C = connect bound watch\n  I = E91A init/bind\n  T = send TEST\n  F = find/vibrate\n  probe <MAC>\n  raw <hex>\n  notify <text>\n  wifi <N> <password> / wifi status / wifi forget");}

static void handleConsoleLine(String line){line.trim();if(!line.length())return;if(line.equalsIgnoreCase("help")||line=="?"){printCommands();return;}if(line.equalsIgnoreCase("wifi scan")){scanWiFiConsole();return;}if(line.equalsIgnoreCase("wifi status")){printWebAddresses();return;}if(line.equalsIgnoreCase("wifi forget")){forgetWiFi();return;}if(line.startsWith("wifi ")){String a=line.substring(5);a.trim();int sp=a.indexOf(' ');int idx=(sp<0?a:a.substring(0,sp)).toInt();String pass=sp<0?"":a.substring(sp+1);pass.trim();if(idx<1||idx>(int)wifiItems.size()){Serial.println("[WIFI] bad index");return;}connectWiFi(wifiItems[idx-1].ssid,pass,true);return;}if(line.startsWith("probe ")){String mac=line.substring(6);mac.trim();probeGatt(mac);return;}if(line.startsWith("raw ")){std::vector<uint8_t>d;if(parseHex(line.substring(4),d))sendRaw(d.data(),d.size());else Serial.println("[RAW] bad hex");return;}if(line.startsWith("notify ")){sendNotification("MeshCore",line.substring(7));return;}Serial.println("[CONSOLE] Unknown command. Type help");}

void setup(){Serial.begin(115200);delay(1600);Serial.println("\n====================================\n ESP32-S3 WATCHBRIDGE MULTI-PROFILE\n====================================");prefs.begin("watchbridge",false);boundMac=prefs.isKey("watch_mac")?prefs.getString("watch_mac",""):"";NimBLEDevice::init("ESP32-S3-WatchBridge");NimBLEDevice::setPower(ESP_PWR_LVL_P9);startWeb();connectSavedWiFi();printWebAddresses();printCommands();if(boundMac.length()){Serial.println("[BLE] saved watch: "+boundMac);connectWatch(boundMac);}else setStatus("ready - no watch bound");}

void loop(){server.handleClient();if(WiFi.status()!=WL_CONNECTED&&prefs.isKey("wifi_ssid")){static uint32_t lastRetry=0;if(millis()-lastRetry>30000){lastRetry=millis();String ssid=prefs.getString("wifi_ssid","");String pass=prefs.getString("wifi_pass","");if(ssid.length())connectWiFi(ssid,pass,false);}}
    while(Serial.available()){char c=Serial.read();if(serialLine.length()==0){if(c=='W'){scanWiFiConsole();continue;}if(c=='S'){runScan();printBleScanToSerial();continue;}if(c=='C'){connectWatch(boundMac);continue;}if(c=='I'){e91aBind();continue;}if(c=='F'){testFindWatch();continue;}if(c=='E'){enableNotifications();continue;}if(c=='T'){sendNotification("MeshCore","TEST FROM ESP32-S3");continue;}}
        if(c=='\r'||c=='\n'){if(serialLine.length()){handleConsoleLine(serialLine);serialLine="";}}else if(c==8||c==127){if(serialLine.length())serialLine.remove(serialLine.length()-1);}else if(c>=32&&c<=126){if(serialLine.length()<240)serialLine+=c;}}
    delay(5);
}
