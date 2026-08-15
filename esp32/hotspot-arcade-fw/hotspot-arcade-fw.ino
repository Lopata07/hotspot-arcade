// Hotspot Arcade firmware for the ESP32-S2 WiFi dev board.
//
// Hosts an open WiFi AP + captive portal that serves a multiplayer game web app
// (streamed from the Flipper into RAM), and acts as the real-time referee over a
// WebSocket while the Flipper drives the session over UART v2. See docs/PROTOCOL.md.
//
// For education/fun on your own hardware. It runs an OPEN access point and a
// catch-all captive page; only operate it where that is allowed.

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>

#include "ha_proto.h"
#include "ha_json.h"
#include "ha_assets.h"
#include "ha_games.h"

#define WS_MSG_MAX 512
#define AP_MAX_CONN 8

static DNSServer dnsServer;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static IPAddress apIP(192, 168, 4, 1);
static char apName[33] = "Hotspot Arcade";
static char apPass[64] = ""; // "" = open network
static bool portalRunning = false;
static uint8_t apMaxConn = AP_MAX_CONN;

static AssetStore assets;
static Engine engine;

// ---------------- status LED ----------------
// The official devboard carries an RGB LED wired to these three pins. They are not
// documented in the Flipper hardware docs; the numbers come from the board's own
// factory firmware (flipperdevices/blackmagic-esp32-s2, main/led.c), which is the
// authoritative source for this board.
//
// Common anode: the pin sinks current, so a LOW duty lights the channel and the PWM
// value has to be inverted. The factory firmware also caps brightness at 20/256 --
// at full duty this LED is genuinely painful to look at on a desk. Same cap here.
#define LED_PIN_RED   6
#define LED_PIN_GREEN 5
#define LED_PIN_BLUE  4
#define LED_CH_RED    5 // LEDC channels 5-7: the engine uses none, so no conflict
#define LED_CH_GREEN  6
#define LED_CH_BLUE   7
#define LED_MAX_DUTY  20 // of 255, matching the factory firmware's own cap

static void ledInit() {
    ledcSetup(LED_CH_RED, 5000, 8);
    ledcSetup(LED_CH_GREEN, 5000, 8);
    ledcSetup(LED_CH_BLUE, 5000, 8);
    ledcAttachPin(LED_PIN_RED, LED_CH_RED);
    ledcAttachPin(LED_PIN_GREEN, LED_CH_GREEN);
    ledcAttachPin(LED_PIN_BLUE, LED_CH_BLUE);
}

// 0..255 per channel, as if this were a normal common-cathode LED -- the inversion
// and the brightness cap are applied here so callers can think in plain RGB.
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LED_CH_RED, 255 - ((uint32_t)r * LED_MAX_DUTY) / 255);
    ledcWrite(LED_CH_GREEN, 255 - ((uint32_t)g * LED_MAX_DUTY) / 255);
    ledcWrite(LED_CH_BLUE, 255 - ((uint32_t)b * LED_MAX_DUTY) / 255);
}

// Serial (to the Flipper) is written from the loop task and the async web/WS
// task; serialize whole frames so bytes can't interleave. Engine state is also
// touched from both tasks, so guard it too.
static SemaphoreHandle_t serialMutex = nullptr;
static SemaphoreHandle_t engineMutex = nullptr;
#define ENGINE_LOCK() xSemaphoreTakeRecursive(engineMutex, portMAX_DELAY)
#define ENGINE_UNLOCK() xSemaphoreGiveRecursive(engineMutex)

// ---------------- UART TX ----------------

static void uartSend(uint8_t type, const uint8_t* payload, size_t len) {
    if(len > HA_MAX_PAYLOAD) len = HA_MAX_PAYLOAD;
    uint8_t hdr[4] = {HA_SYNC, type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    uint8_t crc = ha_crc8_upd(0, type);
    crc = ha_crc8_upd(crc, hdr[2]);
    crc = ha_crc8_upd(crc, hdr[3]);
    for(size_t i = 0; i < len; i++) crc = ha_crc8_upd(crc, payload[i]);

    if(serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.write(hdr, 4);
    if(len) Serial.write(payload, len);
    Serial.write(crc);
    if(serialMutex) xSemaphoreGive(serialMutex);
}

static void uartStatus(const char* token) {
    uartSend(HA_MSG_STATUS, (const uint8_t*)token, strlen(token));
}

// ---------------- sinks used by the engine ----------------

void haWsSendWs(uint32_t wsId, const String& msg) {
    if(!wsId) return;
    ws.text(wsId, msg);
}
void haWsBroadcast(const String& msg) {
    ws.textAll(msg);
}
void haUartJoin(uint8_t pid, const char* nick) {
    uint8_t buf[1 + HA_NICK_LEN + 1];
    buf[0] = pid;
    size_t n = strlen(nick);
    if(n > HA_NICK_LEN) n = HA_NICK_LEN;
    memcpy(buf + 1, nick, n);
    uartSend(HA_MSG_JOIN, buf, 1 + n);
}
void haUartLeave(uint8_t pid) {
    uartSend(HA_MSG_LEAVE, &pid, 1);
}
void haUartScore(uint8_t pid, int delta, const char* reason) {
    uint8_t buf[1 + 2 + 24];
    buf[0] = pid;
    int16_t d = (int16_t)delta;
    buf[1] = (uint8_t)(d & 0xFF);
    buf[2] = (uint8_t)((d >> 8) & 0xFF);
    size_t n = strlen(reason);
    if(n > 24) n = 24;
    memcpy(buf + 3, reason, n);
    uartSend(HA_MSG_SCORE, buf, 3 + n);
}
void haUartEvent(const String& json) {
    uartSend(HA_MSG_EVENT, (const uint8_t*)json.c_str(), json.length());
}
void haUartRoundResult(const String& json) {
    uartSend(HA_MSG_ROUND_RESULT, (const uint8_t*)json.c_str(), json.length());
}

// ---------------- HTTP (captive) ----------------

// Serve the streamed web bundle for every host/path so the captive portal always
// resolves. GET "/" (and every OS captive-probe URL) gets the app; other stored
// asset paths are served by exact match.
class ArcadeHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        (void)request;
        return true;
    }
    void handleRequest(AsyncWebServerRequest* request) override {
        String url = request->url();
        const Asset* a = assets.find(url.c_str());
        if(!a) a = assets.root(); // captive-detection URLs -> the app
        if(!a || !a->buf || a->len == 0) {
            request->send(200, "text/html", "<h1>Hotspot Arcade</h1><p>No bundle loaded.</p>");
            return;
        }
        AsyncWebServerResponse* res =
            request->beginResponse(200, a->mime, (const uint8_t*)a->buf, a->len);
        if(a->gzip) res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        request->send(res);
    }
};

// ---------------- WebSocket ----------------

static void onWsEvent(
    AsyncWebSocket* srv,
    AsyncWebSocketClient* client,
    AwsEventType type,
    void* arg,
    uint8_t* data,
    size_t len) {
    (void)srv;
    if(type == WS_EVT_DISCONNECT) {
        ENGINE_LOCK();
        engine.onWsDisconnect(client->id());
        ENGINE_UNLOCK();
    } else if(type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT &&
           len < WS_MSG_MAX) {
            char buf[WS_MSG_MAX];
            memcpy(buf, data, len);
            buf[len] = '\0';
            ENGINE_LOCK();
            engine.onInput(client->id(), buf);
            ENGINE_UNLOCK();
        }
    }
}

// ---------------- AP lifecycle ----------------

static void startPortal() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    // A non-empty password switches the driver to WPA2-PSK; empty keeps the
    // network open. apPass is validated Flipper-side (0 or 8-63 chars) before it's
    // ever sent, but WiFi.softAP() itself would just silently misbehave on 1-7
    // chars, so that floor lives entirely in the UI (Task 4), not here.
    WiFi.softAP(apName, apPass[0] ? apPass : nullptr, 1, 0, apMaxConn);
    delay(100);
    uartStatus("ap_ok");

    dnsServer.start(53, "*", apIP);
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.addHandler(new ArcadeHandler()).setFilter(ON_AP_FILTER);
    server.begin();
    portalRunning = true;

    // Green = AP up and WPA2-protected, amber = AP up but OPEN. Worth distinguishing:
    // an open AP is the one state where a passer-by can join, and the Flipper's own
    // menu can't prove what the radio actually did with the password it sent.
    if(apPass[0])
        ledSet(0, 255, 0);
    else
        ledSet(255, 90, 0);

    String up = String("up ip=") + WiFi.softAPIP().toString();
    uartStatus(up.c_str());
}

static void stopPortal() {
    if(portalRunning) {
        ws.closeAll();
        server.end();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        portalRunning = false;
    }
    ENGINE_LOCK();
    engine.reset();
    ENGINE_UNLOCK();
    ledSet(0, 0, 40); // back to dim blue: idle, no AP
    uartStatus("stopped");
}

// ---------------- UART RX (framed) ----------------

enum RxState { RX_SYNC, RX_TYPE, RX_LEN0, RX_LEN1, RX_PAYLOAD, RX_CRC, RX_RAW };
static RxState rxState = RX_SYNC;
static uint8_t rxType = 0;
static uint16_t rxLen = 0, rxIdx = 0;
static uint8_t rxCrc = 0;
static uint8_t rxBuf[HA_MAX_PAYLOAD + 1];

// FILE_BEGIN payload: flags(1) pathlen(1) path mimelen(1) mime total(4 LE)
static void handleFileBegin(const uint8_t* p, size_t len) {
    if(len < 3) return;
    size_t i = 0;
    uint8_t flags = p[i++];
    uint8_t pathlen = p[i++];
    if(i + pathlen + 1 > len) return;
    char path[HA_ASSET_PATH];
    size_t pl = pathlen < HA_ASSET_PATH - 1 ? pathlen : HA_ASSET_PATH - 1;
    memcpy(path, p + i, pl);
    path[pl] = '\0';
    i += pathlen;
    uint8_t mimelen = p[i++];
    if(i + mimelen + 4 > len) return;
    char mime[HA_ASSET_MIME];
    size_t ml = mimelen < HA_ASSET_MIME - 1 ? mimelen : HA_ASSET_MIME - 1;
    memcpy(mime, p + i, ml);
    mime[ml] = '\0';
    i += mimelen;
    uint32_t total = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) |
                     ((uint32_t)p[i + 3] << 24);
    assets.begin(path, mime, flags & 1, total);
}

static void dispatchFrame() {
    rxBuf[rxLen] = '\0'; // JSON payloads are text; safe (buf has +1)
    switch(rxType) {
    case HA_MSG_CLEAR_FILES:
        assets.clear();
        uartStatus("cleared");
        break;
    case HA_MSG_FILE_BEGIN:
        handleFileBegin(rxBuf, rxLen);
        if(assets.receiving()) {
            rxState = RX_RAW; // the file bytes follow, unframed
            return;
        }
        uartStatus("fok"); // zero-length file committed immediately
        break;
    case HA_MSG_SET_AP:
        if(rxLen > 0) {
            // Payload is "SSID\0PASSWORD". A NUL inside rxLen means the sending
            // Flipper knows about the password field; without one (older Flipper
            // build) the whole payload is just the SSID, same as before this change.
            size_t ssidLen = 0;
            while(ssidLen < rxLen && rxBuf[ssidLen] != '\0') ssidLen++;
            size_t n = ssidLen < sizeof(apName) - 1 ? ssidLen : sizeof(apName) - 1;
            memcpy(apName, rxBuf, n);
            apName[n] = '\0';

            if(ssidLen < rxLen) {
                size_t passStart = ssidLen + 1;
                size_t passLen = rxLen - passStart;
                size_t pn = passLen < sizeof(apPass) - 1 ? passLen : sizeof(apPass) - 1;
                memcpy(apPass, rxBuf + passStart, pn);
                apPass[pn] = '\0';
            } else {
                apPass[0] = '\0';
            }
        }
        uartStatus("ap_set");
        break;
    case HA_MSG_START:
        startPortal();
        break;
    case HA_MSG_STOP:
        stopPortal();
        break;
    case HA_MSG_RESET:
        uartStatus("resetting");
        delay(50);
        ESP.restart();
        break;
    case HA_MSG_SELECT_GAME:
        if(rxLen >= 1) {
            ENGINE_LOCK();
            engine.selectGame(rxBuf[0]);
            ENGINE_UNLOCK();
        }
        break;
    case HA_MSG_CONTENT_CLEAR:
        ENGINE_LOCK();
        engine.contentClear();
        ENGINE_UNLOCK();
        break;
    case HA_MSG_CONTENT_PACK:
        // payload: game byte, then the pack name (not NUL-terminated on the wire).
        if(rxLen >= 2) {
            char name[64];
            size_t n = rxLen - 1 < sizeof(name) - 1 ? (size_t)(rxLen - 1) : sizeof(name) - 1;
            memcpy(name, rxBuf + 1, n);
            name[n] = '\0';
            ENGINE_LOCK();
            engine.contentPack(rxBuf[0], name);
            ENGINE_UNLOCK();
        }
        break;
    case HA_MSG_CONTENT_ITEM:
        ENGINE_LOCK();
        engine.contentItem((const char*)rxBuf);
        ENGINE_UNLOCK();
        break;
    case HA_MSG_ROUND_END:
        ENGINE_LOCK();
        engine.roundEnd();
        ENGINE_UNLOCK();
        break;
    case HA_MSG_CONFIG: {
        int v;
        if(ha_json_int((const char*)rxBuf, "max", &v) && v >= 1 && v <= 15) apMaxConn = (uint8_t)v;
        char lang[8];
        if(ha_json_str((const char*)rxBuf, "lang", lang, sizeof(lang))) {
            ENGINE_LOCK();
            engine.setLang(lang);
            ENGINE_UNLOCK();
        }
        break;
    }
    case HA_MSG_RESET_SCORES:
        ENGINE_LOCK();
        engine.resetScores();
        ENGINE_UNLOCK();
        break;
    default:
        break;
    }
    rxState = RX_SYNC;
}

static void rxByte(uint8_t c) {
    switch(rxState) {
    case RX_SYNC:
        if(c == HA_SYNC) rxState = RX_TYPE;
        break;
    case RX_TYPE:
        rxType = c;
        rxCrc = ha_crc8_upd(0, c);
        rxState = RX_LEN0;
        break;
    case RX_LEN0:
        rxLen = c;
        rxCrc = ha_crc8_upd(rxCrc, c);
        rxState = RX_LEN1;
        break;
    case RX_LEN1:
        rxLen |= ((uint16_t)c << 8);
        rxCrc = ha_crc8_upd(rxCrc, c);
        if(rxLen > HA_MAX_PAYLOAD) {
            rxState = RX_SYNC; // bogus length, resync
            break;
        }
        rxIdx = 0;
        rxState = rxLen ? RX_PAYLOAD : RX_CRC;
        break;
    case RX_PAYLOAD:
        rxBuf[rxIdx++] = c;
        rxCrc = ha_crc8_upd(rxCrc, c);
        if(rxIdx >= rxLen) rxState = RX_CRC;
        break;
    case RX_CRC:
        if(c == rxCrc) {
            dispatchFrame(); // sets next state (RX_SYNC or RX_RAW)
        } else {
            rxState = RX_SYNC; // corrupt frame, drop and resync
        }
        break;
    case RX_RAW:
        break; // handled in bulk below
    }
}

static void pumpSerial() {
    while(Serial.available()) {
        if(rxState == RX_RAW) {
            // Bulk asset bytes: drain as many as we still need in one go.
            size_t need = assets.remaining();
            if(need == 0) {
                rxState = RX_SYNC;
                uartStatus("fok");
                continue;
            }
            uint8_t tmp[256];
            size_t want = need < sizeof(tmp) ? need : sizeof(tmp);
            int got = Serial.read(tmp, want);
            if(got > 0) assets.feed(tmp, (size_t)got);
            if(assets.remaining() == 0) {
                rxState = RX_SYNC;
                uartStatus("fok");
            }
        } else {
            rxByte((uint8_t)Serial.read());
        }
    }
}

// ---------------- Arduino entry ----------------

void setup() {
    serialMutex = xSemaphoreCreateMutex();
    engineMutex = xSemaphoreCreateRecursiveMutex();
    ledInit();
    // Boot blink: the only sign of life this board gives on its own. Serial goes to
    // UART0 (the Flipper's GPIO header), not USB-CDC, so a board sitting on a desk
    // with just a USB cable enumerates nothing and shows nothing -- this blink is
    // what tells you the firmware is actually running rather than the board being dead.
    ledSet(255, 0, 0);
    delay(150);
    ledSet(0, 0, 0);
    Serial.setRxBufferSize(4096);
    Serial.begin(HA_UART_BAUD);
    delay(100);
    engine.reset();
    ledSet(0, 0, 40); // dim blue: alive, waiting for the Flipper to start a session
    uartStatus("boot");
}

void loop() {
    if(portalRunning) {
        dnsServer.processNextRequest();
        ws.cleanupClients();
        ENGINE_LOCK();
        engine.tick(millis());
        ENGINE_UNLOCK();
    }
    pumpSerial();

    static uint32_t lastPing = 0;
    uint32_t now = millis();
    if(now - lastPing >= 2000) {
        lastPing = now;
        // Identity beacon: magic + version, so the Flipper knows it's our firmware
        // (and which version), not a different project on the same board.
        uint8_t beacon[6] = {
            HA_FW_MAGIC_0,
            HA_FW_MAGIC_1,
            HA_FW_MAGIC_2,
            HA_FW_MAGIC_3,
            (uint8_t)(HA_FW_VERSION & 0xFF),
            (uint8_t)(HA_FW_VERSION >> 8)};
        uartSend(HA_MSG_PING, beacon, sizeof(beacon));
    }
}
