// ==============================================================
//  NIMO DESK BUDDY — ESP32-C3 Super Mini Edition
//  SDA=4  SCL=5  TOUCH=3
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
#define AP_SSID "NIMO-Setup"
#define AP_PASS "nimo1234"
#define DNS_PORT 53

WebServer  server(80);
DNSServer  dnsServer;
Preferences prefs;

bool wifiConfigured  = false;
bool portalActive    = false;
bool wifiConnected   = false;

String savedSSID     = "";
String savedPass     = "";
String savedApiKey   = "fd7820ab9d2308ab6d9cafe6faab9e04";
String savedCity     = "Lahore";
String savedCountry  = "PK";
String savedTimezone = "PKT-5";   // POSIX TZ string

// ── NTP ───────────────────────────────────────────────────────
bool ntpSynced = false;

// ── WEATHER ───────────────────────────────────────────────────
float  wx_temp      = 0;
float  wx_feels     = 0;
int    wx_humidity  = 0;
String wx_main      = "Clear";
String wx_desc      = "Sunny";
unsigned long lastWxUpdate = 0;

struct ForecastDay { String day; int temp; String icon; };
ForecastDay fcast[3];

// ── PAGES ─────────────────────────────────────────────────────
int currentPage = 0;   // 0=face 1=clock 2=weather
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
bool  isAngry   = false;
unsigned long angryEnd  = 0;
float lastAX = 0, lastAY = 0, lastAZ = 0;
float pupilTargX = 0, pupilTargY = 0;
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
    velX=(velX+(targetX-x)*k)*d; x+=velX;
    velY=(velY+(targetY-y)*k)*d; y+=velY;
    velW=(velW+(targetW-w)*k)*d; w+=velW;
    velH=(velH+(targetH-h)*k)*d; h+=velH;
    pVelX=(pVelX+(targetPupilX-pupilX)*pk)*pd; pupilX+=pVelX;
    pVelY=(pVelY+(targetPupilY-pupilY)*pk)*pd; pupilY+=pVelY;
  }
};
Eye leftEye, rightEye;
float breathVal = 0;

// ── TOUCH ─────────────────────────────────────────────────────
bool lastPin = false;
unsigned long pressStart  = 0;
bool longHandled = false;
int  tapCount = 0;
unsigned long lastTap = 0;
const unsigned long LONG_MS   = 800;
const unsigned long DTAP_MS   = 300;

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
const unsigned char bmp_tiny_drop[] PROGMEM = { 0x10,0x38,0x7c,0xfe,0xfe,0x7c,0x38,0x00 };
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
//  CAPTIVE PORTAL HTML — mobile-friendly form
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
        width:100%;max-width:400px;
        box-shadow:0 0 40px rgba(99,102,241,.25)}
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
               color:#e8e8f0;font-size:15px;outline:none;
               transition:border-color .2s}
  input:focus,select:focus{border-color:#6366f1;
    box-shadow:0 0 0 3px rgba(99,102,241,.2)}
  .pw-wrap{position:relative}
  .pw-toggle{position:absolute;right:12px;top:50%;transform:translateY(-50%);
             background:none;border:none;color:#6366f1;font-size:18px;
             cursor:pointer;padding:4px;line-height:1}
  .hint{font-size:11px;color:#475569;margin-top:4px}

  /* ── Network list ── */
  .net-list{margin-top:8px;display:flex;flex-direction:column;gap:6px;
            max-height:220px;overflow-y:auto}
  .net-item{display:flex;align-items:center;justify-content:space-between;
            padding:10px 12px;background:#0f0f2a;border:1.5px solid #2d2d5a;
            border-radius:10px;cursor:pointer;transition:border-color .2s,background .2s;
            gap:8px;-webkit-tap-highlight-color:rgba(99,102,241,.2)}
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

  /* password field that appears after selecting a network */
  #pw-section{display:none;margin-top:10px}
</style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="logo-face">( ◕ ‿ ◕ )</div>
    <h1>N I M O</h1>
    <p>Desk Buddy Setup</p>
  </div>

  <form id="f" onsubmit="save(event)">

    <!-- ── WiFi Section ── -->
    <div class="section">
      <div class="section-title">📶 WiFi Network</div>

      <button type="button" class="scan-btn" onclick="doScan()">🔍 Scan for Networks</button>
      <div class="scan-status" id="scanStatus">Tap scan to find nearby networks</div>
      <div class="net-list" id="netList"></div>

      <!-- Hidden real SSID input submitted with form -->
      <input type="hidden" name="ssid" id="ssid">

      <!-- Manual entry fallback -->
      <label style="margin-top:12px">Or type network name manually</label>
      <input type="text" id="ssidManual" placeholder="WiFi name"
             autocomplete="off" autocorrect="off" autocapitalize="none"
             spellcheck="false"
             oninput="document.getElementById('ssid').value=this.value;
                      document.getElementById('selectedNet').textContent=this.value||'none'">

      <div id="pw-section">
        <label>Password for: <strong id="selectedNet" style="color:#a5b4fc">none</strong></label>
        <div class="pw-wrap">
          <input type="password" name="pass" id="pass"
                 placeholder="WiFi password"
                 autocomplete="new-password">
          <button type="button" class="pw-toggle" onclick="togglePw('pass',this)">👁</button>
        </div>
      </div>
    </div>

    <!-- ── Weather Section ── -->
    <div class="section">
      <div class="section-title">🌤 Weather (OpenWeatherMap)</div>
      <label>API Key</label>
      <input type="text" name="apikey" id="apikey"
             placeholder="Paste your OWM API key"
             autocomplete="off" autocorrect="off" autocapitalize="none"
             spellcheck="false">
      <p class="hint">Free key at openweathermap.org — leave blank to skip</p>
      <label>City</label>
      <input type="text" name="city" id="city" value="Lahore"
             placeholder="Lahore" autocorrect="off" autocapitalize="words"
             spellcheck="false">
      <label>Country Code</label>
      <input type="text" name="country" id="country" value="PK"
             placeholder="PK" maxlength="4"
             autocorrect="off" autocapitalize="characters" spellcheck="false">
    </div>

    <!-- ── Timezone Section ── -->
    <div class="section">
      <div class="section-title">🕐 Timezone</div>
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

    <button class="btn" type="submit">💾 Save &amp; Connect</button>
  </form>

  <div class="status" id="status"></div>
  <div class="footer">NIMO Desk Buddy • ESP32-C3 Super Mini</div>
</div>

<script>
// ── Signal strength → bar count (1–4) ──────────────────────
function rssiToBars(r){
  if(r>=-55) return 4;
  if(r>=-65) return 3;
  if(r>=-75) return 2;
  return 1;
}

function barHTML(rssi){
  var b=rssiToBars(rssi);
  var h='<div class="rssi-bar">';
  for(var i=1;i<=4;i++) h+='<span class="'+(i<=b?'on':'')+'"></span>';
  return h+'</div>';
}

// ── Scan for nearby networks ────────────────────────────────
function doScan(){
  var st=document.getElementById('scanStatus');
  var nl=document.getElementById('netList');
  st.textContent='⏳ Scanning…';
  nl.innerHTML='';

  fetch('/scan')
    .then(function(r){ return r.json(); })
    .then(function(nets){
      if(!nets||nets.length===0){
        st.textContent='No networks found. Try again.';
        return;
      }
      // Sort by signal strength
      nets.sort(function(a,b){ return b.rssi-a.rssi; });
      st.textContent='Found '+nets.length+' network'+(nets.length>1?'s':'')+'. Tap to select.';
      nl.innerHTML='';
      nets.forEach(function(n){
        var div=document.createElement('div');
        div.className='net-item';
        div.innerHTML='<span class="net-name">'+escHtml(n.ssid)+'</span>'
          +'<span class="net-meta">'
          +(n.open?'<span class="net-lock">🔓</span>':'<span class="net-lock">🔒</span>')
          +barHTML(n.rssi)
          +'</span>';
        div.addEventListener('click', function(){
          selectNetwork(n.ssid, n.open);
          // Highlight selected
          document.querySelectorAll('.net-item').forEach(function(el){
            el.classList.remove('selected');
          });
          div.classList.add('selected');
        });
        nl.appendChild(div);
      });
    })
    .catch(function(){
      st.textContent='❌ Scan failed. Check connection.';
    });
}

function escHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
          .replace(/"/g,'&quot;');
}

// ── Select a network from the list ─────────────────────────
function selectNetwork(ssid, isOpen){
  document.getElementById('ssid').value = ssid;
  document.getElementById('ssidManual').value = ssid;
  document.getElementById('selectedNet').textContent = ssid;
  document.getElementById('pw-section').style.display = 'block';
  if(isOpen){
    document.getElementById('pass').value='';
    document.getElementById('pass').placeholder='No password (open network)';
  } else {
    document.getElementById('pass').placeholder='Enter WiFi password';
    document.getElementById('pass').focus();
  }
}

// ── Show/hide password ──────────────────────────────────────
function togglePw(id,btn){
  var i=document.getElementById(id);
  if(i.type==='password'){i.type='text';btn.textContent='🙈';}
  else{i.type='password';btn.textContent='👁';}
}

// ── Submit ──────────────────────────────────────────────────
function save(e){
  e.preventDefault();
  var ssid=document.getElementById('ssid').value.trim();
  if(!ssid){ alert('Please select or enter a WiFi network first.'); return; }

  var btn=document.querySelector('.btn');
  btn.textContent='Saving…';btn.disabled=true;

  var d=new URLSearchParams(new FormData(document.getElementById('f')));
  fetch('/save',{method:'POST',body:d,
    headers:{'Content-Type':'application/x-www-form-urlencoded'}})
  .then(function(r){ return r.text(); })
  .then(function(t){
    var s=document.getElementById('status');
    if(t.indexOf('OK')>=0){
      s.className='status ok';
      s.textContent='✅ Saved! NIMO is connecting… you can close this page.';
    } else {
      s.className='status err';
      s.textContent='❌ Error saving. Please try again.';
      btn.disabled=false;btn.textContent='💾 Save & Connect';
    }
  }).catch(function(){
    document.getElementById('status').className='status err';
    document.getElementById('status').textContent='❌ Network error. Try again.';
    btn.disabled=false;btn.textContent='💾 Save & Connect';
  });
}

// Auto-scan when page loads
window.addEventListener('load', function(){ doScan(); });
</script>
</body>
</html>
)rawliteral";

// ══════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ══════════════════════════════════════════════════════════════
void connectToWiFi();
void handleRoot();
void handleSave();
void handleScan();
void handleCaptive();
void handleCaptiveText();
void handleNotFound();

// ══════════════════════════════════════════════════════════════
//  PORTAL HANDLERS
// ══════════════════════════════════════════════════════════════
void handleRoot() {
  server.send_P(200, "text/html", PORTAL_HTML);
}

void handleSave() {
  if (server.method() != HTTP_POST) { server.send(405,"text/plain","Method Not Allowed"); return; }

  savedSSID    = server.arg("ssid");
  savedPass    = server.arg("pass");
  savedApiKey  = server.arg("apikey");
  savedCity    = server.arg("city");
  savedCountry = server.arg("country");
  savedTimezone= server.arg("tz");

  if (savedSSID.length() == 0) { server.send(400,"text/plain","ERR: SSID required"); return; }

  // Persist
  prefs.begin("nimo", false);
  prefs.putString("ssid",    savedSSID);
  prefs.putString("pass",    savedPass);
  prefs.putString("apikey",  savedApiKey);
  prefs.putString("city",    savedCity);
  prefs.putString("country", savedCountry);
  prefs.putString("tz",      savedTimezone);
  prefs.end();

  server.send(200,"text/plain","OK");
  delay(500);

  // Disconnect AP and connect to saved WiFi
  portalActive   = false;
  wifiConfigured = true;
  WiFi.softAPdisconnect(true);
  dnsServer.stop();
  delay(300);
  connectToWiFi();
}

void handleCaptive() {
  String html = F("<!DOCTYPE html><html><head>"
    "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/'>"
    "<title>NIMO Setup</title></head><body>"
    "<p>Redirecting to NIMO setup...</p>"
    "<script>window.location='http://192.168.4.1/';</script>"
    "</body></html>");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html", html);
}

void handleCaptiveText() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/plain", "Microsoft Connect Test");
}

void handleNotFound() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void handleScan() {
  int n = WiFi.scanNetworks(false, false);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    bool isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\","
            "\"rssi\":"    + String(WiFi.RSSI(i)) + ","
            "\"open\":"    + (isOpen ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();

  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ══════════════════════════════════════════════════════════════
//  WIFI HELPERS
// ══════════════════════════════════════════════════════════════
void startPortal() {
  display.clearDisplay();
  display.setFont(NULL);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(8,0);  display.print("WiFi Setup Mode");
  display.setCursor(0,14); display.print("Connect to:");
  display.setCursor(0,24); display.print(AP_SSID);
  display.setCursor(0,34); display.print("No password");
  display.setCursor(0,46); display.print("Then open any");
  display.setCursor(0,56); display.print("website in browser");
  display.display();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  delay(1000);

  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(apIP);

  dnsServer.setTTL(300);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/",                          HTTP_GET,  handleRoot);
  server.on("/index.html",                HTTP_GET,  handleRoot);
  server.on("/save",                      HTTP_POST, handleSave);
  server.on("/scan",                      HTTP_GET,  handleScan);
  server.on("/generate_204",              HTTP_GET,  handleCaptive);
  server.on("/gen_204",                   HTTP_GET,  handleCaptive);
  server.on("/redirect",                  HTTP_GET,  handleCaptive);
  server.on("/hotspot-detect.html",       HTTP_GET,  handleCaptive);
  server.on("/library/test/success.html", HTTP_GET,  handleCaptive);
  server.on("/ncsi.txt",                  HTTP_GET,  handleCaptiveText);
  server.on("/connecttest.txt",           HTTP_GET,  handleCaptiveText);
  server.on("/success.txt",               HTTP_GET,  handleCaptiveText);
  server.onNotFound(handleNotFound);
  server.begin();

  portalActive = true;
}

void connectToWiFi() {
  if (savedSSID.length() == 0) return;

  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(10,24); display.print("Connecting WiFi...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) {
    delay(400);
    display.print(".");
    display.display();
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    display.clearDisplay();
    display.setCursor(20,24); display.print("WiFi Connected!");
    display.display(); delay(1000);

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", savedTimezone.c_str(), 1);
    tzset();

    struct tm ti; unsigned long nt=millis();
    while(!getLocalTime(&ti,200) && millis()-nt<5000) delay(200);
    ntpSynced = getLocalTime(&ti,200);
  } else {
    display.clearDisplay();
    display.setCursor(15,24); display.print("WiFi Failed!");
    display.setCursor(5,40);  display.print("Hold touch to retry");
    display.display(); delay(2500);
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

  float sx=0, sy=0;
  for(int i=0; i<CALIB_SAMPLES; i++){
    mpu6050.update();
    sx += mpu6050.getAngleX();
    sy += mpu6050.getAngleY();
    delay(8);
  }
  calibX = sx / CALIB_SAMPLES;
  calibY = sy / CALIB_SAMPLES;
  calibrated = true;

  display.clearDisplay();
  display.setCursor(28,30); display.print("Ready!");
  display.display(); delay(900);
}

void updateMotion() {
  if(!calibrated) return;
  mpu6050.update();

  float rawX = mpu6050.getAngleX();
  float rawY = mpu6050.getAngleY();

  float tiltFB = rawX - calibX;
  float tiltLR = rawY - calibY;

  tiltLR = -tiltLR;

  currentRoll  = tiltLR;
  currentPitch = tiltFB;

  float ax = mpu6050.getAccX();
  float ay = mpu6050.getAccY();
  float az = mpu6050.getAccZ();
  float shake = (fabs(ax-lastAX)+fabs(ay-lastAY)+fabs(az-lastAZ))*10.0f;
  shakeIntensity = shake;

  if(shake>20.0f && !isShaking && !isAngry && currentPage==0){
    isShaking=true; shakeEnd=millis()+1500;
    currentMood=MOOD_DIZZY;
  }
  if(isShaking && millis()>=shakeEnd){
    isShaking=false; isAngry=true; angryEnd=millis()+2000;
    currentMood=MOOD_ANGRY;
  }
  if(isAngry && millis()>=angryEnd){
    isAngry=false; currentMood=weatherMood;
  }

  if(!isShaking && !isAngry && currentPage==0){
    float rx = constrain(tiltLR / 5.0f, -12.f, 12.f);
    float ry = constrain(tiltFB / 5.0f, -10.f, 10.f);

    pupilCurrX = pupilCurrX * 0.6f + rx * 0.4f;
    pupilCurrY = pupilCurrY * 0.6f + ry * 0.4f;

    leftEye.targetPupilX  = pupilCurrX;
    leftEye.targetPupilY  = pupilCurrY;
    rightEye.targetPupilX = pupilCurrX;
    rightEye.targetPupilY = pupilCurrY;

    leftEye.targetX  = 28 + (tiltLR / 15.f);
    leftEye.targetY  = 18 + (tiltFB / 15.f);
    rightEye.targetX = 80 + (tiltLR / 15.f);
    rightEye.targetY = 18 + (tiltFB / 15.f);
  }

  lastAX=ax; lastAY=ay; lastAZ=az;
}

// ══════════════════════════════════════════════════════════════
//  WEATHER
// ══════════════════════════════════════════════════════════════
const unsigned char* bigIcon(String w){
  if(w=="Clear") return bmp_clear;
  if(w=="Clouds") return bmp_clouds;
  return bmp_rain;
}
const unsigned char* miniIcon(String w){
  if(w=="Clear") return mini_sun;
  if(w=="Rain"||w=="Drizzle"||w=="Thunderstorm") return mini_rain;
  return mini_cloud;
}

void updateWeatherMood(){
  if(wx_main=="Clear") weatherMood=MOOD_HAPPY;
  else if(wx_main=="Rain"||wx_main=="Drizzle") weatherMood=MOOD_SAD;
  else if(wx_main=="Thunderstorm") weatherMood=MOOD_SURPRISED;
  else if(wx_temp>35) weatherMood=MOOD_ANGRY;
  else if(wx_temp<10) weatherMood=MOOD_SLEEPY;
  else weatherMood=MOOD_NORMAL;
  if(!isShaking&&!isAngry) currentMood=weatherMood;
}

void fetchWeather(){
  if(!wifiConnected||savedApiKey.length()==0) return;
  HTTPClient http;
  String url="http://api.openweathermap.org/data/2.5/weather?q="+savedCity+","+savedCountry
             +"&appid="+savedApiKey+"&units=metric";
  http.begin(url); if(http.GET()==200){
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
  } http.end();

  url="http://api.openweathermap.org/data/2.5/forecast?q="+savedCity+","+savedCountry
      +"&appid="+savedApiKey+"&units=metric";
  http.begin(url); if(http.GET()==200){
    JSONVar fo=JSON.parse(http.getString());
    if(JSON.typeof(fo)!="undefined"){
      struct tm t; getLocalTime(&t);
      const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
      int idx[3]={7,15,23};
      for(int i=0;i<3;i++){
        int j=idx[i];
        fcast[i].temp=(int)(double)fo["list"][j]["main"]["temp"];
        fcast[i].icon=(const char*)fo["list"][j]["weather"][0]["main"];
        fcast[i].day =days[((t.tm_wday+i+1)%7)];
      }
    }
  } http.end();
  lastWxUpdate=millis();
}

// ══════════════════════════════════════════════════════════════
//  TOUCH
// ══════════════════════════════════════════════════════════════
void handleTouch(){
  bool cur=digitalRead(TOUCH_PIN);
  unsigned long now=millis();
  if(cur&&!lastPin){ pressStart=now; longHandled=false; }
  else if(cur&&lastPin){
    if(now-pressStart>LONG_MS&&!longHandled){
      if(currentPage==0&&!isShaking&&!isAngry){
        currentMood=(currentMood+1)%(MOOD_SUSPICIOUS+1);
        weatherMood=currentMood;
      } else if(currentPage==1){ subPage=(subPage==1)?0:1; }
      else if(currentPage==2){ subPage=(subPage==2)?0:2; }
      longHandled=true;
    }
  } else if(!cur&&lastPin){
    if(now-pressStart<LONG_MS&&!longHandled){ tapCount++; lastTap=now; }
  }
  lastPin=cur;
  if(tapCount>0&&now-lastTap>DTAP_MS){
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
  float ang=millis()*0.025f;
  int r=(int)e.w/3;
  display.fillCircle(cx+(int)(cosf(ang)*r),cy+(int)(sinf(ang)*r),4,SH110X_BLACK);
  display.fillCircle(cx-(int)(cosf(ang)*r),cy-(int)(sinf(ang)*r),3,SH110X_BLACK);
}

void drawAngryEye(Eye& e, bool isLeft){
  int ix=(int)e.x,iy=(int)e.y,iw=(int)e.w,ih=(int)e.h;
  display.fillRoundRect(ix,iy,iw,ih,8,SH110X_WHITE);
  int cx=ix+iw/2, cy=iy+ih/2, ps=iw/3;
  display.fillRoundRect(cx-ps/2-2,cy-ps/2,ps,ps,ps/2,SH110X_BLACK);
  if(isLeft){ for(int i=0;i<6;i++) display.drawLine(ix-2,iy-2+i,ix+iw+2,iy+6+i,SH110X_BLACK); }
  else       { for(int i=0;i<6;i++) display.drawLine(ix-2,iy+6+i,ix+iw+2,iy-2+i,SH110X_BLACK); }
}

void drawNormalEye(Eye& e, bool isLeft){
  int ix=(int)e.x,iy=(int)e.y,iw=(int)e.w,ih=(int)e.h;
  int r=8; if(iw<20) r=3;
  display.fillRoundRect(ix,iy,iw,ih,r,SH110X_WHITE);
  int cx=ix+iw/2, cy=iy+ih/2;
  int pw=iw/2, ph=ih/2;
  int px=constrain(cx+(int)e.pupilX-pw/2,ix,ix+iw-pw);
  int py=constrain(cy+(int)e.pupilY-ph/2,iy,iy+ih-ph);
  display.fillRoundRect(px,py,pw,ph,r/2,SH110X_BLACK);
  if(iw>15&&ih>15) display.fillCircle(px+pw-4,py+4,2,SH110X_WHITE);

  if(currentMood==MOOD_HAPPY||currentMood==MOOD_LOVE)
    display.fillRect(ix,iy+ih-10,iw,12,SH110X_BLACK);
  else if(currentMood==MOOD_SLEEPY)
    display.fillRect(ix,iy,iw,ih/2,SH110X_BLACK);
  else if(currentMood==MOOD_SAD){
    if(isLeft){ for(int i=0;i<7;i++) display.drawLine(ix,iy+i,ix+iw,iy+4+i,SH110X_BLACK); }
    else       { for(int i=0;i<7;i++) display.drawLine(ix,iy+4+i,ix+iw,iy+i,SH110X_BLACK); }
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
      for(int i=-6;i<=6;i++){ int y=my-(i*i/16); if(y<my+2) display.drawPixel(mx+i,y,SH110X_WHITE); }
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
  if(isShaking){ drawDizzyEye(leftEye); drawDizzyEye(rightEye);
    unsigned long n=millis(); int off=(n/80)%4;
    display.drawBitmap(6-off,0,bmp_dizzy_stars,16,16,SH110X_WHITE);
    display.drawBitmap(106+off,0,bmp_dizzy_stars,16,16,SH110X_WHITE);
  } else if(isAngry){ drawAngryEye(leftEye,true); drawAngryEye(rightEye,false); }
  else { drawNormalEye(leftEye,true); drawNormalEye(rightEye,false); }
  drawMouth();
  if(!isShaking&&!isAngry){
    if(currentMood==MOOD_LOVE) display.drawBitmap(56,0,bmp_heart,16,16,SH110X_WHITE);
    else if(currentMood==MOOD_SLEEPY) display.drawBitmap(110,0,bmp_zzz,16,16,SH110X_WHITE);
  }
}

void drawClockPage(){
  struct tm t;
  if(!getLocalTime(&t)){ display.setFont(NULL); display.setCursor(30,30); display.print("Syncing..."); return; }
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
  display.setCursor(88,45); display.print("~"); display.print((int)wx_feels);
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

// ══════════════════════════════════════════════════════════════
//  BOOT ANIMATION
// ══════════════════════════════════════════════════════════════
void playBoot(){
  int cx=64,cy=32;
  for(int r=0;r<80;r+=5){ display.clearDisplay(); display.fillCircle(cx,cy,r,SH110X_WHITE); display.display(); delay(6); }
  for(int r=0;r<80;r+=5){ display.clearDisplay(); display.fillCircle(cx,cy,80,SH110X_WHITE); display.fillCircle(cx,cy,r,SH110X_BLACK); display.display(); delay(6); }
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

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  pinMode(TOUCH_PIN, INPUT_PULLUP);

  display.begin(0x3C, true);
  display.setTextColor(SH110X_WHITE);
  display.clearDisplay(); display.display();

  mpu6050.begin();

  leftEye.init(28,18,32,32);
  rightEye.init(80,18,32,32);

  playBoot();
  calibrateMPU();

  prefs.begin("nimo", true);
  savedSSID    = prefs.getString("ssid",    "");
  savedPass    = prefs.getString("pass",    "");
  savedApiKey  = prefs.getString("apikey",  "");
  savedCity    = prefs.getString("city",    "Lahore");
  savedCountry = prefs.getString("country", "PK");
  savedTimezone= prefs.getString("tz",      "PKT-5");
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
  if(portalActive){
    dnsServer.processNextRequest();
    server.handleClient();
  }

  handleTouch();
  updateMotion();

  if(wifiConnected && savedApiKey.length()>0 && millis()-lastWxUpdate>600000){
    fetchWeather();
  }

  static unsigned long lastWifiCheck=0;
  if(millis()-lastWifiCheck>30000){
    lastWifiCheck=millis();
    if(savedSSID.length()>0 && WiFi.status()!=WL_CONNECTED && !portalActive){
      WiFi.reconnect();
    }
  }

  display.clearDisplay();

  if(currentPage==0){
    drawFacePage();
  } else if(currentPage==1){
    if(subPage==1) drawWorldClockPage();
    else           drawClockPage();
  } else if(currentPage==2){
    if(subPage==2) drawForecastPage();
    else           drawWeatherPage();
  }

  if(portalActive){
    display.fillCircle(125,3,3,SH110X_WHITE);
    display.fillCircle(125,3,1,SH110X_BLACK);
  }

  display.display();
  delay(20);
}
