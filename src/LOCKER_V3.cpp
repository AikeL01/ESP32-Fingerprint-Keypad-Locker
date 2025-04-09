#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <SimpleKeypad.h>
#include <Wire.h>
#include <LCD_I2C.h>
#include <EEPROM.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#define CONFIG_ESP_SYSTEM_PM_POWER_DOWN_CPU 1

struct PinConfig {
    static constexpr uint8_t RELAY = 13;
    static constexpr uint8_t FP_RX = 16;
    static constexpr uint8_t FP_TX = 17;
    static constexpr uint8_t BUZZER = 4;  // Using GPIO 32 for buzzer
    static constexpr uint8_t I2C_ADDR = 0x27;
    static constexpr uint8_t BUZZER_CHANNEL = 0;  // LEDC channel for buzzer
    static constexpr uint8_t BUZZER_RESOLUTION = 8;  // 8-bit resolution
    static constexpr uint32_t BUZZER_BASE_FREQ = 2000;  // Base frequency in Hz
    static constexpr uint8_t WAKE_PIN = 23;  // External wake-up pin
    static constexpr uint64_t KEYPAD_WAKE_PINS = ((1ULL << 32) | (1ULL << 33) | (1ULL << 25) | (1ULL << 26) | 
                                                 (1ULL << 27) | (1ULL << 14) | (1ULL << 12));  // Keypad pins for wake-up
};

struct Config {
    // System constants
    static constexpr uint32_t UART_BAUD_RATE = 57600;
    static constexpr uint16_t EEPROM_SIZE = 32;
    
    // Timing constants (ms)
    static constexpr unsigned long INACTIVITY_TIME = 8000;    // Screen timeout
    static constexpr uint16_t FINGERPRINT_TIMEOUT_MS = 10000; // Fingerprint operation timeout
    static constexpr unsigned long UNLOCK_TIME = 3000;        // Door unlock duration in ms
    static constexpr unsigned long LOCKOUT_TIME = 30000;      // Lockout duration in ms
    
    // Security parameters
    static constexpr uint8_t PIN_LENGTH = 6;
    static constexpr uint8_t STAR_THRESHOLD = 12;
    static constexpr uint8_t AUTH_MODE_ADDR = 10;
    static constexpr uint8_t MAX_WRONG_ATTEMPTS = 5;         // Maximum wrong attempts before lockout
    static constexpr char DEFAULT_PIN[7] = "123456";  // More secure default
    
    enum AuthMode {
        SINGLE_FACTOR = 0,
        TWO_FACTOR = 1
    };
};

// Define the static constexpr member
constexpr char Config::DEFAULT_PIN[7];

LCD_I2C lcd(PinConfig::I2C_ADDR, 16, 2);
HardwareSerial fingerprintSerial(2);
Adafruit_Fingerprint finger(&fingerprintSerial);

// Custom characters
byte lockChar[8] = {0b01110,0b10001,0b10001,0b11111,0b11011,0b11011,0b11111,0b00000};
byte unlockChar[8] = {0b01110,0b10000,0b10000,0b11111,0b11011,0b11011,0b11111,0b00000};
byte fingerChar[8] = {0b00000,0b00000,0b01110,0b11111,0b11111,0b11111,0b01110,0b00000};
byte halfLockChar[8] = {0b01110,0b10001,0b10000,0b11111,0b11011,0b11011,0b11111,0b00000}; // Half-lock for 2FA waiting
byte errorLockChar[8] = {0b01110,0b10101,0b10001,0b11111,0b11011,0b11011,0b11111,0b00000}; // Error lock with X pattern
byte emptyBarChar[8] = {B11111,B10001,B10001,B10001,B10001,B10001,B11111,B00000}; // Empty progress bar segment
byte filledBarChar[8] = {B11111,B11111,B11111,B11111,B11111,B11111,B11111,B00000}; // Filled progress bar segment

// Keypad setup
const byte ROWS = 4, COLS = 3;
char keys[ROWS * COLS] = {'1','2','3','4','5','6','7','8','9','*','0','#'};
byte rowPins[ROWS] = {32,33,25,26}, colPins[COLS] = {27,14,12};
SimpleKeypad keypad(keys, rowPins, colPins, ROWS, COLS);

// Add scanning delay configuration
const unsigned long KEY_SCAN_INTERVAL = 50; // 50ms between scans

// State variables using fixed buffer for better memory management
char input_password[Config::PIN_LENGTH + 1];  // +1 for null terminator
uint8_t input_length = 0;
int wrong_attempts = 0;
unsigned long last_activity = 0;
int star_count = 0;
int hash_count = 0;  // Counter for # presses

// Memory management globals
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Add these state variables after other globals
bool pin_verified = false;
bool fingerprint_verified = false;
uint8_t verified_fingerprint_id = 0;

// Add after other state variables
int wrong_pin_attempts = 0;
int wrong_fp_attempts = 0;
unsigned long pin_lockout_start = 0;
unsigned long fp_lockout_start = 0;
bool is_pin_locked_out = false;
bool is_fp_locked_out = false;

// Function declarations
void showReadyScreen();
void IRAM_ATTR unlockDoor();
void displayMessage(String line1, String line2, int delayTime = 0);
void IRAM_ATTR checkPassword();
void enrollFingerprint();
void deleteFingerprint();
String getInput(String prompt, char confirmKey, char clearKey, bool maskInput = true);
void setupPins();
void setupLCD();
void setupFingerprintSensor();
void IRAM_ATTR handleFingerprint();
void IRAM_ATTR handleKeypad();
void handleInactivity();
void displayMaskedInput();
void setPassword(const String &newPassword);
String getPassword();
void changePassword();
bool getFingerprintEnroll(uint8_t id);
uint8_t getFingerprintID();
bool initFingerprint();
void playTone(const uint16_t freq, const uint32_t dur);
void noTone();
void setAuthMode(Config::AuthMode mode);
Config::AuthMode getAuthMode();
void soundBuzzer(int pattern);

void setup() {
    // Initialize Serial communication
    Serial.begin(115200);
    Serial.println("System starting...");
    
    // Check wake-up cause and print detailed debug info
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    uint64_t ext1_wakeup_pins = 0;
    
    if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
        switch(wakeup_reason) {
            case ESP_SLEEP_WAKEUP_EXT0:
                Serial.println("Wake up from EXT0 (GPIO23)");
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ext1_wakeup_pins = esp_sleep_get_ext1_wakeup_status();
                Serial.printf("Wake up from EXT1 (Keypad). Pin mask: 0x%llx\n", ext1_wakeup_pins);
                break;
            default:
                Serial.printf("Wake up from other source: %d\n", wakeup_reason);
                break;
        }
    }
    
    // Initialize EEPROM with minimal size needed
    EEPROM.begin(Config::EEPROM_SIZE);
    
    setupPins();
    
    // Configure GPIO23 as input with strong pull-down for wake-up
    pinMode(PinConfig::WAKE_PIN, INPUT_PULLDOWN);
    rtc_gpio_pulldown_en((gpio_num_t)PinConfig::WAKE_PIN);
    
    // Configure keypad pins with pull-down
    const byte allPins[] = {32, 33, 25, 26, 27, 14, 12};  // All keypad pins
    for (byte pin : allPins) {
        pinMode(pin, INPUT_PULLDOWN);
        rtc_gpio_pulldown_en((gpio_num_t)pin);
    }
    
    // Initialize I2C and LCD
    Wire.begin();
    delay(100);  // Give I2C bus time to stabilize
    
    setupLCD();  // This will now handle all LCD initialization including custom chars
    
    setupFingerprintSensor();
    
    // Initialize default password if EEPROM is empty
    if (EEPROM.read(0) == 0xFF) {
        setPassword("123456");
        EEPROM.commit();
    }
    
    // Display wake-up source if from deep sleep
    if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
        String wakeMsg = "Wake: ";
        wakeMsg += (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) ? "GPIO23" : 
                   (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) ? "Keypad" : "Other";
        displayMessage(wakeMsg, "", 1000);
    }
    
    showReadyScreen();
    last_activity = millis();
}

void IRAM_ATTR loop() {
    // Critical section for state management
    static uint32_t lastInactivityCheck = 0;
    uint32_t now = millis();
    
    // Serial handling non-critical, move outside critical section
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command == "readpass") {
            Serial.println("Stored password: " + getPassword());
        }
    }

    // Handle time-sensitive tasks first
    handleKeypad();
    handleFingerprint();
    
    // Less critical tasks with optimized timing
    if (now - lastInactivityCheck >= 1000) {
        handleInactivity();
        lastInactivityCheck = now;
    }

    // Use shorter delay to improve responsiveness while maintaining power efficiency
    delayMicroseconds(100);
}

void setupPins() {
    pinMode(PinConfig::RELAY, OUTPUT);
    digitalWrite(PinConfig::RELAY, HIGH);

    // Configure buzzer pin for digital output
    pinMode(PinConfig::BUZZER, OUTPUT);
    digitalWrite(PinConfig::BUZZER, LOW);

    // Configure wake pin with pull-down only
    pinMode(PinConfig::WAKE_PIN, INPUT);
    gpio_set_pull_mode((gpio_num_t)PinConfig::WAKE_PIN, GPIO_PULLDOWN_ONLY);
    gpio_wakeup_enable((gpio_num_t)PinConfig::WAKE_PIN, GPIO_INTR_HIGH_LEVEL);

    // Configure keypad rows as outputs
    for(byte i = 0; i < ROWS; i++) {
        pinMode(rowPins[i], OUTPUT);
        digitalWrite(rowPins[i], HIGH);
    }
    
    // Configure keypad columns with pull-up only
    for(byte i = 0; i < COLS; i++) {
        pinMode(colPins[i], INPUT);
        gpio_set_pull_mode((gpio_num_t)colPins[i], GPIO_PULLUP_ONLY);
    }

    // Configure buzzer pin for PWM output using LEDC
    ledcSetup(PinConfig::BUZZER_CHANNEL, PinConfig::BUZZER_BASE_FREQ, PinConfig::BUZZER_RESOLUTION);
    ledcAttachPin(PinConfig::BUZZER, PinConfig::BUZZER_CHANNEL);
    ledcWrite(PinConfig::BUZZER_CHANNEL, 0);
}

void setupLCD() {
    Wire.begin();
    lcd.begin();
    lcd.backlight();  // Ensure backlight is on after wake-up
    
    // Create all custom characters in one place
    lcd.createChar(0, lockChar);
    lcd.createChar(1, unlockChar);
    lcd.createChar(2, fingerChar);
    lcd.createChar(3, halfLockChar);    // Half-lock for 2FA waiting
    lcd.createChar(4, errorLockChar);   // Error lock for lockout
    lcd.createChar(5, emptyBarChar);    // Empty progress bar segment
    lcd.createChar(6, filledBarChar);   // Filled progress bar segment

    displayMessage("  Waking Up...","");
    last_activity = millis();  // Reset activity timer after wake-up
}

void setupFingerprintSensor() {
    fingerprintSerial.begin(Config::UART_BAUD_RATE, SERIAL_8N1, PinConfig::FP_RX, PinConfig::FP_TX);
    delay(50);
    finger.begin(Config::UART_BAUD_RATE);

    if (initFingerprint()) {
        // Set high security level for better accuracy
        finger.setSecurityLevel(4);
    } else {
        displayMessage("Sensor Failed!","System limited",2000);
    }
}

void displaySensorParameters() {
    uint8_t p = finger.getParameters();
    if (p == FINGERPRINT_OK) {
        Serial.println("Sensor parameters:");
        Serial.print("Status: 0x");
        Serial.println(finger.status_reg, HEX);
        Serial.print("System ID: 0x");
        Serial.println(finger.system_id, HEX);
        Serial.print("Capacity: ");
        Serial.println(finger.capacity);
        Serial.print("Security level: ");
        Serial.println(finger.security_level);
        Serial.print("Device address: ");
        Serial.println(finger.device_addr, HEX);
        Serial.print("Packet length: ");
        Serial.println(finger.packet_len);
        Serial.print("Baud rate: ");
        Serial.println(finger.baud_rate);
    }
}

void IRAM_ATTR handleFingerprint() {
    static uint32_t lastCheck = 0;
    const uint32_t minCheckInterval = 100; // 100ms between checks for power efficiency
    
    uint32_t now = millis();
    if (now - lastCheck < minCheckInterval) return;
    lastCheck = now;
    
    portENTER_CRITICAL(&mux);
    bool isPinVerified = pin_verified;
    Config::AuthMode mode = getAuthMode();
    bool isPinLockedOut = is_pin_locked_out;
    portEXIT_CRITICAL(&mux);
    
    uint8_t fingerprintID = getFingerprintID();
    if (fingerprintID != 0) {
        if (mode == Config::TWO_FACTOR) {
            if (isPinVerified) {
                portENTER_CRITICAL(&mux);
                wrong_attempts = 0;
                pin_verified = false;
                fingerprint_verified = false;
                portEXIT_CRITICAL(&mux);
                
                displayMessage("ID #" + String(fingerprintID), "Access Granted");
                unlockDoor();
            } else {
                portENTER_CRITICAL(&mux);
                fingerprint_verified = true;
                verified_fingerprint_id = fingerprintID;
                portEXIT_CRITICAL(&mux);
                
                if (isPinLockedOut) {
                    displayMessage("Finger Verified", "Wait for PIN");
                    delay(2000);
                } else {
                    displayMessage("Fingerprint OK", "Enter PIN", 2000);
                }
            }
        } else {
            portENTER_CRITICAL(&mux);
            wrong_attempts = 0;
            portEXIT_CRITICAL(&mux);
            
            displayMessage("ID #" + String(fingerprintID), "Access Granted");
            unlockDoor();
        }
        last_activity = now;
        showReadyScreen();
    }
}

char scanKeypad() {
    static char lastKey = 0;
    static uint32_t lastDebounceTime = 0;
    const uint32_t debounceDelay = 50;
    
    // Set all rows to input with pull-up to start
    for(byte r = 0; r < ROWS; r++) {
        pinMode(rowPins[r], INPUT_PULLUP);
    }
    
    // Set all columns to input with pull-up
    for(byte c = 0; c < COLS; c++) {
        pinMode(colPins[c], INPUT_PULLUP);
    }
    
    // Scan each row
    for(byte r = 0; r < ROWS; r++) {
        // Set current row as OUTPUT LOW
        pinMode(rowPins[r], OUTPUT);
        digitalWrite(rowPins[r], LOW);
        delayMicroseconds(10);  // Small delay for signal to stabilize
        
        // Check each column
        for(byte c = 0; c < COLS; c++) {
            if(digitalRead(colPins[c]) == LOW) {  // Key is pressed
                uint32_t now = millis();
                // If enough time has passed, treat this as a new keypress
                if((now - lastDebounceTime) > debounceDelay) {
                    lastDebounceTime = now;
                    char key = keys[r * COLS + c];
                    if(key != lastKey) {
                        lastKey = key;
                        // Reset the row pin back to input pull-up
                        pinMode(rowPins[r], INPUT_PULLUP);
                        return key;
                    }
                }
                // Reset the row pin back to input pull-up
                pinMode(rowPins[r], INPUT_PULLUP);
                return 0;
            }
        }
        // Reset the row pin back to input pull-up
        pinMode(rowPins[r], INPUT_PULLUP);
    }
    lastKey = 0;
    return 0;
}

void IRAM_ATTR handleKeypad() {
    static uint32_t lastKeyCheck = 0;
    uint32_t now = millis();
    
    // Throttle scanning rate
    if(now - lastKeyCheck < KEY_SCAN_INTERVAL) return;
    lastKeyCheck = now;
    
    char key = scanKeypad();  // Use our custom scanning function
    if(!key) return;
    
    last_activity = now;
    lcd.backlight();

    // Check for PIN lockout status immediately
    if (is_pin_locked_out) {
        if (millis() - pin_lockout_start < Config::LOCKOUT_TIME) {
            unsigned long remainingTime = (Config::LOCKOUT_TIME - (millis() - pin_lockout_start)) / 1000;
            // In 2FA mode, show that fingerprint is still available
            if (getAuthMode() == Config::TWO_FACTOR && !is_fp_locked_out) {
                displayMessage("PIN Locked Out", String(remainingTime) + "s");
            } else {
                displayMessage("PIN Locked " + String(remainingTime) + "s", "Use Fingerprint");
            }
            soundBuzzer(1);
            delay(2000);
            showReadyScreen();
            return;
        } else {
            is_pin_locked_out = false;
            wrong_pin_attempts = 0;
        }
    }
    
    // Rest of the existing handleKeypad code...
    if(key == '*') {
        if(++star_count >= Config::STAR_THRESHOLD) {
            changePassword();
            star_count = 0;
        } else {
            input_length = 0;
            showReadyScreen();
        }
        return;
    }
    
    if (key == '#') {
        hash_count++;
        if (hash_count >= 12) {
            // First verify PIN
            String verifyPin = getInput("  PIN Required", '#', '*', true);
            
            if (verifyPin != getPassword()) {
                displayMessage("Access Denied", "", 2000);
                showReadyScreen();
                hash_count = 0;
                input_length = 0;
                return;
            }
            
            displayMessage("1:Enroll 2:Del", "3:Auth *:Exit");
            while (true) {
                char choice = keypad.getKey();
                if (choice == '1') {
                    enrollFingerprint();
                    break;
                } else if (choice == '2') {
                    deleteFingerprint();
                    break;
                } else if (choice == '3') {
                    // Toggle authentication mode
                    Config::AuthMode currentMode = getAuthMode();
                    Config::AuthMode newMode = currentMode == Config::SINGLE_FACTOR ? Config::TWO_FACTOR : Config::SINGLE_FACTOR;
                    setAuthMode(newMode);
                    displayMessage(newMode == Config::TWO_FACTOR ? "2FA Enabled" : "2FA Disabled", "", 2000);
                    showReadyScreen();
                    break;
                } else if (choice == '*') {
                    showReadyScreen();
                    break;
                }
                delay(10);
            }
            hash_count = 0;
            return;
        }
        if (input_length > 0) {
            checkPassword();
        }
        input_length = 0;
        return;
    }

    hash_count = 0;  // Reset hash counter on any other key
    if (input_length < Config::PIN_LENGTH) {
        input_password[input_length++] = key;
        displayMaskedInput();
        if (input_length >= Config::PIN_LENGTH) {
            checkPassword();
        }
    }
    star_count = 0;
}

void handleInactivity() {
    if (millis() - last_activity > Config::INACTIVITY_TIME) {
        // First dim the LCD
        lcd.noBacklight();
        
        // If another 5 seconds pass with no activity, go to deep sleep
        if (millis() - last_activity > Config::INACTIVITY_TIME + 5000) {
            // Prepare for deep sleep
            displayMessage("Enter Sleep", "Mode...");
            delay(1000);
            lcd.noBacklight();
            lcd.noDisplay();
            
            // Configure column pins as outputs driving HIGH and enable hold
            for (uint8_t pin : {27, 14, 12}) {  // Column pins
                pinMode(pin, OUTPUT);
                digitalWrite(pin, HIGH);
                rtc_gpio_init((gpio_num_t)pin);
                rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_OUTPUT_ONLY);
                rtc_gpio_set_level((gpio_num_t)pin, 1);
                rtc_gpio_hold_en((gpio_num_t)pin);
            }
            
            // Configure row pins (RTC-capable) for wake-up with strong pull-down
            const uint64_t row_pin_mask = (1ULL << 32) | (1ULL << 33) | (1ULL << 25) | (1ULL << 26);
            
            // Configure each row pin using RTC GPIO functions
            for (uint8_t pin : {32, 33, 25, 26}) {
                rtc_gpio_init((gpio_num_t)pin);
                rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_INPUT_ONLY);
                rtc_gpio_pulldown_en((gpio_num_t)pin);
                rtc_gpio_pullup_dis((gpio_num_t)pin);
                rtc_gpio_hold_en((gpio_num_t)pin);
            }
            
            // Enable wake-up on row pins going HIGH (when any key is pressed)
            esp_sleep_enable_ext1_wakeup(row_pin_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
            
            // Keep RTC peripherals powered
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
            
            Serial.println("Entering deep sleep...");
            Serial.flush();
            
            // Release all RTC GPIO holds before sleep
            rtc_gpio_hold_dis((gpio_num_t)27);
            rtc_gpio_hold_dis((gpio_num_t)14);
            rtc_gpio_hold_dis((gpio_num_t)12);
            for (uint8_t pin : {32, 33, 25, 26}) {
                rtc_gpio_hold_dis((gpio_num_t)pin);
            }
            
            delay(100);
            esp_deep_sleep_start();
        }
    } else {
        lcd.backlight();
    }
}

void displayMaskedInput() {
    static String lastInput = "";
    String currentInput = String(input_password).substring(0, input_length);
    if (currentInput != lastInput) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("      PIN:");
        lcd.setCursor(5, 1);
        for(int i = 0; i < input_length; i++) lcd.print("*");
        lastInput = currentInput;
    }
}

void playTone(const uint16_t freq, const uint32_t dur) {
    ledcSetup(PinConfig::BUZZER_CHANNEL, static_cast<uint32_t>(freq), PinConfig::BUZZER_RESOLUTION);
    ledcWrite(PinConfig::BUZZER_CHANNEL, 127); // 50% duty cycle
    delay(dur);
    ledcWrite(PinConfig::BUZZER_CHANNEL, 0);
}

void noTone() {
    ledcWrite(PinConfig::BUZZER_CHANNEL, 0);
}

void soundBuzzer(int pattern) {
    struct BuzzerPattern {
        uint16_t frequency;  // Frequency in Hz
        uint16_t duration;   // Duration in ms
        uint16_t pause;      // Pause after beep in ms
        uint8_t count;       // Number of beeps
    };
    
    static const BuzzerPattern patterns[] = {
        {2000, 100, 100, 2},  // Success - high pitched ascending beeps
        {400, 200, 100, 3},   // Error - very low pitched beeps (changed from 1000 to 400 Hz)
        {1500, 150, 200, 2},  // Warning - medium pitched alternating beeps
        {800, 100, 50, 9}     // Alarm - SOS pattern with low pitch
    };
    
    switch(pattern) {
        case 0: // Success - ascending beeps
            playTone(1800, patterns[0].duration);
            delay(patterns[0].pause);
            playTone(2000, patterns[0].duration);
            break;
            
        case 1: // Error - low pitched beeps
            for(int i = 0; i < patterns[1].count; i++) {
                playTone(400 - (i * 50), patterns[1].duration); // Starting at 400Hz and going lower
                delay(patterns[1].pause);
            }
            break;
            
        // ...existing warning and alarm cases...
        case 2: // Warning - alternating beeps
            for(int i = 0; i < patterns[2].count; i++) {
                playTone(i % 2 ? 1200 : 1800, patterns[2].duration);
                delay(patterns[2].pause);
            }
            break;
            
        case 3: // Alarm - SOS pattern
            // 3 short beeps
            for(int i = 0; i < 3; i++) {
                playTone(800, 100);
                delay(100);
            }
            delay(200);
            
            // 3 long beeps
            for(int i = 0; i < 3; i++) {
                playTone(800, 300);
                delay(100);
            }
            delay(200);
            
            // 3 short beeps
            for(int i = 0; i < 3; i++) {
                playTone(800, 100);
                delay(100);
            }
            break;
    }
    ledcWrite(PinConfig::BUZZER_CHANNEL, 0); // Ensure buzzer is off after pattern
}

void displayMessage(String line1, String line2, int delayTime) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
    delayTime > 0 ? delay(delayTime) : (void)0;
}

bool initFingerprint() {
    return finger.verifyPassword() ? (finger.getParameters(), true) :
           (displayMessage("Sensor Error!", "Trying alt pass..."),
            finger.setPassword(0x00000000),
            finger.verifyPassword() ? (finger.getParameters(), true) : false);
}

void showReadyScreen() {
    lcd.clear();
    lcd.setCursor(0, 0);
    
    if (is_pin_locked_out || is_fp_locked_out) {
        lcd.write(4);  // Error lock character
    } else if (pin_verified && !fingerprint_verified && getAuthMode() == Config::TWO_FACTOR) {
        lcd.write(3);  // Half-lock character for 2FA waiting state
    } else {
        lcd.write(0);  // Normal lock character
    }
    lcd.print("    Ready");

    // In 2FA mode, show verification status with progress bars
    if (getAuthMode() == Config::TWO_FACTOR) {
        lcd.setCursor(0, 1);
        lcd.print("P:");
        // Display three segments for PIN progress
        for (int i = 0; i < 3; i++) {
            lcd.write(pin_verified ? 6 : 5);  // filled or empty progress bar segment
        }
        lcd.print(" F:");
        // Display three segments for Fingerprint progress
        for (int i = 0; i < 3; i++) {
            lcd.write(fingerprint_verified ? 6 : 5);  // filled or empty progress bar segment
        }
    }
}

uint8_t getFingerprintID() {
    // Check if fingerprint is locked out but still allow PIN input in 2FA mode
    if (is_fp_locked_out) {
        if (millis() - fp_lockout_start < Config::LOCKOUT_TIME) {
            // Only show lockout message if actively trying to use fingerprint
            if (finger.getImage() == FINGERPRINT_OK) {
                unsigned long remainingTime = (Config::LOCKOUT_TIME - (millis() - fp_lockout_start)) / 1000;
                displayMessage("FP Locked Out", String(remainingTime) + "s");
                soundBuzzer(1);
                delay(2000);
                showReadyScreen();
            }
            return 0;
        } else {
            is_fp_locked_out = false;
            wrong_fp_attempts = 0;
        }
    }

    if (finger.getImage() != FINGERPRINT_OK) return 0;

    displayMessage("  Processing...","");
    last_activity = millis();

    if (finger.image2Tz() != FINGERPRINT_OK) {
        displayMessage("Image Error","Try again", 1500);
        showReadyScreen();
        return 0;
    }

    if (finger.fingerFastSearch() != FINGERPRINT_OK) {
        portENTER_CRITICAL(&mux);
        wrong_fp_attempts++;
        int remaining_attempts = Config::MAX_WRONG_ATTEMPTS - wrong_fp_attempts;
        
        if (wrong_fp_attempts >= Config::MAX_WRONG_ATTEMPTS) {
            is_fp_locked_out = true;
            fp_lockout_start = millis();
            portEXIT_CRITICAL(&mux);
            displayMessage("FP Locked 30s", "FP Locked 30s");
            soundBuzzer(3); // Use alarm sound
            delay(2000);
        } else {
            portEXIT_CRITICAL(&mux);
            displayMessage("No Match", String(remaining_attempts) + " tries left");
            soundBuzzer(1);
            delay(2000);
        }
        showReadyScreen();
        return 0;
    }

    // Reset wrong attempts on successful match
    portENTER_CRITICAL(&mux);
    wrong_fp_attempts = 0;
    portEXIT_CRITICAL(&mux);
    
    return finger.fingerID;
}

void IRAM_ATTR unlockDoor() {
    lcd.setCursor(15, 0);
    lcd.write(1);
    soundBuzzer(0);
    digitalWrite(PinConfig::RELAY, LOW);
    delay(Config::UNLOCK_TIME);
    digitalWrite(PinConfig::RELAY, HIGH);
}

String getInput(String prompt, char confirmKey, char clearKey, bool maskInput) {
    String input = "";
    lcd.clear();
    lcd.print(prompt);
    lcd.setCursor(0, 1);

    while (true) {
        char key = keypad.getKey();
        if (key) {
            if (key == confirmKey) break;
            if (key == clearKey) {
                input = "";
                lcd.setCursor(0, 1);
                lcd.print("                ");  // Clear line
                lcd.setCursor(0, 1);
            } else {
                input += key;
                String displayChar = String(maskInput ? '*' : key);
                lcd.print(displayChar);
            }
        }
        delay(10);
    }
    return input;
}

int getIDFromInput() {
    String idStr = getInput("Enter ID:", '#', '*', false);  // false means don't mask input
    return idStr.toInt();
}

void enrollFingerprint() {
    displayMessage("Enrollment Mode", "Enter ID:#");
    int id = getIDFromInput();

    if (id == 0) {
        displayMessage("ID #0 Invalid!", "Try Again", 2000);
        showReadyScreen();
    } else {
        displayMessage("Enrolling ID:" + String(id), "Place Finger");
        getFingerprintEnroll(id);
    }

    last_activity = millis();  // Reset activity timer after enrollment
    showReadyScreen();
}

bool captureFingerprintImage(uint8_t bufferID) {
    unsigned long startTime = millis();
    uint8_t p;
    while ((millis() - startTime) < Config::FINGERPRINT_TIMEOUT_MS) {
        if ((p = finger.getImage()) == FINGERPRINT_OK) {
            return (finger.image2Tz(bufferID) == FINGERPRINT_OK) ? 
                   true : 
                   (displayMessage("Image Error", "Try Again", 2000), false);
        }
        delay(100);
    }
    displayMessage("Timeout!", "Try Again", 2000);
    return false;
}

bool getFingerprintEnroll(uint8_t id) {
    if (!captureFingerprintImage(1)) return false;

    displayMessage("Got Image!", "Remove Finger");
    
    // Wait for finger removal
    unsigned long startTime = millis();
    while ((millis() - startTime) < 5000) {
        if (finger.getImage() == FINGERPRINT_NOFINGER) {
            delay(1000);  // Give time to fully remove finger
            break;
        }
        delay(100);
    }

    displayMessage("Place Same", "Finger Again");
    return !captureFingerprintImage(2) ? false :
           (displayMessage("Processing...", "Please Wait"),
            finger.createModel() != FINGERPRINT_OK ? 
            (displayMessage("Failed!", "Try Again", 2000), false) :
            finger.storeModel(id) != FINGERPRINT_OK ? 
            (displayMessage("Storage Failed!", "Try Again", 2000), false) :
            (displayMessage("Success!", "ID #" + String(id), 2000), true));
}

void deleteFingerprint() {
    int id = getIDFromInput();
    (finger.deleteModel(id) == FINGERPRINT_OK) ? 
        displayMessage("Deleted ID:", String(id), 2000) : 
        displayMessage("Failed to Delete", "Try Again", 2000);
    showReadyScreen();
}

void IRAM_ATTR checkPassword() {
    // Check if PIN is locked out
    if (is_pin_locked_out) {
        if (millis() - pin_lockout_start < Config::LOCKOUT_TIME) {
            unsigned long remainingTime = (Config::LOCKOUT_TIME - (millis() - pin_lockout_start)) / 1000;
            // In 2FA mode, show that fingerprint is still available
            if (getAuthMode() == Config::TWO_FACTOR && !is_fp_locked_out) {
                displayMessage("PIN Locked Out", String(remainingTime) + "s");
            } else {
                displayMessage("PIN Locked " + String(remainingTime) + "s", "Use Fingerprint");
            }
            soundBuzzer(1);
            delay(2000);
            showReadyScreen();
            return;
        } else {
            is_pin_locked_out = false;
            wrong_pin_attempts = 0;
        }
    }

    char storedPass[Config::PIN_LENGTH + 1];
    memset(storedPass, 0, sizeof(storedPass));
    
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < Config::PIN_LENGTH; i++) {
        storedPass[i] = EEPROM.read(i);
    }
    portEXIT_CRITICAL(&mux);
    
    bool match = true;
    for (int i = 0; i < input_length && i < Config::PIN_LENGTH; i++) {
        match = (input_password[i] == storedPass[i]) ? match : false;
    }
    
    if (match && input_length == Config::PIN_LENGTH) {
        if (getAuthMode() == Config::TWO_FACTOR) {
            if (fingerprint_verified) {
                // Fingerprint was already verified, grant access
                portENTER_CRITICAL(&mux);
                wrong_pin_attempts = 0;
                pin_verified = false;
                fingerprint_verified = false;
                portEXIT_CRITICAL(&mux);
                displayMessage(" PIN Verified", " Access Granted");
                unlockDoor();
            } else if (is_fp_locked_out) {
                // If fingerprint is locked out, still allow PIN verification
                pin_verified = true;
                displayMessage("PIN Verified", "Wait for FP", 2000);
                showReadyScreen();
            } else {
                pin_verified = true;
                displayMessage("PIN Verified", "Place Finger", 2000);
                showReadyScreen();
            }
        } else {
            // In single factor mode, correct PIN always grants access
            portENTER_CRITICAL(&mux);
            wrong_pin_attempts = 0;
            portEXIT_CRITICAL(&mux);
            displayMessage("     Access","    Granted");
            unlockDoor();
        }
    } else {
        portENTER_CRITICAL(&mux);
        wrong_pin_attempts++;
        int remaining_attempts = Config::MAX_WRONG_ATTEMPTS - wrong_pin_attempts;
        
        if (wrong_pin_attempts >= Config::MAX_WRONG_ATTEMPTS) {
            is_pin_locked_out = true;
            pin_lockout_start = millis();
            portEXIT_CRITICAL(&mux);
            // Even when PIN is locked, show a message indicating fingerprint is still available
            if (getAuthMode() == Config::TWO_FACTOR && !is_fp_locked_out) {
                displayMessage("PIN Locked 30s", "Use Fingerprint");
            } else {
                displayMessage("PIN Locked 30s", "PIN Locked 30s");
            }
            soundBuzzer(3); // Use alarm sound
            delay(2000);
        } else {
            portEXIT_CRITICAL(&mux);
            displayMessage("Invalid PIN", String(remaining_attempts) + " tries left");
            soundBuzzer(1);
            delay(2000);
        }
    }
    
    memset(storedPass, 0, sizeof(storedPass));
    input_length = 0;
    showReadyScreen();
}

void setPassword(const String &newPassword) {
    for (int i = 0; i < Config::PIN_LENGTH; i++) {
        EEPROM.write(i, i < newPassword.length() ? newPassword[i] : 0);
    }
    EEPROM.commit();
}

String getPassword() {
    String password = "";
    for (int i = 0; i < Config::PIN_LENGTH; i++) {
        char c = EEPROM.read(0 + i);
        if (c == 0) break;
        password += c;
    }
    return password;
}

void changePassword() {
    String currentPassword = getInput("  Current PIN:",'#','*', true);
    if (currentPassword != getPassword()) {
        displayMessage("   PIN Error","",2000);
        showReadyScreen();
        return;
    }
    
    String newPassword = getInput("    New PIN:", '#', '*', true);
    (newPassword.length() > 0 && newPassword.length() <= Config::PIN_LENGTH) ?
        (setPassword(newPassword), displayMessage("  PIN Updated","",2000)) :
        displayMessage("   PIN Error","   No Change",2000);
    
    last_activity = millis();
    showReadyScreen();
}

void setAuthMode(Config::AuthMode mode) {
    EEPROM.write(Config::AUTH_MODE_ADDR, mode);
    EEPROM.commit();
}

Config::AuthMode getAuthMode() {
    uint8_t mode = EEPROM.read(Config::AUTH_MODE_ADDR);
    return (mode == Config::TWO_FACTOR) ? Config::TWO_FACTOR : Config::SINGLE_FACTOR;
}
