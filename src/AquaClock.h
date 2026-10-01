// SPDX-License-Identifier: MIT
// Uhr für die Aquarium-Steuerung:
// - Systemzeit (UTC) per SNTP (UDP/123), Server aus WLAN-Config "ntp1" (leer = de.pool.ntp.org)
// - Lokalzeit = UTC + Zeitzone (Stunden) + 1 h Sommerzeit nach EU-Regel
//   (letzter Sonntag im März 01:00 UTC bis letzter Sonntag im Oktober 01:00 UTC)
// - Manuelle Zeiteinstellung (Lokalzeit → UTC → settimeofday)
// - NTP-Status: nie gesetzt / Synchronisierung fehlgeschlagen / synchronisiert
#pragma once
#include <Arduino.h>
#include <time.h>

namespace aqua {

enum class NtpStatus : uint8_t { NEVER = 0, FAILED = 1, SYNCED = 2 };

class Clock {
public:
    void begin();                       // SNTP-Callback registrieren
    void loop(bool wifiConnected);      // Status nachführen, angeforderte Resyncs ausführen

    void setZone(int8_t tzHours, bool dst) { _tzHours = tzHours; _dst = dst; }
    void requestResync()                   { _resyncPending = true; }   // thread-sicher (Flag)

    bool   valid() const;               // Uhrzeit gesetzt (NTP oder manuell)
    time_t utc() const                  { return time(nullptr); }
    time_t local() const                { const time_t u = utc(); return u + offsetSeconds(u); }
    int    offsetSeconds(time_t utc) const;
    bool   isDst(time_t utc) const;
    bool   setLocal(time_t localEpoch); // manuelle Zeiteinstellung

    // Datum/Uhrzeit → Epoch-Sekunden ohne Zeitzonenumrechnung
    static time_t makeEpoch(int y, int mon, int d, int h, int mi, int s);

    NtpStatus status() const;
    time_t    lastSyncUtc() const       { return _lastSync; }

private:
    static void onSync(struct timeval* tv);
    static time_t lastSundayUtc(int year, int month);   // 01:00 UTC

    volatile int8_t   _tzHours = 1;
    volatile bool     _dst     = true;

    volatile bool     _resyncPending = false;
    bool              _wasConnected  = false;
    uint32_t          _requestedAt   = 0;     // millis() der laufenden Anforderung; 0 = keine
    volatile bool     _syncedSinceRequest = false;
    volatile bool     _everSynced    = false;
    volatile time_t   _lastSync      = 0;
    bool              _failed        = false;
};

} // namespace aqua
