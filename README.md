# 🔒 ESP32 Biometric Locker System

A secure, reliable electronic locker system powered by ESP32 with fingerprint and keypad authentication.

![Locker System](https://raw.githubusercontent.com/AikeL01/ESP32-Fingerprint-Keypad-Locker/refs/heads/main/preview.png)

## ✨ Features

- **Dual Authentication**: Fingerprint sensor (ZW101) and keypad options
- **LCD Display**: Real-time status and feedback via I2C LCD
- **Security Features**: 
  - Lockout after multiple failed attempts
  - Auto-dimming display to save power
  - Masked password input
- **Audible Feedback**: Different sound patterns for various actions
- **Serial Management**: Enroll/delete fingerprints via serial commands

## 🛠️ Hardware

- ESP32 development board
- ZA620_M5 fingerprint sensor
- 4x3 keypad matrix
- 16x2 I2C LCD display
- Relay module for lock control
- Piezo buzzer for audio feedback

## 📋 Pin Configuration

| Component | Pins |
|-----------|------|
| Relay | GPIO26 |
| Fingerprint RX/TX | GPIO16/GPIO17 |
| Buzzer | GPIO15 |
| Keypad Rows | GPIO2, GPIO0, GPIO4, GPIO5 |
| Keypad Columns | GPIO18, GPIO19, GPIO23 |
| LCD | I2C (SDA/SCL default pins) |

## 🚀 Setup Instructions

1. Connect all components according to the pin configuration
2. Install the required libraries:
   - Adafruit Fingerprint Library
   - SimpleKeypad Library
   - LCD_I2C Library
3. Upload the code to your ESP32
4. Open the serial monitor at 115200 baud to view system status

## 💻 Usage

### Unlock Methods

- **Fingerprint**: Place registered finger on the sensor
- **Keypad**: Enter the 4-digit PIN (default: 0000)

### Management Commands

Send these commands via the Serial Monitor:

- `e` - Enroll a new fingerprint
- `d` - Delete a stored fingerprint

## 🔐 Security Features

- **Lockout System**: After 5 incorrect attempts, system locks for 30 seconds
- **Inactivity Timeout**: Display backlight turns off after 10 seconds of inactivity
- **Secure Input**: Password input is masked on the display

## 🧩 System Workflow

```mermaid
flowchart TD
    A([Start]) --> B[System Initialization]
    B --> C[Ready Screen]
    C --> D{User Action?}
    
    D -->|Fingerprint Scan| E[/Scan Fingerprint/]
    D -->|Keypad Input| F[/Enter Password/]
    D -->|Press ******| G[Change Password]
    D -->|Serial Command| H[Admin Functions]
    
    E --> I{Fingerprint\nValid?}
    I -->|Yes| J[[Unlock Door]]
    I -->|No| K[Increment Attempts]
    K --> L{Max Attempts\nReached?}
    L -->|Yes| M[Lockout Mode]
    L -->|No| C
    
    F --> N{Password\nCorrect?}
    N -->|Yes| J
    N -->|No| K
    
    G --> O{Authentication\nSuccess?}
    O -->|Yes| P[/Enter New Password/]
    O -->|No| C
    P --> Q[[Save New Password]]
    Q --> C
    
    H --> R{Command Type?}
    R -->|Enroll| S[/Enroll Fingerprint/]
    R -->|Delete| T[/Delete Fingerprint/]
    R -->|Change PIN| G
    
    J --> U[Timer Countdown]
    U --> V[[Relock Door]]
    V --> C
    
    M --> W[Countdown Timer]
    W -->|Time Expired| C
    
    %% Shape Legend
    style A fill:#4CAF50,stroke:#388E3C
    style J fill:#2196F3,stroke:#1976D2
    style M fill:#F44336,stroke:#D32F2F
    style G fill:#FF9800,stroke:#F57C00
    style H fill:#9C27B0,stroke:#7B1FA2
    
    %% Proper shape usage
    classDef startEnd fill:#4CAF50,stroke:#388E3C,color:white
    classDef process fill:#f5f5f5,stroke:#333
    classDef decision fill:#FFEB3B,stroke:#FBC02D
    classDef io fill:#B3E5FC,stroke:#03A9F4
    classDef subroutine fill:#C8E6C9,stroke:#4CAF50
    classDef terminator fill:#E1BEE7,stroke:#9C27B0
    
    class A startEnd
    class B,C,F,E,S,T,P process
    class D,I,L,N,O,R decision
    class J,V,Q subroutine
    class G,H terminator
    class M,W io
```

## 📝 Customization

- Change the default password by modifying `DEFAULT_PASSWORD`
- Adjust the lockout duration by changing `LOCKOUT_DURATION`
- Modify the number of allowed attempts by changing `MAX_WRONG_ATTEMPTS`
- Customize inactivity timeout with `INACTIVITY_TIMEOUT`

## 📜 License

This project is licensed under the MIT License - see the LICENSE file for details.

## 🙏 Acknowledgements

- Adafruit for their excellent fingerprint sensor library

---

Made with ❤️ by Aikel01
