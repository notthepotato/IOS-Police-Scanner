// Board pins and shared constants for the ESP32-2432S028R "Cheap Yellow Display".
#pragma once

namespace Pin {
// Display: ILI9341 on the HSPI bus
constexpr int TftSck = 14, TftMiso = 12, TftMosi = 13, TftCs = 15, TftDc = 2, TftBacklight = 21;
// Touch: XPT2046, bit-banged in software. The display and the SD card each need one of the
// ESP32's two hardware SPI buses, so touch (which needs very little speed) goes in software.
constexpr int TouchClk = 25, TouchMiso = 39, TouchMosi = 32, TouchCs = 33;
// SD card slot on the VSPI bus
constexpr int SdSck = 18, SdMiso = 19, SdMosi = 23, SdCs = 5;
// RGB LED on the back (active LOW), speaker amplifier ("SPEAK" connector), BOOT button
constexpr int LedRed = 4, LedGreen = 16, LedBlue = 17;
constexpr int Speaker = 26;
constexpr int BootButton = 0;
}  // namespace Pin

// Bluetooth UUIDs shared with the iPhone app (RoadScanner/SensorLink.swift). Change both together.
#define RSCAN_SERVICE_UUID "6E3A0001-5C1B-4B3D-9E1E-7A2F4C8D0B11"
#define RSCAN_ALERT_UUID   "6E3A0002-5C1B-4B3D-9E1E-7A2F4C8D0B11"  // board -> phone: detections
#define RSCAN_TIME_UUID    "6E3A0003-5C1B-4B3D-9E1E-7A2F4C8D0B11"  // phone -> board: clock time
#define RSCAN_COMMAND_UUID "6E3A0004-5C1B-4B3D-9E1E-7A2F4C8D0B11"  // phone -> board: e.g. "TEST,SPEAKER"

#define RSCAN_DEVICE_NAME "RScan"
