#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Pin and hardware setup
#define RELAY_PIN 26       // ESP32 GPIO pin connected to relay
#define FINGERPRINT_RX 16  // ESP32 RX pin connected to ZA620_M5 TX
#define FINGERPRINT_TX 17  // ESP32 TX pin connected to ZA620_M5 RX
#define BUZZER_PIN 15      // ESP32 GPIO pin connected to piezo buzzer
#define I2C_ADDR 0x27      // I2C address of LCD
#define LCD_COLS 16        // LCD columns
#define LCD_ROWS 2         // LCD rows

// Create hardware objects
LiquidCrystal_I2C lcd(I2C_ADDR, LCD_COLS, LCD_ROWS);
HardwareSerial fingerprintSerial(2);  // Using ESP32's Hardware Serial 2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerprintSerial);

// Custom characters for LCD display
byte lockChar[8] = { 0b01110, 0b10001, 0b10001, 0b11111, 0b11011, 0b11011, 0b11111, 0b00000 };
byte unlockChar[8] = { 0b01110, 0b10000, 0b10000, 0b11111, 0b11011, 0b11011, 0b11111, 0b00000 };
byte fingerChar[8] = { 0b00000, 0b00000, 0b01110, 0b11111, 0b11111, 0b11111, 0b01110, 0b00000 };

// Keypad configuration
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = { { '1', '2', '3' }, { '4', '5', '6' }, { '7', '8', '9' }, { '*', '0', '#' } };
byte rowPins[ROWS] = { 2, 0, 4, 5 };
byte colPins[COLS] = { 18, 19, 23 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Password variables
const String DEFAULT_PASSWORD = "0000";
String input_password = "";
int wrong_attempts = 0;
const int MAX_WRONG_ATTEMPTS = 5;
bool lockout_mode = false;
unsigned long lockout_start_time = 0;
const unsigned long LOCKOUT_DURATION = 30000;  // 30 seconds lockout

// Backlight timeout variables
unsigned long lastActivityTime = 0;
const unsigned long INACTIVITY_TIMEOUT = 10000;  // 10 seconds

// Function declarations
void showReadyScreen();
void unlockDoor();
void displayMessage(String line1, String line2, int delayTime = 0);
uint8_t getFingerprintID();
bool initFingerprint();
bool getFingerprintEnroll(uint8_t id);
void soundBuzzer(int pattern);
void checkPassword();
void activateLockoutMode();
int getIDFromInput();

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 ZA620_M5 Fingerprint and Keypad Lock System with LCD");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // REVERSED LOGIC: HIGH = locked

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // Ensure buzzer is off

  // Initialize LCD
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, lockChar);
  lcd.createChar(1, unlockChar);
  lcd.createChar(2, fingerChar);

  displayMessage("Biometric Locker", "Initializing...");

  // Initialize fingerprint sensor
  fingerprintSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  delay(100);
  finger.begin(57600);

  if (!initFingerprint()) {
    Serial.println("Failed to connect to fingerprint sensor!");
    displayMessage("Sensor Failed!", "System limited", 2000);
  } else {
    Serial.println("ZA620_M5 fingerprint sensor connected successfully");

    // Get sensor parameters if available
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

      displayMessage("Sensor Ready", "Capacity:" + String(finger.capacity), 2000);
    }
  }

  showReadyScreen();
  Serial.println("Ready to scan fingerprint or enter password...");

  // Initialize last activity time
  lastActivityTime = millis();
}

void loop() {
  // Check if system is in lockout mode
  if (lockout_mode) {
    unsigned long current_time = millis();
    if (current_time - lockout_start_time >= LOCKOUT_DURATION) {
      // Lockout period over
      lockout_mode = false;
      wrong_attempts = 0;
      showReadyScreen();
      Serial.println("Lockout period ended. System ready.");
    } else {
      // Still in lockout, update countdown
      int seconds_left = (LOCKOUT_DURATION - (current_time - lockout_start_time)) / 1000;
      lcd.setCursor(0, 1);
      lcd.print("Lockout: ");
      lcd.print(seconds_left);
      lcd.print("s   ");
      delay(1000);
      return;  // Skip the rest of the loop while in lockout
    }
  }

  // Check for menu commands from serial
  if (Serial.available() > 0) {
    char command = Serial.read();
    switch (command) {
      case 'e':
        enrollFingerprint();
        break;
      case 'd':
        deleteFingerprint();
        break;
      default:
        Serial.println("Valid commands:");
        Serial.println("e - Enroll a new fingerprint");
        Serial.println("d - Delete a stored fingerprint");
        break;
    }
    lastActivityTime = millis();  // Update last activity time
  }

  // Check fingerprint
  uint8_t fingerprintID = getFingerprintID();
  if (fingerprintID != 0) {
    Serial.print("Matched fingerprint #");
    Serial.println(fingerprintID);

    displayMessage("ID #" + String(fingerprintID) + " Match!", "Access Granted");
    wrong_attempts = 0;  // Reset wrong attempts on successful fingerprint auth
    unlockDoor();
    showReadyScreen();
    lastActivityTime = millis();  // Update last activity time
  }

  // Check keypad
  char key = keypad.getKey();
  if (key) {
    Serial.print("Key pressed: ");
    Serial.println(key);

    if (key == '*') {
      // Clear input
      input_password = "";
      Serial.println("Input cleared");

      displayMessage("Password:", "Cleared", 1000);
      showReadyScreen();
    } else if (key == '#') {
      checkPassword();
    } else {
      // Add key to password
      input_password += key;

      // Show masked input on LCD
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Password:");
      lcd.setCursor(0, 1);
      for (int i = 0; i < input_password.length(); i++) {
        lcd.print("*");
      }

      // Mask the input for security when printing to serial
      Serial.print("Current input: ");
      for (int i = 0; i < input_password.length(); i++) {
        Serial.print("*");
      }
      Serial.println();

      // Auto-check password if it reaches 4 digits
      if (input_password.length() >= 4) {
        checkPassword();
      }
    }
    lastActivityTime = millis();  // Update last activity time
  }

  // Check inactivity and turn off backlight if necessary
  if (millis() - lastActivityTime > INACTIVITY_TIMEOUT) {
    lcd.noBacklight();
  } else {
    lcd.backlight();
  }

  delay(50);  // Short delay for stability
}

// Buzzer patterns: 0=success, 1=error, 2=warning, 3=alarm
void soundBuzzer(int pattern) {
  switch (pattern) {
    case 0:  // Success beep
      tone(BUZZER_PIN, 1000, 100);
      delay(100);
      tone(BUZZER_PIN, 2000, 100);
      break;
    case 1:  // Error beep
      tone(BUZZER_PIN, 300, 200);
      break;
    case 2:  // Warning beep
      tone(BUZZER_PIN, 500, 100);
      delay(100);
      tone(BUZZER_PIN, 500, 100);
      break;
    case 3:  // Alarm
      for (int i = 0; i < 5; i++) {
        tone(BUZZER_PIN, 800, 100);
        delay(100);
        tone(BUZZER_PIN, 600, 100);
        delay(100);
      }
      break;
  }
}

// Display utility function
void displayMessage(String line1, String line2, int delayTime) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (delayTime > 0) delay(delayTime);
}

// Initialize fingerprint sensor - tries alternate password if needed
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
  lcd.print(" Locker");
  lcd.setCursor(0, 1);
  lcd.print("Ready to unlock");
}

uint8_t getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return 0;

  Serial.println("Finger detected!");
  displayMessage("Finger detected", "Processing...");

  // Update last activity time when a fingerprint is detected
  lastActivityTime = millis();

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("Image conversion failed");
    displayMessage("Image Error", "Try again", 1500);
    showReadyScreen();
    return 0;
  }

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    Serial.println("Fingerprint not recognized");
    displayMessage("No Match Found", "Access Denied");
    soundBuzzer(1);  // Short error beep
    delay(2000);
    showReadyScreen();
    return 0;
  }

  return finger.fingerID;
}

void unlockDoor() {
  Serial.println("Unlocking door...");

  // Update lock status on LCD
  lcd.setCursor(15, 0);
  lcd.write(1);  // Unlock character

  // Success beep
  soundBuzzer(0);

  digitalWrite(RELAY_PIN, LOW);  // Unlock

  // Countdown timer
  for (int i = 3; i > 0; i--) {
    lcd.setCursor(15, 1);
    lcd.print(i);
    delay(1000);
  }

  digitalWrite(RELAY_PIN, HIGH);  // Lock
  Serial.println("Door locked");
}

// Utility function to get input from keypad until confirm key is pressed
String getInputFromKeypad(char confirmKey, char clearKey) {
  String input = "";
  while (true) {
    char key = keypad.getKey();
    if (key) {
      Serial.print("Key pressed: ");
      Serial.println(key);

      if (key == confirmKey) {
        break;
      } else if (key == clearKey) {
        input = "";
        Serial.println("Input cleared");
        lcd.setCursor(0, 1);
        lcd.print("                ");
        lcd.setCursor(0, 1);
        lcd.print("Enter current:");
      } else {
        input += key;
        // Show masked input
        lcd.setCursor(14, 1);
        lcd.print("   ");
        lcd.setCursor(14, 1);
        lcd.print(input.length());

        // Mask the input for security
        Serial.print("Current input: ");
        for (int i = 0; i < input.length(); i++) {
          Serial.print("*");
        }
        Serial.println();
      }
    }

    // Also allow input from Serial for testing
    if (Serial.available()) {
      String serialInput = Serial.readStringUntil('\n');
      input = serialInput;
      Serial.println(serialInput);
      break;
    }

    delay(10);
  }
  return input;
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

bool getFingerprintEnroll(uint8_t id) {
  Serial.println("Place finger on sensor...");

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    Serial.println("Place finger again");
    return false;
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("Image conversion failed");
    delay(2000);
    return false;
  }

  Serial.println("DO NOT Remove finger");
  delay(2000);

  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
  }

  p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    Serial.println("Place finger again");
    return false;
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("Image conversion failed");
    delay(2000);
    return false;
  }

  Serial.println("Creating fingerprint model...");

  p = finger.createModel();
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

// New function to check password
void checkPassword() {
  if (input_password == DEFAULT_PASSWORD) {
    Serial.println("Correct password!");
    displayMessage("Password Correct", "Access Granted");
    wrong_attempts = 0;  // Reset wrong attempts counter
    unlockDoor();
  } else {
    Serial.println("Incorrect password!");
    wrong_attempts++;
    Serial.print("Wrong attempts: ");
    Serial.print(wrong_attempts);
    Serial.print(" of ");
    Serial.println(MAX_WRONG_ATTEMPTS);

    if (wrong_attempts >= MAX_WRONG_ATTEMPTS) {
      activateLockoutMode();
    } else {
      displayMessage("Password Error!", "Access Denied");
      soundBuzzer(1);  // Short error beep
      delay(2000);
    }
  }
  // Reset input
  input_password = "";
  if (!lockout_mode) showReadyScreen();
}

// New function to activate lockout mode
void activateLockoutMode() {
  Serial.println("Too many wrong attempts! System locked for 30 seconds.");
  displayMessage("Too Many Attempts", "System Locked!");
  lockout_mode = true;
  lockout_start_time = millis();
  soundBuzzer(3);  // Long alarm pattern
}

// New function to get ID from keypad or serial input
int getIDFromInput() {
  int id = 0;
  while (true) {
    char key = keypad.getKey();
    if (key) {
      Serial.print("Key pressed: ");
      Serial.println(key);

      if (key == '#') break;
      else if (key == '*') {
        id = 0;
        Serial.println("ID cleared");
      } else if (isdigit(key)) {
        id = id * 10 + (key - '0');
        Serial.print("Current ID: ");
        Serial.println(id);
      }
    }

    if (Serial.available()) {
      char c = Serial.read();
      if (isdigit(c)) {
        id = id * 10 + (c - '0');
        Serial.print("Current ID: ");
        Serial.println(id);
      } else if (c == '\n' || c == '\r') {
        break;
      }
    }

    delay(10);
  }
  return id;
}
