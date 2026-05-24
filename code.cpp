// ==============================================================
//  NIMO DESK BUDDY — ESP32-C3 Super Mini Edition
//  SDA=4  SCL=5  TOUCH=3
//  FIXED: async scan, single DNS call, deferred WiFi connect
// ==============================================================
//  FEATURES
//  ─────────────────────────────────────────────────────────────
//  • Captive-portal WiFi setup (phone connects, fills form)
//  • Credentials saved to NVS (Preferences) — survive reboot
//  • Physics-engine eyes with MPU6050 tilt tracking
//  • 10 moods: Normal, Happy, Surprised, Sleepy, Angry, Sad,
//    Excited, Love, Suspicious, Dizzy
//  • Shake → Dizzy → auto-Angry sequence
//  • Clock, World-Clock, Weather, 3-Day Forecast pages
//  • Weather mood auto-sync
//  • Bitmap weather icons + emotion particles
//  • Boot animation
//  • Touch: short=next page, long=cycle mood / sub-page
//  • ADMIN PAGE: hold touch 3s on face page → admin mode
//    - Access http://192.168.x.x/admin when on same network
//    - Restart device
//    - Factory reset (clears all NVS)
//    - Re-open WiFi setup portal
//    - View system info (uptime, IP, free heap, WiFi RSSI)
// ==============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <MPU6050_tockn.h>
#include "time.h"
#include <math.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// ── PINS ──────────────────────────────────────────────────────
#define SDA_PIN    4
#define SCL_PIN    5
#define TOUCH_PIN  3

// ── DISPLAY ───────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ── MPU6050 ───────────────────────────────────────────────────
MPU6050 mpu6050(Wire);

// ── CAPTIVE PORTAL ────────────────────────────────────────────
#define AP_SSID      "NIMO-Setup"
#define PORTAL_PIN   "5674"       // ← change this to your desired PIN
#define DNS_PORT 53

// Portal auth: track which client IPs have authenticated
String authedIPs[8];
int    authedIPCount = 0;

bool isAuthed(String ip) {
  for (int i = 0; i < authedIPCount; i++)
    if (authedIPs[i] == ip) return true;
  return false;
}
void addAuthed(String ip) {
  if (isAuthed(ip)) return;
  if (authedIPCount < 8) authedIPs[authedIPCount++] = ip;
}

WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;

bool wifiConfigured = false;
bool portalActive   = false;
bool wifiConnected  = false;

// FIX: deferred connect flag — avoids blocking inside HTTP handler
bool shouldConnect  = false;

// ── ADMIN ─────────────────────────────────────────────────────
bool adminMode          = false;
bool adminServerRunning = false;
unsigned long adminModeStart = 0;
#define ADMIN_HOLD_MS  3000

// ── CREDENTIALS ───────────────────────────────────────────────
String savedSSID     = "";
String savedPass     = "";
String savedApiKey   = "";
String savedCity     = "Lahore";
String savedCountry  = "PK";
String savedTimezone = "PKT-5";

// ── NTP ───────────────────────────────────────────────────────
bool ntpSynced = false;

// ── WEATHER ───────────────────────────────────────────────────
float  wx_temp     = 0;
float  wx_feels    = 0;
int    wx_humidity = 0;
String wx_main     = "Clear";
String wx_desc     = "Sunny";
unsigned long lastWxUpdate = 0;

struct ForecastDay { String day; int temp; String icon; };
ForecastDay fcast[3];

// ── PAGES ─────────────────────────────────────────────────────
int currentPage = 0;   // 0=face  1=clock  2=weather
int subPage     = 0;

// ── MOODS ─────────────────────────────────────────────────────
#define MOOD_NORMAL     0
#define MOOD_HAPPY      1
#define MOOD_SURPRISED  2
#define MOOD_SLEEPY     3
#define MOOD_ANGRY      4
#define MOOD_SAD        5
#define MOOD_EXCITED    6
#define MOOD_LOVE       7
#define MOOD_SUSPICIOUS 8
#define MOOD_DIZZY      9
int currentMood = MOOD_HAPPY;
int weatherMood = MOOD_HAPPY;

// ── MOTION ────────────────────────────────────────────────────
#define CALIB_SAMPLES 100
float calibX = 0, calibY = 0;
bool  calibrated = false;
float currentRoll = 0, currentPitch = 0;
float shakeIntensity = 0;
bool  isShaking = false;
unsigned long shakeEnd = 0;
bool  isAngry = false;
unsigned long angryEnd = 0;
float lastAX = 0, lastAY = 0, lastAZ = 0;
float pupilCurrX = 0, pupilCurrY = 0;

// ── EYE PHYSICS ───────────────────────────────────────────────
struct Eye {
  float x, y, w, h;
  float targetX, targetY, targetW, targetH;
  float pupilX, pupilY, targetPupilX, targetPupilY;
  float velX, velY, velW, velH, pVelX, pVelY;
  float k=0.15, d=0.65, pk=0.22, pd=0.60;
  bool  blinking;
  unsigned long lastBlink, nextBlinkTime;

  void init(float _x,float _y,float _w,float _h){
    x=targetX=_x; y=targetY=_y; w=targetW=_w; h=targetH=_h;
    pupilX=targetPupilX=0; pupilY=targetPupilY=0;
    velX=velY=velW=velH=pVelX=pVelY=0;
    blinking=false; lastBlink=0;
    nextBlinkTime=millis()+random(1500,4000);
  }
  void update(){
    velX=(velX+(targetX-x)*k)*d;   x+=velX;
    velY=(velY+(targetY-y)*k)*d;   y+=velY;
    velW=(velW+(targetW-w)*k)*d;   w+=velW;
    velH=(velH+(targetH-h)*k)*d;   h+=velH;
    pVelX=(pVelX+(targetPupilX-pupilX)*pk)*pd; pupilX+=pVelX;
    pVelY=(pVelY+(targetPupilY-pupilY)*pk)*pd; pupilY+=pVelY;
  }
};
Eye leftEye, rightEye;
float breathVal = 0;

// ── TOUCH ─────────────────────────────────────────────────────
bool lastPin = false;
unsigned long pressStart = 0;
bool longHandled = false;
int  tapCount = 0;
unsigned long lastTap = 0;
const unsigned long LONG_MS  = 800;
const unsigned long DTAP_MS  = 300;
const unsigned long ADMIN_MS = 3000;

// ── BITMAPS ───────────────────────────────────────────────────
const unsigned char bmp_clear[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x01,0x80,0x00,0x00,0x01,0x80,0x00,
  0x00,0x00,0x00,0x00,0x01,0x03,0xc0,0x80,0x00,0x0f,0xf0,0x00,0x00,0x3f,0xfc,0x00,
  0x00,0x7f,0xfe,0x00,0x00,0xff,0xff,0x00,0x06,0xff,0xff,0x60,0x06,0xff,0xff,0x60,
  0x06,0xff,0xff,0x60,0x00,0xff,0xff,0x00,0x3e,0xff,0xff,0x7c,0x3e,0xff,0xff,0x7c,
  0x3e,0xff,0xff,0x7c,0x00,0xff,0xff,0x00,0x06,0xff,0xff,0x60,0x06,0xff,0xff,0x60,
  0x06,0xff,0xff,0x60,0x00,0xff,0xff,0x00,0x00,0x7f,0xfe,0x00,0x00,0x3f,0xfc,0x00,
  0x01,0x0f,0xf0,0x80,0x00,0x03,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x80,0x00,
  0x00,0x01,0x80,0x00,0x00,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char bmp_clouds[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xe0,0x00,
  0x00,0x0f,0xf8,0x00,0x00,0x1f,0xfc,0x00,0x00,0x3f,0xfe,0x00,0x00,0x3f,0xff,0x00,
  0x00,0x7f,0xff,0x80,0x00,0xff,0xff,0xc0,0x00,0xff,0xff,0xe0,0x01,0xff,0xff,0xf0,
  0x03,0xff,0xff,0xf8,0x07,0xff,0xff,0xfc,0x07,0xff,0xff,0xfc,0x0f,0xff,0xff,0xfe,
  0x0f,0xff,0xff,0xfe,0x1f,0xff,0xff,0xff,0x1f,0xff,0xff,0xff,0x1f,0xff,0xff,0xff,
  0x1f,0xff,0xff,0xff,0x1f,0xff,0xff,0xff,0x1f,0xff,0xff,0xff,0x0f,0xff,0xff,0xfe,
  0x07,0xff,0xff,0xfc,0x03,0xff,0xff,0xf8,0x00,0xff,0xff,0xe0,0x00,0x3f,0xff,0x80,
  0x00,0x0f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char bmp_rain[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x03,0xe0,0x00,0x00,0x0f,0xf8,0x00,0x00,0x1f,0xfc,0x00,
  0x00,0x3f,0xfe,0x00,0x00,0x7f,0xff,0x80,0x00,0xff,0xff,0xc0,0x01,0xff,0xff,0xf0,
  0x03,0xff,0xff,0xf8,0x07,0xff,0xff,0xfc,0x0f,0xff,0xff,0xfe,0x1f,0xff,0xff,0xff,
  0x1f,0xff,0xff,0xff,0x1f,0xff,0xff,0xff,0x1f,0xff,0xff,0xff,0x0f,0xff,0xff,0xfe,
  0x07,0xff,0xff,0xfc,0x03,0xff,0xff,0xf8,0x00,0xff,0xff,0xe0,0x00,0x3f,0xff,0x80,
  0x00,0x0f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x0c,0x00,0x00,0x60,0x0c,0x00,
  0x00,0xe0,0x1c,0x00,0x00,0xc0,0x18,0x00,0x03,0x80,0x70,0x00,0x03,0x80,0x70,0x00,
  0x03,0x00,0x60,0x00,0x02,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char mini_sun[] PROGMEM = {
  0x00,0x00,0x01,0x80,0x00,0x00,0x10,0x08,0x04,0x20,0x03,0xc0,
  0x27,0xe4,0x07,0xe0,0x07,0xe0,0x27,0xe4,0x03,0xc0,0x04,0x20,
  0x10,0x08,0x00,0x00,0x01,0x80,0x00,0x00
};
const unsigned char mini_cloud[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x01,0xc0,0x07,0xe0,0x0f,0xf0,0x1f,0xf8,
  0x1f,0xf8,0x3f,0xfc,0x3f,0xfc,0x7f,0xfe,0x3f,0xfe,0x1f,0xfc,
  0x0f,0xf0,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char mini_rain[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x01,0xc0,0x07,0xe0,0x0f,0xf0,0x1f,0xf8,
  0x1f,0xf8,0x3f,0xfc,0x3f,0xfc,0x7f,0xfe,0x3f,0xfe,0x1f,0xfc,
  0x00,0x00,0x44,0x44,0x22,0x22,0x11,0x11
};
const unsigned char bmp_tiny_drop[] PROGMEM = {
  0x10,0x38,0x7c,0xfe,0xfe,0x7c,0x38,0x00
};
const unsigned char bmp_heart[] PROGMEM = {
  0x00,0x00,0x0c,0x60,0x1e,0xf0,0x3f,0xf8,0x7f,0xfc,0x7f,0xfc,
  0x7f,0xfc,0x3f,0xf8,0x1f,0xf0,0x0f,0xe0,0x07,0xc0,0x03,0x80,
  0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char bmp_zzz[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x00,0x0c,0x00,0x18,
  0x00,0x30,0x00,0x7e,0x00,0x00,0x3c,0x00,0x0c,0x00,0x18,0x00,
  0x30,0x00,0x7c,0x00,0x00,0x00,0x00,0x00
};
const unsigned char bmp_dizzy_stars[] PROGMEM = {
  0x08,0x20,0x14,0x50,0x22,0x88,0x41,0x04,0x82,0x02,0x41,0x04,
  0x22,0x88,0x14,0x50,0x08,0x20,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char bmp_angry_mark[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x08,0x00,0x1c,0x00,0x3e,0x00,0x7f,0x00,
  0x3e,0x00,0x1c,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

// ══════════════════════════════════════════════════════════════
//  PIN LOGIN HTML
// ══════════════════════════════════════════════════════════════
const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>NIMO — Enter PIN</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
       background:#0f0f1a;color:#e8e8f0;min-height:100vh;
       display:flex;align-items:center;justify-content:center;padding:16px}
  .card{background:#1a1a2e;border-radius:20px;padding:36px 28px;
        width:100%;max-width:340px;box-shadow:0 0 40px rgba(99,102,241,.25);text-align:center}
  .logo-face{font-size:52px;line-height:1;margin-bottom:8px}
  h1{font-size:20px;font-weight:700;color:#a5b4fc;letter-spacing:4px;margin-bottom:4px}
  p{font-size:12px;color:#6366f1;margin-bottom:28px}
  .pin-wrap{display:flex;gap:10px;justify-content:center;margin-bottom:24px}
  .pin-dot{width:18px;height:18px;border-radius:50%;background:#2d2d5a;
           transition:background .2s}
  .pin-dot.filled{background:#6366f1}
  .keypad{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:16px}
  .key{padding:16px;background:#111130;border:1.5px solid #2d2d5a;border-radius:14px;
       font-size:20px;font-weight:600;color:#e8e8f0;cursor:pointer;
       transition:background .15s,border-color .15s;user-select:none}
  .key:active,.key.pressed{background:#18184a;border-color:#6366f1}
  .key.del{font-size:16px;color:#6366f1}
  .key.zero{grid-column:2}
  .err{font-size:13px;color:#f87171;min-height:20px;margin-top:4px}
  .shake{animation:shake .35s ease}
  @keyframes shake{0%,100%{transform:translateX(0)}
    20%{transform:translateX(-8px)}40%{transform:translateX(8px)}
    60%{transform:translateX(-6px)}80%{transform:translateX(6px)}}
</style>
</head>
<body>
<div class="card">
  <div class="logo-face">( &#9685; &#8767; &#9685; )</div>
  <h1>N I M O</h1>
  <p>Enter setup PIN to continue</p>
  <div class="pin-wrap" id="dots">
    <div class="pin-dot" id="d0"></div>
    <div class="pin-dot" id="d1"></div>
    <div class="pin-dot" id="d2"></div>
    <div class="pin-dot" id="d3"></div>
  </div>
  <div class="keypad">
    <div class="key" onclick="press('1')">1</div>
    <div class="key" onclick="press('2')">2</div>
    <div class="key" onclick="press('3')">3</div>
    <div class="key" onclick="press('4')">4</div>
    <div class="key" onclick="press('5')">5</div>
    <div class="key" onclick="press('6')">6</div>
    <div class="key" onclick="press('7')">7</div>
    <div class="key" onclick="press('8')">8</div>
    <div class="key" onclick="press('9')">9</div>
    <div class="key del zero" onclick="del()">&#9003;</div>
  </div>
  <div class="err" id="err"></div>
</div>
<script>
var pin='';
function updateDots(){
  for(var i=0;i<4;i++)
    document.getElementById('d'+i).className='pin-dot'+(i<pin.length?' filled':'');
}
function press(d){
  if(pin.length>=4) return;
  pin+=d; updateDots();
  if(pin.length===4) submit();
}
function del(){
  pin=pin.slice(0,-1); updateDots();
  document.getElementById('err').textContent='';
}
function submit(){
  fetch('/login',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'pin='+encodeURIComponent(pin)})
  .then(function(r){return r.text();})
  .then(function(t){
    if(t==='OK'){location.href='/';}
    else{
      document.getElementById('err').textContent='Wrong PIN. Try again.';
      var d=document.getElementById('dots');
      d.classList.remove('shake');
      void d.offsetWidth;
      d.classList.add('shake');
      pin=''; updateDots();
    }
  }).catch(function(){
    document.getElementById('err').textContent='Error. Try again.';
    pin=''; updateDots();
  });
}
</script>
</body>
</html>
)rawliteral";

// ══════════════════════════════════════════════════════════════
//  SETUP PORTAL HTML
// ══════════════════════════════════════════════════════════════
const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>NIMO Setup</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
       background:#0f0f1a;color:#e8e8f0;min-height:100vh;
       display:flex;align-items:center;justify-content:center;padding:16px}
  .card{background:#1a1a2e;border-radius:20px;padding:28px 24px;
        width:100%;max-width:400px;box-shadow:0 0 40px rgba(99,102,241,.25)}
  .logo{text-align:center;margin-bottom:24px}
  .logo-face{font-size:48px;line-height:1}
  .logo h1{font-size:22px;font-weight:700;color:#a5b4fc;margin-top:6px;letter-spacing:4px}
  .logo p{font-size:12px;color:#6366f1;margin-top:2px}
  .section{background:#111130;border-radius:14px;padding:16px;margin-bottom:14px}
  .section-title{font-size:11px;font-weight:600;color:#6366f1;
                 text-transform:uppercase;letter-spacing:.1em;margin-bottom:12px}
  label{display:block;font-size:13px;color:#94a3b8;margin-bottom:4px;margin-top:10px}
  label:first-of-type{margin-top:0}
  input,select{width:100%;padding:11px 14px;background:#0f0f2a;
               border:1.5px solid #2d2d5a;border-radius:10px;
               color:#e8e8f0;font-size:15px;outline:none;transition:border-color .2s}
  input:focus,select:focus{border-color:#6366f1;box-shadow:0 0 0 3px rgba(99,102,241,.2)}
  .pw-wrap{position:relative}
  .pw-toggle{position:absolute;right:12px;top:50%;transform:translateY(-50%);
             background:none;border:none;color:#6366f1;font-size:18px;
             cursor:pointer;padding:4px;line-height:1}
  .hint{font-size:11px;color:#475569;margin-top:4px}
  .net-list{margin-top:8px;display:flex;flex-direction:column;gap:6px;
            max-height:220px;overflow-y:auto}
  .net-item{display:flex;align-items:center;justify-content:space-between;
            padding:10px 12px;background:#0f0f2a;border:1.5px solid #2d2d5a;
            border-radius:10px;cursor:pointer;
            transition:border-color .2s,background .2s;gap:8px}
  .net-item:active,.net-item.selected{border-color:#6366f1;background:#18184a}
  .net-name{font-size:14px;font-weight:500;flex:1;word-break:break-all}
  .net-meta{display:flex;align-items:center;gap:6px;flex-shrink:0}
  .net-lock{font-size:13px}
  .rssi-bar{display:flex;align-items:flex-end;gap:2px;height:14px}
  .rssi-bar span{width:4px;background:#2d2d5a;border-radius:2px;display:block}
  .rssi-bar span.on{background:#6366f1}
  .rssi-bar span:nth-child(1){height:4px}
  .rssi-bar span:nth-child(2){height:7px}
  .rssi-bar span:nth-child(3){height:10px}
  .rssi-bar span:nth-child(4){height:14px}
  .scan-btn{width:100%;padding:9px;background:#1e1e40;border:1.5px solid #2d2d5a;
            border-radius:10px;color:#a5b4fc;font-size:13px;cursor:pointer;
            margin-bottom:10px;transition:border-color .2s}
  .scan-btn:active{border-color:#6366f1}
  .scan-status{font-size:11px;color:#6366f1;text-align:center;
               min-height:16px;margin-bottom:6px}
  .btn{width:100%;padding:14px;background:linear-gradient(135deg,#6366f1,#8b5cf6);
       border:none;border-radius:12px;color:#fff;font-size:16px;font-weight:600;
       cursor:pointer;margin-top:8px;letter-spacing:.5px;transition:opacity .2s}
  .btn:active{opacity:.85}
  .status{text-align:center;margin-top:14px;font-size:13px;
          padding:10px;border-radius:10px;display:none}
  .status.ok{background:#052e16;color:#4ade80;display:block}
  .status.err{background:#2d0a0a;color:#f87171;display:block}
  .footer{text-align:center;font-size:11px;color:#334155;margin-top:18px}
  select option{background:#1a1a2e}
  #pw-section{display:none;margin-top:10px}
</style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="logo-face">( &#9685; &#8767; &#9685; )</div>
    <h1>N I M O</h1>
    <p>Desk Buddy Setup</p>
  </div>
  <form id="f" onsubmit="save(event)">
    <div class="section">
      <div class="section-title">&#128246; WiFi Network</div>
      <button type="button" class="scan-btn" onclick="doScan()">&#128269; Scan for Networks</button>
      <div class="scan-status" id="scanStatus">Tap scan to find nearby networks</div>
      <div class="net-list" id="netList"></div>
      <input type="hidden" name="ssid" id="ssid">
      <label style="margin-top:12px">Or type network name manually</label>
      <input type="text" id="ssidManual" placeholder="WiFi name"
             autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false"
             oninput="document.getElementById('ssid').value=this.value;
                      document.getElementById('selectedNet').textContent=this.value||'none'">
      <div id="pw-section">
        <label>Password for: <strong id="selectedNet" style="color:#a5b4fc">none</strong></label>
        <div class="pw-wrap">
          <input type="password" name="pass" id="pass"
                 placeholder="WiFi password" autocomplete="new-password">
          <button type="button" class="pw-toggle" onclick="togglePw('pass',this)">&#128065;</button>
        </div>
      </div>
    </div>
    <div class="section">
      <div class="section-title">&#127780; Weather (OpenWeatherMap)</div>
      <label>API Key</label>
      <input type="text" name="apikey" id="apikey"
             placeholder="Paste your OWM API key"
             autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false">
      <p class="hint">Free key at openweathermap.org — leave blank to skip</p>
      <label>City</label>
      <input type="text" name="city" id="city" value="Lahore"
             placeholder="Lahore" autocorrect="off" autocapitalize="words" spellcheck="false">
      <label>Country Code</label>
      <input type="text" name="country" id="country" value="PK"
             placeholder="PK" maxlength="4"
             autocorrect="off" autocapitalize="characters" spellcheck="false">
    </div>
    <div class="section">
      <div class="section-title">&#128336; Timezone</div>
      <label>Select your timezone</label>
      <select name="tz" id="tz">
        <option value="PKT-5">Pakistan — PKT (UTC+5)</option>
        <option value="IST-5:30">India — IST (UTC+5:30)</option>
        <option value="UTC0">UTC (UTC+0)</option>
        <option value="GMT0BST,M3.5.0/1,M10.5.0">UK — GMT/BST</option>
        <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe — CET (UTC+1)</option>
        <option value="EST5EDT,M3.2.0,M11.1.0">US East — EST (UTC-5)</option>
        <option value="CST6CDT,M3.2.0,M11.1.0">US Central — CST (UTC-6)</option>
        <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain — MST (UTC-7)</option>
        <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific — PST (UTC-8)</option>
        <option value="JST-9">Japan — JST (UTC+9)</option>
        <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia East — AEST (UTC+10)</option>
        <option value="CST-8">China — CST (UTC+8)</option>
        <option value="GST-4">Gulf — GST (UTC+4)</option>
      </select>
    </div>
    <button class="btn" type="submit">&#128190; Save &amp; Connect</button>
  </form>
  <div class="status" id="status"></div>
  <div class="footer">NIMO Desk Buddy</div>
</div>
<script>
// ── FIX: async scan with polling ──────────────────────────────
var scanPoller = null;
var scanAttempts = 0;

function rssiToBars(r){if(r>=-55)return 4;if(r>=-65)return 3;if(r>=-75)return 2;return 1;}
function barHTML(rssi){
  var b=rssiToBars(rssi),h='<div class="rssi-bar">';
  for(var i=1;i<=4;i++)h+='<span class="'+(i<=b?'on':'')+'"></span>';
  return h+'</div>';
}
function escHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')
          .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
function renderNets(nets){
  var st=document.getElementById('scanStatus');
  var nl=document.getElementById('netList');
  if(!nets||nets.length===0){
    st.textContent='No networks found. Try again.';
    return;
  }
  nets.sort(function(a,b){return b.rssi-a.rssi;});
  st.textContent='Found '+nets.length+' network'+(nets.length>1?'s':'')+'. Tap to select.';
  nl.innerHTML='';
  nets.forEach(function(n){
    var div=document.createElement('div');
    div.className='net-item';
    div.innerHTML='<span class="net-name">'+escHtml(n.ssid)+'</span>'
      +'<span class="net-meta">'
      +(n.open?'<span class="net-lock">&#128275;</span>':'<span class="net-lock">&#128274;</span>')
      +barHTML(n.rssi)+'</span>';
    div.addEventListener('click',function(){
      selectNetwork(n.ssid,n.open);
      document.querySelectorAll('.net-item').forEach(function(el){el.classList.remove('selected');});
      div.classList.add('selected');
    });
    nl.appendChild(div);
  });
}

function pollScan(){
  fetch('/scan')
    .then(function(r){return r.json();})
    .then(function(nets){
      if(!nets||nets.length===0){
        // Still scanning — poll again
        scanAttempts++;
        if(scanAttempts<10){
          scanPoller=setTimeout(pollScan,2000);
          document.getElementById('scanStatus').textContent='Scanning... ('+scanAttempts+')';
        } else {
          document.getElementById('scanStatus').textContent='No networks found. Tap scan to retry.';
        }
        return;
      }
      renderNets(nets);
    })
    .catch(function(){
      document.getElementById('scanStatus').textContent='Scan failed. Tap to retry.';
    });
}

function doScan(){
  if(scanPoller){clearTimeout(scanPoller);scanPoller=null;}
  scanAttempts=0;
  var st=document.getElementById('scanStatus');
  st.textContent='Starting scan...';
  document.getElementById('netList').innerHTML='';
  // Kick off scan on device, then start polling
  fetch('/scan_start')
    .then(function(){ scanPoller=setTimeout(pollScan,2500); })
    .catch(function(){ st.textContent='Could not reach device.'; });
}

function selectNetwork(ssid,isOpen){
  document.getElementById('ssid').value=ssid;
  document.getElementById('ssidManual').value=ssid;
  document.getElementById('selectedNet').textContent=ssid;
  document.getElementById('pw-section').style.display='block';
  var p=document.getElementById('pass');
  if(isOpen){p.value='';p.placeholder='No password (open network)';}
  else{p.placeholder='Enter WiFi password';p.focus();}
}

function togglePw(id,btn){
  var i=document.getElementById(id);
  if(i.type==='password'){i.type='text';btn.textContent='&#128584;';}
  else{i.type='password';btn.textContent='&#128065;';}
}

function save(e){
  e.preventDefault();
  var ssid=document.getElementById('ssid').value.trim();
  if(!ssid){alert('Please select or enter a WiFi network first.');return;}
  var btn=document.querySelector('.btn');
  btn.textContent='Saving...';btn.disabled=true;
  var d=new URLSearchParams(new FormData(document.getElementById('f')));
  fetch('/save',{method:'POST',body:d,headers:{'Content-Type':'application/x-www-form-urlencoded'}})
  .then(function(r){return r.text();})
  .then(function(t){
    var s=document.getElementById('status');
    if(t.indexOf('OK')>=0){
      s.className='status ok';
      s.textContent='Saved! NIMO is connecting. You can close this page.';
    } else {
      s.className='status err';s.textContent='Error saving. Please try again.';
      btn.disabled=false;btn.textContent='Save & Connect';
    }
  }).catch(function(){
    document.getElementById('status').className='status err';
    document.getElementById('status').textContent='Network error. Try again.';
    btn.disabled=false;btn.textContent='Save & Connect';
  });
}

// Do NOT auto-scan on load — avoids blocking server at startup
// User taps Scan button manually
</script>
</body>
</html>
)rawliteral";

// ══════════════════════════════════════════════════════════════
//  ADMIN PAGE HTML
// ══════════════════════════════════════════════════════════════
const char ADMIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>NIMO Admin</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
       background:#0f0f1a;color:#e8e8f0;min-height:100vh;padding:16px}
  h1{font-size:20px;font-weight:700;color:#f87171;letter-spacing:3px;
     text-align:center;margin:20px 0 4px}
  .subtitle{text-align:center;font-size:12px;color:#6366f1;margin-bottom:24px}
  .card{background:#1a1a2e;border-radius:16px;padding:20px;
        max-width:420px;margin:0 auto 16px;
        box-shadow:0 0 30px rgba(248,113,113,.1)}
  .info-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .info-box{background:#111130;border-radius:12px;padding:14px;text-align:center}
  .info-val{font-size:20px;font-weight:700;color:#a5b4fc;margin-bottom:4px}
  .info-lbl{font-size:10px;color:#6366f1;text-transform:uppercase;letter-spacing:.08em}
  .section-title{font-size:11px;font-weight:600;color:#f87171;
                 text-transform:uppercase;letter-spacing:.1em;margin-bottom:12px}
  .action-btn{width:100%;padding:14px;border:none;border-radius:12px;
              font-size:15px;font-weight:600;cursor:pointer;margin-bottom:10px;
              transition:opacity .2s,transform .1s;letter-spacing:.3px;
              display:flex;align-items:center;justify-content:center;gap:8px}
  .action-btn:active{opacity:.8;transform:scale(.98)}
  .btn-restart{background:linear-gradient(135deg,#f59e0b,#d97706);color:#000}
  .btn-portal {background:linear-gradient(135deg,#6366f1,#8b5cf6);color:#fff}
  .btn-reset  {background:linear-gradient(135deg,#ef4444,#b91c1c);color:#fff}
  .overlay{position:fixed;inset:0;background:rgba(0,0,0,.7);
           display:none;align-items:center;justify-content:center;z-index:999;padding:20px}
  .overlay.show{display:flex}
  .confirm-box{background:#1a1a2e;border:2px solid #ef4444;border-radius:20px;
               padding:28px 24px;max-width:320px;width:100%;text-align:center}
  .confirm-box h3{color:#f87171;font-size:18px;margin-bottom:10px}
  .confirm-box p{color:#94a3b8;font-size:13px;margin-bottom:20px;line-height:1.6}
  .confirm-row{display:flex;gap:10px}
  .confirm-row button{flex:1;padding:12px;border:none;border-radius:10px;
                      font-size:14px;font-weight:600;cursor:pointer}
  .btn-cancel{background:#1e1e40;color:#94a3b8;border:1.5px solid #2d2d5a}
  .btn-confirm{background:#ef4444;color:#fff}
  .toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);
         background:#1a1a2e;border:1.5px solid #6366f1;border-radius:12px;
         padding:12px 20px;font-size:13px;color:#a5b4fc;
         transition:transform .3s;white-space:nowrap;z-index:1000}
  .toast.show{transform:translateX(-50%) translateY(0)}
  .wifi-row{display:flex;justify-content:space-between;align-items:center;
            padding:8px 0;border-bottom:1px solid #1e1e40;font-size:13px}
  .wifi-row:last-child{border:none}
  .wifi-key{color:#6366f1}
  .wifi-val{color:#e8e8f0;text-align:right;max-width:200px;word-break:break-all}
  .back-link{display:block;text-align:center;margin-top:10px;
             font-size:12px;color:#6366f1}
</style>
</head>
<body>
<h1>&#9881; ADMIN</h1>
<p class="subtitle">NIMO Desk Buddy</p>
<div class="card">
  <div class="section-title">&#128202; System Info</div>
  <div class="info-grid">
    <div class="info-box"><div class="info-val" id="uptime">--</div><div class="info-lbl">Uptime</div></div>
    <div class="info-box"><div class="info-val" id="heap">--</div><div class="info-lbl">Free Heap</div></div>
    <div class="info-box"><div class="info-val" id="rssi">--</div><div class="info-lbl">WiFi RSSI</div></div>
    <div class="info-box"><div class="info-val" id="ip">--</div><div class="info-lbl">IP Address</div></div>
  </div>
</div>
<div class="card">
  <div class="section-title">&#128246; WiFi Details</div>
  <div id="wifiDetails">
    <div class="wifi-row"><span class="wifi-key">SSID</span><span class="wifi-val" id="wi-ssid">--</span></div>
    <div class="wifi-row"><span class="wifi-key">IP</span><span class="wifi-val" id="wi-ip">--</span></div>
    <div class="wifi-row"><span class="wifi-key">Gateway</span><span class="wifi-val" id="wi-gw">--</span></div>
    <div class="wifi-row"><span class="wifi-key">MAC</span><span class="wifi-val" id="wi-mac">--</span></div>
    <div class="wifi-row"><span class="wifi-key">City</span><span class="wifi-val" id="wi-city">--</span></div>
    <div class="wifi-row"><span class="wifi-key">Timezone</span><span class="wifi-val" id="wi-tz">--</span></div>
  </div>
</div>
<div class="card">
  <div class="section-title">&#9881; Actions</div>
  <button class="action-btn btn-restart" onclick="doAction('restart','Restart Device',
    'NIMO will restart. The page will reload in 8 seconds.')">&#128260; Restart Device</button>
  <button class="action-btn btn-portal" onclick="doAction('portal','Open WiFi Setup',
    'NIMO will restart into WiFi setup mode. Connect to the NIMO-Setup network.')">&#128246; Re-open WiFi Setup</button>
  <button class="action-btn btn-reset" onclick="doAction('reset','Factory Reset',
    'ALL saved data (WiFi, API key, city) will be erased. This cannot be undone!')">&#128465; Factory Reset</button>
</div>
<a class="back-link" href="/">&#8592; Back to Setup</a>
<div class="overlay" id="overlay">
  <div class="confirm-box">
    <h3 id="confirmTitle">Confirm</h3>
    <p id="confirmMsg">Are you sure?</p>
    <div class="confirm-row">
      <button class="btn-cancel" onclick="closeConfirm()">Cancel</button>
      <button class="btn-confirm" id="confirmBtn" onclick="confirmed()">Confirm</button>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
var pendingAction='';
function doAction(action,title,msg){
  pendingAction=action;
  document.getElementById('confirmTitle').textContent=title;
  document.getElementById('confirmMsg').textContent=msg;
  document.getElementById('overlay').classList.add('show');
}
function closeConfirm(){document.getElementById('overlay').classList.remove('show');pendingAction='';}
function confirmed(){
  closeConfirm();showToast('Sending command...');
  fetch('/admin/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'action='+pendingAction})
  .then(function(r){return r.text();})
  .then(function(){
    if(pendingAction==='restart'){showToast('Restarting... reconnect in 8s');setTimeout(function(){location.reload();},8000);}
    else if(pendingAction==='reset'){showToast('Reset done! Connect to NIMO-Setup');setTimeout(function(){location.href='http://192.168.4.1/';},5000);}
    else if(pendingAction==='portal'){showToast('Opening setup... connect to NIMO-Setup');}
  })
  .catch(function(){showToast('Command sent (device may restart)');});
}
function showToast(msg){var t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');setTimeout(function(){t.classList.remove('show');},3500);}
function loadInfo(){
  fetch('/admin/info').then(function(r){return r.json();}).then(function(d){
    var s=Math.floor(d.uptime/1000),h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
    var ut=h>0?h+'h '+m+'m':m>0?m+'m '+sec+'s':sec+'s';
    document.getElementById('uptime').textContent=ut;
    document.getElementById('heap').textContent=Math.round(d.heap/1024)+'KB';
    document.getElementById('rssi').textContent=d.rssi+'dBm';
    document.getElementById('ip').textContent=d.ip;
    document.getElementById('wi-ssid').textContent=d.ssid;
    document.getElementById('wi-ip').textContent=d.ip;
    document.getElementById('wi-gw').textContent=d.gateway;
    document.getElementById('wi-mac').textContent=d.mac;
    document.getElementById('wi-city').textContent=d.city+', '+d.country;
    document.getElementById('wi-tz').textContent=d.tz;
  }).catch(function(){});
}
loadInfo();setInterval(loadInfo,5000);
</script>
</body>
</html>
)rawliteral";

// ══════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ══════════════════════════════════════════════════════════════
void connectToWiFi();
void startPortal();
void startAdminServer();
void handleLogin();
void handleRoot();
void handleSave();
void handleScanStart();
void handleScan();
void handleCaptive();
void handleCaptiveText();
void handleNotFound();
void handleAdmin();
void handleAdminInfo();
void handleAdminAction();

// ══════════════════════════════════════════════════════════════
//  PORTAL HANDLERS
// ══════════════════════════════════════════════════════════════

// Gate: redirect to login page if client is not authed
void handleRoot() {
  String clientIP = server.client().remoteIP().toString();
  if (portalActive && !isAuthed(clientIP)) {
    server.sendHeader("Location","http://192.168.4.1/login",true);
    server.send(302,"text/plain","");
    return;
  }
  server.send_P(200, "text/html", PORTAL_HTML);
}

// PIN login handler
void handleLogin() {
  if (server.method() == HTTP_GET) {
    server.send_P(200, "text/html", LOGIN_HTML);
    return;
  }
  // POST — check PIN
  String entered = server.arg("pin");
  String clientIP = server.client().remoteIP().toString();
  if (entered == String(PORTAL_PIN)) {
    addAuthed(clientIP);
    server.send(200,"text/plain","OK");
  } else {
    server.send(200,"text/plain","FAIL");
  }
}

void handleCaptive() {
  String html = "<!DOCTYPE html><html><head>"
    "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/login'>"
    "<title>NIMO</title></head><body>"
    "<script>window.location='http://192.168.4.1/login';<\/script>"
    "</body></html>";
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma","no-cache");
  server.sendHeader("Expires","-1");
  server.send(200,"text/html",html);
}

void handleCaptiveText() {
  server.sendHeader("Cache-Control","no-cache");
  server.send(200,"text/plain","Microsoft Connect Test");
}

void handleNotFound() {
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma","no-cache");
  server.sendHeader("Expires","-1");
  server.sendHeader("Location","http://192.168.4.1/login",true);
  server.send(302,"text/plain","");
}

// FIX: Split scan into two endpoints:
//   /scan_start  → kicks off async scan, returns immediately
//   /scan        → returns results (empty array if still scanning)
void handleScanStart() {
  // Start async scan (non-blocking)
  WiFi.scanNetworks(true /*async*/, false /*show hidden*/);
  server.sendHeader("Cache-Control","no-cache");
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200,"text/plain","OK");
}

void handleScan() {
  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED) {
    // Not ready yet — return empty array; JS will poll again
    server.sendHeader("Cache-Control","no-cache");
    server.sendHeader("Access-Control-Allow-Origin","*");
    server.send(200,"application/json","[]");
    return;
  }

  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    bool isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    String ssid = WiFi.SSID(i);
    ssid.replace("\"","\\\"");
    json += "{\"ssid\":\"" + ssid + "\","
            "\"rssi\":"    + String(WiFi.RSSI(i)) + ","
            "\"open\":"    + (isOpen ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();  // free memory after reading

  server.sendHeader("Cache-Control","no-cache");
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200,"application/json",json);
}

void handleSave() {
  if (server.method() != HTTP_POST) {
    server.send(405,"text/plain","Method Not Allowed"); return;
  }
  // Auth check
  String clientIP = server.client().remoteIP().toString();
  if (portalActive && !isAuthed(clientIP)) {
    server.send(403,"text/plain","Not authorised"); return;
  }
  savedSSID     = server.arg("ssid");
  savedPass     = server.arg("pass");
  savedApiKey   = server.arg("apikey");
  savedCity     = server.arg("city");
  savedCountry  = server.arg("country");
  savedTimezone = server.arg("tz");

  if (savedSSID.length() == 0) {
    server.send(400,"text/plain","ERR: SSID required"); return;
  }

  prefs.begin("nimo",false);
  prefs.putString("ssid",    savedSSID);
  prefs.putString("pass",    savedPass);
  prefs.putString("apikey",  savedApiKey);
  prefs.putString("city",    savedCity);
  prefs.putString("country", savedCountry);
  prefs.putString("tz",      savedTimezone);
  prefs.end();

  // FIX: Respond FIRST, then set flag — never block inside handler
  server.send(200,"text/plain","OK");

  // Signal loop() to connect after response is flushed
  shouldConnect = true;
}

// ══════════════════════════════════════════════════════════════
//  ADMIN HANDLERS
// ══════════════════════════════════════════════════════════════
void handleAdmin() {
  server.send_P(200,"text/html",ADMIN_HTML);
}

void handleAdminInfo() {
  String ip      = WiFi.localIP().toString();
  String gateway = WiFi.gatewayIP().toString();
  String mac     = WiFi.macAddress();
  int    rssi    = WiFi.RSSI();
  uint32_t heap  = ESP.getFreeHeap();
  unsigned long up = millis();

  String json = "{";
  json += "\"uptime\":"    + String(up)   + ",";
  json += "\"heap\":"      + String(heap) + ",";
  json += "\"rssi\":"      + String(rssi) + ",";
  json += "\"ip\":\""      + ip           + "\",";
  json += "\"gateway\":\"" + gateway      + "\",";
  json += "\"mac\":\""     + mac          + "\",";
  json += "\"ssid\":\""    + savedSSID    + "\",";
  json += "\"city\":\""    + savedCity    + "\",";
  json += "\"country\":\"" + savedCountry + "\",";
  json += "\"tz\":\""      + savedTimezone + "\"";
  json += "}";

  server.sendHeader("Cache-Control","no-cache");
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200,"application/json",json);
}

void handleAdminAction() {
  if (server.method() != HTTP_POST) {
    server.send(405,"text/plain","Method Not Allowed"); return;
  }
  String action = server.arg("action");
  server.send(200,"text/plain","OK");   // respond BEFORE acting
  delay(200);

  if (action == "restart") {
    display.clearDisplay();
    display.setFont(NULL);
    display.setCursor(22,20); display.print("Restarting...");
    display.setCursor(10,36); display.print("Admin requested");
    display.display();
    delay(1000);
    ESP.restart();
  }
  else if (action == "reset") {
    display.clearDisplay();
    display.setFont(NULL);
    display.setCursor(18,16); display.print("Factory Reset!");
    display.setCursor(10,30); display.print("Clearing all data");
    display.setCursor(10,44); display.print("Restarting...");
    display.display();
    delay(800);

    prefs.begin("nimo",false);
    prefs.clear();
    prefs.end();

    WiFi.disconnect(true, true);
    delay(500);

    Preferences wifiPrefs;
    wifiPrefs.begin("nvs.net80211",false);
    wifiPrefs.clear();
    wifiPrefs.end();

    delay(300);
    ESP.restart();
  }
  else if (action == "portal") {
    display.clearDisplay();
    display.setFont(NULL);
    display.setCursor(8,20);  display.print("Opening WiFi");
    display.setCursor(8,34);  display.print("Setup Portal...");
    display.display();
    delay(800);

    prefs.begin("nimo",false);
    prefs.putString("ssid","");
    prefs.end();

    WiFi.disconnect(true,true);
    delay(300);
    ESP.restart();
  }
}

// ══════════════════════════════════════════════════════════════
//  WIFI HELPERS
// ══════════════════════════════════════════════════════════════
void startPortal() {
  // Reset auth list each time portal opens
  authedIPCount = 0;
  for (int i = 0; i < 8; i++) authedIPs[i] = "";

  display.clearDisplay();
  display.setFont(NULL);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(8, 0);  display.print("WiFi Setup Mode");
  display.setCursor(0,12);  display.print("Connect to:");
  display.setCursor(0,22);  display.print(AP_SSID);
  display.setCursor(0,34);  display.print("No password needed");
  display.setCursor(0,44);  display.print("Setup PIN:");
  display.setCursor(0,54);  display.print(PORTAL_PIN);
  display.display();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  delay(1000);

  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(apIP);

  dnsServer.setTTL(300);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT,"*",apIP);

  server.on("/",                          HTTP_GET,  handleRoot);
  server.on("/index.html",                HTTP_GET,  handleRoot);
  server.on("/login",                     HTTP_GET,  handleLogin);
  server.on("/login",                     HTTP_POST, handleLogin);
  server.on("/save",                      HTTP_POST, handleSave);
  server.on("/scan_start",                HTTP_GET,  handleScanStart);
  server.on("/scan",                      HTTP_GET,  handleScan);
  server.on("/generate_204",              HTTP_GET,  handleCaptive);
  server.on("/gen_204",                   HTTP_GET,  handleCaptive);
  server.on("/redirect",                  HTTP_GET,  handleCaptive);
  server.on("/hotspot-detect.html",       HTTP_GET,  handleCaptive);
  server.on("/library/test/success.html", HTTP_GET,  handleCaptive);
  server.on("/success.html",              HTTP_GET,  handleCaptive);
  server.on("/ncsi.txt",                  HTTP_GET,  handleCaptiveText);
  server.on("/connecttest.txt",           HTTP_GET,  handleCaptiveText);
  server.on("/success.txt",               HTTP_GET,  handleCaptiveText);
  server.onNotFound(handleNotFound);
  server.begin();

  portalActive = true;
}

void startAdminServer() {
  if (adminServerRunning) return;
  server.on("/",             HTTP_GET,  handleRoot);
  server.on("/admin",        HTTP_GET,  handleAdmin);
  server.on("/admin/info",   HTTP_GET,  handleAdminInfo);
  server.on("/admin/action", HTTP_POST, handleAdminAction);
  server.on("/scan_start",   HTTP_GET,  handleScanStart);
  server.on("/scan",         HTTP_GET,  handleScan);
  server.onNotFound([](){ server.sendHeader("Location","/admin",true); server.send(302); });
  server.begin();
  adminServerRunning = true;
  Serial.print("Admin panel: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/admin");
}

void connectToWiFi() {
  if (savedSSID.length() == 0) { startPortal(); return; }

  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(10,16); display.print("Connecting to:");
  display.setCursor(0,28);  display.print(savedSSID);
  display.setCursor(10,40); display.print("Please wait...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false,false);
  delay(100);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());

  unsigned long t0 = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) {
    delay(400);
    // Animated dots on screen
    display.fillRect(0, 52, 128, 12, SH110X_BLACK);
    String d = "";
    for (int i=0; i<=dots; i++) d += ".";
    display.setCursor(10, 54); display.print(d);
    display.display();
    dots = (dots + 1) % 5;
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    display.clearDisplay();
    display.setFont(NULL);
    display.setCursor(20,16); display.print("WiFi Connected!");
    display.setCursor(0,30);  display.print(WiFi.localIP().toString());
    display.setCursor(0,44);  display.print("Admin: /admin");
    display.display(); delay(2000);

    configTime(0,0,"pool.ntp.org","time.nist.gov");
    setenv("TZ",savedTimezone.c_str(),1);
    tzset();

    struct tm ti; unsigned long nt=millis();
    while (!getLocalTime(&ti,200) && millis()-nt < 5000) delay(200);
    ntpSynced = getLocalTime(&ti,200);

    startAdminServer();
  } else {
    // ── FIX: WiFi not found → show message then open setup portal ──
    display.clearDisplay();
    display.setFont(NULL);
    display.setCursor(15,8);  display.print("WiFi Not Found!");
    display.setCursor(0,22);  display.print(savedSSID);
    display.setCursor(0,36);  display.print("Opening setup...");
    display.setCursor(0,50);  display.print("Connect: " AP_SSID);
    display.display();
    delay(3000);

    // Clear only SSID so user is forced back to portal
    prefs.begin("nimo",false);
    prefs.putString("ssid","");
    prefs.end();
    savedSSID = "";

    startPortal();
  }
}

// ══════════════════════════════════════════════════════════════
//  MPU6050
// ══════════════════════════════════════════════════════════════
void calibrateMPU() {
  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(18,16); display.print("Calibrating...");
  display.setCursor(8,30);  display.print("Keep device STILL");
  display.display();
  float sx=0,sy=0;
  for (int i=0;i<CALIB_SAMPLES;i++) {
    mpu6050.update();
    sx+=mpu6050.getAngleX();
    sy+=mpu6050.getAngleY();
    delay(8);
  }
  calibX=sx/CALIB_SAMPLES;
  calibY=sy/CALIB_SAMPLES;
  calibrated=true;
  display.clearDisplay();
  display.setCursor(28,30); display.print("Ready!");
  display.display(); delay(900);
}

void updateMotion() {
  if (!calibrated) return;
  mpu6050.update();

  float rawX = mpu6050.getAngleX();
  float rawY = mpu6050.getAngleY();
  float tiltFB =  (rawX - calibX);
  float tiltLR = -(rawY - calibY);
  currentRoll  = tiltLR;
  currentPitch = tiltFB;

  float ax=mpu6050.getAccX(), ay=mpu6050.getAccY(), az=mpu6050.getAccZ();
  float shake=(fabs(ax-lastAX)+fabs(ay-lastAY)+fabs(az-lastAZ))*10.0f;
  shakeIntensity=shake;

  if (shake>20.0f && !isShaking && !isAngry && currentPage==0) {
    isShaking=true; shakeEnd=millis()+1500; currentMood=MOOD_DIZZY;
  }
  if (isShaking && millis()>=shakeEnd) {
    isShaking=false; isAngry=true; angryEnd=millis()+2000; currentMood=MOOD_ANGRY;
  }
  if (isAngry && millis()>=angryEnd) {
    isAngry=false; currentMood=weatherMood;
  }

  if (!isShaking && !isAngry && currentPage==0) {
    float rx=constrain(tiltLR/5.0f,-12.f,12.f);
    float ry=constrain(tiltFB/5.0f,-10.f,10.f);
    pupilCurrX=pupilCurrX*0.6f+rx*0.4f;
    pupilCurrY=pupilCurrY*0.6f+ry*0.4f;
    leftEye.targetPupilX=pupilCurrX; leftEye.targetPupilY=pupilCurrY;
    rightEye.targetPupilX=pupilCurrX; rightEye.targetPupilY=pupilCurrY;
    leftEye.targetX=28+(tiltLR/15.f);  leftEye.targetY=18+(tiltFB/15.f);
    rightEye.targetX=80+(tiltLR/15.f); rightEye.targetY=18+(tiltFB/15.f);
  }
  lastAX=ax; lastAY=ay; lastAZ=az;
}

// ══════════════════════════════════════════════════════════════
//  WEATHER
// ══════════════════════════════════════════════════════════════
const unsigned char* bigIcon(String w){
  if(w=="Clear")  return bmp_clear;
  if(w=="Clouds") return bmp_clouds;
  return bmp_rain;
}
const unsigned char* miniIcon(String w){
  if(w=="Clear") return mini_sun;
  if(w=="Rain"||w=="Drizzle"||w=="Thunderstorm") return mini_rain;
  return mini_cloud;
}

void updateWeatherMood(){
  if(wx_main=="Clear")                          weatherMood=MOOD_HAPPY;
  else if(wx_main=="Rain"||wx_main=="Drizzle")  weatherMood=MOOD_SAD;
  else if(wx_main=="Thunderstorm")              weatherMood=MOOD_SURPRISED;
  else if(wx_temp>35)                           weatherMood=MOOD_ANGRY;
  else if(wx_temp<10)                           weatherMood=MOOD_SLEEPY;
  else                                          weatherMood=MOOD_NORMAL;
  if(!isShaking&&!isAngry) currentMood=weatherMood;
}

void fetchWeather(){
  if(!wifiConnected||savedApiKey.length()==0) return;
  HTTPClient http;
  String url="http://api.openweathermap.org/data/2.5/weather?q="
             +savedCity+","+savedCountry+"&appid="+savedApiKey+"&units=metric";
  http.begin(url);
  if(http.GET()==200){
    JSONVar d=JSON.parse(http.getString());
    if(JSON.typeof(d)!="undefined"){
      wx_temp    =(float)(double)d["main"]["temp"];
      wx_feels   =(float)(double)d["main"]["feels_like"];
      wx_humidity=(int)d["main"]["humidity"];
      wx_main    =(const char*)d["weather"][0]["main"];
      wx_desc    =(const char*)d["weather"][0]["description"];
      if(wx_desc.length()>0) wx_desc[0]=toupper(wx_desc[0]);
      updateWeatherMood();
    }
  }
  http.end();

  url="http://api.openweathermap.org/data/2.5/forecast?q="
      +savedCity+","+savedCountry+"&appid="+savedApiKey+"&units=metric";
  http.begin(url);
  if(http.GET()==200){
    JSONVar fo=JSON.parse(http.getString());
    if(JSON.typeof(fo)!="undefined"){
      struct tm t; getLocalTime(&t);
      const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
      int idx[3]={7,15,23};
      for(int i=0;i<3;i++){
        int j=idx[i];
        fcast[i].temp=(int)(double)fo["list"][j]["main"]["temp"];
        fcast[i].icon=(const char*)fo["list"][j]["weather"][0]["main"];
        fcast[i].day=days[((t.tm_wday+i+1)%7)];
      }
    }
  }
  http.end();
  lastWxUpdate=millis();
}

// ══════════════════════════════════════════════════════════════
//  TOUCH
// ══════════════════════════════════════════════════════════════
void handleTouch(){
  bool cur=digitalRead(TOUCH_PIN);
  unsigned long now=millis();

  if(cur&&!lastPin){
    pressStart=now; longHandled=false;
  }
  else if(cur&&lastPin){
    unsigned long held=now-pressStart;

    // 3-second hold on face page → admin mode
    if(held>=ADMIN_MS && !longHandled && currentPage==0 && !adminMode){
      adminMode=true;
      adminModeStart=now;
      longHandled=true;
      display.clearDisplay();
      display.setFont(NULL);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(20, 0); display.print("ADMIN MODE");
      display.setCursor(0,14); display.print("Open browser:");
      display.setCursor(0,26); display.print("http://");
      display.setCursor(0,38); display.print(WiFi.localIP().toString());
      display.setCursor(0,50); display.print("/admin");
      display.display();
      return;
    }

    // Normal 800ms long press
    if(held>=LONG_MS && !longHandled && held<ADMIN_MS){
      if(currentPage==0&&!isShaking&&!isAngry){
        currentMood=(currentMood+1)%(MOOD_SUSPICIOUS+1);
        weatherMood=currentMood;
      } else if(currentPage==1){ subPage=(subPage==1)?0:1; }
        else if(currentPage==2){ subPage=(subPage==2)?0:2; }
      longHandled=true;
    }
  }
  else if(!cur&&lastPin){
    if(now-pressStart<LONG_MS&&!longHandled){
      if(adminMode){
        adminMode=false;
      } else {
        tapCount++; lastTap=now;
      }
    }
  }
  lastPin=cur;

  // Auto-exit admin mode after 30s
  if(adminMode && now-adminModeStart>30000){
    adminMode=false;
  }

  if(!adminMode && tapCount>0 && now-lastTap>DTAP_MS){
    if(tapCount==1){
      if(subPage!=0) subPage=0;
      else{ currentPage=(currentPage+1)%3; subPage=0; }
    }
    tapCount=0;
  }
}

// ══════════════════════════════════════════════════════════════
//  EYE DRAW HELPERS
// ══════════════════════════════════════════════════════════════
void updateEyePhysics(){
  unsigned long now=millis();
  breathVal=sinf(now/800.0f)*1.5f;
  int bDelay=(isShaking||isAngry)?60:120;
  int bMin=(isShaking||isAngry)?300:2000;
  int bMax=(isShaking||isAngry)?800:6000;
  if(now>leftEye.nextBlinkTime){
    leftEye.blinking=rightEye.blinking=true;
    leftEye.lastBlink=now;
    leftEye.nextBlinkTime=now+random(bMin,bMax);
  }
  if(leftEye.blinking){
    leftEye.targetH=rightEye.targetH=2;
    if(now-leftEye.lastBlink>bDelay){
      leftEye.blinking=rightEye.blinking=false;
      leftEye.targetH=rightEye.targetH=32;
    }
  }
  leftEye.update(); rightEye.update();
}

void drawDizzyEye(Eye& e){
  int cx=(int)e.x+(int)e.w/2, cy=(int)e.y+(int)e.h/2;
  display.fillCircle(cx,cy,(int)e.w/2,SH110X_WHITE);
  float ang=millis()*0.025f; int r=(int)e.w/3;
  display.fillCircle(cx+(int)(cosf(ang)*r),cy+(int)(sinf(ang)*r),4,SH110X_BLACK);
  display.fillCircle(cx-(int)(cosf(ang)*r),cy-(int)(sinf(ang)*r),3,SH110X_BLACK);
}

void drawAngryEye(Eye& e, bool isLeft){
  int ix=(int)e.x,iy=(int)e.y,iw=(int)e.w,ih=(int)e.h;
  display.fillRoundRect(ix,iy,iw,ih,8,SH110X_WHITE);
  int cx=ix+iw/2, cy=iy+ih/2, ps=iw/3;
  display.fillRoundRect(cx-ps/2-2,cy-ps/2,ps,ps,ps/2,SH110X_BLACK);
  if(isLeft){for(int i=0;i<6;i++) display.drawLine(ix-2,iy-2+i,ix+iw+2,iy+6+i,SH110X_BLACK);}
  else      {for(int i=0;i<6;i++) display.drawLine(ix-2,iy+6+i,ix+iw+2,iy-2+i,SH110X_BLACK);}
}

void drawNormalEye(Eye& e, bool isLeft){
  int ix=(int)e.x,iy=(int)e.y,iw=(int)e.w,ih=(int)e.h;
  int r=(iw<20)?3:8;
  display.fillRoundRect(ix,iy,iw,ih,r,SH110X_WHITE);
  int cx=ix+iw/2, cy=iy+ih/2, pw=iw/2, ph=ih/2;
  int px=constrain(cx+(int)e.pupilX-pw/2,ix,ix+iw-pw);
  int py=constrain(cy+(int)e.pupilY-ph/2,iy,iy+ih-ph);
  display.fillRoundRect(px,py,pw,ph,r/2,SH110X_BLACK);
  if(iw>15&&ih>15) display.fillCircle(px+pw-4,py+4,2,SH110X_WHITE);
  if(currentMood==MOOD_HAPPY||currentMood==MOOD_LOVE)
    display.fillRect(ix,iy+ih-10,iw,12,SH110X_BLACK);
  else if(currentMood==MOOD_SLEEPY)
    display.fillRect(ix,iy,iw,ih/2,SH110X_BLACK);
  else if(currentMood==MOOD_SAD){
    if(isLeft){for(int i=0;i<7;i++) display.drawLine(ix,iy+i,ix+iw,iy+4+i,SH110X_BLACK);}
    else      {for(int i=0;i<7;i++) display.drawLine(ix,iy+4+i,ix+iw,iy+i,SH110X_BLACK);}
  }
}

void drawMouth(){
  int mx=64,my=54;
  if(isShaking){
    for(int i=-8;i<=8;i++)
      display.drawPixel(mx+i,my+(int)(sinf(i*0.8f+millis()*0.03f)*4),SH110X_WHITE);
    return;
  }
  if(isAngry){
    display.fillRect(mx-8,my-2,16,5,SH110X_BLACK);
    for(int i=-5;i<=5;i+=3) display.drawLine(mx+i,my-2,mx+i,my+2,SH110X_WHITE);
    return;
  }
  switch(currentMood){
    case MOOD_HAPPY: case MOOD_EXCITED:
      for(int i=-6;i<=6;i++){int y=my-(i*i/16);if(y<my+2)display.drawPixel(mx+i,y,SH110X_WHITE);}
      break;
    case MOOD_SAD:
      for(int i=-6;i<=6;i++) display.drawPixel(mx+i,my+2+(i*i/16),SH110X_WHITE);
      break;
    case MOOD_SURPRISED:
      display.fillCircle(mx,my+1,3,SH110X_BLACK);
      display.drawCircle(mx,my+1,3,SH110X_WHITE);
      break;
    case MOOD_SLEEPY:
      display.drawLine(mx-4,my,mx+4,my,SH110X_WHITE);
      break;
    case MOOD_LOVE:
      display.drawBitmap(mx-6,my-2,bmp_heart,12,12,SH110X_WHITE);
      break;
    default:
      display.drawLine(mx-4,my,mx+4,my,SH110X_WHITE);
      break;
  }
}

// ══════════════════════════════════════════════════════════════
//  PAGE DRAWS
// ══════════════════════════════════════════════════════════════
void drawFacePage(){
  updateEyePhysics();
  if(isShaking){
    drawDizzyEye(leftEye); drawDizzyEye(rightEye);
    unsigned long n=millis(); int off=(n/80)%4;
    display.drawBitmap(6-off,0,bmp_dizzy_stars,16,16,SH110X_WHITE);
    display.drawBitmap(106+off,0,bmp_dizzy_stars,16,16,SH110X_WHITE);
  } else if(isAngry){
    drawAngryEye(leftEye,true); drawAngryEye(rightEye,false);
  } else {
    drawNormalEye(leftEye,true); drawNormalEye(rightEye,false);
  }
  drawMouth();
  if(!isShaking&&!isAngry){
    if(currentMood==MOOD_LOVE)   display.drawBitmap(56,0,bmp_heart,16,16,SH110X_WHITE);
    if(currentMood==MOOD_SLEEPY) display.drawBitmap(110,0,bmp_zzz,16,16,SH110X_WHITE);
  }
  if(adminMode){
    display.setFont(NULL);
    display.setCursor(0,0); display.print("ADM");
  }
}

void drawClockPage(){
  struct tm t;
  if(!getLocalTime(&t)){
    display.setFont(NULL); display.setCursor(30,30); display.print("Syncing..."); return;
  }
  String ap=(t.tm_hour>=12)?"PM":"AM";
  int h12=t.tm_hour%12; if(h12==0) h12=12;
  display.setFont(NULL); display.setCursor(114,0); display.print(ap);
  char ts[6]; sprintf(ts,"%02d:%02d",h12,t.tm_min);
  int16_t x1,y1; uint16_t w,h2;
  display.setFont(&FreeSansBold18pt7b);
  display.getTextBounds(ts,0,0,&x1,&y1,&w,&h2);
  display.setCursor((SCREEN_WIDTH-w)/2,42); display.print(ts);
  char ds[20]; strftime(ds,20,"%a, %b %d",&t);
  display.setFont(&FreeSans9pt7b);
  display.getTextBounds(ds,0,0,&x1,&y1,&w,&h2);
  display.setCursor((SCREEN_WIDTH-w)/2,60); display.print(ds);
}

void drawWorldClockPage(){
  struct tm t; if(!getLocalTime(&t)) return;
  time_t now; time(&now);
  struct tm* lt=&t;
  struct tm* uk=gmtime(&now);
  time_t nyEpoch=now-(4*3600);
  struct tm* ny=gmtime(&nyEpoch);
  display.fillRect(0,0,128,14,SH110X_WHITE);
  display.setFont(NULL); display.setTextColor(SH110X_BLACK);
  display.setCursor(22,4); display.print("WORLD CLOCK");
  display.setTextColor(SH110X_WHITE);
  display.drawLine(42,16,42,63,SH110X_WHITE);
  display.drawLine(85,16,85,63,SH110X_WHITE);
  display.setCursor(12,18); display.print("LHR");
  display.setCursor(52,18); display.print("LDN");
  display.setCursor(94,18); display.print("NYC");
  int16_t x1,y1; uint16_t w,h2; char s[10];
  sprintf(s,"%02d:%02d",lt->tm_hour,lt->tm_min);
  display.getTextBounds(s,0,0,&x1,&y1,&w,&h2);
  display.setCursor(21-(w/2),48); display.print(s);
  sprintf(s,"%02d:%02d",uk->tm_hour,uk->tm_min);
  display.getTextBounds(s,0,0,&x1,&y1,&w,&h2);
  display.setCursor(64-(w/2),48); display.print(s);
  sprintf(s,"%02d:%02d",ny->tm_hour,ny->tm_min);
  display.getTextBounds(s,0,0,&x1,&y1,&w,&h2);
  display.setCursor(106-(w/2),48); display.print(s);
}

void drawWeatherPage(){
  if(!wifiConnected||savedApiKey.length()==0){
    display.setFont(NULL);
    display.setCursor(10,24); display.print("No weather data");
    display.setCursor(5,40);  display.print("Set API key in setup");
    return;
  }
  display.drawBitmap(96,0,bigIcon(wx_main),32,32,SH110X_WHITE);
  display.setFont(&FreeSansBold9pt7b);
  String c=savedCity; c.toUpperCase();
  if(c.length()>9) c=c.substring(0,8)+".";
  display.setCursor(0,14); display.print(c);
  display.setFont(&FreeSansBold18pt7b);
  int ti=(int)wx_temp;
  display.setCursor(0,48); display.print(ti);
  int16_t x1,y1; uint16_t w,h2;
  display.getTextBounds(String(ti).c_str(),0,48,&x1,&y1,&w,&h2);
  display.fillCircle(x1+w+5,26,4,SH110X_WHITE);
  display.setFont(NULL);
  display.drawBitmap(88,32,bmp_tiny_drop,8,8,SH110X_WHITE);
  display.setCursor(100,32); display.print(wx_humidity); display.print("%");
  display.setCursor(88,45);  display.print("~"); display.print((int)wx_feels);
  display.drawLine(0,52,128,52,SH110X_WHITE);
  display.setCursor(0,55);
  String sd=wx_desc; if(sd.length()>15) sd=sd.substring(0,13)+"..";
  display.print(sd);
}

void drawForecastPage(){
  display.fillRect(0,0,128,14,SH110X_WHITE);
  display.setFont(NULL); display.setTextColor(SH110X_BLACK);
  display.setCursor(16,4); display.print("3-DAY FORECAST");
  display.setTextColor(SH110X_WHITE);
  display.drawLine(42,16,42,63,SH110X_WHITE);
  display.drawLine(85,16,85,63,SH110X_WHITE);
  for(int i=0;i<3;i++){
    int xS=i*43, cX=xS+21;
    display.setFont(NULL);
    String d=fcast[i].day; if(d=="") d="---";
    display.setCursor(cX-(d.length()*3),18); display.print(d);
    display.drawBitmap(cX-8,26,miniIcon(fcast[i].icon),16,16,SH110X_WHITE);
    display.setFont(&FreeSansBold9pt7b);
    int16_t x1,y1; uint16_t w,h2;
    display.getTextBounds(String(fcast[i].temp).c_str(),0,0,&x1,&y1,&w,&h2);
    display.setCursor(cX-(w/2)-2,58); display.print(fcast[i].temp);
    display.fillCircle(cX+(w/2)+1,50,2,SH110X_WHITE);
  }
}

void drawAdminPage(){
  display.setFont(NULL);
  display.setTextColor(SH110X_WHITE);
  if((millis()/500)%2==0){
    display.fillRect(0,0,128,12,SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor(28,2); display.print("ADMIN MODE");
    display.setTextColor(SH110X_WHITE);
  } else {
    display.setCursor(28,2); display.print("ADMIN MODE");
  }
  display.setCursor(0,16); display.print("IP:");
  display.setCursor(20,16); display.print(WiFi.localIP().toString());
  display.setCursor(0,28); display.print("Open in browser:");
  display.setCursor(0,40); display.print(WiFi.localIP().toString());
  display.setCursor(0,52); display.print("/admin");
  int remaining=(int)(30000-(millis()-adminModeStart))/1000;
  display.setCursor(100,52);
  display.print(remaining); display.print("s");
}

// ══════════════════════════════════════════════════════════════
//  BOOT ANIMATION
// ══════════════════════════════════════════════════════════════
void playBoot(){
  int cx=64,cy=32;
  for(int r=0;r<80;r+=5){
    display.clearDisplay(); display.fillCircle(cx,cy,r,SH110X_WHITE);
    display.display(); delay(6);
  }
  for(int r=0;r<80;r+=5){
    display.clearDisplay();
    display.fillCircle(cx,cy,80,SH110X_WHITE);
    display.fillCircle(cx,cy,r,SH110X_BLACK);
    display.display(); delay(6);
  }
  display.clearDisplay();
  display.setFont(&FreeSansBold9pt7b);
  int16_t x1,y1; uint16_t w,h2;
  display.getTextBounds("N I M O",0,0,&x1,&y1,&w,&h2);
  display.setCursor((SCREEN_WIDTH-w)/2,36); display.print("N I M O");
  display.setFont(NULL); display.setCursor(22,48); display.print("Desk Buddy v3");
  display.display(); delay(1800);
}

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);
  delay(300);

  Wire.begin(SDA_PIN,SCL_PIN);
  Wire.setClock(400000);
  pinMode(TOUCH_PIN,INPUT_PULLUP);

  display.begin(0x3C,true);
  display.setTextColor(SH110X_WHITE);
  display.clearDisplay(); display.display();

  mpu6050.begin();
  leftEye.init(28,18,32,32);
  rightEye.init(80,18,32,32);

  playBoot();
  calibrateMPU();

  prefs.begin("nimo",true);
  savedSSID     = prefs.getString("ssid",    "");
  savedPass     = prefs.getString("pass",    "");
  savedApiKey   = prefs.getString("apikey",  "");
  savedCity     = prefs.getString("city",    "Lahore");
  savedCountry  = prefs.getString("country", "PK");
  savedTimezone = prefs.getString("tz",      "PKT-5");
  prefs.end();

  if(savedSSID.length()==0){
    startPortal();
  } else {
    connectToWiFi();
    if(wifiConnected){
      fetchWeather();
      lastWxUpdate=millis();
    }
  }
}

// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop(){
  // FIX: Only ONE dnsServer.processNextRequest() per loop iteration
  if(portalActive){
    dnsServer.processNextRequest();
    server.handleClient();
  }

  // Admin / normal WiFi server
  if(wifiConnected && adminServerRunning){
    server.handleClient();
  }

  // FIX: Deferred connect — triggered by handleSave() flag
  if(shouldConnect){
    shouldConnect = false;
    portalActive  = false;
    WiFi.softAPdisconnect(true);
    dnsServer.stop();
    delay(500);   // let the HTTP response fully flush first
    connectToWiFi();
    if(wifiConnected){
      fetchWeather();
      lastWxUpdate=millis();
    }
  }

  handleTouch();
  updateMotion();

  // Weather refresh every 10 min
  if(wifiConnected&&savedApiKey.length()>0&&millis()-lastWxUpdate>600000){
    fetchWeather();
  }

  // WiFi reconnect watchdog
  static unsigned long lastWifiCheck=0;
  if(millis()-lastWifiCheck>30000){
    lastWifiCheck=millis();
    if(savedSSID.length()>0&&WiFi.status()!=WL_CONNECTED&&!portalActive){
      WiFi.reconnect();
    }
  }

  display.clearDisplay();

  if(adminMode){
    drawAdminPage();
  } else if(currentPage==0){
    drawFacePage();
  } else if(currentPage==1){
    if(subPage==1) drawWorldClockPage();
    else           drawClockPage();
  } else if(currentPage==2){
    if(subPage==2) drawForecastPage();
    else           drawWeatherPage();
  }

  // Portal indicator dot
  if(portalActive){
    display.fillCircle(125,3,3,SH110X_WHITE);
    display.fillCircle(125,3,1,SH110X_BLACK);
  }

  display.display();
  delay(20);
}
