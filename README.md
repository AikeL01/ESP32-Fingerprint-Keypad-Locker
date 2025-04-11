# 🔒 ESP32 Biometric Locker System V3

A secure electronic locker system with dual-factor authentication powered by ESP32, featuring fingerprint and PIN verification.

![Locker System](https://raw.githubusercontent.com/AikeL01/ESP32-Fingerprint-Keypad-Locker/refs/heads/main/preview.png)

## ✨ Features

- **Dual Authentication Modes**:
  - Single-factor (PIN or Fingerprint)
  - Two-factor (PIN + Fingerprint)
- **Advanced Security**:
  - Separate lockout counters for PIN and fingerprint
  - Configurable lockout duration (30s default)
  - Masked PIN input with visual feedback
- **Power Management**:
  - Deep sleep mode with wake-on-keypad/finger
  - Auto-dimming display
- **Management Features**:
  - Fingerprint enrollment/deletion
  - PIN change functionality
  - Admin mode with verification
- **Audible Feedback**:
  - Distinct sound patterns for success/failure/warning

## 🛠️ Hardware Configuration

| Component | ESP32 Pin | Notes |
|-----------|----------|-------|
| Relay | GPIO13 | Controls lock mechanism |
| Fingerprint Sensor | RX:GPIO16, TX:GPIO17 |
| Buzzer | GPIO4 | PWM capable pin |
| Keypad Rows | GPIO32,33,25,26 | 4x3 matrix |
| Keypad Columns | GPIO27,14,12 | 4x3 matrix |
| LCD | I2C 0x27 | 16x2 character display |
| Wake Pin | GPIO36 | Fingerprint interrupt |

## 📋 System Configuration

```cpp
// Key parameters from LOCKER_V3.cpp
#define PIN_LENGTH 6                // 6-digit PIN
#define MAX_WRONG_ATTEMPTS 5        // Attempts before lockout
#define LOCKOUT_TIME 30000          // 30s lockout duration
#define INACTIVITY_TIME 8000        // 8s until display dims
#define UNLOCK_TIME 3000            // 3s unlock duration
#define STAR_THRESHOLD 12           // * presses for admin
