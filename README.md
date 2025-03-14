# Fingerprint-Keypad-Locker

A secure and smart locker system built with **ESP32**, featuring **fingerprint authentication**, **keypad access**, and an **LCD display**. This project combines biometric security with traditional password-based access, providing a versatile and reliable solution for secure storage or door access.

---

## Features

- **Fingerprint Authentication**: Uses a AS608/ZA620-M5 fingerprint sensor for biometric access.
- **Keypad Access**: Supports password-based entry with a 4x3/4 keypad.
- **LCD Display**: Provides real-time feedback and system status on a 16x2 I2C LCD.
- **Relay Control**: Controls a lock mechanism via a relay module.
- **Inactivity Timeout**: Automatically turns off the LCD backlight after a period of inactivity.
- **Lockout Mode**: Temporarily locks the system after multiple failed access attempts.
- **Buzzer Feedback**: Provides audio feedback for successful or failed access attempts.

---

## Hardware Components

- **ESP32 Microcontroller**
- **AS608/ZA620-M5 Fingerprint Sensor**
- **4x3/4 Matrix Keypad**
- **16x2 I2C LCD Display**
- **Relay Module**
- **Piezo Buzzer**
- **Lock Mechanism** (e.g., solenoid lock)

---

## How It Works

1. **Fingerprint Access**: Place your finger on the sensor for biometric authentication.
2. **Keypad Access**: Enter a 4-digit password using the keypad.
3. **Access Granted**: If authentication is successful, the relay activates to unlock the door/locker.
4. **Access Denied**: Failed attempts trigger a buzzer and increment a counter. After 5 failed attempts, the system enters lockout mode for 30 seconds.

---

## Getting Started

1. **Clone the Repository**:
   ```bash
   git clone https://github.com/your-username/Fingerprint-Keypad-Locker.git
