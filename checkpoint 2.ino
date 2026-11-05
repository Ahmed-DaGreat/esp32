// DeskBot v2.2 - USB Serial (COM7) + Auto-WiFi Scanner + Active/Passive Buzzer + Raw MPU6050
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#define PIXEL_ON SH110X_WHITE
#define PIXEL_OFF SH110X_BLACK

#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BUZZER 4
#define PIN_BTN_MAIN 19   // Button 1 (D19 -> GND)
#define PIN_BTN_MODE 18   // Button 2 (D18 -> GND)

const char* TARGET_SSID = "(YO) A25 5G";
const char* TARGET_PASS = "my mom is best";

WiFiUDP udp;
const int UDP_PORT = 4210;

enum AppMode {
  MODE_ROBOEYES,
  MODE_TIMER_CAT,
  MODE_SPOTIFY
};

enum EyeMood {
  MOOD_DEFAULT,
  MOOD_HAPPY_PET,
  MOOD_DIZZY,
  MOOD_TIRED,
  MOOD_SLEEPING
};

AppMode currentMode = MODE_ROBOEYES;
AppMode modeBeforeSpotify = MODE_ROBOEYES;
EyeMood currentMood = MOOD_DEFAULT;

// Raw I2C MPU6050 State
uint8_t mpuAddr = 0x68;
bool mpuConnected = false;
int16_t prevAX = 0, prevAY = 0, prevAZ = 0;
bool firstMpuRead = true;
unsigned long lastMpuCheck = 0;

// 30-Second Sleep & Mood Timers
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT_MS = 30000;
unsigned long moodRevertTime = 0;

// Smooth RoboEyes Physics
float eyeLX = 22, eyeLY = 16, eyeLW = 34, eyeLH = 32;
float eyeRX = 72, eyeRY = 16, eyeRW = 34, eyeRH = 32;
float targetLX = 22, targetLY = 16, targetLW = 34, targetLH = 32;
float targetRX = 72, targetRY = 16, targetRW = 34, targetRH = 32;
unsigned long lastBlinkTime = 0, nextBlinkInterval = 3000;
unsigned long lastSaccadeTime = 0, nextSaccadeInterval = 2400;
bool isBlinking = false;
unsigned long blinkStartTime = 0;

// 1-Hour Study Timer + Pixel Cat
const unsigned long ONE_HOUR_MS = 3600UL * 1000UL;
unsigned long timerRemainingMs = ONE_HOUR_MS;
bool timerRunning = true;
unsigned long lastTimerTick = 0;
float catX = 18.0;
int catDir = 1, catAction = 0, animFrame = 0;
unsigned long lastCatStateChange = 0, lastAnimTick = 0;

// Live Spotify & Lyrics State
bool showLyricsView = false;
bool spotifyPlaying = false;
String currentTrack = "Waiting for Song";
String currentArtist = "Run spotify_bridge.py";
String currentLyricLine = "Press Play on Spotify ~";
unsigned long trackProgressMs = 0;
unsigned long trackDurationMs = 180000;
unsigned long lastProgressTick = 0;
int titleScrollOffset = 0;
unsigned long lastScrollTick = 0;

// Bitmaps
const uint8_t PROGMEM heart8x7[] = {
  0b01101100, 0b11111110, 0b11111110, 0b01111100,
  0b00111000, 0b00010000, 0b00000000
};

const uint8_t PROGMEM musicNote8x8[] = {
  0b00011110, 0b00010010, 0b00010010, 0b00010010,
  0b01110110, 0b11110110, 0b01100000, 0b00000000
};

const uint8_t PROGMEM catWalk1_16x12[] = {
  0x20, 0x80, 0x71, 0xC0, 0x7F, 0xC0, 0x55, 0x40,
  0x7F, 0xC3, 0x3F, 0x86, 0x1F, 0xFE, 0x1F, 0xFC,
  0x1F, 0xFC, 0x1B, 0x6C, 0x12, 0x04, 0x00, 0x00
};

const uint8_t PROGMEM catWalk2_16x12[] = {
  0x20, 0x80, 0x71, 0xC0, 0x7F, 0xC0, 0x55, 0x40,
  0x7F, 0xC6, 0x3F, 0x83, 0x1F, 0xFE, 0x1F, 0xFC,
  0x1F, 0xFC, 0x0D, 0xB0, 0x08, 0x10, 0x00, 0x00
};

const uint8_t PROGMEM catSleep_16x12[] = {
  0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x3B, 0x80,
  0x3F, 0xFE, 0x2A, 0xFF, 0x3F, 0xFF, 0x1F, 0xFF,
  0x0F, 0xFE, 0x07, 0xFC, 0x00, 0x00, 0x00, 0x00
};

// ============================================================================
// HYBRID ACTIVE + PASSIVE BUZZER ENGINE (Pin D4)
// ============================================================================
void hybridBeep(int dcMs, int freqHz, int toneMs) {
  // 1. Solid DC HIGH pulse so Active Buzzers beep loud and clear!
  digitalWrite(PIN_BUZZER, HIGH);
  delay(dcMs);
  digitalWrite(PIN_BUZZER, LOW);

  // 2. Hardware tone pulse in case it's a Passive Buzzer!
  if (toneMs > 0) {
    tone(PIN_BUZZER, freqHz, toneMs);
    delay(toneMs);
    noTone(PIN_BUZZER);
  }
  digitalWrite(PIN_BUZZER, LOW);
}

void sfxStartup() {
  hybridBeep(45, 800, 30);  delay(35);
  hybridBeep(45, 1200, 30); delay(35);
  hybridBeep(80, 1600, 40);
}

void sfxPetHappy() {
  hybridBeep(35, 1000, 25); delay(25);
  hybridBeep(35, 1400, 25); delay(25);
  hybridBeep(65, 1800, 35); delay(25);
  hybridBeep(90, 2000, 45);
}

void sfxDizzyShake() {
  for (int i = 0; i < 3; i++) {
    hybridBeep(30, 1200, 20); delay(20);
    hybridBeep(30, 500, 20);  delay(20);
  }
}

void sfxWakeUp() {
  hybridBeep(40, 700, 25); delay(25);
  hybridBeep(70, 1400, 35);
}

void sfxSleepy() {
  hybridBeep(60, 800, 30); delay(40);
  hybridBeep(90, 450, 40);
}

void sfxClick() {
  hybridBeep(25, 1500, 15);
}

void sfxModeSwitch() {
  hybridBeep(35, 900, 20); delay(25);
  hybridBeep(55, 1400, 30);
}

// ============================================================================
// RAW I2C MPU6050 DRIVER (Auto-reconnects live when wires touch!)
// ============================================================================
bool initRawMPU() {
  uint8_t addresses[] = {0x68, 0x69};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(addresses[i]);
    if (Wire.endTransmission() == 0) {
      mpuAddr = addresses[i];
      Wire.beginTransmission(mpuAddr);
      Wire.write(0x6B);
      Wire.write(0x00);
      Wire.endTransmission();
      firstMpuRead = true;
      return true;
    }
  }
  return false;
}

void registerActivity(bool playWakeSound = false) {
  if (currentMood == MOOD_SLEEPING || currentMood == MOOD_TIRED) {
    currentMood = MOOD_DEFAULT;
    if (playWakeSound) sfxWakeUp();
  }
  lastActivityTime = millis();
}

void checkMpuShake() {
  unsigned long now = millis();
  if (now - lastMpuCheck < 60) return;
  lastMpuCheck = now;

  if (!mpuConnected) {
    if (now % 1500 < 70) {
      if (initRawMPU()) {
        mpuConnected = true;
        sfxClick(); // Beeps the instant your MPU6050 wires make contact!
      }
    }
    return;
  }

  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) {
    mpuConnected = false;
    return;
  }

  if (Wire.requestFrom((int)mpuAddr, 6, (int)true) == 6) {
    int16_t ax = (Wire.read() << 8) | Wire.read();
    int16_t ay = (Wire.read() << 8) | Wire.read();
    int16_t az = (Wire.read() << 8) | Wire.read();

    if (firstMpuRead) {
      prevAX = ax; prevAY = ay; prevAZ = az;
      firstMpuRead = false;
      return;
    }

    long delta = abs((long)ax - prevAX) + abs((long)ay - prevAY) + abs((long)az - prevAZ);
    prevAX = ax; prevAY = ay; prevAZ = az;

    if (delta > 8500) {
      bool wasAsleep = (currentMood == MOOD_SLEEPING || currentMood == MOOD_TIRED);
      registerActivity(false);
      if (wasAsleep) {
        currentMood = MOOD_DEFAULT;
        sfxWakeUp();
      } else if (currentMode == MODE_ROBOEYES && currentMood != MOOD_DIZZY) {
        currentMood = MOOD_DIZZY;
        moodRevertTime = now + 2600;
        sfxDizzyShake();
      }
    }
  }
}

// ============================================================================
// ROBOEYES ENGINE
// ============================================================================
float lerpVal(float current, float target, float speed) {
  return current + (target - current) * speed;
}

void updateAndDrawRoboEyes() {
  unsigned long now = millis();
  unsigned long idleMs = now - lastActivityTime;

  if (idleMs > SLEEP_TIMEOUT_MS) {
    if (currentMood != MOOD_SLEEPING) {
      currentMood = MOOD_SLEEPING;
      sfxSleepy();
    }
  } else if (idleMs > (SLEEP_TIMEOUT_MS - 5000) && currentMood == MOOD_DEFAULT) {
    currentMood = MOOD_TIRED;
  }

  if ((currentMood == MOOD_HAPPY_PET || currentMood == MOOD_DIZZY) && now > moodRevertTime) {
    currentMood = MOOD_DEFAULT;
  }

  if (currentMood == MOOD_SLEEPING) {
    display.clearDisplay();
    display.fillRoundRect(22, 34, 32, 4, 2, PIXEL_ON);
    display.fillRoundRect(74, 34, 32, 4, 2, PIXEL_ON);

    int zOffset = (now / 350) % 3;
    display.setTextSize(1);
    display.setTextColor(PIXEL_ON);
    display.setCursor(96, 22 - zOffset * 3);
    display.print("z");
    if (zOffset >= 1) { display.setCursor(104, 14 - zOffset * 2); display.print("Z"); }
    if (zOffset >= 2) { display.setCursor(112, 6); display.print("Z"); }
    display.display();
    return;
  }

  if (currentMood == MOOD_DEFAULT && (now - lastSaccadeTime > nextSaccadeInterval)) {
    lastSaccadeTime = now;
    nextSaccadeInterval = random(1800, 4000);
    int offsetX = random(-12, 13);
    int offsetY = random(-8, 9);
    int leftH  = 32 + (offsetX < -4 ? 4 : 0);
    int rightH = 32 + (offsetX > 4 ? 4 : 0);

    targetLX = 22 + offsetX; targetLY = 16 + offsetY; targetLW = 34; targetLH = leftH;
    targetRX = 72 + offsetX; targetRY = 16 + offsetY; targetRW = 34; targetRH = rightH;

    if (random(0, 4) == 0) hybridBeep(12, 1200, 10);
  }

  if (currentMood == MOOD_HAPPY_PET) {
    int bounce = ((now / 120) % 2 == 0) ? -2 : 2;
    targetLX = 22; targetLY = 14 + bounce; targetLW = 36; targetLH = 28;
    targetRX = 70; targetRY = 14 + bounce; targetRW = 36; targetRH = 28;
  } else if (currentMood == MOOD_DIZZY) {
    int shakeX = random(-8, 9);
    int shakeY = random(-5, 6);
    bool flip = ((now / 90) % 2 == 0);
    targetLX = 22 + shakeX; targetLY = 16 + shakeY; targetLW = 32; targetLH = flip ? 16 : 38;
    targetRX = 72 + shakeX; targetRY = 16 - shakeY; targetRW = 32; targetRH = flip ? 38 : 16;
  } else if (currentMood == MOOD_TIRED) {
    targetLX = 22; targetLY = 24; targetLW = 34; targetLH = 18;
    targetRX = 72; targetRY = 24; targetRW = 34; targetRH = 18;
  }

  if (!isBlinking && currentMood == MOOD_DEFAULT && (now - lastBlinkTime > nextBlinkInterval)) {
    isBlinking = true;
    blinkStartTime = now;
    lastBlinkTime = now;
    nextBlinkInterval = random(2500, 5000);
  }

  eyeLX = lerpVal(eyeLX, targetLX, 0.35);
  eyeLY = lerpVal(eyeLY, targetLY, 0.35);
  eyeLW = lerpVal(eyeLW, targetLW, 0.35);
  eyeLH = lerpVal(eyeLH, targetLH, 0.35);

  eyeRX = lerpVal(eyeRX, targetRX, 0.35);
  eyeRY = lerpVal(eyeRY, targetRY, 0.35);
  eyeRW = lerpVal(eyeRW, targetRW, 0.35);
  eyeRH = lerpVal(eyeRH, targetRH, 0.35);

  float drawLH = eyeLH, drawRH = eyeRH, drawLY = eyeLY, drawRY = eyeRY;
  if (isBlinking) {
    if (now - blinkStartTime < 110) {
      drawLH = 4; drawRH = 4;
      drawLY = eyeLY + (eyeLH / 2);
      drawRY = eyeRY + (eyeRH / 2);
    } else {
      isBlinking = false;
    }
  }

  display.clearDisplay();
  display.fillRoundRect((int)eyeLX, (int)drawLY, (int)eyeLW, (int)drawLH, 8, PIXEL_ON);
  display.fillRoundRect((int)eyeRX, (int)drawRY, (int)eyeRW, (int)drawRH, 8, PIXEL_ON);

  if (currentMood == MOOD_HAPPY_PET) {
    display.fillRoundRect((int)eyeLX - 2, (int)drawLY + 14, (int)eyeLW + 4, (int)drawLH, 10, PIXEL_OFF);
    display.fillRoundRect((int)eyeRX - 2, (int)drawRY + 14, (int)eyeRW + 4, (int)drawRH, 10, PIXEL_OFF);
    int floatY = (now / 150) % 6;
    display.drawBitmap(58, 4 + floatY, heart8x7, 8, 7, PIXEL_ON);
    display.drawBitmap(10, 6 + (5 - floatY), heart8x7, 8, 7, PIXEL_ON);
    display.drawBitmap(110, 6 + (5 - floatY), heart8x7, 8, 7, PIXEL_ON);
  } else if (currentMood == MOOD_TIRED) {
    display.fillTriangle((int)eyeLX, (int)drawLY, (int)(eyeLX + eyeLW), (int)drawLY, (int)eyeLX, (int)drawLY + 10, PIXEL_OFF);
    display.fillTriangle((int)eyeRX, (int)drawRY, (int)(eyeRX + eyeRW), (int)drawRY, (int)(eyeRX + eyeRW), (int)drawRY + 10, PIXEL_OFF);
  }

  display.display();
}

// ============================================================================
// 1-HOUR STUDY TIMER + PIXEL CAT
// ============================================================================
void updateAndDrawTimerCat() {
  unsigned long now = millis();

  if (timerRunning && timerRemainingMs > 0) {
    if (now - lastTimerTick >= 1000) {
      unsigned long elapsedSec = (now - lastTimerTick) / 1000;
      lastTimerTick += elapsedSec * 1000;
      if (timerRemainingMs > elapsedSec * 1000) {
        timerRemainingMs -= elapsedSec * 1000;
      } else {
        timerRemainingMs = 0;
        timerRunning = false;
        sfxPetHappy();
      }
    }
  } else {
    lastTimerTick = now;
  }

  if (now - lastCatStateChange > 6000) {
    lastCatStateChange = now;
    catAction = random(0, 3);
  }

  if (now - lastAnimTick > 220) {
    lastAnimTick = now;
    animFrame = !animFrame;
    if (catAction == 0) {
      catX += catDir * 3.5;
      if (catX > 104) { catX = 104; catDir = -1; }
      if (catX < 8)   { catX = 8;   catDir = 1;  }
    }
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(PIXEL_ON);
  display.setCursor(4, 2);
  display.print(timerRunning ? "FOCUS TIMER" : "PAUSED");
  display.drawBitmap(114, 2, heart8x7, 8, 7, PIXEL_ON);
  display.drawLine(0, 12, 127, 12, PIXEL_ON);

  unsigned long totalSecs = timerRemainingMs / 1000;
  char timeBuf[10];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", totalSecs / 60, totalSecs % 60);

  display.setTextSize(3);
  display.setCursor(19, 18);
  display.print(timeBuf);

  display.drawLine(0, 62, 127, 62, PIXEL_ON);

  int cx = (int)catX;
  if (catAction == 0) {
    display.drawBitmap(cx, 50, animFrame ? catWalk1_16x12 : catWalk2_16x12, 16, 12, PIXEL_ON);
  } else if (catAction == 1) {
    display.drawBitmap(cx, 50, catSleep_16x12, 16, 12, PIXEL_ON);
    display.setTextSize(1);
    display.setCursor(cx + 18, animFrame ? 46 : 49);
    display.print("zZ");
  } else {
    int jumpY = animFrame ? 46 : 50;
    display.drawBitmap(cx, jumpY, catWalk1_16x12, 16, 12, PIXEL_ON);
    display.drawBitmap(cx + 18, animFrame ? 49 : 44, heart8x7, 8, 7, PIXEL_ON);
  }

  display.display();
}

// ============================================================================
// SLEEK PIXEL-ART SPOTIFY DECK & SYNCED LYRICS
// ============================================================================
void formatMsTime(unsigned long ms, char* outBuf, size_t bufSize) {
  unsigned long totalSec = ms / 1000;
  snprintf(outBuf, bufSize, "%02lu:%02lu", totalSec / 60, totalSec % 60);
}

void updateAndDrawSpotify() {
  unsigned long now = millis();

  if (spotifyPlaying && (now - lastProgressTick >= 200)) {
    unsigned long dt = now - lastProgressTick;
    lastProgressTick = now;
    if (trackProgressMs + dt < trackDurationMs) {
      trackProgressMs += dt;
    }
  } else {
    lastProgressTick = now;
  }

  display.clearDisplay();
  display.setTextColor(PIXEL_ON);
  display.setTextSize(1);

  if (!showLyricsView) {
    display.drawRoundRect(2, 2, 34, 34, 5, PIXEL_ON);
    display.drawCircle(19, 19, 12, PIXEL_ON);
    display.drawCircle(19, 19, 4, PIXEL_ON);
    display.fillCircle(19, 19, 1, PIXEL_ON);

    int spinPhase = (now / 180) % 4;
    int gx = 19 + (spinPhase == 0 ? 7 : (spinPhase == 2 ? -7 : 0));
    int gy = 19 + (spinPhase == 1 ? 7 : (spinPhase == 3 ? -7 : 0));
    display.fillCircle(gx, gy, 1, PIXEL_ON);

    String shownTitle = currentTrack;
    if (currentTrack.length() > 13) {
      if (now - lastScrollTick > 220) {
        lastScrollTick = now;
        titleScrollOffset = (titleScrollOffset + 1) % (currentTrack.length() + 4);
      }
      String padded = currentTrack + "   " + currentTrack;
      shownTitle = padded.substring(titleScrollOffset, titleScrollOffset + 13);
    }
    display.setCursor(41, 6);
    display.print(shownTitle);

    display.setCursor(41, 18);
    display.print(currentArtist.substring(0, 14));
    display.drawBitmap(118, (now / 300) % 2 ? 4 : 6, musicNote8x8, 8, 8, PIXEL_ON);

    char curTimeStr[8], totTimeStr[8];
    formatMsTime(trackProgressMs, curTimeStr, sizeof(curTimeStr));
    formatMsTime(trackDurationMs, totTimeStr, sizeof(totTimeStr));

    display.setCursor(41, 29);
    display.print(curTimeStr);
    display.setCursor(96, 29);
    display.print(totTimeStr);

    int barMax = 120;
    int fillW = (trackDurationMs > 0) ? map(trackProgressMs, 0, trackDurationMs, 0, barMax) : 0;
    fillW = constrain(fillW, 0, barMax);
    display.drawLine(4, 42, 124, 42, PIXEL_ON);
    display.fillRect(4, 41, fillW, 3, PIXEL_ON);
    display.fillCircle(4 + fillW, 42, 2, PIXEL_ON);

    display.fillRect(28, 51, 2, 8, PIXEL_ON);
    display.fillTriangle(30, 55, 36, 51, 36, 59, PIXEL_ON);

    display.drawRoundRect(53, 48, 22, 14, 4, PIXEL_ON);
    if (spotifyPlaying) {
      display.fillRect(60, 51, 3, 8, PIXEL_ON);
      display.fillRect(65, 51, 3, 8, PIXEL_ON);
    } else {
      display.fillTriangle(61, 51, 61, 59, 68, 55, PIXEL_ON);
    }

    display.fillTriangle(98, 55, 92, 51, 92, 59, PIXEL_ON);
    display.fillRect(98, 51, 2, 8, PIXEL_ON);

    display.drawBitmap(110, 51, ((now / 260) % 2) ? catWalk1_16x12 : catWalk2_16x12, 16, 12, PIXEL_ON);
  } else {
    display.drawBitmap(2, 1, musicNote8x8, 8, 8, PIXEL_ON);
    display.setCursor(14, 1);
    display.print(currentTrack.substring(0, 18));
    display.drawLine(0, 10, 127, 10, PIXEL_ON);

    String lrc = currentLyricLine;
    lrc.trim();
    if (lrc.length() == 0) lrc = "~ Instrumental ~";

    int yPos = 16;
    int idx = 0;
    int lineCount = 0;
    while (idx < (int)lrc.length() && lineCount < 3) {
      int remaining = lrc.length() - idx;
      String chunk;
      if (remaining <= 20) {
        chunk = lrc.substring(idx);
        idx = lrc.length();
      } else {
        int splitAt = lrc.lastIndexOf(' ', idx + 20);
        if (splitAt <= idx) splitAt = idx + 20;
        chunk = lrc.substring(idx, splitAt);
        idx = splitAt + 1;
      }
      int xPos = max(0, (128 - (int)chunk.length() * 6) / 2);
      display.setCursor(xPos, yPos);
      display.print(chunk);
      yPos += 11;
      lineCount++;
    }

    int fillW = (trackDurationMs > 0) ? map(trackProgressMs, 0, trackDurationMs, 0, 127) : 0;
    display.drawLine(0, 63, constrain(fillW, 0, 127), 63, PIXEL_ON);
    display.drawBitmap(56, 50, ((now / 240) % 2) ? catWalk1_16x12 : catWalk2_16x12, 16, 12, PIXEL_ON);
  }

  display.display();
}

// ============================================================================
// USB SERIAL (COM7) + UDP WIFI PACKET RECEIVER
// ============================================================================
void applyIncomingTrackData(bool playing, String title, String artist, unsigned long pos, unsigned long dur, String lyric) {
  bool wasPlaying = spotifyPlaying;
  spotifyPlaying = playing;

  if (title.length() > 0 && title != currentTrack) {
    currentTrack = title;
    titleScrollOffset = 0;
  }
  if (artist.length() > 0) currentArtist = artist;
  trackProgressMs = pos;
  lastProgressTick = millis();
  if (dur > 1000) trackDurationMs = dur;
  if (lyric.length() > 0) currentLyricLine = lyric;

  if (spotifyPlaying && currentMode != MODE_SPOTIFY) {
    modeBeforeSpotify = currentMode;
    currentMode = MODE_SPOTIFY;
    registerActivity(false);
    sfxModeSwitch();
  } else if (!spotifyPlaying && wasPlaying && currentMode == MODE_SPOTIFY) {
    currentMode = modeBeforeSpotify;
    registerActivity(false);
  }
}

void parsePipePacket(String pkt) {
  int startIdx = pkt.indexOf("SPOT|");
  if (startIdx == -1) return;
  pkt = pkt.substring(startIdx);

  int p1 = pkt.indexOf('|', 5);
  int p2 = pkt.indexOf('|', p1 + 1);
  int p3 = pkt.indexOf('|', p2 + 1);
  int p4 = pkt.indexOf('|', p3 + 1);
  int p5 = pkt.indexOf('|', p4 + 1);
  if (p1 == -1 || p2 == -1 || p3 == -1 || p4 == -1 || p5 == -1) return;

  bool playing = (pkt.substring(5, p1) == "1");
  String title = pkt.substring(p1 + 1, p2);
  String artist = pkt.substring(p2 + 1, p3);
  unsigned long pos = pkt.substring(p3 + 1, p4).toInt();
  unsigned long dur = pkt.substring(p4 + 1, p5).toInt();
  String lyric = pkt.substring(p5 + 1);

  applyIncomingTrackData(playing, title, artist, pos, dur, lyric);
}

void checkUdpAndSerialSpotify() {
  // 1. Check Wi-Fi UDP packets
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char buf[512];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      parsePipePacket(String(buf));
    }
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.print("OK");
    udp.endPacket();
  }

  // 2. Check Direct USB Cable (COM7) Serial packets!
  while (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.indexOf("SPOT|") != -1) {
      parsePipePacket(line);
    }
  }
}

// ============================================================================
// BUTTON HANDLER (D19)
// ============================================================================
bool lastBtnState = HIGH;
unsigned long btnPressStart = 0;
bool longPressFired = false;
int clickCount = 0;
unsigned long lastClickRelease = 0;

void handleButtons() {
  unsigned long now = millis();
  bool reading = digitalRead(PIN_BTN_MAIN);

  if (reading == LOW && lastBtnState == HIGH && (now - btnPressStart > 40)) {
    btnPressStart = now;
    longPressFired = false;
    registerActivity(true);
  }

  if (reading == LOW && !longPressFired && (now - btnPressStart > 600)) {
    longPressFired = true;
    clickCount = 0;
    sfxModeSwitch();

    if (currentMode == MODE_ROBOEYES) {
      currentMode = MODE_TIMER_CAT;
      lastTimerTick = now;
    } else if (currentMode == MODE_TIMER_CAT) {
      currentMode = MODE_SPOTIFY;
      lastProgressTick = now;
    } else {
      currentMode = MODE_ROBOEYES;
    }
  }

  if (reading == HIGH && lastBtnState == LOW) {
    if (!longPressFired && (now - btnPressStart > 30)) {
      clickCount++;
      lastClickRelease = now;
    }
  }

  if (clickCount > 0 && (now - lastClickRelease > 250)) {
    if (clickCount == 1) {
      if (currentMode == MODE_ROBOEYES) {
        currentMood = MOOD_HAPPY_PET;
        moodRevertTime = now + 3000;
        sfxPetHappy();
      } else if (currentMode == MODE_TIMER_CAT) {
        timerRunning = !timerRunning;
        lastTimerTick = now;
        sfxClick();
      } else if (currentMode == MODE_SPOTIFY) {
        showLyricsView = !showLyricsView;
        sfxClick();
      }
    } else if (clickCount >= 2) {
      if (currentMode == MODE_TIMER_CAT) {
        timerRemainingMs = ONE_HOUR_MS;
        timerRunning = false;
        sfxModeSwitch();
      } else {
        currentMood = MOOD_HAPPY_PET;
        moodRevertTime = now + 3000;
        sfxPetHappy();
      }
    }
    clickCount = 0;
  }

  lastBtnState = reading;
}

// ============================================================================
// SMART WIFI SCANNER & SETUP
// ============================================================================
void connectSmartWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.setSleep(false);

  String matchedSSID = TARGET_SSID;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.indexOf("A25") != -1 || s.indexOf("YO") != -1) {
      matchedSSID = s; // Automatically catches trailing spaces or slight name changes!
      break;
    }
  }

  WiFi.begin(matchedSSID.c_str(), TARGET_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start < 10000)) {
    delay(300);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(25);

  pinMode(PIN_BTN_MAIN, INPUT_PULLUP);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(PIN_SDA, PIN_SCL);
  display.begin(OLED_ADDR, true);

  mpuConnected = initRawMPU();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(PIXEL_ON);
  display.setCursor(4, 4);
  display.println("DESKBOT OS v2.2");
  display.setCursor(4, 18);
  display.print("MPU6050: ");
  display.println(mpuConnected ? "OK! (0x68)" : "MOVE WIRES!");
  display.setCursor(4, 32);
  display.println("USB COM7: READY!");
  display.setCursor(4, 46);
  display.println("Scanning Hotspot...");
  display.display();

  sfxStartup(); // Tests both Active DC & Passive Tone on D4!

  connectSmartWiFi();

  display.clearDisplay();
  display.setCursor(4, 8);
  display.print("MPU6050: ");
  display.println(mpuConnected ? "READY!" : "CHECK RAILS");
  display.setCursor(4, 24);
  if (WiFi.status() == WL_CONNECTED) {
    display.println("WiFi Connected!");
    display.setCursor(4, 38);
    display.print("IP: ");
    display.println(WiFi.localIP().toString());
  } else {
    display.println("USB Bridge Mode ON!");
    display.setCursor(4, 38);
    display.println("(Works via COM7!)");
  }
  display.display();
  delay(2200);

  udp.begin(UDP_PORT);
  lastActivityTime = millis();
  lastTimerTick = millis();
}

void loop() {
  checkUdpAndSerialSpotify();
  handleButtons();
  checkMpuShake();

  if (currentMode == MODE_ROBOEYES) {
    updateAndDrawRoboEyes();
  } else if (currentMode == MODE_TIMER_CAT) {
    updateAndDrawTimerCat();
  } else if (currentMode == MODE_SPOTIFY) {
    updateAndDrawSpotify();
  }

  delay(16);
}