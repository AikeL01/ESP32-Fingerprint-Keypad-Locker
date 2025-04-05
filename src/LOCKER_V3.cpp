#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <SimpleKeypad.h>
#include <Wire.h>
#include <LCD_I2C.h>
#include <EEPROM.h>

struct PinConfig {
    static const uint8_t RELAY = 13;
    static const uint8_t FP_RX = 16;
    static const uint8_t FP_TX = 17;
    static const uint8_t BUZZER = 12;
    static const uint8_t I2C_ADDR = 0x27;
};

struct Config {
    static const unsigned long UNLOCK_TIME = 3000;
    static const unsigned long LOCKOUT_TIME = 30000;
    static const unsigned long INACTIVITY_TIME = 10000;
    static const uint8_t MAX_ATTEMPTS = 5;
    static const uint8_t PIN_LENGTH = 6;
    static const uint8_t STAR_THRESHOLD = 12;
};

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
byte rowPins[ROWS] = {2,0,4,5}, colPins[COLS] = {18,19,23};
SimpleKeypad keypad(keys, rowPins, colPins, ROWS, COLS);

// State variables
String input_password;
int wrong_attempts = 0;
bool lockout_mode = false;
unsigned long last_activity = 0;
unsigned long lockout_start = 0;
int star_count = 0;

// Function declarations
void showReadyScreen();
void unlockDoor();
void displayMessage(String line1, String line2, int delayTime = 0);
void checkPassword();
void activateLockoutMode();
int getIDFromInput();
void enrollFingerprint();
void deleteFingerprint();
String getInput(String prompt, char confirmKey, char clearKey);
void setupPins();
void setupLCD();
void setupFingerprintSensor();
void displaySensorParameters();
void handleLockoutMode();
void handleSerialCommands();
void handleFingerprint();
void handleKeypad();
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

void setup() {
    EEPROM.begin(512);
    setupPins();
    setupLCD();
    setupFingerprintSensor();
    
    if (EEPROM.read(0) == 0xFF) {
        setPassword("000000");
    }
    
    showReadyScreen();
    last_activity = millis();
}

void loop() {
    if (lockout_mode) {
        handleLockoutMode();
        return;
    }

    handleSerialCommands();
    handleFingerprint();
    handleKeypad();
    handleInactivity();

    if (millis() - last_activity > Config::INACTIVITY_TIME * 2) {
        handleHibernation();
    }
    delay(10);
}

void setupPins() {
    pinMode(PinConfig::RELAY, OUTPUT);
    digitalWrite(PinConfig::RELAY, HIGH);

    pinMode(PinConfig::BUZZER, OUTPUT);
    digitalWrite(PinConfig::BUZZER, LOW);  // Ensure buzzer is off
}

void setupLCD() {
    Wire.begin();
    lcd.begin();
    lcd.backlight();
    lcd.createChar(0, lockChar);
    lcd.createChar(1, unlockChar);
    lcd.createChar(2, fingerChar);

    displayMessage("  Waking Up...","");
}

void setupFingerprintSensor() {
    fingerprintSerial.begin(57600, SERIAL_8N1, PinConfig::FP_RX, PinConfig::FP_TX);
    delay(100);
    finger.begin(57600);

    if (!initFingerprint()) {
        Serial.println("Failed to connect to fingerprint sensor!");
        displayMessage("Sensor Failed!","System limited",2000);
    } else {
        Serial.println("ZW101 fingerprint sensor connected successfully");
        displaySensorParameters();
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
    unsigned long current_time = millis();
    if (current_time - lockout_start >= Config::LOCKOUT_TIME) {
        lockout_mode = false;
        wrong_attempts = 0;
        showReadyScreen();
        Serial.println("Lockout period ended. System ready.");
        last_activity = millis();
    } else {
        int seconds_left = (Config::LOCKOUT_TIME - (current_time - lockout_start)) / 1000;
        lcd.setCursor(6, 1);
        lcd.print("   ");
        lcd.setCursor(6, 1);
        lcd.print(seconds_left);
        lcd.print("s");
        delay(1000);
    }
}

void handleSerialCommands() {
    if (Serial.available() > 0) {
        char command = Serial.read();
        switch (command) {
            case 'e':
                enrollFingerprint();
                break;
            case 'd':
                deleteFingerprint();
                break;
            case 'p':
                changePassword();
                break;
            default:
                Serial.println("Valid commands:");
                Serial.println("e - Enroll a new fingerprint");
                Serial.println("d - Delete a stored fingerprint");
                Serial.println("p - Change the password");
                break;
        }
        last_activity = millis();
    }
}

void handleFingerprint() {
    uint8_t fingerprintID = getFingerprintID();
    if (fingerprintID != 0) {
        Serial.print("Matched fingerprint #");
        Serial.println(fingerprintID);

        displayMessage("ID #" + String(fingerprintID) + " Match!", "Access Granted");
        wrong_attempts = 0;  // Reset wrong attempts on successful fingerprint auth
        unlockDoor();
        showReadyScreen();
        last_activity = millis();
    }
}

void handleKeypad() {
    char key = keypad.getKey();
    if (!key) return;
    
    last_activity = millis();
    
    if (key == '*') {
        if (++star_count >= Config::STAR_THRESHOLD) {
            changePassword();
            star_count = 0;
        } else {
            input_password = "";
            showReadyScreen();
        }
        return;
    }
    
    star_count = 0;
    if (key == '#') {
        checkPassword();
    } else {
        input_password += key;
        displayMaskedInput();
        if (input_password.length() >= Config::PIN_LENGTH) {
            checkPassword();
        }
    }
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
    if (input_password != lastInput) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("      PIN:");
        lcd.setCursor(5, 1);  // Start masked input at the 6th column
        for (int i = 0; i < input_password.length(); i++) {
            lcd.print("*");
        }
        lastInput = input_password;
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
    if (finger.verifyPassword()) return true;

    displayMessage("Sensor Error!", "Trying alt pass...");
    Serial.println("Trying alternate password...");

    finger.setPassword(0x00000000);
    return finger.verifyPassword();
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

    Serial.println("Finger detected!");
    displayMessage("  Processing...","");

    last_activity = millis();

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
        Serial.println("Image conversion failed");
        displayMessage("Image Error","Try again", 1500);
        showReadyScreen();
        return 0;
    }

    p = finger.fingerFastSearch();
    if (p != FINGERPRINT_OK) {
        Serial.println("Fingerprint not recognized");
        displayMessage("No Match","Access Denied");
        soundBuzzer(1);  // Short error beep
        
        wrong_attempts++;
        Serial.print("Wrong attempts: ");
        Serial.print(wrong_attempts);
        Serial.print(" of ");
        Serial.println(Config::MAX_ATTEMPTS);
        
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

void unlockDoor() {
    lcd.setCursor(15, 0);
    lcd.write(1);
    soundBuzzer(0);
    digitalWrite(PinConfig::RELAY, LOW);
    delay(Config::UNLOCK_TIME);
    digitalWrite(PinConfig::RELAY, HIGH);
}

String getInput(String prompt, char confirmKey, char clearKey) {
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
                lcd.print("*");  // Mask input
            }
        }

        if (Serial.available()) {
            input = Serial.readStringUntil('\n');
            break;
        }

        delay(10);
    }
    return input;
}

int getIDFromInput() {
    String idStr = getInput("Enter ID:", '#', '*');
    return idStr.toInt();
}

void enrollFingerprint() {
    Serial.println("Ready to enroll a fingerprint!");

    int id = getIDFromInput();

    if (id == 0) {
        Serial.println("ID #0 not allowed. Try again.");
        showReadyScreen();
        return;
    }

    Serial.print("Enrolling ID #");
    Serial.println(id);

    while (!getFingerprintEnroll(id))
        ;

    showReadyScreen();
}

bool captureFingerprintImage(uint8_t bufferID) {
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) {
        Serial.println("Place finger again");
        return false;
    }

    p = finger.image2Tz(bufferID);
    if (p != FINGERPRINT_OK) {
        Serial.println("Image conversion failed");
        delay(2000);
        return false;
    }

    return true;
}

bool getFingerprintEnroll(uint8_t id) {
    Serial.println("Place finger");

    if (!captureFingerprintImage(1)) return false;

    Serial.println("Remove finger");
    delay(2000);

    while (finger.getImage() != FINGERPRINT_NOFINGER) {
        // Wait for the finger to be removed
    }

    Serial.println("Place the same finger again...");
    if (!captureFingerprintImage(2)) return false;

    Serial.println("Creating fingerprint model...");
    uint8_t p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        Serial.println("Failed to create fingerprint model");
        delay(2000);
        return false;
    }

    p = finger.storeModel(id);
    if (p != FINGERPRINT_OK) {
        Serial.println("Failed to store fingerprint model");
        delay(2000);
        return false;
    }

    Serial.println("Fingerprint enrolled successfully!");
    delay(2000);

    return true;
}

void deleteFingerprint() {
    Serial.println("Enter ID # to delete...");

    int id = getIDFromInput();

    Serial.print("Deleting ID #");
    Serial.println(id);

    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.println("Fingerprint deleted!");
    } else {
        Serial.println("Failed to delete fingerprint");
    }

    delay(2000);
    showReadyScreen();
}

void checkPassword() {
    String storedPassword = getPassword();
    if (input_password == storedPassword) {
        Serial.println("Correct PIN!");
        displayMessage("     Access","    Granted");
        wrong_attempts = 0;  // Reset wrong attempts counter
        unlockDoor();
    } else {
        Serial.println("Incorrect password!");
        wrong_attempts++;
        Serial.print("Wrong attempts: ");
        Serial.print(wrong_attempts);
        Serial.print(" of ");
        Serial.println(Config::MAX_ATTEMPTS);

        if (wrong_attempts >= Config::MAX_ATTEMPTS) {
            activateLockoutMode();
        } else {
            displayMessage("      PIN:","     Wrong");
            soundBuzzer(1);  // Short error beep
            delay(2000);
        }
    }
    input_password = "";
    if (!lockout_mode) showReadyScreen();
}

void activateLockoutMode() {
    Serial.println("Too many wrong attempts! Lockeout for 30 seconds.");
    displayMessage("    Lockout:","");
    lockout_mode = true;
    lockout_start = millis();
    soundBuzzer(3);  // Long alarm pattern
}

void setPassword(const String &newPassword) {
    for (int i = 0; i < Config::PIN_LENGTH; i++) {
        if (i < newPassword.length()) {
            EEPROM.write(0 + i, newPassword[i]);
        } else {
            EEPROM.write(0 + i, 0);  // Null-terminate
        }
    }
    EEPROM.commit();
    Serial.println("Password updated successfully!");
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
    Serial.println("Changing password...");
    
    String currentPassword = getInput("  Current PIN:",'#','*');
    if (currentPassword != getPassword()) {
        Serial.println("Incorrect current PIN!");
        displayMessage("   PIN Error","",2000);
        showReadyScreen();
        return;
    }

    String newPassword = getInput("    New PIN:", '#', '*');
    if (newPassword.length() > 0 && newPassword.length() <= Config::PIN_LENGTH) {
        setPassword(newPassword);
        displayMessage("  PIN Updated","",2000);
    } else {
        displayMessage("   PIN Error","   No Change",2000);
    }
    showReadyScreen();
}

void handleHibernation() {
    esp_sleep_enable_ext1_wakeup(GPIO_SEL_14, ESP_EXT1_WAKEUP_ANY_HIGH);
    lcd.backlight();
    displayMessage(" Hibernating...", "", 2000);
    lcd.noBacklight();
    Serial.println("Entering hibernation...");
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_deep_sleep_start();
}
