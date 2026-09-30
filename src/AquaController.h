// SPDX-License-Identifier: MIT
// Aquarium-LED-Steuerung (WS2812B):
// - 4 Farbkanäle, LEDs reihum verteilt (LED 1,5,9… = Kanal 1 usw.)
// - Zeitsteuerung mit 2 Zeiträumen je Kanal und 3-stufiger Dimm-Rampe an den Schaltzeiten
// - Mondbeleuchtung mit Helligkeit nach Mondphase; Sonnenauf-/-untergang (sunMoon)
// - Wassertemperatur per DS18B20 (nicht-blockierend)
// - HTTP-Seiten /led und /config sowie API /api/aqua/* auf dem STA-Server
#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESPAsyncWebServer.h>
#include "AquaConfig.h"
#include "AquaClock.h"

namespace aqua {

class Controller {
public:
    Controller(uint8_t stripPin, uint8_t oneWirePin);

    void begin();                               // Config laden, Strip/Sensor/Uhr starten
    void loop(bool wifiConnected);              // nicht-blockierend, im loop() aufrufen
    void registerRoutes(AsyncWebServer& server);

private:
    struct ChannelState { bool on; uint8_t level; uint32_t color; };

    // Laufzeitstatus: vom loop() geschrieben, von HTTP-Handlern (AsyncTCP-Task) gelesen
    struct State {
        ChannelState ch[kChannels];
        bool    moonLit;
        uint8_t moonAge;        // 0–29 Tage
        uint8_t moonBright;     // 0–30 (/256)
        time_t  sunrise;        // Lokalzeit-Epoch; 0 = keine Angabe
        time_t  sunset;
        float   temp;           // °C; 999.99 = Sensorfehler
    };

    // Frame-Schlüssel: Strip nur neu senden, wenn sich etwas geändert hat
    struct FrameKey {
        uint32_t color[kChannels];
        uint32_t moonColor;
        LedRange moon[kMoonRanges];
        uint16_t ledCount;
        bool     moonLit;
        bool operator==(const FrameKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
    };

    Config getConfig() const;
    void   setConfig(const Config& c);
    State  getState() const;

    void    updateLight(const Config& c);
    uint8_t channelLevel(const Config& c, const Channel& ch, uint16_t minute) const;
    void    render(const Config& c, const State& s);
    void    updateAstro(const Config& c);
    void    updateTemp();

    void handleStatus(AsyncWebServerRequest* r);
    void handleSettingsGet(AsyncWebServerRequest* r);
    void handleSettingsPost(AsyncWebServerRequest* r, JsonVariant& json);

    Adafruit_NeoPixel _strip;
    OneWire           _ow;
    DallasTemperature _ds;
    DeviceAddress     _dsAddr = {};
    bool              _dsFound = false;
    Clock             _clock;

    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    Config   _cfg;
    uint32_t _cfgVersion = 0;
    State    _state = {};

    uint32_t _lastCfgVersion = UINT32_MAX;
    uint32_t _lastLight  = 0;
    uint32_t _lastAstro  = 0;
    bool     _astroValid = false;   // letzte Sonne/Mond-Berechnung mit gültiger Uhr
    uint32_t _lastShow   = 0;
    uint32_t _tempReqAt  = 0;
    uint32_t _tempScanAt = 0;
    FrameKey _lastFrame  = {};
    bool     _frameValid = false;
};

} // namespace aqua
