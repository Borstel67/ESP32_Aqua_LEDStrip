// SPDX-License-Identifier: MIT
// Aquarium-LED-Konfiguration: Defaults, JSON-Konvertierung, NVS-Persistenz
#include "AquaConfig.h"
#include "AWM_Logging.h"
#include <Preferences.h>

namespace aqua {

static const char* kNvsNs  = "aqua";
static const char* kNvsKey = "cfg";

// ── Defaults ──────────────────────────────────────────────────────────────────

void setDefaults(Config& c) {
    c = Config{};
    c.ledCount = 60;

    static const uint32_t colorsOn[kChannels] = { 0xFFFFFF, 0x3060FF, 0xFFB070, 0xFF4020 };
    for (uint8_t i = 0; i < kChannels; ++i) {
        c.ch[i].colorOn   = colorsOn[i];
        c.ch[i].colorOff  = 0x000000;
        c.ch[i].mode      = ChannelMode::CLOCK;
        c.ch[i].period[0] = { 10 * 60, 13 * 60 };
        c.ch[i].period[1] = { 15 * 60, 21 * 60 };
    }

    c.rampStepMin  = 10;
    c.rampLevel[0] = 33;
    c.rampLevel[1] = 66;
    c.rampLevel[2] = 100;

    c.moonColor = 0x6080FF;
    c.moonMode  = MoonMode::AUTO;

    c.tzHours   = 1;
    c.dst       = true;
    c.latitude  = 52.52f;
    c.longitude = 13.40f;
}

// ── Konvertierungen ───────────────────────────────────────────────────────────

String minutesToText(uint16_t m) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60) % 24, (unsigned)(m % 60));
    return buf;
}

bool textToMinutes(const char* s, uint16_t& out) {
    unsigned h, m;
    if (!s || sscanf(s, "%u:%u", &h, &m) != 2 || h > 23 || m > 59) return false;
    out = h * 60 + m;
    return true;
}

String colorToText(uint32_t rgb) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%06lx", (unsigned long)(rgb & 0xFFFFFF));
    return buf;
}

bool textToColor(const char* s, uint32_t& out) {
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    char* end = nullptr;
    const unsigned long v = strtoul(s + 1, &end, 16);
    if (*end) return false;
    out = v;
    return true;
}

String rangesToText(const LedRange* r) {
    String s;
    for (uint8_t i = 0; i < kMoonRanges; ++i) {
        if (!r[i].from) continue;
        if (s.length()) s += ';';
        s += r[i].from;
        if (r[i].to != r[i].from) { s += '-'; s += r[i].to; }
    }
    return s;
}

// Format "von-bis;einzeln;von-bis", max. kMoonRanges Einträge; Leerzeichen erlaubt
bool textToRanges(const char* s, LedRange* r, uint16_t maxLed, String& err) {
    LedRange tmp[kMoonRanges] = {};
    uint8_t n = 0;
    String text = s ? s : "";
    text.replace(" ", "");
    int pos = 0;
    while (pos < (int)text.length()) {
        int sep = text.indexOf(';', pos);
        if (sep < 0) sep = text.length();
        const String item = text.substring(pos, sep);
        pos = sep + 1;
        if (!item.length()) continue;
        if (n >= kMoonRanges) { err = "Maximal 3 Mond-Bereiche"; return false; }

        unsigned a, b;
        char dash;
        const int got = sscanf(item.c_str(), "%u%c%u", &a, &dash, &b);
        if (got == 1)                       b = a;
        else if (got != 3 || dash != '-')   { err = "Ungültiger Mond-Bereich: " + item; return false; }
        if (a < 1 || b < a || b > maxLed)   { err = "Mond-Bereich außerhalb 1–" + String(maxLed) + ": " + item; return false; }
        tmp[n++] = { (uint16_t)a, (uint16_t)b };
    }
    memcpy(r, tmp, sizeof(tmp));
    return true;
}

// ── JSON ──────────────────────────────────────────────────────────────────────

void toJson(const Config& c, JsonObject o) {
    o["leds"] = c.ledCount;

    JsonObject ramp = o["ramp"].to<JsonObject>();
    ramp["step"] = c.rampStepMin;
    JsonArray lv = ramp["lv"].to<JsonArray>();
    for (uint8_t i = 0; i < kRampStages; ++i) lv.add(c.rampLevel[i]);

    JsonArray chs = o["ch"].to<JsonArray>();
    for (uint8_t i = 0; i < kChannels; ++i) {
        JsonObject ch = chs.add<JsonObject>();
        ch["on"]   = colorToText(c.ch[i].colorOn);
        ch["off"]  = colorToText(c.ch[i].colorOff);
        ch["mode"] = (uint8_t)c.ch[i].mode;
        JsonArray ps = ch["p"].to<JsonArray>();
        for (uint8_t k = 0; k < kPeriods; ++k) {
            JsonObject p = ps.add<JsonObject>();
            p["on"]  = minutesToText(c.ch[i].period[k].on);
            p["off"] = minutesToText(c.ch[i].period[k].off);
        }
    }

    JsonObject moon = o["moon"].to<JsonObject>();
    moon["ranges"] = rangesToText(c.moon);
    moon["color"]  = colorToText(c.moonColor);
    moon["mode"]   = (uint8_t)c.moonMode;

    o["tz"]  = c.tzHours;
    o["dst"] = c.dst;
    o["lat"] = c.latitude;
    o["lon"] = c.longitude;
}

bool fromJson(JsonObjectConst o, Config& c, String& err) {
    Config n = c;   // erst vollständig prüfen, dann übernehmen

    if (!o["leds"].isNull()) {
        const int v = o["leds"] | -1;
        if (v < 1 || v > kMaxLeds) { err = "LED-Anzahl muss 1–" + String(kMaxLeds) + " sein"; return false; }
        n.ledCount = v;
    }

    JsonObjectConst ramp = o["ramp"];
    if (ramp) {
        if (!ramp["step"].isNull()) {
            const int v = ramp["step"] | -1;
            if (v < 0 || v > 120) { err = "Dimmstufen-Dauer muss 0–120 min sein"; return false; }
            n.rampStepMin = v;
        }
        JsonArrayConst lv = ramp["lv"];
        if (lv) {
            if (lv.size() != kRampStages) { err = "Genau 3 Dimmstufen erwartet"; return false; }
            for (uint8_t i = 0; i < kRampStages; ++i) {
                const int v = lv[i] | -1;
                if (v < 1 || v > 100) { err = "Dimmstufen müssen 1–100 % sein"; return false; }
                n.rampLevel[i] = v;
            }
        }
    }

    JsonArrayConst chs = o["ch"];
    if (chs) {
        if (chs.size() != kChannels) { err = "Genau 4 Kanäle erwartet"; return false; }
        for (uint8_t i = 0; i < kChannels; ++i) {
            JsonObjectConst ch = chs[i];
            const String tag = "Kanal " + String(i + 1) + ": ";
            if (!textToColor(ch["on"] | "", n.ch[i].colorOn) || !textToColor(ch["off"] | "", n.ch[i].colorOff)) {
                err = tag + "ungültige Farbe"; return false;
            }
            const int mode = ch["mode"] | -1;
            if (mode < 0 || mode > 2) { err = tag + "ungültige Betriebsart"; return false; }
            n.ch[i].mode = (ChannelMode)mode;
            JsonArrayConst ps = ch["p"];
            if (ps.size() != kPeriods) { err = tag + "2 Schaltzeiträume erwartet"; return false; }
            for (uint8_t k = 0; k < kPeriods; ++k) {
                if (!textToMinutes(ps[k]["on"] | "", n.ch[i].period[k].on) ||
                    !textToMinutes(ps[k]["off"] | "", n.ch[i].period[k].off)) {
                    err = tag + "ungültige Uhrzeit"; return false;
                }
            }
        }
    }

    JsonObjectConst moon = o["moon"];
    if (moon) {
        if (!textToRanges(moon["ranges"] | "", n.moon, kMaxLeds, err)) return false;
        if (!textToColor(moon["color"] | "", n.moonColor)) { err = "Mond: ungültige Farbe"; return false; }
        const int mode = moon["mode"] | -1;
        if (mode < 0 || mode > 2) { err = "Mond: ungültige Betriebsart"; return false; }
        n.moonMode = (MoonMode)mode;
    }

    if (!o["tz"].isNull()) {
        const int v = o["tz"] | 99;
        if (v < -12 || v > 13) { err = "Zeitzone muss GMT-12 … GMT+13 sein"; return false; }
        n.tzHours = v;
    }
    if (!o["dst"].isNull()) n.dst = o["dst"] | true;
    if (!o["lat"].isNull() || !o["lon"].isNull()) {
        const float lat = o["lat"] | n.latitude;
        const float lon = o["lon"] | n.longitude;
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180) { err = "Ungültige Koordinaten"; return false; }
        n.latitude = lat; n.longitude = lon;
    }

    c = n;
    return true;
}

// ── NVS ───────────────────────────────────────────────────────────────────────

bool load(Config& c) {
    setDefaults(c);
    Preferences p;
    if (!p.begin(kNvsNs, true)) return false;
    const String json = p.getString(kNvsKey, "");
    p.end();
    if (!json.length()) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json)) { AWM_LOGW("⚠️ Aqua-Config in NVS ungültig → Defaults"); return false; }
    String err;
    if (!fromJson(doc.as<JsonObjectConst>(), c, err)) {
        AWM_LOGW("⚠️ Aqua-Config: %s → Defaults", err.c_str());
        setDefaults(c);
        return false;
    }
    return true;
}

bool save(const Config& c) {
    JsonDocument doc;
    toJson(c, doc.to<JsonObject>());
    String json;
    serializeJson(doc, json);
    Preferences p;
    if (!p.begin(kNvsNs, false)) return false;
    const size_t w = p.putString(kNvsKey, json);
    p.end();
    return w > 0;
}

} // namespace aqua
