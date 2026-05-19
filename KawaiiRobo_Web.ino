// =============================================================================
//  KawaiiRobo_Web.ino — ESP32-C3 Super Mini
//  OLEDFace v2.0 (tick engine) + Touch gestures + Full Async Web UI
//
//  Place this file in the same folder as OLEDFace.h and OLEDFace.cpp
//
//  PINS:
//    OLED  SDA → GPIO 8  |  SCL → GPIO 9
//    Touch OUT → GPIO 4
//    Motor PWM → GPIO 2  ← MUST use NPN transistor driver (see wiring below)
//
//  MOTOR WIRING (NEVER connect motor directly to GPIO):
//    GPIO 2 ──[1kΩ]── Base (NPN: 2N2222 / BC547 / S8050)
//    Collector ────── Motor (+) ──── VIN / 5V
//    Emitter   ────── GND
//    1N4007 flyback diode across motor terminals (cathode toward +)
//
//  LIBRARIES (install via Library Manager):
//    - Adafruit SSD1306
//    - Adafruit GFX Library
//    - ESPAsyncWebServer  (me-no-dev or mathieucarbou fork)
//    - AsyncTCP           (me-no-dev)
//
//  WEB UI:  http://<device-ip>/
//    - Live face preview grid (all 44 scenes + symbols + text)
//    - Touch gesture simulator buttons
//    - Emotion state controls
//    - Name / message input
//    - FPS slider
//    - Sleep trigger
//    - Live status panel (current scene, emotion, uptime)
//
//  TOUCH GESTURES (unchanged from original KawaiiRobo):
//    Single tap          → CURIOUS
//    Double tap          → cycle next emotion ring
//    Triple+ tap         → ANGRY
//    5 rapid taps / 2s   → CONFUSED
//    Long press ≥ 1.2s   → CUDDLE
//    Idle 5s             → IDLE (auto)
//    Idle 2 min          → light sleep (touch to wake)
// =============================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "OLEDFace.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "driver/gpio.h"
#include "esp_sleep.h"

// =============================================================================
//  USER CONFIGURATION  ← edit these
// =============================================================================


#define SDA_PIN 8
#define SCL_PIN 9
#define TOUCH_PIN 4
#define MOTOR_PIN 2
#define OLED_ADDR 0x3C

// =============================================================================
//  TIMING CONSTANTS
// =============================================================================

#define FRAME_MS 33ul              // ~30 fps animation slice
#define TOUCH_POLL_MS 5ul          // touch sampled every 5 ms
#define EMOTION_HOLD_MS 5000ul     // non-idle emotion held 5 s then → IDLE
#define SLEEP_TIMEOUT_MS 120000ul  // 2 min idle → light sleep
#define MULTI_TAP_WIN 700ul        // window to collect multi-tap counts
#define LONG_PRESS_MS 1200ul       // hold threshold for CUDDLE
#define RAPID_WIN 2000ul           // window for 5-rapid-tap detection
#define RAPID_THRESH 5             // taps within RAPID_WIN → CONFUSED

// =============================================================================
//  ENUMS
// =============================================================================

enum Emotion : uint8_t {
  EMO_IDLE = 0,
  EMO_CURIOUS,
  EMO_V_CURIOUS,
  EMO_ANGRY,
  EMO_CONFUSED,
  EMO_CUDDLE,
  EMO_SAD,
  EMO_HAPPY,
  EMO_SLEEPY,
  EMO_LOVE,
  EMO_COUNT
};

enum HapticPattern : uint8_t {
  HAP_NONE = 0,
  HAP_CURIOUS,
  HAP_V_CURIOUS,
  HAP_ANGRY,
  HAP_CONFUSED,
  HAP_CUDDLE,
  HAP_SAD,
  HAP_HAPPY,
  HAP_SLEEPY,
  HAP_LOVE,
  HAP_COUNT
};

// =============================================================================
//  GLOBALS
// =============================================================================

OLEDFace face;
AsyncWebServer server(80);

// Raw display object used ONLY for blanking screen in sleep
Adafruit_SSD1306 rawDisplay(128, 64, &Wire, -1);

// ── Scene / emotion names ─────────────────────────────────────────────────────
const char* const SCENE_NAMES[SCENE_COUNT] = {
  "Cute Happy", "Emo Stare", "UwU", "Crying", "Angry Glitch",
  "Heart Eyes", "Dead Inside", "Sleepy", "Dizzy", "Wink Cute",
  "TwT", "Surprise", "Star Eyes", "Dot Bounce", "Glitch Scan",
  "Square Eyes", "Pixel Happy", "Robot Scan", "Loading", "Shy Blush",
  "Skeptical", "Wave", "Thumbs Up", "Handshake", "Bullet Dodge",
  "Smug", "Nervous", "Excited", "Cool", "Love Struck",
  "Thinking", "Woozy", "Scream",
  "♥ Heart", "☮ Peace", "★ Star", "♪ Music", "Zzz", "⚡ Bolt",
  "Hi!", "Hello", "Bye", "OK", "LOL"
};

const char* const EMOTION_NAMES[EMO_COUNT] = {
  "Idle", "Curious", "Very Curious", "Angry", "Confused",
  "Cuddle", "Sad", "Happy", "Sleepy", "Love"
};

// ── Emotion → scene + haptic mapping ─────────────────────────────────────────
struct EmotionMapping {
  uint8_t sceneId;
  HapticPattern haptic;
};
static const EmotionMapping EMO_MAP[EMO_COUNT] = {
  { SCENE_CUTE_HAPPY, HAP_NONE },     // IDLE
  { SCENE_DOT_BOUNCE, HAP_CURIOUS },  // CURIOUS
  { SCENE_SURPRISE, HAP_V_CURIOUS },  // V_CURIOUS
  { SCENE_ANGRY_GLITCH, HAP_ANGRY },  // ANGRY
  { SCENE_DIZZY, HAP_CONFUSED },      // CONFUSED
  { SCENE_HEART_EYES, HAP_CUDDLE },   // CUDDLE
  { SCENE_CRYING, HAP_SAD },          // SAD
  { SCENE_WINK_CUTE, HAP_HAPPY },     // HAPPY
  { SCENE_SLEEPY, HAP_SLEEPY },       // SLEEPY
  { SCENE_HEART_EYES, HAP_LOVE },     // LOVE
};

// ── State ─────────────────────────────────────────────────────────────────────
volatile Emotion g_emotion = EMO_IDLE;
volatile uint8_t g_activeScene = SCENE_CUTE_HAPPY;
uint32_t g_emotionTimer = 0;
uint32_t g_lastActivityTime = 0;
uint8_t g_nextEmotion = EMO_CURIOUS;  // double-tap cycle pointer
bool g_sleepPending = false;
bool g_autoReturn = true;  // auto-return to IDLE after hold
uint8_t g_fps = 30;

// Web-triggered one-shot flags (set by HTTP handler, consumed by loop)
volatile bool g_webSceneReq = false;
volatile uint8_t g_webSceneId = 0;
volatile bool g_webEmoReq = false;
volatile uint8_t g_webEmoId = 0;
volatile bool g_webNameReq = false;
volatile bool g_webMsgReq = false;
char g_webName[24] = { 0 };
char g_webMsg[64] = { 0 };

// =============================================================================
//  HAPTICS — non-blocking PWM step sequencer (identical to original)
// =============================================================================

#define HAPTIC_FREQ 5000
#define HAPTIC_RES 8

struct HapStep {
  uint8_t duty;
  uint16_t dur;
};

static const HapStep P_NONE[] = { { 0, 1 } };
static const HapStep P_CURIOUS[] = { { 120, 80 }, { 0, 30 } };
static const HapStep P_V_CURIOUS[] = { { 150, 70 }, { 0, 70 }, { 150, 70 }, { 0, 20 } };
static const HapStep P_ANGRY[] = {
  { 255, 130 }, { 0, 50 }, { 255, 130 }, { 0, 50 }, { 255, 130 }, { 0, 50 }, { 255, 130 }, { 0, 50 }, { 255, 100 }, { 0, 30 }
};
static const HapStep P_CONFUSED[] = {
  { 180, 60 }, { 0, 40 }, { 120, 100 }, { 0, 30 }, { 200, 50 }, { 0, 50 }, { 150, 80 }, { 0, 20 }, { 100, 40 }, { 0, 60 }, { 220, 70 }, { 0, 20 }
};
static const HapStep P_CUDDLE[] = {
  { 50, 20 }, { 70, 18 }, { 90, 16 }, { 110, 15 }, { 130, 15 }, { 150, 15 }, { 160, 20 }, { 150, 15 }, { 130, 15 }, { 110, 15 }, { 90, 16 }, { 70, 18 }, { 50, 20 }, { 0, 40 }, { 50, 20 }, { 70, 18 }, { 90, 16 }, { 110, 15 }, { 130, 15 }, { 150, 15 }, { 160, 20 }, { 150, 15 }, { 130, 15 }, { 110, 15 }, { 90, 16 }, { 70, 18 }, { 50, 20 }, { 0, 30 }
};
static const HapStep P_SAD[] = {
  { 200, 200 }, { 0, 100 }, { 140, 150 }, { 0, 80 }, { 80, 100 }, { 0, 60 }, { 40, 80 }, { 0, 20 }
};
static const HapStep P_HAPPY[] = { { 160, 60 }, { 0, 40 }, { 200, 90 }, { 0, 20 } };
static const HapStep P_SLEEPY[] = {
  { 40, 30 }, { 60, 25 }, { 80, 20 }, { 100, 20 }, { 80, 20 }, { 60, 25 }, { 40, 30 }, { 0, 100 }
};
static const HapStep P_LOVE[] = {
  { 180, 80 }, { 130, 50 }, { 0, 60 }, { 180, 80 }, { 130, 50 }, { 0, 60 }, { 180, 80 }, { 130, 50 }, { 0, 40 }
};

struct PatInfo {
  const HapStep* steps;
  uint8_t count;
};
static const PatInfo PATTERNS[HAP_COUNT] = {
  { P_NONE, sizeof(P_NONE) / sizeof(HapStep) },
  { P_CURIOUS, sizeof(P_CURIOUS) / sizeof(HapStep) },
  { P_V_CURIOUS, sizeof(P_V_CURIOUS) / sizeof(HapStep) },
  { P_ANGRY, sizeof(P_ANGRY) / sizeof(HapStep) },
  { P_CONFUSED, sizeof(P_CONFUSED) / sizeof(HapStep) },
  { P_CUDDLE, sizeof(P_CUDDLE) / sizeof(HapStep) },
  { P_SAD, sizeof(P_SAD) / sizeof(HapStep) },
  { P_HAPPY, sizeof(P_HAPPY) / sizeof(HapStep) },
  { P_SLEEPY, sizeof(P_SLEEPY) / sizeof(HapStep) },
  { P_LOVE, sizeof(P_LOVE) / sizeof(HapStep) },
};

HapticPattern g_hapPat = HAP_NONE;
uint8_t g_hapStep = 0;
uint32_t g_hapTime = 0;
bool g_hapActive = false;

void haptics_init() {
  ledcAttach(MOTOR_PIN, HAPTIC_FREQ, HAPTIC_RES);
  ledcWrite(MOTOR_PIN, 0);
}

void haptics_play(HapticPattern pat) {
  if (pat >= HAP_COUNT) return;
  g_hapPat = pat;
  g_hapStep = 0;
  g_hapTime = millis();
  g_hapActive = (pat != HAP_NONE);
  ledcWrite(MOTOR_PIN, g_hapActive ? PATTERNS[pat].steps[0].duty : 0);
}

void haptics_stop() {
  g_hapActive = false;
  ledcWrite(MOTOR_PIN, 0);
}

void handleHaptics() {
  if (!g_hapActive) return;
  uint32_t now = millis();
  const PatInfo& pi = PATTERNS[g_hapPat];
  if (now - g_hapTime >= pi.steps[g_hapStep].dur) {
    if (++g_hapStep >= pi.count) {
      haptics_stop();
      return;
    }
    g_hapTime = now;
    ledcWrite(MOTOR_PIN, pi.steps[g_hapStep].duty);
  }
}

// =============================================================================
//  EMOTION STATE MACHINE
// =============================================================================

void emotion_set(Emotion emo) {
  g_emotion = emo;
  g_emotionTimer = millis();
  g_lastActivityTime = millis();

  uint8_t next = (uint8_t)((emo + 1) % EMO_COUNT);
  if (next == EMO_IDLE) next = EMO_CURIOUS;
  g_nextEmotion = next;

  g_activeScene = EMO_MAP[emo].sceneId;
  face.requestScene(g_activeScene);
  haptics_play(EMO_MAP[emo].haptic);

  Serial.print(F("[EMO] "));
  Serial.print(EMOTION_NAMES[emo]);
  Serial.print(F("  → scene "));
  Serial.println(g_activeScene);
}

void emotion_tick(uint32_t now) {
  if (g_autoReturn && g_emotion != EMO_IDLE && now - g_emotionTimer > EMOTION_HOLD_MS) {
    emotion_set(EMO_IDLE);
  }
}

// =============================================================================
//  TOUCH HANDLER  (identical gesture logic to original KawaiiRobo)
// =============================================================================

bool g_tLastRaw = false;
bool g_tActive = false;
bool g_tLongDone = false;
uint32_t g_tStart = 0;
uint32_t g_tLastTap = 0;
uint8_t g_tCount = 0;
uint8_t g_tRapid = 0;
uint32_t g_tRapidWin = 0;

void handleTouch() {
  bool raw = (digitalRead(TOUCH_PIN) == HIGH);
  uint32_t now = millis();

  if (raw && !g_tLastRaw) {  // press start
    g_tStart = now;
    g_tLongDone = false;
    g_tActive = true;
  }

  if (raw && g_tActive && !g_tLongDone &&  // long press
      now - g_tStart >= LONG_PRESS_MS) {
    g_tLongDone = true;
    g_tCount = g_tRapid = 0;
    emotion_set(EMO_CUDDLE);
  }

  if (!raw && g_tLastRaw && g_tActive) {  // release
    g_tActive = false;
    uint32_t dur = now - g_tStart;
    if (!g_tLongDone && dur > 50 && dur < LONG_PRESS_MS) {
      g_tLastTap = now;
      if (now - g_tRapidWin > RAPID_WIN) {
        g_tRapidWin = now;
        g_tRapid = 0;
      }
      g_tRapid++;
      g_tCount++;
      if (g_tRapid >= RAPID_THRESH) {
        emotion_set(EMO_CONFUSED);
        g_tCount = g_tRapid = 0;
      }
    }
  }

  if (!raw && g_tCount > 0 && now - g_tLastTap > MULTI_TAP_WIN) {
    if (g_tCount == 1) emotion_set(EMO_CURIOUS);
    else if (g_tCount == 2) emotion_set((Emotion)g_nextEmotion);
    else emotion_set(EMO_ANGRY);
    g_tCount = 0;
  }

  if (raw) g_lastActivityTime = now;
  g_tLastRaw = raw;
}
void handleSleep() {

  // sleep only if forced OR timeout reached
  if (!g_sleepPending &&
      millis() - g_lastActivityTime < SLEEP_TIMEOUT_MS) {
    return;
  }

  Serial.println(F("[SLEEP] Preparing sleep"));

  // stop haptics
  haptics_stop();

  // blank OLED
  rawDisplay.clearDisplay();
  rawDisplay.display();

  // IMPORTANT:
  // turn OFF WiFi before sleep
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  delay(100);

  // wake on touch pin HIGH
  gpio_wakeup_enable((gpio_num_t)TOUCH_PIN, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  Serial.println(F("[SLEEP] Entering light sleep"));
  delay(50);

  esp_light_sleep_start();

  // =========================
  // resumes here after wake
  // =========================

  Serial.println(F("[WAKE] Woke up"));

  // restart WiFi AP
  WiFi.mode(WIFI_AP);

  bool ok = WiFi.softAP("KawaiiRobo", "12345678");

  if (ok) {
    Serial.print(F("[WiFi] AP Restarted → http://"));
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println(F("[WiFi] Restart Failed"));
  }

  // restore timers
  g_lastActivityTime = millis();
  g_sleepPending = false;

  // restore idle face
  emotion_set(EMO_IDLE);
}

// =============================================================================
//  SLEEP HANDLER
// =============================================================================



// =============================================================================
//  WEB SERVER — HTML/CSS/JS (self-contained, no SD card)
// =============================================================================

// The entire UI is one long const string served from flash.
// Using raw string literal R"rawhtml(...)rawhtml" for readability.

static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KawaiiRobo</title>
<style>
  :root{--bg:#0f0f13;--card:#1a1a24;--border:#2a2a3a;--accent:#7c6ff7;
        --accent2:#f77cc2;--text:#e8e8f0;--sub:#888;--green:#4caf69;--red:#f76c6c}
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font-family:'Segoe UI',sans-serif;
       min-height:100vh;padding:16px}
  h1{text-align:center;font-size:1.5rem;margin-bottom:4px;
     background:linear-gradient(135deg,var(--accent),var(--accent2));
     -webkit-background-clip:text;-webkit-text-fill-color:transparent}
  .subtitle{text-align:center;color:var(--sub);font-size:.8rem;margin-bottom:20px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:14px}
  .card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px}
  .card h2{font-size:.9rem;color:var(--accent);margin-bottom:12px;
           text-transform:uppercase;letter-spacing:.08em}
  /* Status bar */
  #statusBar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:14px}
  .pill{background:var(--card);border:1px solid var(--border);border-radius:20px;
        padding:5px 14px;font-size:.78rem;display:flex;align-items:center;gap:6px}
  .dot{width:8px;height:8px;border-radius:50%;background:var(--green);
       animation:pulse 2s infinite}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
  /* Scene grid */
  .scene-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:6px}
  .scene-btn{background:#12121c;border:1px solid var(--border);border-radius:8px;
             padding:8px 6px;font-size:.72rem;color:var(--text);cursor:pointer;
             text-align:center;transition:all .15s;line-height:1.3}
  .scene-btn:hover{border-color:var(--accent);background:#1e1e2e;color:var(--accent)}
  .scene-btn.active{border-color:var(--accent);background:var(--accent);color:#fff}
  /* Emotion buttons */
  .emo-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:6px}
  .emo-btn{padding:9px 6px;border-radius:8px;font-size:.78rem;cursor:pointer;
           text-align:center;border:1px solid var(--border);background:#12121c;
           color:var(--text);transition:all .15s}
  .emo-btn:hover{border-color:var(--accent2);color:var(--accent2)}
  .emo-btn.active{background:var(--accent2);border-color:var(--accent2);color:#fff}
  /* Gesture simulator */
  .gesture-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
  .gest-btn{padding:11px 6px;border-radius:8px;font-size:.8rem;cursor:pointer;
            border:1px solid var(--border);background:#12121c;color:var(--text);
            transition:all .15s;text-align:center}
  .gest-btn:hover{border-color:var(--green);color:var(--green)}
  .gest-btn:active{transform:scale(.95)}
  /* Text inputs */
  .input-row{display:flex;gap:8px;margin-top:8px}
  input[type=text]{flex:1;background:#12121c;border:1px solid var(--border);
                   border-radius:8px;padding:9px 12px;color:var(--text);font-size:.85rem}
  input[type=text]:focus{outline:none;border-color:var(--accent)}
  .send-btn{padding:9px 16px;border-radius:8px;background:var(--accent);
            border:none;color:#fff;cursor:pointer;font-size:.85rem;white-space:nowrap}
  .send-btn:hover{opacity:.85}
  /* FPS slider */
  .slider-row{display:flex;align-items:center;gap:12px;margin-top:8px}
  input[type=range]{flex:1;accent-color:var(--accent)}
  .val-badge{background:var(--accent);color:#fff;border-radius:6px;
             padding:2px 9px;font-size:.8rem;min-width:36px;text-align:center}
  /* Misc buttons */
  .danger-btn{width:100%;padding:10px;border-radius:8px;border:1px solid var(--red);
              background:transparent;color:var(--red);cursor:pointer;font-size:.85rem;
              margin-top:8px;transition:all .15s}
  .danger-btn:hover{background:var(--red);color:#fff}
  .toggle-row{display:flex;align-items:center;justify-content:space-between;
              margin-top:10px;font-size:.85rem}
  /* Toggle switch */
  .toggle{position:relative;display:inline-block;width:44px;height:24px}
  .toggle input{opacity:0;width:0;height:0}
  .slider-t{position:absolute;inset:0;background:#2a2a3a;border-radius:24px;
            cursor:pointer;transition:.3s}
  .slider-t:before{content:"";position:absolute;width:18px;height:18px;
                   left:3px;top:3px;background:#fff;border-radius:50%;transition:.3s}
  input:checked+.slider-t{background:var(--accent)}
  input:checked+.slider-t:before{transform:translateX(20px)}
  /* Toast */
  #toast{position:fixed;bottom:24px;right:24px;background:var(--accent);color:#fff;
         padding:10px 18px;border-radius:10px;font-size:.85rem;opacity:0;
         transition:opacity .3s;pointer-events:none;z-index:99}
  #toast.show{opacity:1}
</style>
</head>
<body>
<h1>KawaiiRobo</h1>
<p class="subtitle">Real-time OLED face controller</p>

<!-- Status bar -->
<div id="statusBar">
  <div class="pill"><span class="dot"></span><span id="st-scene">–</span></div>
  <div class="pill">🎭 <span id="st-emo">–</span></div>
  <div class="pill">⏱ <span id="st-up">0s</span></div>
  <div class="pill">📶 <span id="st-ip">–</span></div>
</div>

<div class="grid">

  <!-- Scene picker -->
  <div class="card" style="grid-column:1/-1">
    <h2>Scenes</h2>
    <div class="scene-grid" id="sceneGrid"></div>
  </div>

  <!-- Emotion state -->
  <div class="card">
    <h2>Emotion State</h2>
    <div class="emo-grid" id="emoGrid"></div>
    <div class="toggle-row">
      <span>Auto-return to IDLE</span>
      <label class="toggle">
        <input type="checkbox" id="autoReturnToggle" checked
               onchange="setAutoReturn(this.checked)">
        <span class="slider-t"></span>
      </label>
    </div>
  </div>

  <!-- Gesture simulator -->
  <div class="card">
    <h2>Gesture Simulator</h2>
    <div class="gesture-grid">
      <button class="gest-btn" onclick="gesture('single')">👆 Single tap<br><small>→ Curious</small></button>
      <button class="gest-btn" onclick="gesture('double')">✌️ Double tap<br><small>→ Cycle emo</small></button>
      <button class="gest-btn" onclick="gesture('triple')">🖖 Triple tap<br><small>→ Angry</small></button>
      <button class="gest-btn" onclick="gesture('long')">🤜 Long press<br><small>→ Cuddle</small></button>
      <button class="gest-btn" onclick="gesture('rapid')">⚡ Rapid ×5<br><small>→ Confused</small></button>
      <button class="gest-btn" onclick="gesture('idle')">😴 Force idle<br><small>→ IDLE</small></button>
    </div>
  </div>

  <!-- Name display -->
  <div class="card">
    <h2>Name Greeting</h2>
    <p style="font-size:.8rem;color:var(--sub);margin-bottom:4px">
      Non-blocking — animates via tick()
    </p>
    <div class="input-row">
      <input type="text" id="nameInput" placeholder="Enter name…" maxlength="20">
      <button class="send-btn" onclick="sendName()">Show</button>
    </div>
    <div class="input-row" style="margin-top:6px">
      <button class="send-btn" style="width:100%;background:#2a2a3a;border:1px solid var(--border)"
              onclick="sendBlockingName()">Blocking showName()</button>
    </div>
  </div>

  <!-- Scroll message -->
  <div class="card">
    <h2>Scroll Message</h2>
    <p style="font-size:.8rem;color:var(--sub);margin-bottom:4px">
      Non-blocking — scrolls continuously via tick()
    </p>
    <div class="input-row">
      <input type="text" id="msgInput" placeholder="Enter message…" maxlength="60">
      <button class="send-btn" onclick="sendMsg()">Scroll</button>
    </div>
    <div class="input-row" style="margin-top:6px">
      <button class="send-btn" style="width:100%;background:#2a2a3a;border:1px solid var(--border)"
              onclick="sendBlockingMsg()">Blocking showMessage()</button>
    </div>
  </div>

  <!-- FPS & settings -->
  <div class="card">
    <h2>Settings</h2>
    <label style="font-size:.85rem">FPS</label>
    <div class="slider-row">
      <input type="range" id="fpsSlider" min="5" max="60" value="30"
             oninput="document.getElementById('fpsVal').textContent=this.value"
             onchange="setFps(this.value)">
      <span class="val-badge" id="fpsVal">30</span>
    </div>
    <button class="danger-btn" onclick="forceSleep()">😴 Force Sleep</button>
    <button class="danger-btn" style="margin-top:6px;border-color:var(--accent);color:var(--accent)"
            onclick="restartDemo()">🔄 Restart to IDLE</button>
  </div>

</div>

<div id="toast"></div>

<script>
// ── Scene names (must match SCENE_COUNT order in firmware) ───
const SCENES=[
  "Cute Happy","Emo Stare","UwU","Crying","Angry Glitch",
  "Heart Eyes","Dead Inside","Sleepy","Dizzy","Wink Cute",
  "TwT","Surprise","Star Eyes","Dot Bounce","Glitch Scan",
  "Square Eyes","Pixel Happy","Robot Scan","Loading","Shy Blush",
  "Skeptical","Wave","Thumbs Up","Handshake","Bullet Dodge",
  "Smug","Nervous","Excited","Cool","Love Struck",
  "Thinking","Woozy","Scream",
  "♥ Heart","☮ Peace","★ Star","♪ Music","Zzz","⚡ Bolt",
  "Hi!","Hello","Bye","OK","LOL"
];

const EMOTIONS=["Idle","Curious","Very Curious","Angry","Confused",
                 "Cuddle","Sad","Happy","Sleepy","Love"];

let curScene=0, curEmo=0;

// Build scene grid
const sg=document.getElementById('sceneGrid');
SCENES.forEach((name,i)=>{
  const b=document.createElement('button');
  b.className='scene-btn';b.id='sb'+i;
  b.textContent=i+'. '+name;
  b.onclick=()=>setScene(i);
  sg.appendChild(b);
});

// Build emotion grid
const eg=document.getElementById('emoGrid');
EMOTIONS.forEach((name,i)=>{
  const b=document.createElement('button');
  b.className='emo-btn';b.id='eb'+i;
  b.textContent=name;
  b.onclick=()=>setEmotion(i);
  eg.appendChild(b);
});

function api(url,cb){
  fetch(url).then(r=>r.json()).then(d=>{if(cb)cb(d);}).catch(()=>{});
}

function toast(msg,clr='var(--accent)'){
  const t=document.getElementById('toast');
  t.style.background=clr;t.textContent=msg;
  t.classList.add('show');
  setTimeout(()=>t.classList.remove('show'),2200);
}

function setScene(id){
  api('/api/scene?id='+id,d=>{
    if(d.ok) toast('Scene: '+SCENES[id]);
  });
}

function setEmotion(id){
  api('/api/emotion?id='+id,d=>{
    if(d.ok) toast('Emotion: '+EMOTIONS[id],'var(--accent2)');
  });
}

function gesture(type){
  api('/api/gesture?type='+type,d=>{
    if(d.ok) toast('Gesture: '+type,'var(--green)');
  });
}

function sendName(){
  const n=document.getElementById('nameInput').value.trim();
  if(!n){toast('Enter a name first','var(--red)');return;}
  api('/api/name?v='+encodeURIComponent(n)+'&blocking=0',d=>{
    if(d.ok) toast('Showing: '+n);
  });
}

function sendBlockingName(){
  const n=document.getElementById('nameInput').value.trim();
  if(!n){toast('Enter a name first','var(--red)');return;}
  toast('Showing (blocking 2.5s)…');
  api('/api/name?v='+encodeURIComponent(n)+'&blocking=1');
}

function sendMsg(){
  const m=document.getElementById('msgInput').value.trim();
  if(!m){toast('Enter a message first','var(--red)');return;}
  api('/api/msg?v='+encodeURIComponent(m)+'&blocking=0',d=>{
    if(d.ok) toast('Scrolling: '+m);
  });
}

function sendBlockingMsg(){
  const m=document.getElementById('msgInput').value.trim();
  if(!m){toast('Enter a message first','var(--red)');return;}
  toast('Scrolling (blocking)…');
  api('/api/msg?v='+encodeURIComponent(m)+'&blocking=1');
}

function setFps(v){
  api('/api/fps?v='+v,d=>{if(d.ok)toast('FPS: '+v);});
}

function forceSleep(){
  api('/api/sleep',d=>{if(d.ok)toast('Sleeping…','var(--sub)');});
}

function restartDemo(){
  api('/api/emotion?id=0',d=>{if(d.ok)toast('Back to IDLE');});
}

function setAutoReturn(v){
  api('/api/autoreturn?v='+(v?1:0));
}

// ── Status polling (every 1.2 s) ─────────────────────────────
function updateStatus(){
  api('/api/status',d=>{
    if(!d) return;
    document.getElementById('st-scene').textContent=
      d.sceneId+': '+(SCENES[d.sceneId]||'?');
    document.getElementById('st-emo').textContent=
      EMOTIONS[d.emotion]||'?';
    document.getElementById('st-up').textContent=
      fmtUptime(d.uptime);
    document.getElementById('st-ip').textContent=d.ip||'?';

    // Highlight active scene button
    if(d.sceneId!==curScene){
      document.getElementById('sb'+curScene)?.classList.remove('active');
      document.getElementById('sb'+d.sceneId)?.classList.add('active');
      curScene=d.sceneId;
    }
    // Highlight active emotion button
    if(d.emotion!==curEmo){
      document.getElementById('eb'+curEmo)?.classList.remove('active');
      document.getElementById('eb'+d.emotion)?.classList.add('active');
      curEmo=d.emotion;
    }
  });
}

function fmtUptime(s){
  if(s<60) return s+'s';
  if(s<3600) return Math.floor(s/60)+'m '+( s%60)+'s';
  return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';
}

updateStatus();
setInterval(updateStatus,1200);
</script>
</body>
</html>
)rawhtml";

// =============================================================================
//  WEB SERVER ROUTE SETUP
// =============================================================================

void setupWebServer() {
  // Serve main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  // GET /api/status  → JSON
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint32_t up = millis() / 1000;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"sceneId\":%u,\"emotion\":%u,\"uptime\":%lu,"
             "\"fps\":%u,\"ip\":\"%s\"}",
             (unsigned)face.currentScene(),
             (unsigned)g_emotion,
             (unsigned long)up,
             (unsigned)g_fps,
             WiFi.softAPIP().toString().c_str());
    req->send(200, "application/json", buf);
  });

  // GET /api/scene?id=N
  server.on("/api/scene", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) {
      req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing id\"}");
      return;
    }
    uint8_t id = (uint8_t)req->getParam("id")->value().toInt();
    g_webSceneId = id;
    g_webSceneReq = true;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/emotion?id=N
  server.on("/api/emotion", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) {
      req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing id\"}");
      return;
    }
    uint8_t id = (uint8_t)req->getParam("id")->value().toInt();
    if (id >= EMO_COUNT) id = 0;
    g_webEmoId = id;
    g_webEmoReq = true;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/gesture?type=single|double|triple|long|rapid|idle
  server.on("/api/gesture", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("type")) {
      req->send(400, "application/json", "{\"ok\":false}");
      return;
    }
    String t = req->getParam("type")->value();
    // Simulate gestures directly — safe because emotion_set is simple assignment
    if (t == "single") {
      g_webEmoId = EMO_CURIOUS;
      g_webEmoReq = true;
    } else if (t == "double") {
      g_webEmoId = g_nextEmotion;
      g_webEmoReq = true;
    } else if (t == "triple") {
      g_webEmoId = EMO_ANGRY;
      g_webEmoReq = true;
    } else if (t == "long") {
      g_webEmoId = EMO_CUDDLE;
      g_webEmoReq = true;
    } else if (t == "rapid") {
      g_webEmoId = EMO_CONFUSED;
      g_webEmoReq = true;
    } else if (t == "idle") {
      g_webEmoId = EMO_IDLE;
      g_webEmoReq = true;
    }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/name?v=<name>&blocking=0|1
  server.on("/api/name", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("v")) {
      req->send(400, "application/json", "{\"ok\":false}");
      return;
    }
    String val = req->getParam("v")->value();
    val.toCharArray(g_webName, sizeof(g_webName));
    bool blocking = req->hasParam("blocking") && req->getParam("blocking")->value() == "1";
    if (blocking) {
      // Blocking path — runs on the async handler thread, OK for one-shot
      face.showName(g_webName);
      face.requestScene(SCENE_CUTE_HAPPY);
    } else {
      g_webNameReq = true;
    }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/msg?v=<message>&blocking=0|1
  server.on("/api/msg", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("v")) {
      req->send(400, "application/json", "{\"ok\":false}");
      return;
    }
    String val = req->getParam("v")->value();
    val.toCharArray(g_webMsg, sizeof(g_webMsg));
    bool blocking = req->hasParam("blocking") && req->getParam("blocking")->value() == "1";
    if (blocking) {
      face.showMessage(g_webMsg);
      face.requestScene(SCENE_CUTE_HAPPY);
    } else {
      g_webMsgReq = true;
    }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/fps?v=N
  server.on("/api/fps", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("v")) {
      uint8_t f = (uint8_t)req->getParam("v")->value().toInt();
      if (f >= 5 && f <= 60) {
        g_fps = f;
        face.setFPS(f);
      }
    }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/sleep
  server.on("/api/sleep", HTTP_GET, [](AsyncWebServerRequest* req) {
    g_sleepPending = true;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/autoreturn?v=0|1
  server.on("/api/autoreturn", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("v"))
      g_autoReturn = req->getParam("v")->value() == "1";
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println(F("[WEB] Server started"));
}

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== KawaiiRobo Web Build ==="));

  // I2C with custom pins (ESP32-C3)
  Wire.begin(SDA_PIN, SCL_PIN);

  // Init raw display object (used only for sleep blanking)
  rawDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  // Init OLEDFace — pass false because Wire.begin() already called above
  if (!face.begin(OLED_ADDR, false)) {
    Serial.println(F("[OLED] Not found — check wiring"));
  } else {
    face.setFPS(30);
    face.showSplash();
    Serial.println(F("[OLED] Ready"));
  }

  haptics_init();

  // Touch pin — INPUT_PULLDOWN so floating pin can't trigger phantom taps
  // If your touch module drives LOW on touch, change to INPUT_PULLUP and
  // flip the digitalRead() comparison in handleTouch() to == LOW
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // WiFi AP Mode
  Serial.println(F("[WiFi] Starting AP..."));

  WiFi.mode(WIFI_AP);

  bool ok = WiFi.softAP("KawaiiRobo", "12345678");

  uint8_t tries = 0;

  while (!ok && tries < 10) {
    delay(250);
    Serial.print('.');
    tries++;

    // Keep original loading animation logic
    face.requestScene(SCENE_LOADING);
    face.tick();

    ok = WiFi.softAP("KawaiiRobo", "12345678");
  }

  Serial.println();

  if (ok) {
    Serial.print(F("[WiFi] AP Started → http://"));
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println(F("[WiFi] AP Failed"));
  }

  setupWebServer();

  // State init
  g_lastActivityTime = millis();
  g_emotionTimer = millis();
  emotion_set(EMO_IDLE);

  randomSeed(esp_random());

  Serial.println(F("[READY]"));
  Serial.println(F("  Touch gestures active."));
  Serial.println(F("  Web UI at http://<ip>/"));
}

// =============================================================================
//  LOOP — nothing blocks here
// =============================================================================

uint32_t g_lastFrame = 0;
uint32_t g_lastTouchPoll = 0;

void loop() {
  uint32_t now = millis();

  // ── 1. Touch + haptics at 5 ms (highest priority) ────────────────────────
  if (now - g_lastTouchPoll >= TOUCH_POLL_MS) {
    g_lastTouchPoll = now;
    handleTouch();
    handleHaptics();
  }

  // ── 2. Consume web-triggered requests ────────────────────────────────────
  //    These flags are set by async HTTP handlers and consumed here in loop()
  //    so that all display/scene calls happen on the main Arduino task,
  //    never from the async handler thread.

  if (g_webSceneReq) {
    g_webSceneReq = false;
    uint8_t id = g_webSceneId;
    if (id < SCENE_COUNT || id == SCENE_TEXT_NAME || id == SCENE_TEXT_MSG) {
      g_activeScene = id;
      face.requestScene(id);
      // If web manually picks a scene, cancel emotion auto-return timer
      g_emotionTimer = millis();
      Serial.print(F("[WEB] Scene → "));
      Serial.println(id);
    }
  }

  if (g_webEmoReq) {
    g_webEmoReq = false;
    emotion_set((Emotion)g_webEmoId);
  }

  if (g_webNameReq) {
    g_webNameReq = false;
    face.setTickName(g_webName);
    face.requestScene(SCENE_TEXT_NAME);
    g_emotionTimer = millis();
    Serial.print(F("[WEB] Name → "));
    Serial.println(g_webName);
  }

  if (g_webMsgReq) {
    g_webMsgReq = false;
    face.setTickMessage(g_webMsg);
    face.requestScene(SCENE_TEXT_MSG);
    g_emotionTimer = millis();
    Serial.print(F("[WEB] Msg → "));
    Serial.println(g_webMsg);
  }

  // ── 3. 30 fps frame slice ─────────────────────────────────────────────────
  if (now - g_lastFrame >= FRAME_MS) {
    g_lastFrame = now;
    emotion_tick(now);  // auto-return to IDLE check
    handleSleep();      // deep sleep check (also triggered by g_sleepPending)
  }

  // ── 4. Draw one display frame — the only display call in loop() ───────────
  //    tick() checks its own FPS gate internally; returns in <1ms if not due.
  face.tick();
}

// =============================================================================
//  END OF FILE
//
//  Quick API reference for adding your own integrations:
//
//  face.requestScene(SCENE_CUTE_HAPPY);       // switch any scene instantly
//  face.setTickName("Alice");                 // set name for SCENE_TEXT_NAME
//  face.requestScene(OLEDFace::SCENE_TEXT_NAME);
//  face.setTickMessage("Hello world!");       // set text for SCENE_TEXT_MSG
//  face.requestScene(OLEDFace::SCENE_TEXT_MSG);
//  face.setFPS(20);                           // slow down for lower CPU use
//  face.currentScene();                       // read active scene id
//  face.tick();                               // always in loop(), always safe
// =============================================================================
