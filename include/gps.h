#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <math.h>
#include "config.h"

struct GpsData {
    double   latitude;
    double   longitude;
    float    speed_kmh;
    float    altitude_m;
    float    hdop;
    float    course_deg;
    uint8_t  satellites;
    bool     valid;
};

// Hitung jarak dua koordinat dalam meter (Haversine formula)
inline double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * DEG_TO_RAD;
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dLon / 2) * sin(dLon / 2);
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

class GpsManager {
public:
    GpsManager() : _serial(1) {}

    void begin() {
        pinMode(PIN_VEXT, OUTPUT);
        digitalWrite(PIN_VEXT, LOW);       // LOW = ON untuk Heltec Wireless Tracker

        pinMode(PIN_GPS_PWR, OUTPUT);
        digitalWrite(PIN_GPS_PWR, HIGH);   // HIGH = GPS power ON
        pinMode(PIN_GPS_RST, OUTPUT);
        digitalWrite(PIN_GPS_RST, HIGH);   // HIGH = release UC6580 from reset

        delay(500);
        _serial.begin(GPS_BAUD_RATE, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[GPS] Initialized.");
    }

    // Non-blocking: drain serial buffer dan feed ke TinyGPS.
    // Panggil sesering mungkin di loop() agar data GPS selalu fresh.
    void update() {
        while (_serial.available()) {
            _gps.encode(_serial.read());
        }
    }

    // Apakah GPS sudah punya fix valid?
    bool hasFix() {
        return _gps.location.isValid() &&
               _gps.location.age() < 2000 &&
               _gps.satellites.isValid() &&
               _gps.satellites.value() >= GPS_MIN_SATELLITES;
    }

    // Matikan GPS sebelum deep sleep — hemat ~20mA
    void powerDown() {
        _serial.end();
        digitalWrite(PIN_GPS_PWR, LOW);
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[GPS] Power down.");
    }

    // Ambil data GPS terkini (panggil hasFix() dulu)
    GpsData getCurrent() {
        GpsData d;
        d.valid      = hasFix();
        d.latitude   = _gps.location.lat();
        d.longitude  = _gps.location.lng();
        d.speed_kmh  = _gps.speed.isValid()      ? _gps.speed.kmph()      : 0.0f;
        d.altitude_m = _gps.altitude.isValid()   ? _gps.altitude.meters() : 0.0f;
        d.hdop       = _gps.hdop.isValid()       ? _gps.hdop.hdop()       : 99.0f;
        d.course_deg = _gps.course.isValid()     ? _gps.course.deg()      : 0.0f;
        d.satellites = _gps.satellites.isValid() ? _gps.satellites.value(): 0;
        return d;
    }


private:
    TinyGPSPlus    _gps;
    HardwareSerial _serial;
};
