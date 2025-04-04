#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <SimpleKeypad.h>
#include <Wire.h>
#include <LCD_I2C.h>
#include <EEPROM.h>

// Pin and hardware setup
#define RELAY_PIN 13
#define FINGERPRINT_RX 16  // ESP32 RX pin connected to ZW101 TX
#define FINGERPRINT_TX 17  // ESP32 TX pin connected to ZW101 RX
#define BUZZER_PIN 12
#define I2C_ADDR 0x27      // I2C address of LCD
#define LCD_COLS 16
#define LCD_ROWS 2
#define WAKEUP_PIN 14

// Create hardware objects
LCD_I2C lcd(I2C_ADDR, LCD_COLS, LCD_ROWS);
HardwareSerial fingerprintSerial(2);  // Using ESP32's Hardware Serial 2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerprintSerial);

// Custom characters for LCD display
byte lockChar[8] = { 0b01110, 0b10001, 0b10001, 0b11111, 0b11011, 0b11011, 0b11111, 0b00000 };
byte unlockChar[8] = { 0b01110, 0b10000, 0b10000, 0b11111, 0b11011, 0b11011, 0b11111, 0b00000 };
byte fingerChar[8] = { 0b00000, 0b00000, 0b01110, 0b11111, 0b11111, 0b11111, 0b01110, 0b00000 };

// Keypad configuration
const byte ROWS = 4, COLS = 3;
char keys[ROWS * COLS] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#' };
byte rowPins[ROWS] = { 2, 0, 4, 5 };
byte colPins[COLS] = { 18, 19, 23 };
SimpleKeypad keypad(keys, rowPins, colPins, ROWS, COLS);

// Password variables
const String DEFAULT_PASSWORD = "0000";
String input_password = "";
int wrong_attempts = 0;
const int MAX_WRONG_ATTEMPTS = 5;
bool lockout_mode = false;
unsigned long lockout_start_time = 0;
const unsigned long LOCKOUT_DURATION = 30000;

// Backlight timeout variables
unsigned long lastActivityTime = 0;
const unsigned long INACTIVITY_TIMEOUT = 10000;
const unsigned long HIBERNATION_TIMEOUT = INACTIVITY_TIMEOUT * 2;

// Replace magic numbers with constants
const int UNLOCK_DURATION = 3000;
const int BUZZER_SHORT_BEEP = 100;
const int BUZZER_LONG_BEEP = 200;

// EEPROM address for storing the password
#define PASSWORD_ADDR 0
#define PASSWORD_MAX_LENGTH 6

// Add a counter for consecutive '*' presses
int star_press_count = 0;
const int STAR_PRESS_THRESHOLD = 12;

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
void setupHibernation();
void enterHibernation();

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 ZA620_M5 Fingerprint and Keypad Lock System with LCD");

  EEPROM.begin(512);  // Initialize EEPROM with 512 bytes
  setupPins();
  setupLCD();
  setupFingerprintSensor();

  // Check if a password exists in EEPROM, otherwise set the default password
  if (EEPROM.read(PASSWORD_ADDR) == 0xFF) {
    setPassword(DEFAULT_PASSWORD);
  }

  pinMode(WAKEUP_PIN, INPUT);  // Set wake-up pin as input
  setupHibernation();

  showReadyScreen();
  Serial.println("Ready to scan fingerprint or enter password...");

  lastActivityTime = millis();
}

void loop() {
  if (lockout_mode) {
    handleLockoutMode();
    return;
  }

  handleSerialCommands();
  handleFingerprint();
  handleKeypad();  // Ensure keypad is checked frequently
  handleInactivity();

  // Check for inactivity and enter hibernation
  if (millis() - lastActivityTime > HIBERNATION_TIMEOUT) {
    enterHibernation();
  }
  delay(10);
}

void setupPins() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // Ensure buzzer is off
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
  fingerprintSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
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

    displayMessage("Sensor Ready","Capacity:" + String(finger.capacity), 2000);
  }
}

void handleLockoutMode() {
  unsigned long current_time = millis();
  if (current_time - lockout_start_time >= LOCKOUT_DURATION) {
    lockout_mode = false;
    wrong_attempts = 0;
    showReadyScreen();
    Serial.println("Lockout period ended. System ready.");
    lastActivityTime = millis();
  } else {
    int seconds_left = (LOCKOUT_DURATION - (current_time - lockout_start_time)) / 1000;
    lcd.setCursor(6, 1);
    lcd.print(seconds_left);
    lcd.print("s   ");
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
    lastActivityTime = millis();
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
    lastActivityTime = millis();
  }
}

void handleKeypad() {
  char key = keypad.getKey();  // SimpleKeypad uses the same getKey() method
  if (key) {
    // Process key press immediately
    Serial.print("Key pressed: ");
    Serial.println(key);

    if (key == '*') {
      star_press_count++;
      if (star_press_count >= STAR_PRESS_THRESHOLD) {
        Serial.println("Entering change PIN mode...");
        displayMessage("   PIN Change","  Initialized",2000);
        changePassword();
        star_press_count = 0;  // Reset the counter
      } else {
        input_password = "";
        Serial.println("Input cleared");
        displayMessage("      PIN:", "    Cleared", 500);
        showReadyScreen();
      }
    } else {
      star_press_count = 0;  // Reset the counter if any other key is pressed
      if (key == '#') {
        checkPassword();
      } else {
        input_password += key;
        displayMaskedInput();
        if (input_password.length() >= 6) {
          checkPassword();
        }
      }
    }
    lastActivityTime = millis();
  }
}

void handleInactivity() {
  if (millis() - lastActivityTime > INACTIVITY_TIMEOUT) {
    lcd.noBacklight();
  } else {
    lcd.backlight();
  }
}

// Optimize LCD updates in `displayMaskedInput`
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

// Buzzer patterns: 0=success, 1=error, 2=warning, 3=alarm
void soundBuzzer(int pattern) {
  switch (pattern) {
    case 0:  // Success beep
      tone(BUZZER_PIN, 1000, BUZZER_SHORT_BEEP);
      delay(BUZZER_SHORT_BEEP);
      tone(BUZZER_PIN, 2000, BUZZER_SHORT_BEEP);
      break;
    case 1:  // Error beep
      tone(BUZZER_PIN, 300, BUZZER_LONG_BEEP);
      break;
    case 2:  // Warning beep
      tone(BUZZER_PIN, 500, BUZZER_SHORT_BEEP);
      delay(BUZZER_SHORT_BEEP);
      tone(BUZZER_PIN, 500, BUZZER_SHORT_BEEP);
      break;
    case 3:  // Alarm
      for (int i = 0; i < 5; i++) {
        tone(BUZZER_PIN, 800, BUZZER_SHORT_BEEP);
        delay(BUZZER_SHORT_BEEP);
        tone(BUZZER_PIN, 600, BUZZER_SHORT_BEEP);
        delay(BUZZER_SHORT_BEEP);
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
  lcd.print("    Ready");
}

uint8_t getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return 0;

  Serial.println("Finger detected!");
  displayMessage("  Processing...","");

  // Update last activity time when a fingerprint is detected
  lastActivityTime = millis();

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
    
    // Increment wrong attempts counter for failed fingerprint recognition
    wrong_attempts++;
    Serial.print("Wrong attempts: ");
    Serial.print(wrong_attempts);
    Serial.print(" of ");
    Serial.println(MAX_WRONG_ATTEMPTS);
    
    // Check if maximum wrong attempts reached
    if (wrong_attempts >= MAX_WRONG_ATTEMPTS) {
      activateLockoutMode();
    } else {
      delay(2000);
      showReadyScreen();
    }
    
    return 0;
  }

  return finger.fingerID;
}

// Update `unlockDoor` to use the new constant
void unlockDoor() {
  Serial.println("Unlocking door...");

  // Update lock status on LCD
  lcd.setCursor(15, 0);
  lcd.write(1);  // Unlock character

  // Success beep
  soundBuzzer(0);

  digitalWrite(RELAY_PIN, LOW);  // Unlock

  // Countdown timer
  delay(UNLOCK_DURATION);

  digitalWrite(RELAY_PIN, HIGH);  // Lock
  Serial.println("Door locked");
}

// Refactored function to handle input from keypad or serial
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

// Refactor `getIDFromInput` to use the new `getInput` function
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

// Check password
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
    Serial.println(MAX_WRONG_ATTEMPTS);

    if (wrong_attempts >= MAX_WRONG_ATTEMPTS) {
      activateLockoutMode();
    } else {
      displayMessage("      PIN:","     Wrong");
      soundBuzzer(1);  // Short error beep
      delay(2000);
    }
  }
  // Reset input
  input_password = "";
  if (!lockout_mode) showReadyScreen();
}

// Activate lockout mode
void activateLockoutMode() {
  Serial.println("Too many wrong attempts! System locked for 30 seconds.");
  displayMessage("Try Again Later","");
  lockout_mode = true;
  lockout_start_time = millis();
  soundBuzzer(3);  // Long alarm pattern
}

// Function to set a new password in EEPROM
void setPassword(const String &newPassword) {
  for (int i = 0; i < PASSWORD_MAX_LENGTH; i++) {
    if (i < newPassword.length()) {
      EEPROM.write(PASSWORD_ADDR + i, newPassword[i]);
    } else {
      EEPROM.write(PASSWORD_ADDR + i, 0);  // Null-terminate
    }
  }
  EEPROM.commit();
  Serial.println("Password updated successfully!");
}

// Function to retrieve the password from EEPROM
String getPassword() {
  String password = "";
  for (int i = 0; i < PASSWORD_MAX_LENGTH; i++) {
    char c = EEPROM.read(PASSWORD_ADDR + i);
    if (c == 0) break;  // Stop at null terminator
    password += c;
  }
  return password;
}

// Add a new function to allow the user to change the password
void changePassword() {
  Serial.println("Changing password...");
  
  // Ask for the current PIN
  String currentPassword = getInput("  Current PIN:",'#','*');
  if (currentPassword != getPassword()) {
    Serial.println("Incorrect current PIN!");
    displayMessage("   PIN Error","",2000);
    showReadyScreen();
    return;
  }

  // Ask for the new PIN
  String newPassword = getInput("    New PIN:", '#', '*');
  if (newPassword.length() > 0 && newPassword.length() <= PASSWORD_MAX_LENGTH) {
    setPassword(newPassword);
    displayMessage("  PIN Updated","",2000);
  } else {
    displayMessage("   PIN Error","   No Change",2000);
  }
  showReadyScreen();
}

// Function to configure hibernation
void setupHibernation() {
  esp_sleep_enable_ext1_wakeup(GPIO_SEL_14, ESP_EXT1_WAKEUP_ANY_HIGH);
}

// Function to enter hibernation
void enterHibernation() {
  lcd.backlight();
  displayMessage(" Hibernating...","",2000);
  lcd.noBacklight();
  Serial.println("Entering hibernation...");
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  esp_deep_sleep_start();
}
