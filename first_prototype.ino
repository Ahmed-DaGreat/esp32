#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <FluxGarage_RoboEyes.h>

// ================= PIN DEFINITIONS =================
#define SDA_PIN     21
#define SCL_PIN     22
#define BUZZER_PIN  4
#define BTN1_PIN    18   // Button 1: Pet the Robo
#define BTN2_PIN    19   // Button 2: 60-Min Timer (Hold both 3s = Power Off)

// ================= DISPLAY & EYES ==================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SH1106G> eyes(display);

// ================= WIFI & SPOTIFY ==================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// Put your Spotify API / proxy URL and Bearer token here
const char* SPOTIFY_URL   = "https://api.spotify.com/v1/me/player/currently-playing";
const char* SPOTIFY_TOKEN = "YOUR_SPOTIFY_BEARER_TOKEN";

unsigned long lastSpotifyCheck = 0;
const unsigned long SPOTIFY_INTERVAL = 8000; // Check every 8 seconds
String currentTrack  = "";
String currentArtist = "";
bool isPlayingMusic  = false;
unsigned long showSpotifyUntil = 0;

// ================= MPU6050 SETTINGS ================
#define MPU_ADDR        0x68
#define SHAKE_THRESHOLD 7500  // Delta sensitivity for waking/shaking
int16_t prevAx = 0, prevAy = 0, prevAz = 0;
bool mpuReady = false;
unsigned long lastMpuRead = 0;

// ================= STATE MACHINE ===================
enum BuddyState {
  STATE_AWAKE,
  STATE_SLEEP,
  STATE_TIMER,
  STATE_OFF
};

BuddyState currentState = STATE_AWAKE;

// Timing variables
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT = 30000; // Sleep after 30s of no interaction

// Petting state
bool isBeingPetted = false;
unsigned long petEndTime = 0;

// 60-Minute Timer variables
const unsigned long TIMER_DURATION = 60UL * 60UL * 1000UL; // 60 mins in ms
unsigned long timerStartTime = 0;

// Button handling
unsigned long bothButtonsHoldStart = 0;
bool bothButtonsHolding = false;
bool waitForButtonRelease = false;
unsigned long lastBtn1Press = 0;
unsigned long lastBtn2Press = 0;

// ================= BUZZER SOUNDS ===================
void beep(int freq, int duration) {
  tone(BUZZER_PIN, freq, duration);
  delay(duration);
  noTone(BUZZER_PIN);
}

void playBootSound() {
  beep(523, 80); // C5
  beep(659, 80); // E5
  beep(784, 120); // G5
}

void playPetSound() {
  beep(880, 60);
  beep(1046, 80);
  beep(1318, 120);
}

void playWakeSound() {
  beep(600, 70);
  beep(900, 100);
}

void playSleepSound() {
  beep(600, 120);
  beep(400, 200);
}

void playTimerBeep() {
  beep(1000, 100);
}

void playPowerOffSound() {
  beep(784, 100);
  beep(659, 100);
  beep(523, 100);
  beep(392, 250);
}

// ================= MPU6050 FUNCTIONS ===============
void initMPU6050() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // PWR_MGMT_1 register
  Wire.write(0x00); // Wake up MPU6050
  if (Wire.endTransmission(true) == 0) {
    mpuReady = true;
    // Prime initial readings
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 6, true);
    if (Wire.available() >= 6) {
      prevAx = (int16_t)((Wire.read() << 8) | Wire.read());
      prevAy = (int16_t)((Wire.read() << 8) | Wire.read());
      prevAz = (int16_t)((Wire.read() << 8) | Wire.read());
    }
  }
}

bool checkMPUShake() {
  if (!mpuReady || millis() - lastMpuRead < 80) return false;
  lastMpuRead = millis();

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // Starting with register 0x3B (ACCEL_XOUT_H)
  if (Wire.endTransmission(false) != 0) return false;

  Wire.requestFrom(MPU_ADDR, 6, true);
  if (Wire.available() < 6) return false;

  int16_t ax = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t ay = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t az = (int16_t)((Wire.read() << 8) | Wire.read());

  int32_t delta = abs(ax - prevAx) + abs(ay - prevAy) + abs(az - prevAz);
  prevAx = ax;
  prevAy = ay;
  prevAz = az;

  return (delta > SHAKE_THRESHOLD);
}

// ================= SPOTIFY FETCH ===================
void fetchSpotifyEvery8Secs() {
  if (millis() - lastSpotifyCheck < SPOTIFY_INTERVAL) return;
  lastSpotifyCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;
  if (String(SPOTIFY_TOKEN) == "YOUR_SPOTIFY_BEARER_TOKEN") return;

  HTTPClient http;
  http.begin(SPOTIFY_URL);
  http.addHeader("Authorization", String("Bearer ") + SPOTIFY_TOKEN);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      isPlayingMusic = doc["is_playing"] | false;
      const char* trackName = doc["item"]["name"];
      const char* artistName = doc["item"]["artists"][0]["name"];
      if (trackName && artistName) {
        String newTrack = String(trackName);
        if (newTrack != currentTrack) {
          currentTrack = newTrack;
          currentArtist = String(artistName);
          showSpotifyUntil = millis() + 3000; // Show overlay for 3s on track update
        }
      }
    }
  } else if (httpCode == 204) {
    isPlayingMusic = false;
  }
  http.end();
}

// ================= CUSTOM SCREENS ==================
void drawSleepScreen() {
  display.clearDisplay();
  // Closed sleeping eyes
  display.fillRoundRect(22, 34, 32, 4, 2, SH110X_WHITE);
  display.fillRoundRect(74, 34, 32, 4, 2, SH110X_WHITE);

  // Floating 'z z Z' animation
  int step = (millis() / 400) % 3;
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(92, 22);
  display.print("z");
  if (step >= 1) {
    display.setCursor(102, 14);
    display.print("z");
  }
  if (step >= 2) {
    display.setCursor(112, 6);
    display.print("Z");
  }
  display.display();
}

void drawTimerScreen() {
  unsigned long elapsed = millis() - timerStartTime;
  if (elapsed >= TIMER_DURATION) {
    // 60 Minutes Finished!
    playBootSound();
    playPetSound();
    currentState = STATE_AWAKE;
    eyes.setMood(HAPPY);
    eyes.anim_laugh();
    lastActivityTime = millis();
    return;
  }

  unsigned long remainingSeconds = (TIMER_DURATION - elapsed) / 1000;
  int mins = remainingSeconds / 60;
  int secs = remainingSeconds % 60;

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // Top Header
  display.setTextSize(1);
  display.setCursor(22, 4);
  display.print("FOCUS TIMER");

  // Big Countdown MM:SS
  char buf[8];
  sprintf(buf, "%02d:%02d", mins, secs);
  display.setTextSize(3);
  display.setCursor(20, 22);
  display.print(buf);

  // Progress Bar at Bottom
  int barWidth = map(elapsed, 0, TIMER_DURATION, 108, 0);
  display.drawRect(10, 52, 108, 8, SH110X_WHITE);
  display.fillRect(10, 52, barWidth, 8, SH110X_WHITE);

  display.display();
}

void drawSpotifyBanner() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 4);
  display.print("Spotify Now Playing:");
  display.drawLine(0, 14, 128, 14, SH110X_WHITE);

  display.setCursor(0, 24);
  display.println(currentTrack);
  display.setCursor(0, 44);
  display.println(currentArtist);
  display.display();
}

// ================= SETUP ===========================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize OLED
  display.begin(OLED_ADDR, true);
  display.clearDisplay();
  display.display();

  // Initialize RoboEyes
  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 60);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setWidth(36, 36);
  eyes.setHeight(36, 36);
  eyes.setBorderradius(8, 8);
  eyes.setSpacebetween(10);
  eyes.setMood(DEFAULT);
  eyes.open();

  // Initialize MPU6050
  initMPU6050();

  // Quick non-blocking Wi-Fi connect (max 4 seconds wait)
  if (String(WIFI_SSID) != "YOUR_WIFI_SSID") {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 4000) {
      delay(100);
    }
  }

  playBootSound();
  // Reset activity timer at the VERY END of setup so he starts AWAKE
  lastActivityTime = millis();
}

// ================= MAIN LOOP =======================
void loop() {
  bool btn1Down = (digitalRead(BTN1_PIN) == LOW);
  bool btn2Down = (digitalRead(BTN2_PIN) == LOW);

  // Wait until buttons are released after power-off/wake
  if (waitForButtonRelease) {
    if (!btn1Down && !btn2Down) {
      waitForButtonRelease = false;
      delay(50);
    }
    return;
  }

  // --- 1. IF CURRENTLY OFF: Press either button to turn back ON ---
  if (currentState == STATE_OFF) {
    if (btn1Down || btn2Down) {
      display.oled_command(SH110X_DISPLAYON);
      currentState = STATE_AWAKE;
      eyes.setMood(DEFAULT);
      eyes.open();
      playBootSound();
      lastActivityTime = millis();
      waitForButtonRelease = true;
    }
    return;
  }

  // --- 2. CHECK BOTH BUTTONS HELD FOR 3 SECONDS -> TURN OFF ---
  if (btn1Down && btn2Down) {
    if (!bothButtonsHolding) {
      bothButtonsHolding = true;
      bothButtonsHoldStart = millis();
    } else if (millis() - bothButtonsHoldStart >= 3000) {
      // 3 Seconds reached: Power Off!
      playPowerOffSound();
      display.clearDisplay();
      display.display();
      display.oled_command(SH110X_DISPLAYOFF);
      currentState = STATE_OFF;
      bothButtonsHolding = false;
      waitForButtonRelease = true;
      return;
    }
    return; // Ignore single-button actions while holding both
  } else {
    bothButtonsHolding = false;
  }

  // --- 3. BUTTON 1 PRESSED -> PET THE ROBO ---
  if (btn1Down && (millis() - lastBtn1Press > 300)) {
    lastBtn1Press = millis();
    lastActivityTime = millis();

    currentState = STATE_AWAKE;
    isBeingPetted = true;
    petEndTime = millis() + 3000;
    eyes.open();
    eyes.setMood(HAPPY);
    eyes.anim_laugh();
    playPetSound();
  }

  // --- 4. BUTTON 2 PRESSED -> TOGGLE 60-MIN TIMER ---
  if (btn2Down && (millis() - lastBtn2Press > 300)) {
    lastBtn2Press = millis();
    lastActivityTime = millis();
    playTimerBeep();

    if (currentState != STATE_TIMER) {
      currentState = STATE_TIMER;
      timerStartTime = millis();
    } else {
      currentState = STATE_AWAKE;
      eyes.setMood(DEFAULT);
      eyes.open();
    }
  }

  // --- 5. CHECK MPU6050 SHAKE -> WAKE UP OR REACT ---
  if (checkMPUShake()) {
    lastActivityTime = millis();
    if (currentState == STATE_SLEEP) {
      currentState = STATE_AWAKE;
      eyes.open();
      eyes.setMood(DEFAULT);
      eyes.anim_confused();
      playWakeSound();
    } else if (currentState == STATE_AWAKE && !isBeingPetted) {
      eyes.anim_confused();
      beep(750, 50);
    }
  }

  // --- 6. SPOTIFY CHECK EVERY 8 SECONDS ---
  fetchSpotifyEvery8Secs();

  // --- 7. RENDER CURRENT STATE ---
  if (currentState == STATE_TIMER) {
    drawTimerScreen();
  } 
  else if (currentState == STATE_SLEEP) {
    drawSleepScreen();
  } 
  else if (currentState == STATE_AWAKE) {
    // End petting mood after 3 seconds
    if (isBeingPetted && millis() > petEndTime) {
      isBeingPetted = false;
      eyes.setMood(DEFAULT);
    }

    // Check if idle for 30 seconds -> Go to sleep
    if (millis() - lastActivityTime > SLEEP_TIMEOUT) {
      currentState = STATE_SLEEP;
      playSleepSound();
      return;
    }

    // Show Spotify track overlay briefly when song changes, else RoboEyes
    if (isPlayingMusic && millis() < showSpotifyUntil) {
      drawSpotifyBanner();
    } else {
      eyes.update();
    }
  }
}