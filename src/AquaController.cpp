// SPDX-License-Identifier: MIT
// Aquarium-LED-Steuerung: Licht, Mond/Sonne, Temperatur, HTTP-API
#include "AquaController.h"
#include "AWM_Logging.h"
#include "ESPWiFiManagerCommon.h"
#include <AsyncJson.h>
#include <sunMoon.h>

namespace aqua {

static const float    kTempError      = 999.99f;
static const uint32_t kLightEveryMs   = 250;
static const uint32_t kAstroEveryMs   = 60000;
static const uint32_t kRefreshMs      = 10000;    // Strip periodisch neu senden (Störimpulse)
static const uint32_t kTempConvMs     = 800;      // 12 Bit Wandlung ≤ 750 ms
static const uint32_t kTempRescanMs   = 30000;
static const uint8_t  kMoonFullBright = 30;       // Vollmond ≈ 30/256

static const char* const kNtpText[] = { "nie gesetzt", "Synchronisierung fehlgeschlagen", "synchronisiert" };

// Farbe zwischen AUS (0 %) und EIN (100 %) mischen
static uint32_t blend(uint32_t off, uint32_t on, uint8_t pct) {
    uint32_t out = 0;
    for (uint8_t sh = 0; sh <= 16; sh += 8) {
        const int a = (off >> sh) & 0xFF, b = (on >> sh) & 0xFF;
        out |= (uint32_t)(a + (b - a) * pct / 100) << sh;
    }
    return out;
}

// Farbe mit Helligkeit bright/256 skalieren
static uint32_t scale(uint32_t rgb, uint8_t bright) {
    uint32_t out = 0;
    for (uint8_t sh = 0; sh <= 16; sh += 8)
        out |= ((((rgb >> sh) & 0xFF) * bright) >> 8) << sh;
    return out;
}

static String fmtLocal(time_t local, const char* fmt) {
    if (!local) return String();
    struct tm t;
    gmtime_r(&local, &t);     // Lokalzeit liegt bereits als Epoch vor
    char buf[24];
    strftime(buf, sizeof(buf), fmt, &t);
    return buf;
}

static void sendJson(AsyncWebServerRequest* r, int code, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    r->send(code, "application/json", out);
}

static void sendError(AsyncWebServerRequest* r, int code, const String& msg) {
    JsonDocument doc;
    doc["ok"]    = false;
    doc["error"] = msg;
    sendJson(r, code, doc);
}

// ── Lebenszyklus ──────────────────────────────────────────────────────────────

Controller::Controller(uint8_t stripPin, uint8_t oneWirePin)
    : _strip(kMaxLeds, stripPin, NEO_GRB + NEO_KHZ800), _ow(oneWirePin), _ds(&_ow) {
    setDefaults(_cfg);
}

void Controller::begin() {
    Config c;
    if (load(c)) AWM_LOGI("✅ Aqua-Config geladen (%u LEDs)", (unsigned)c.ledCount);
    else         AWM_LOGI("ℹ️ Aqua-Config: Standardwerte");
    setConfig(c);

    _strip.begin();
    _strip.clear();
    _strip.show();

    _clock.begin();

    _ds.begin();
    _ds.setWaitForConversion(false);
    _dsFound = _ds.getAddress(_dsAddr, 0);
    if (_dsFound) {
        _ds.setResolution(_dsAddr, 12);
        _ds.requestTemperaturesByAddress(_dsAddr);
    } else {
        AWM_LOGW("⚠️ Kein DS18B20 gefunden");
    }
    _tempReqAt = _tempScanAt = millis();
    portENTER_CRITICAL(&_mux);
    _state.temp = kTempError;
    portEXIT_CRITICAL(&_mux);
}

void Controller::loop(bool wifiConnected) {
    _clock.loop(wifiConnected);
    updateTemp();

    const uint32_t now = millis();
    portENTER_CRITICAL(&_mux);
    const bool cfgChanged = (_cfgVersion != _lastCfgVersion);
    _lastCfgVersion = _cfgVersion;
    portEXIT_CRITICAL(&_mux);

    if (!cfgChanged && now - _lastLight < kLightEveryMs) return;
    _lastLight = now;

    const Config c = getConfig();
    // Sonne/Mond minütlich, nach Config-Änderung und sobald die Uhr erstmals gültig ist
    const bool astroMissing = !_astroValid && _clock.valid();
    if (cfgChanged || astroMissing || now - _lastAstro >= kAstroEveryMs) {
        _lastAstro = now;
        updateAstro(c);
    }
    updateLight(c);
}

// ── Config / State (thread-sicher) ────────────────────────────────────────────

Config Controller::getConfig() const {
    portENTER_CRITICAL(&_mux);
    const Config c = _cfg;
    portEXIT_CRITICAL(&_mux);
    return c;
}

void Controller::setConfig(const Config& c) {
    portENTER_CRITICAL(&_mux);
    _cfg = c;
    ++_cfgVersion;
    portEXIT_CRITICAL(&_mux);
    _clock.setZone(c.tzHours, c.dst);
}

Controller::State Controller::getState() const {
    portENTER_CRITICAL(&_mux);
    const State s = _state;
    portEXIT_CRITICAL(&_mux);
    return s;
}

// ── Licht ─────────────────────────────────────────────────────────────────────

// Helligkeit (%) eines Kanals im UHR-Modus; 0 = außerhalb aller Zeiträume.
// Rampe: die ersten/letzten Minuten eines Zeitraums laufen in Stufen
// rampLevel[0] → [1] → [2] hoch bzw. rückwärts wieder herunter.
uint8_t Controller::channelLevel(const Config& c, const Channel& ch, uint16_t minute) const {
    uint8_t best = 0;
    for (const Period& p : ch.period) {
        if (p.on == p.off) continue;                              // deaktiviert
        const int len   = (p.off - p.on + 1440) % 1440;
        const int since = (minute - p.on + 1440) % 1440;
        if (since >= len) continue;
        const int until = len - since;                            // ≥ 1

        uint8_t lvl = 100;
        if (c.rampStepMin) {
            int stage = min(since / c.rampStepMin, (until - 1) / c.rampStepMin);
            if (stage >= kRampStages) stage = kRampStages - 1;
            lvl = c.rampLevel[stage];
        }
        if (lvl > best) best = lvl;
    }
    return best;
}

void Controller::updateLight(const Config& c) {
    const bool   valid  = _clock.valid();
    const time_t local  = _clock.local();
    struct tm t;
    gmtime_r(&local, &t);
    const uint16_t minute = t.tm_hour * 60 + t.tm_min;

    // Kanäle laufen nur bei Mond = AUTO; bei Mond EIN/AUS sind alle Kanäle aus
    const bool run = (c.moonMode == MoonMode::AUTO);
    ChannelState chs[kChannels];
    bool anyOn = false;
    for (uint8_t i = 0; i < kChannels; ++i) {
        const Channel& ch = c.ch[i];
        uint8_t lvl = 0;
        if (run) {
            switch (ch.mode) {
            case ChannelMode::ON:    lvl = 100; break;
            case ChannelMode::OFF:   lvl = 0;   break;
            case ChannelMode::CLOCK: lvl = valid ? channelLevel(c, ch, minute) : 0; break;
            }
        }
        chs[i].on    = lvl > 0;
        chs[i].level = lvl;
        chs[i].color = run ? blend(ch.colorOff, ch.colorOn, lvl) : 0;
        anyOn |= chs[i].on;
    }

    bool moonLit = false;
    switch (c.moonMode) {
    case MoonMode::AUTO: moonLit = !anyOn; break;   // an, sobald alle Kanäle außerhalb ihrer Zeiten sind
    case MoonMode::ON:   moonLit = true;  break;
    case MoonMode::OFF:  moonLit = false; break;
    }

    portENTER_CRITICAL(&_mux);
    memcpy(_state.ch, chs, sizeof(chs));
    _state.moonLit = moonLit;
    const State s = _state;
    portEXIT_CRITICAL(&_mux);

    render(c, s);
}

void Controller::render(const Config& c, const State& s) {
    FrameKey k;
    memset(&k, 0, sizeof(k));   // Padding nullen (memcmp-Vergleich)
    for (uint8_t i = 0; i < kChannels; ++i) k.color[i] = s.ch[i].color;
    k.ledCount = c.ledCount;
    k.moonLit  = s.moonLit;
    if (s.moonLit) {
        // EIN (Handbetrieb): feste Vollmond-Helligkeit; AUTO: nach Mondphase
        const uint8_t bright = (c.moonMode == MoonMode::ON) ? kMoonFullBright : s.moonBright;
        k.moonColor = scale(c.moonColor, bright);
        memcpy(k.moon, c.moon, sizeof(k.moon));
    }

    const uint32_t now = millis();
    if (_frameValid && k == _lastFrame && now - _lastShow < kRefreshMs) return;

    // LEDs jenseits von ledCount explizit aus (z. B. nach Verkleinern der Anzahl)
    for (uint16_t i = 0; i < kMaxLeds; ++i)
        _strip.setPixelColor(i, i < k.ledCount ? k.color[i % kChannels] : 0);
    if (k.moonLit) {
        for (const LedRange& r : k.moon) {
            if (!r.from) continue;
            for (uint16_t led = r.from; led <= r.to && led <= k.ledCount; ++led)
                _strip.setPixelColor(led - 1, k.moonColor);
        }
    }
    _strip.show();

    _lastFrame  = k;
    _frameValid = true;
    _lastShow   = now;
}

// ── Sonne / Mond ──────────────────────────────────────────────────────────────

void Controller::updateAstro(const Config& c) {
    time_t rise = 0, set = 0;
    uint8_t age = 0, bright = 0;

    _astroValid = _clock.valid();
    if (_astroValid) {
        const time_t local = _clock.local();
        sunMoon sm;
        if (sm.init(_clock.offsetSeconds(_clock.utc()) / 60, c.latitude, c.longitude)) {
            rise = sm.sunRise(local);
            set  = sm.sunSet(local);
            age  = sm.moonDay(local);
            if (age > 29) age = 29;
            // Helligkeit = 2 × Tage bis/seit Neumond (Vollmond ≈ 30/256)
            const uint8_t daysFromNew = min<uint8_t>(age, 30 - age);
            bright = 2 * daysFromNew;
        }
    }

    portENTER_CRITICAL(&_mux);
    _state.sunrise    = rise;
    _state.sunset     = set;
    _state.moonAge    = age;
    _state.moonBright = bright;
    portEXIT_CRITICAL(&_mux);
}

// ── Temperatur (DS18B20, nicht-blockierend) ───────────────────────────────────

void Controller::updateTemp() {
    const uint32_t now = millis();
    if (now - _tempReqAt < kTempConvMs) return;

    float t = kTempError;
    if (_dsFound) {
        const float v = _ds.getTempC(_dsAddr);
        // -127 = getrennt, 85 = Power-On-Wert (keine gültige Wandlung)
        if (v != DEVICE_DISCONNECTED_C && v != 85.0f && v > -55.0f && v < 125.0f) t = v;
    }
    if (t == kTempError && now - _tempScanAt >= kTempRescanMs) {
        // Sensor evtl. neu angesteckt → Bus erneut durchsuchen
        _tempScanAt = now;
        _ds.begin();
        _ds.setWaitForConversion(false);
        _dsFound = _ds.getAddress(_dsAddr, 0);
        if (_dsFound) _ds.setResolution(_dsAddr, 12);
    }
    if (_dsFound) _ds.requestTemperaturesByAddress(_dsAddr);
    _tempReqAt = now;

    portENTER_CRITICAL(&_mux);
    _state.temp = t;
    portEXIT_CRITICAL(&_mux);
}

// ── HTTP ──────────────────────────────────────────────────────────────────────

void Controller::registerRoutes(AsyncWebServer& server) {
    server.on("/led", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (!serveFileFromLittleFS(r, "/led.html", "text/html"))
            r->send(500, "text/html", "<h1>led.html nicht gefunden</h1>");
    });
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (!serveFileFromLittleFS(r, "/config.html", "text/html"))
            r->send(500, "text/html", "<h1>config.html nicht gefunden</h1>");
    });
    server.on("/api/aqua/status",   HTTP_GET, [this](AsyncWebServerRequest* r) { handleStatus(r); });
    server.on("/api/aqua/settings", HTTP_GET, [this](AsyncWebServerRequest* r) { handleSettingsGet(r); });

    auto* post = new AsyncCallbackJsonWebHandler("/api/aqua/settings",
        [this](AsyncWebServerRequest* r, JsonVariant& json) { handleSettingsPost(r, json); });
    post->setMethod(HTTP_POST);
    server.addHandler(post);
}

// GET /api/aqua/status: Live-Werte für die LED-Seite (Abfrage jede Sekunde)
void Controller::handleStatus(AsyncWebServerRequest* r) {
    const State  s     = getState();
    const bool   valid = _clock.valid();
    const time_t utc   = _clock.utc();
    const time_t local = _clock.local();

    JsonDocument doc;
    doc["valid"]     = valid;
    doc["date"]      = valid ? fmtLocal(local, "%d.%m.%Y") : String("--.--.----");
    doc["time"]      = valid ? fmtLocal(local, "%H:%M:%S") : String("--:--:--");
    doc["dstActive"] = valid && _clock.isDst(utc) && getConfig().dst;
    doc["ntp"]       = (uint8_t)_clock.status();
    doc["ntpText"]   = kNtpText[(uint8_t)_clock.status()];
    const time_t ls  = _clock.lastSyncUtc();
    doc["lastSync"]  = ls ? fmtLocal(ls + _clock.offsetSeconds(ls), "%d.%m.%Y %H:%M") : String();
    doc["sunrise"]   = fmtLocal(s.sunrise, "%H:%M");
    doc["sunset"]    = fmtLocal(s.sunset,  "%H:%M");
    doc["moonAge"]   = s.moonAge;
    // tatsächliche Helligkeit wie in render(): aus = 0, EIN = Vollmond, AUTO = nach Mondphase
    const bool moonOn = getConfig().moonMode == MoonMode::ON;
    doc["moonBright"]= s.moonLit ? (moonOn ? kMoonFullBright : s.moonBright) : 0;
    doc["moonLit"]   = s.moonLit;
    doc["temp"]      = serialized(String(s.temp, 2));

    JsonArray chs = doc["ch"].to<JsonArray>();
    for (const ChannelState& c : s.ch) {
        JsonObject o = chs.add<JsonObject>();
        o["on"]    = c.on;
        o["level"] = c.level;
        o["color"] = colorToText(c.color);
    }
    sendJson(r, 200, doc);
}

// GET /api/aqua/settings: komplette Konfiguration inkl. NTP-Server und aktueller Lokalzeit
void Controller::handleSettingsGet(AsyncWebServerRequest* r) {
    JsonDocument doc;
    toJson(getConfig(), doc.to<JsonObject>());

    JsonDocument wifi;
    doc["ntp"] = loadConfigDoc(wifi) ? String(wifi["ntp1"] | "") : String();
    doc["ntpDefault"] = "de.pool.ntp.org";
    doc["ntpStatus"]  = kNtpText[(uint8_t)_clock.status()];
    doc["now"]        = _clock.valid() ? fmtLocal(_clock.local(), "%Y-%m-%dT%H:%M") : String();
    sendJson(r, 200, doc);
}

// POST /api/aqua/settings (JSON): Teil-Updates erlaubt.
// Zusätzlich: "ntp" (Server, leer = Default) und "setTime" ("YYYY-MM-DDTHH:MM[:SS]", Lokalzeit).
void Controller::handleSettingsPost(AsyncWebServerRequest* r, JsonVariant& json) {
    if (!json.is<JsonObject>()) { sendError(r, 400, "JSON-Objekt erwartet"); return; }
    JsonObjectConst o = json.as<JsonObjectConst>();

    // 1. Alles prüfen, bevor etwas gespeichert wird
    Config c = getConfig();
    String err;
    if (!fromJson(o, c, err)) { sendError(r, 400, err); return; }

    const bool hasNtp = !o["ntp"].isNull();
    String ntp = o["ntp"] | "";
    ntp.trim();
    if (ntp.length() > 63 || ntp.indexOf(' ') >= 0) { sendError(r, 400, "Ungültiger NTP-Server"); return; }

    time_t manual = 0;
    const char* setTime = o["setTime"] | "";
    if (*setTime) {
        int y, mo, d, h, mi, sec = 0;
        if (sscanf(setTime, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5 ||
            y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59) {
            sendError(r, 400, "Ungültiges Datum/Uhrzeit");
            return;
        }
        manual = Clock::makeEpoch(y, mo, d, h, mi, sec);
    }

    // 2. Übernehmen
    if (!save(c)) { sendError(r, 500, "Speichern fehlgeschlagen (NVS)"); return; }
    setConfig(c);

    if (hasNtp) {
        JsonDocument wifi;
        loadConfigDoc(wifi);   // SSID/Passwort usw. erhalten
        wifi["ntp1"] = ntp;
        String out;
        serializeJson(wifi, out);
        if (!saveConfigJson(out)) { sendError(r, 500, "NTP-Server speichern fehlgeschlagen"); return; }
        _clock.requestResync();
    }
    if (manual && !_clock.setLocal(manual)) { sendError(r, 500, "Uhrzeit setzen fehlgeschlagen"); return; }

    JsonDocument ok;
    ok["ok"] = true;
    sendJson(r, 200, ok);
}

} // namespace aqua
