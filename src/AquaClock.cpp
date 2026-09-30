// SPDX-License-Identifier: MIT
// Uhr für die Aquarium-Steuerung (Zeitzone, EU-Sommerzeit, NTP-Status, manuelle Zeit)
#include "AquaClock.h"
#include "AWM_Logging.h"
#include "ESPWiFiManagerCommon.h"
#include <esp_sntp.h>
#include <sys/time.h>

namespace aqua {

static Clock* s_clock = nullptr;
static const uint32_t kNtpFailMs  = 30000;        // ohne Antwort → "fehlgeschlagen"
static const time_t   kValidEpoch = 1600000000;   // 2020-09-13: darunter gilt die Uhr als nicht gesetzt

// Tage seit 1970-01-01 für ein Datum (proleptischer Gregorianischer Kalender, H. Hinnant)
static long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const int  yoe = y - era * 400;
    const int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = (long)yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void Clock::begin() {
    s_clock = this;
    sntp_set_time_sync_notification_cb(&Clock::onSync);
}

// Läuft im SNTP-/tcpip-Task: nur Flags setzen
void Clock::onSync(struct timeval* tv) {
    if (!s_clock) return;
    s_clock->_lastSync           = tv ? tv->tv_sec : time(nullptr);
    s_clock->_everSynced         = true;
    s_clock->_syncedSinceRequest = true;
}

void Clock::loop(bool wifiConnected) {
    // Erste Verbindung: Sync läuft bereits (connectToWiFiSTA/checkReconnect) → nur überwachen
    if (wifiConnected && !_wasConnected && !_everSynced && !_requestedAt) {
        _requestedAt = millis() | 1;
    }
    _wasConnected = wifiConnected;

    // Neuer NTP-Server: SNTP im loop()-Task neu konfigurieren (nicht im AsyncTCP-Handler)
    if (_resyncPending && wifiConnected) {
        _resyncPending      = false;
        _syncedSinceRequest = false;
        _failed             = false;
        _requestedAt        = millis() | 1;
        syncTimeDefault(0);
        AWM_LOGI("🕒 NTP-Resync angefordert");
    }

    if (_requestedAt) {
        if (_syncedSinceRequest) {
            _requestedAt = 0;
        } else if (millis() - _requestedAt > kNtpFailMs) {
            if (!_failed) AWM_LOGW("⚠️ NTP-Synchronisierung fehlgeschlagen");
            _failed      = true;
            _requestedAt = 0;
        }
    }
    if (_syncedSinceRequest) _failed = false;   // spätere erfolgreiche Wiederholung durch SNTP
}

NtpStatus Clock::status() const {
    if (_failed)     return NtpStatus::FAILED;
    if (_everSynced) return NtpStatus::SYNCED;
    return NtpStatus::NEVER;
}

bool Clock::valid() const { return utc() > kValidEpoch; }

time_t Clock::makeEpoch(int y, int mon, int d, int h, int mi, int s) {
    return (time_t)daysFromCivil(y, mon, d) * 86400 + h * 3600 + mi * 60 + s;
}

time_t Clock::lastSundayUtc(int year, int month) {
    long days = daysFromCivil(year, month, 31);   // März und Oktober haben 31 Tage
    days -= (days + 4) % 7;                       // 1970-01-01 war ein Donnerstag; 0 = Sonntag
    return (time_t)days * 86400 + 3600;
}

bool Clock::isDst(time_t utc) const {
    struct tm g;
    gmtime_r(&utc, &g);
    const int year = g.tm_year + 1900;
    return utc >= lastSundayUtc(year, 3) && utc < lastSundayUtc(year, 10);
}

int Clock::offsetSeconds(time_t utc) const {
    return _tzHours * 3600 + ((_dst && isDst(utc)) ? 3600 : 0);
}

bool Clock::setLocal(time_t localEpoch) {
    if (localEpoch < kValidEpoch) return false;
    time_t u = localEpoch - _tzHours * 3600;
    if (_dst && isDst(u - 3600)) u -= 3600;
    struct timeval tv = { u, 0 };
    if (settimeofday(&tv, nullptr) != 0) return false;
    AWM_LOGI("🕒 Uhrzeit manuell gesetzt");
    return true;
}

} // namespace aqua
