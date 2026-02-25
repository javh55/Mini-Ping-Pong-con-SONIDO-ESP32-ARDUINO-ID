/*
  ESP32 + OLED SSD1306 128x64 (I2C) - Mini Ping-Pong con SONIDO (usando tone())
  - Pantalla de INICIO: "ESP32 / POGN" con parpadeo + "press > to start!"
  - Paleta con doble velocidad
  - Cronómetro mm:ss en HUD (arriba derecha)
  - HUD: "Pong's: "
  - GAME OVER: tiempo total, BEST persistente (NVS)
  - Reinicio manteniendo ambos botones: "Hold <+> to restart!"
  - SONIDO con tone() en GPIO 25:
     * Paleta: beep corto agudo
     * Paredes/Techo: Opción B (doble micropulso suave)
     * Pierde punto: 2 graves
     * Primer start: medio
     * Nuevo BEST: “fiesta” 2 s
  - NUEVO: Alternancia de velocidad por ciclo:
     * 20 s normal (x1) = bola sólida
     *  5 s rápida (x2)  = bola contorno
     * y se repite
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// --- Pantalla ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Botones (INPUT_PULLUP: activos LOW) ---
const int BTN_LEFT  = 32;  // "<"
const int BTN_RIGHT = 33;  // ">" (start)

// --- Estados del juego ---
enum GameState { STATE_START, STATE_PLAYING, STATE_GAMEOVER };
GameState gameState = STATE_START;

// --- Juego ---
int score = 10;

// Paleta
float paddleX;
const int   paddleWidth   = 24;
const int   paddleHeight  = 4;
const int   paddleY       = SCREEN_HEIGHT - paddleHeight - 1;
const float paddleSpeed   = 5.2f;  // Doble velocidad

// Bola
float ballX, ballY;
float ballVX, ballVY;
const int ballR = 2;

// --- Alternancia de velocidad (20s normal / 5s rápida) ---
const uint16_t SLOW_SEC = 20;
const uint16_t FAST_SEC = 5;
float     speedMultiplier = 1.0f; // 1.0 normal, 2.0 doble
bool      fastMode        = false; // true => contorno/x2
uint32_t  modeStartSec    = 0;     // instante (en elapsedSec) cuando inició el modo actual

// Timing (FPS)
const uint16_t FPS = 60;
const uint32_t FRAME_INTERVAL = 1000UL / FPS;
uint32_t lastFrameMs = 0;

// --- Cronómetro ---
uint32_t runStartMs = 0;
uint32_t lastSecondMs = 0;
uint16_t elapsedSec = 0;

// --- Tiempo final capturado al perder ---
bool     finalTimeCaptured = false;
uint16_t finalElapsedSec   = 0;

// --- Best time persistente ---
Preferences prefs;
uint16_t bestTimeSec = 0; // se carga de NVS en setup()

// --- Start screen: parpadeo del título ---
bool     blinkOn = true;
uint32_t lastBlinkMs = 0;
const    uint32_t BLINK_INTERVAL = 500; // ms

// --- Flag: beep de inicio solo la primera vez tras encender ---
bool hasPlayedFirstStartBeep = false;

// ============ AUDIO (tone()) ============
#define AUDIO_PIN 25

void audioInit() {
  pinMode(AUDIO_PIN, OUTPUT);
  noTone(AUDIO_PIN); // silencio inicial
}

inline void toneOn(unsigned int freq) {
  if (freq == 0) { noTone(AUDIO_PIN); return; }
  tone(AUDIO_PIN, freq);
}

inline void toneOff() {
  noTone(AUDIO_PIN);
}

void beep(uint16_t freq, uint16_t ms, uint16_t gapMs = 0) {
  toneOn(freq);
  delay(ms);
  toneOff();
  if (gapMs) delay(gapMs);
}

// --- SFX predefinidos ---
void sfxPaddleHit() {           // corto y agudo
  beep(2400, 25, 0);
}

// Rebote paredes/techo (Opción B)
void sfxWallBounce() {
  beep(1800, 4, 2);   // 4 ms ON, 2 ms OFF
  beep(1500, 3, 0);   // 3 ms ON
}

// 2 graves al perder punto
void sfxLosePoint() {
  for (int i = 0; i < 2; ++i) {
    beep(300, 120, 80);
  }
}

void sfxStartFirst() {          // medio, al primer start
  beep(1100, 120, 0);
}

// Fiesta por BEST: 2 s
void sfxParty(uint32_t ms = 2000) {
  const uint16_t notes[] = { 880, 988, 1047, 1175, 1319, 1480 }; // A5..F#6
  const uint8_t  N = sizeof(notes) / sizeof(notes[0]);
  const uint16_t onMs = 90, offMs = 30;
  uint32_t end = millis() + ms;
  uint16_t i = 0;
  while (millis() < end) {
    beep(notes[i % N], onMs, offMs);
    i++;
  }
}

// =========================================

// Utilidad: ambos botones para reinicio (en GAME OVER)
bool bothButtonsHeld(uint16_t msRequired = 800) {
  static uint32_t t0 = 0;
  bool l = (digitalRead(BTN_LEFT)  == LOW);
  bool r = (digitalRead(BTN_RIGHT) == LOW);
  if (l && r) {
    if (t0 == 0) t0 = millis();
    return (millis() - t0) >= msRequired;
  } else {
    t0 = 0;
    return false;
  }
}

void resetBall() {
  ballX = SCREEN_WIDTH / 2.0f;
  ballY = SCREEN_HEIGHT / 3.0f;

  float s = 1.7f; // velocidad base de bola
  ballVX = (random(0, 2) == 0) ? -s : s;
  ballVY = s;
}

// --- Alternancia: helpers ---
void setSpeedMode(bool fast) {
  fastMode = fast;
  speedMultiplier = fast ? 2.0f : 1.0f;
  modeStartSec = elapsedSec; // marca inicio del modo
}

void updateSpeedMode() {
  uint16_t elapsedInMode = elapsedSec - modeStartSec;
  if (fastMode) {
    if (elapsedInMode >= FAST_SEC) setSpeedMode(false);  // 5 s rápida -> vuelve a normal
  } else {
    if (elapsedInMode >= SLOW_SEC) setSpeedMode(true);   // 20 s normal -> pasa a rápida
  }
}

void resetGameRuntime() {
  runStartMs   = millis();
  lastSecondMs = runStartMs;
  elapsedSec   = 0;

  // Empieza en modo normal (sólida, x1) por 20 s
  setSpeedMode(false);

  finalTimeCaptured = false;
  finalElapsedSec   = 0;
}

void resetGame() {
  score = 10;
  paddleX = (SCREEN_WIDTH - paddleWidth) / 2.0f;
  resetBall();
  resetGameRuntime();
}

void formatTimeMMSS(uint16_t secs, char* out, size_t n) {
  uint16_t mm = secs / 60;
  uint16_t ss = secs % 60;
  if (n >= 6) snprintf(out, n, "%02u:%02u", mm, ss);
  else if (n > 0) out[0] = '\0';
}

void drawHUD() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Izquierda: Contador
  display.setCursor(0, 0);
  display.print(F("Pong's: "));
  display.print(score);

  // Derecha: Cronómetro mm:ss
  char tbuf[8];
  formatTimeMMSS(elapsedSec, tbuf, sizeof(tbuf));
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(tbuf, 0, 0, &x1, &y1, &w, &h);
  int tx = SCREEN_WIDTH - (int)w - 1;
  display.setCursor(tx, 0);
  display.print(tbuf);
}

void drawPaddle() {
  display.fillRect((int)roundf(paddleX), paddleY, paddleWidth, paddleHeight, SSD1306_WHITE);
}

void drawBall() {
  int bx = (int)roundf(ballX);
  int by = (int)roundf(ballY);
  if (fastMode) {
    display.drawCircle(bx, by, ballR, SSD1306_WHITE); // contorno (x2)
  } else {
    display.fillCircle(bx, by, ballR, SSD1306_WHITE); // sólida (x1)
  }
}

void handleButtonsPaddle() {
  if (digitalRead(BTN_LEFT)  == LOW) paddleX -= paddleSpeed;
  if (digitalRead(BTN_RIGHT) == LOW) paddleX += paddleSpeed;
  if (paddleX < 0) paddleX = 0;
  if (paddleX > (SCREEN_WIDTH - paddleWidth)) paddleX = SCREEN_WIDTH - paddleWidth;
}

void captureFinalTimeAndUpdateBest() {
  if (!finalTimeCaptured) {
    uint32_t now = millis();
    finalElapsedSec = (uint16_t)((now - runStartMs) / 1000UL);
    finalTimeCaptured = true;
    if (finalElapsedSec > bestTimeSec) {
      bestTimeSec = finalElapsedSec;
      prefs.putUShort("best", bestTimeSec);
      sfxParty(2000);  // fiesta 2 s
    }
  }
}

void updateBall() {
  // Movimiento con multiplicador de velocidad
  ballX += ballVX * speedMultiplier;
  ballY += ballVY * speedMultiplier;

  // Rebotes laterales
  if (ballX - ballR <= 0) {
    ballX = ballR;
    ballVX = fabs(ballVX);
    sfxWallBounce();
  } else if (ballX + ballR >= SCREEN_WIDTH) {
    ballX = SCREEN_WIDTH - ballR;
    ballVX = -fabs(ballVX);
    sfxWallBounce();
  }

  // Rebote techo
  if (ballY - ballR <= 0) {
    ballY = ballR;
    ballVY = fabs(ballVY);
    sfxWallBounce();
  }

  // Colisión paleta o fallo
  bool baja = (ballVY > 0);
  if (baja && (ballY + ballR >= paddleY)) {
    if (ballX >= paddleX && ballX <= (paddleX + paddleWidth)) {
      // Rebote en paleta
      ballY = paddleY - ballR;
      ballVY = -fabs(ballVY);

      float cx = paddleX + paddleWidth / 2.0f;
      float offset = (ballX - cx) / (paddleWidth / 2.0f); // -1..1
      float maxVX = 2.8f;
      ballVX += offset * 0.8f;
      if (ballVX >  maxVX) ballVX =  maxVX;
      if (ballVX < -maxVX) ballVX = -maxVX;

      float acc = 1.04f;
      ballVX *= acc;
      ballVY *= acc;

      sfxPaddleHit();
    } else {
      // Pierde un punto
      score--;
      sfxLosePoint();

      if (score <= 0) {
        captureFinalTimeAndUpdateBest();
        gameState = STATE_GAMEOVER;
      } else {
        resetBall();
      }
    }
  }
}

void drawStartScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- Título en dos líneas, centrado, con parpadeo ---
  const uint8_t TITLE_SIZE = 2;

  if (blinkOn) {
    display.setTextSize(TITLE_SIZE);

    // "ESP32"
    const char* line1 = "ESP32";
    int16_t x1, y1; uint16_t w1, h1;
    display.getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);
    int lx1 = (SCREEN_WIDTH - (int)w1) / 2;
    int ly1 = 8;
    display.setCursor(lx1, ly1);
    display.print(line1);

    // "POGN"
    const char* line2 = "POGN";
    int16_t x2, y2; uint16_t w2, h2;
    display.getTextBounds(line2, 0, 0, &x2, &y2, &w2, &h2);
    int lineGap = 6; 
    int ly2 = ly1 + h1 + lineGap;
    int lx2 = (SCREEN_WIDTH - (int)w2) / 2;
    display.setCursor(lx2, ly2);
    display.print(line2);
  }

  // Prompt inferior
  display.setTextSize(1);
  const char* prompt = "press > to start!";
  int16_t px1, py1; uint16_t pw, ph;
  display.getTextBounds(prompt, 0, 0, &px1, &py1, &pw, &ph);
  int px = (SCREEN_WIDTH - (int)pw) / 2;
  int py = SCREEN_HEIGHT - ph - 4;
  display.setCursor(px, py);
  display.print(prompt);

  display.display();
}

void drawGameOver() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Título
  display.setTextSize(2);
  const char* msg = "GAME OVER";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  int gx = (SCREEN_WIDTH - (int)w) / 2;
  int gy = 2;
  display.setCursor(gx, gy);
  display.print(msg);

  // Tiempo total
  char timeStr[8];
  formatTimeMMSS(finalElapsedSec, timeStr, sizeof(timeStr));
  char tbuf[18];
  snprintf(tbuf, sizeof(tbuf), "Tiempo: %s", timeStr);
  display.setTextSize(1);
  display.getTextBounds(tbuf, 0, 0, &x1, &y1, &w, &h);
  int tx = (SCREEN_WIDTH - (int)w) / 2;
  int ty = 28;
  display.setCursor(tx, ty);
  display.print(tbuf);

  // Best persistente (NVS)
  char bestStr[8];
  formatTimeMMSS(bestTimeSec, bestStr, sizeof(bestStr));
  char bbuf[18];
  snprintf(bbuf, sizeof(bbuf), "Best:   %s", bestStr);
  display.getTextBounds(bbuf, 0, 0, &x1, &y1, &w, &h);
  int bx = (SCREEN_WIDTH - (int)w) / 2;
  int by = 40;
  display.setCursor(bx, by);
  display.print(bbuf);

  // Mensaje de reinicio
  display.setCursor(8, 56);
  display.print(F("Hold <+> to restart!"));

  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);

  Wire.begin(21, 22); // SDA=21, SCL=22

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;){ } // OLED no encontrado
  }

  display.clearDisplay();
  display.display();

  randomSeed(analogRead(34));

  // Cargar BEST de NVS
  prefs.begin("pong", false);
  bestTimeSec = prefs.getUShort("best", 0);

  // AUDIO
  audioInit();

  // Estado inicial: pantalla de inicio
  gameState   = STATE_START;
  blinkOn     = true;
  lastBlinkMs = millis();
}

void loop() {
  uint32_t now = millis();
  if (now - lastFrameMs < FRAME_INTERVAL) {
    delay(1);
    return;
  }
  lastFrameMs = now;

  switch (gameState) {
    case STATE_START: {
      if (now - lastBlinkMs >= BLINK_INTERVAL) {
        blinkOn = !blinkOn;
        lastBlinkMs = now;
      }
      drawStartScreen();

      if (digitalRead(BTN_RIGHT) == LOW) {
        if (!hasPlayedFirstStartBeep) {
          sfxStartFirst();
          hasPlayedFirstStartBeep = true;
        }
        delay(60);
        while (digitalRead(BTN_RIGHT) == LOW) { delay(10); }
        resetGame();
        gameState = STATE_PLAYING;
      }
    } break;

    case STATE_PLAYING: {
      // Cronómetro (de a 1 s)
      if (now - lastSecondMs >= 1000UL) {
        uint32_t steps = (now - lastSecondMs) / 1000UL;
        elapsedSec += steps;
        lastSecondMs += steps * 1000UL;
      }

      // Alternar modos según 20s/5s
      updateSpeedMode();

      handleButtonsPaddle();
      updateBall();

      display.clearDisplay();
      drawHUD();
      drawPaddle();
      drawBall();
      display.display();
    } break;

    case STATE_GAMEOVER: {
      drawGameOver();
      if (bothButtonsHeld(800)) {
        delay(300);
        gameState = STATE_START;
        blinkOn = true;
        lastBlinkMs = millis();
      }
    } break;
  }
}
