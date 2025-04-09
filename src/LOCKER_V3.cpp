#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <SimpleKeypad.h>
#include <Wire.h>
#include <LCD_I2C.h>
#include <EEPROM.h>

#define CONFIG_ESP_SYSTEM_PM_POWER_DOWN_CPU 1

struct PinConfig {
    static constexpr uint8_t RELAY = 13;
    static constexpr uint8_t FP_RX = 16;
    static constexpr uint8_t FP_TX = 17;
    static constexpr uint8_t BUZZER = 5;
    static constexpr uint8_t I2C_ADDR = 0x27;
};

struct Config {
    // System constants
    static constexpr uint32_t UART_BAUD_RATE = 57600;
    static constexpr uint16_t EEPROM_SIZE = 32;
    
    // Timing constants (ms)
    static constexpr unsigned long LOCKOUT_TIME = 300000;     // Increased to 5 minutes for better security
    static constexpr unsigned long INACTIVITY_TIME = 8000;    // Power saving timeout
    static constexpr uint16_t DEEP_SLEEP_DELAY = 16000;      // Time before deep sleep
    static constexpr uint16_t FINGERPRINT_TIMEOUT_MS = 10000; // Fingerprint operation timeout
    static constexpr unsigned long UNLOCK_TIME = 3000;        // Door unlock duration in ms
    
    // Security parameters
    static constexpr uint8_t MAX_ATTEMPTS = 5;
    static constexpr uint8_t PIN_LENGTH = 6;
    static constexpr uint8_t STAR_THRESHOLD = 12;
    static constexpr uint8_t AUTH_MODE_ADDR = 10;
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

// Keypad setup
const byte ROWS = 4, COLS = 3;
char keys[ROWS * COLS] = {'1','2','3','4','5','6','7','8','9','*','0','#'};
byte rowPins[ROWS] = {32,33,25,26}, colPins[COLS] = {27,14,12};
SimpleKeypad keypad(keys, rowPins, colPins, ROWS, COLS);

// Add scanning delay configuration
const unsigned long KEY_SCAN_INTERVAL = 20; // 20ms between scans
const unsigned long DEBOUNCE_DELAY = 50;    // 50ms debounce

// State variables using fixed buffer for better memory management
char input_password[Config::PIN_LENGTH + 1];  // +1 for null terminator
uint8_t input_length = 0;
int wrong_attempts = 0;
bool lockout_mode = false;
unsigned long last_activity = 0;
unsigned long lockout_start = 0;
int star_count = 0;
int hash_count = 0;  // Counter for # presses

// Memory management globals
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Add these state variables after other globals
bool pin_verified = false;
bool fingerprint_verified = false;
uint8_t verified_fingerprint_id = 0;

// Function declarations
void showReadyScreen();
void IRAM_ATTR unlockDoor();
void displayMessage(String line1, String line2, int delayTime = 0);
void IRAM_ATTR checkPassword();
void activateLockoutMode();
void enrollFingerprint();
void deleteFingerprint();
String getInput(String prompt, char confirmKey, char clearKey, bool maskInput = true);
void setupPins();
void setupLCD();
void setupFingerprintSensor();
void handleLockoutMode();
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
void handleHibernation();
bool captureFingerprintImage(uint8_t bufferID);
void setAuthMode(Config::AuthMode mode);
Config::AuthMode getAuthMode();
void enterLightSleep();

void setup() {
    // Initialize Serial communication
    Serial.begin(115200);
    Serial.println("System starting...");
    
    // Set CPU frequency to 80MHz to save power while maintaining good performance
    setCpuFrequencyMhz(80);
    
    // Initialize EEPROM with minimal size needed
    EEPROM.begin(Config::EEPROM_SIZE);
    
    setupPins();
    
    // Re-initialize I2C and LCD after deep sleep
    Wire.begin();
    delay(100);  // Give I2C bus time to stabilize
    
    lcd.begin();
    lcd.display();  // Ensure display is on
    lcd.backlight();  // Turn on backlight
    lcd.createChar(0, lockChar);
    lcd.createChar(1, unlockChar);
    lcd.createChar(2, fingerChar);
    
    setupFingerprintSensor();
    
    // Initialize default password if EEPROM is empty
    if (EEPROM.read(0) == 0xFF) {
        // Write default password 123456
        setPassword("123456");
        EEPROM.commit();
    }
    
    // Configure GPIO 34 as wake-up source with pull-up
    gpio_pullup_en((gpio_num_t)34);
    gpio_pulldown_dis((gpio_num_t)34);
    
    // Configure sleep wakeup sources (deep sleep)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)34, 1);  // Wake up when GPIO34 goes HIGH
    esp_sleep_enable_timer_wakeup(Config::DEEP_SLEEP_DELAY * 1000ULL);
    
    showReadyScreen();
    last_activity = millis();
}

void IRAM_ATTR loop() {
    // Critical tasks with higher priority
    portENTER_CRITICAL(&mux);
    if (lockout_mode) {
        handleLockoutMode();
        portEXIT_CRITICAL(&mux);
        return;
    }
    portEXIT_CRITICAL(&mux);

    // Check for serial commands
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        
        if (command == "readpass") {
            String storedPass = getPassword();
            Serial.println("Stored password: " + storedPass);
        }
    }

    // Handle time-sensitive tasks first
    handleKeypad();
    handleFingerprint();
    
    // Less critical tasks
    static uint32_t lastInactivityCheck = 0;
    if (millis() - lastInactivityCheck >= 1000) {  // Check every second
        handleInactivity();
        lastInactivityCheck = millis();
    }

    // Power management task
    static uint32_t lastPowerCheck = 0;
    if (millis() - lastPowerCheck >= 2000) {  // Check every 2 seconds
        if (millis() - last_activity > Config::INACTIVITY_TIME * 2) {
            handleHibernation();
        }
        lastPowerCheck = millis();
    }
    
    // Reduced delay for better responsiveness
    delay(5);
}

void setupPins() {
    pinMode(PinConfig::RELAY, OUTPUT);
    digitalWrite(PinConfig::RELAY, HIGH);

    pinMode(PinConfig::BUZZER, OUTPUT);
    digitalWrite(PinConfig::BUZZER, LOW);  // Ensure buzzer is off

    // Configure keypad pins - start with columns as inputs with pull-ups
    for(byte i = 0; i < COLS; i++) {
        pinMode(colPins[i], INPUT_PULLUP);
    }
    
    // Configure rows as outputs initially LOW
    for(byte i = 0; i < ROWS; i++) {
        pinMode(rowPins[i], OUTPUT);
        digitalWrite(rowPins[i], LOW);
    }
}

void setupLCD() {
    Wire.begin();
    lcd.begin();
    lcd.backlight();  // Ensure backlight is on after wake-up
    lcd.createChar(0, lockChar);
    lcd.createChar(1, unlockChar);
    lcd.createChar(2, fingerChar);

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

void handleLockoutMode() {
    static bool init = true;
    if (millis() - lockout_start >= Config::LOCKOUT_TIME) {
        lockout_mode = init = false;
        wrong_attempts = 0;
        last_activity = millis();  // Reset activity timer after lockout ends
        showReadyScreen();
        return;
    }
    
    int bar = ((Config::LOCKOUT_TIME - (millis() - lockout_start)) * 16) / Config::LOCKOUT_TIME;
    if (init) {
        lcd.clear();
        lcd.print("    Lockout:");
        init = false;
    }
    lcd.setCursor(0, 1);
    for(byte i = 0; i < 16; i++) lcd.write(i < bar ? 0xFF : 32);
}

void IRAM_ATTR handleFingerprint() {
    static uint32_t lastCheck = 0;
    uint32_t now = millis();
    
    // Throttle fingerprint checks to save power
    if (now - lastCheck < 100) return;  // Check max 10 times per second
    lastCheck = now;
    
    uint8_t fingerprintID = getFingerprintID();
    if (fingerprintID != 0) {
        if (getAuthMode() == Config::TWO_FACTOR) {
            if (pin_verified) {
                // PIN was already verified, grant access
                portENTER_CRITICAL(&mux);
                wrong_attempts = 0;
                pin_verified = false;  // Reset state
                fingerprint_verified = false;
                portEXIT_CRITICAL(&mux);
                
                displayMessage("ID #" + String(fingerprintID) + " Match!", "Access Granted");
                unlockDoor();
                showReadyScreen();
            } else {
                // Store fingerprint verification and wait for PIN
                fingerprint_verified = true;
                verified_fingerprint_id = fingerprintID;
                displayMessage("Fingerprint OK", "Enter PIN", 2000);
                showReadyScreen();
            }
        } else {
            // Single factor mode - grant access immediately
            portENTER_CRITICAL(&mux);
            wrong_attempts = 0;
            portEXIT_CRITICAL(&mux);
            
            displayMessage("ID #" + String(fingerprintID) + " Match!", "Access Granted");
            unlockDoor();
            showReadyScreen();
        }
        last_activity = now;
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
        lcd.noBacklight();
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
        lcd.setCursor(5, 1);  // Start masked input at the 6th column
        for (int i = 0; i < input_length; i++) {
            lcd.print("*");
        }
        lastInput = currentInput;
    }
}

void soundBuzzer(int pattern) {
    static const int patterns[][2] = {
        {1000, 2000}, // Success
        {300, 0},     // Error  
        {500, 500},   // Warning
        {800, 600}    // Alarm
    };
    
    static const int repeats[] = {1,1,2,5};
    
    for(int i = 0; i < repeats[pattern]; i++) {
        tone(PinConfig::BUZZER, patterns[pattern][0], 100);
        delay(100);
        if(patterns[pattern][1]) {
            tone(PinConfig::BUZZER, patterns[pattern][1], 100);
            delay(100);
        }
    }
}

void displayMessage(String line1, String line2, int delayTime) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
    if (delayTime > 0) delay(delayTime);
}

bool initFingerprint() {
    if (finger.verifyPassword()) {
        finger.getParameters(); // Cache parameters
        return true;
    }

    displayMessage("Sensor Error!", "Trying alt pass...");
    finger.setPassword(0x00000000);
    if (finger.verifyPassword()) {
        finger.getParameters(); // Cache parameters
        return true;
    }
    return false;
}

void showReadyScreen() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.write(0);  // Lock character
    lcd.print("    Ready");
}

uint8_t getFingerprintID() {
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) return 0;

    displayMessage("  Processing...","");

    last_activity = millis();

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
        displayMessage("Image Error","Try again", 1500);
        showReadyScreen();
        return 0;
    }

    p = finger.fingerFastSearch();
    if (p != FINGERPRINT_OK) {
        displayMessage("    No Match"," Access Denied");
        soundBuzzer(1);  // Short error beep
        
        wrong_attempts++;
        
        if (wrong_attempts >= Config::MAX_ATTEMPTS) {
            activateLockoutMode();
        } else {
            delay(2000);
            showReadyScreen();
        }
        
        return 0;
    }

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
                String displayChar = maskInput ? String("*") : String(key);
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
        return;
    }

    displayMessage("Enrolling ID:" + String(id), "Place Finger");

    while (!getFingerprintEnroll(id))
        ;

    last_activity = millis();  // Reset activity timer after enrollment
    showReadyScreen();
}

bool captureFingerprintImage(uint8_t bufferID) {
    unsigned long startTime = millis();
    uint8_t p;
    while ((millis() - startTime) < Config::FINGERPRINT_TIMEOUT_MS) { // Timeout based on Config
        if ((p = finger.getImage()) == FINGERPRINT_OK) {
            if (finger.image2Tz(bufferID) != FINGERPRINT_OK) {
                displayMessage("Image Error", "Try Again", 2000);
                return false;
            }
            return true;
        }
        delay(100);
    }
    displayMessage("Timeout!", "Try Again", 2000);
    return false;
}

bool getFingerprintEnroll(uint8_t id) {
    if (!captureFingerprintImage(1)) {
        return false;
    }

    displayMessage("Got Image!", "Remove Finger");
    
    // Wait for finger removal with timeout
    unsigned long startTime = millis();
    while ((millis() - startTime) < 5000) { // 5 second timeout
        if (finger.getImage() == FINGERPRINT_NOFINGER) {
            delay(1000); // Give user time to fully remove finger
            break;
        }
        delay(100);
    }

    displayMessage("Place Same", "Finger Again");
    
    if (!captureFingerprintImage(2)) {
        return false;
    }

    displayMessage("Processing...", "Please Wait");
    uint8_t p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        displayMessage("Failed!", "Try Again", 2000);
        return false;
    }

    p = finger.storeModel(id);
    if (p != FINGERPRINT_OK) {
        displayMessage("Storage Failed!", "Try Again", 2000);
        return false;
    }

    displayMessage("Success!", "ID #" + String(id), 2000);
    return true;
}

void deleteFingerprint() {
    int id = getIDFromInput();

    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        displayMessage("Deleted ID:", String(id), 2000);
    } else {
        displayMessage("Failed to Delete", "Try Again", 2000);
    }

    showReadyScreen();
}

void IRAM_ATTR checkPassword() {
    String storedPassword = getPassword();
    String currentInput = String(input_password).substring(0, input_length);
    
    if (currentInput.length() != storedPassword.length()) {
        wrong_attempts++;
        if (wrong_attempts >= Config::MAX_ATTEMPTS) {
            activateLockoutMode();
        } else {
            displayMessage("Invalid Length", String(currentInput.length()) + "!=" + String(storedPassword.length()), 2000);
            soundBuzzer(1);
        }
        input_length = 0;
        if (!lockout_mode) showReadyScreen();
        return;
    }
    
    if (currentInput == storedPassword) {
        if (getAuthMode() == Config::TWO_FACTOR) {
            if (fingerprint_verified) {
                // Fingerprint was already verified, grant access
                displayMessage("  PIN Verified", " Access Granted");
                wrong_attempts = 0;
                pin_verified = false;  // Reset state
                fingerprint_verified = false;
                unlockDoor();
            } else {
                // Store PIN verification and wait for fingerprint
                pin_verified = true;
                displayMessage("PIN Verified", "Place Finger", 2000);
                showReadyScreen();
            }
        } else {
            // Single factor mode - grant access immediately
            displayMessage("     Access","    Granted");
            wrong_attempts = 0;
            unlockDoor();
        }
    } else {
        wrong_attempts++;
        if (wrong_attempts >= Config::MAX_ATTEMPTS) {
            activateLockoutMode();
        } else {
            displayMessage("      PIN:","    Invalid");
            soundBuzzer(1);
            delay(2000);
        }
    }
    input_length = 0;
    if (!lockout_mode) showReadyScreen();
}

void activateLockoutMode() {
    lockout_mode = true;
    lockout_start = millis();
    soundBuzzer(3);  // Long alarm pattern
}

void setPassword(const String &newPassword) {
    // Clear the password area first
    for (int i = 0; i < Config::PIN_LENGTH; i++) {
        EEPROM.write(i, 0);
    }
    
    // Write new password
    for (int i = 0; i < newPassword.length() && i < Config::PIN_LENGTH; i++) {
        EEPROM.write(i, newPassword[i]);
    }
    EEPROM.commit();
}

String getPassword() {
    String password = "";
    for (int i = 0; i < Config::PIN_LENGTH; i++) {
        char c = EEPROM.read(0 + i);
        if (c == 0) break;  // Stop at null terminator
        password += c;
    }
    return password;
}

void changePassword() {
    String currentPassword = getInput("  Current PIN:",'#','*', true);  // true for masking PIN
    if (currentPassword != getPassword()) {
        displayMessage("   PIN Error","",2000);
        showReadyScreen();
        return;
    }

    String newPassword = getInput("    New PIN:", '#', '*', true);  // true for masking PIN
    if (newPassword.length() > 0 && newPassword.length() <= Config::PIN_LENGTH) {
        setPassword(newPassword);
        displayMessage("  PIN Updated","",2000);
    } else {
        displayMessage("   PIN Error","   No Change",2000);
    }
    last_activity = millis();  // Reset activity timer after PIN change
    showReadyScreen();
}

void handleHibernation() {
    // Power down peripherals before deep sleep
    Wire.end();  // Shutdown I2C
    fingerprintSerial.end();  // Shutdown UART
    
    lcd.noBacklight();
    displayMessage(" Hibernating...", "", 1000);
    lcd.noDisplay();  // Turn off LCD display
    
    // Configure deep sleep power domains
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // Keep RTC peripherals on
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
    
    // Configure GPIO 34 as wake-up source with pull-up
    gpio_pullup_en((gpio_num_t)34);
    gpio_pulldown_dis((gpio_num_t)34);
    
    // Configure wake-up sources
    esp_sleep_enable_ext0_wakeup((gpio_num_t)34, 1);  // Wake up when GPIO34 goes HIGH
    esp_sleep_enable_timer_wakeup(Config::DEEP_SLEEP_DELAY * 1000ULL);
    
    esp_deep_sleep_start();
}

void enterLightSleep() {
    esp_sleep_enable_timer_wakeup(1000000);  // 1 second
    esp_light_sleep_start();
}

void setAuthMode(Config::AuthMode mode) {
    EEPROM.write(Config::AUTH_MODE_ADDR, mode);
    EEPROM.commit();
}

Config::AuthMode getAuthMode() {
    uint8_t mode = EEPROM.read(Config::AUTH_MODE_ADDR);
    return mode == Config::TWO_FACTOR ? Config::TWO_FACTOR : Config::SINGLE_FACTOR;
}
