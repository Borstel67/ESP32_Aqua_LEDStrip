// SPDX-License-Identifier: MIT
// Aquarium-LED-Konfiguration:
// - 4 Farbkanäle (Farbe EIN/AUS, Betriebsart, zwei Schaltzeiträume)
// - Dimm-Rampe in 3 Stufen an den Schaltzeiten
// - Mondbeleuchtung (LED-Bereiche, Farbe, Betriebsart)
// - Zeitzone/Sommerzeit und Standort für Sonne/Mond
// Persistenz als JSON in NVS (Namespace "aqua"), getrennt von der WLAN-Config.
// Die Struktur ist bewusst POD, damit sie zwischen AsyncTCP- und loop()-Task
// unter einem Spinlock kopiert werden kann.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace aqua {

constexpr uint8_t  kChannels   = 4;     // ledanzcol: LEDs reihum auf die Kanäle verteilt
constexpr uint16_t kMaxLeds    = 300;
constexpr uint8_t  kMoonRanges = 3;
constexpr uint8_t  kRampStages = 3;
constexpr uint8_t  kPeriods    = 2;

enum class ChannelMode : uint8_t { CLOCK = 0, ON = 1, OFF = 2 };
enum class MoonMode    : uint8_t { AUTO = 0, ON = 1, OFF = 2 };

// Minuten seit Mitternacht; on == off → Zeitraum deaktiviert; on > off → über Mitternacht
struct Period   { uint16_t on; uint16_t off; };
// LED-Nummern 1-basiert, inklusiv; from == 0 → Eintrag unbenutzt
struct LedRange { uint16_t from; uint16_t to; };

struct Channel {
    uint32_t    colorOn;
    uint32_t    colorOff;
    ChannelMode mode;
    Period      period[kPeriods];
};

struct Config {
    uint16_t ledCount;
    Channel  ch[kChannels];

    uint8_t  rampStepMin;               // Minuten je Dimmstufe; 0 = hart schalten
    uint8_t  rampLevel[kRampStages];    // Helligkeit je Stufe in %

    LedRange moon[kMoonRanges];
    uint32_t moonColor;
    MoonMode moonMode;

    int8_t   tzHours;                   // -12 … +13 (Standardzeit)
    bool     dst;                       // EU-Sommerzeitregel anwenden
    float    latitude;
    float    longitude;
};

void setDefaults(Config& c);
bool load(Config& c);                   // false → Defaults aktiv
bool save(const Config& c);

// JSON für API und NVS (gleiches Format)
void toJson(const Config& c, JsonObject o);
// Übernimmt vorhandene Felder aus JSON in c; false + Fehlertext bei ungültigen Werten
bool fromJson(JsonObjectConst o, Config& c, String& err);

// Konvertierungen UI ↔ intern
String minutesToText(uint16_t m);                       // "HH:MM"
bool   textToMinutes(const char* s, uint16_t& out);
String colorToText(uint32_t rgb);                       // "#rrggbb"
bool   textToColor(const char* s, uint32_t& out);
String rangesToText(const LedRange* r);                 // "37-48;67-78;107"
bool   textToRanges(const char* s, LedRange* r, uint16_t maxLed, String& err);

} // namespace aqua
