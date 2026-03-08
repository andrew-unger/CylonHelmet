#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <esp_https_server.h>
#include <math.h>

/* ================= USER CONFIG ================= */
#define LED_PIN          4
#define LED_COUNT        32
#define COLOR_ORDER      NEO_GRBW

const char* ssid     = "SnoodleDoodleDoo";
const char* password = "areyouawizardiot";

#define I2S_BCK   5
#define I2S_LRCK  6
#define I2S_DOUT  7

#define I2S_MIC_SD   15
#define I2S_MIC_SCK  16
#define I2S_MIC_WS   17

#define RADAR_RX_PIN  9
#define RADAR_TX_PIN  10
#define RADAR_BAUD    256000
/* ============================================== */

// LED runtime values
volatile int minDelay        = 15;
volatile int maxDelay        = 135;
volatile int stripBrightness = 180;
int trailLength              = 7;
float trailDecay             = 0.3;
float eyeBoostFactor         = 1.7;
int edgePauseDelay           = 10;
volatile bool ledEnabled     = true;

portMUX_TYPE ledMux      = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE radarMux    = portMUX_INITIALIZER_UNLOCKED;
static float decayLUT[16] = {};

// Audio
volatile int  audioVolume  = 2000;
volatile bool playTone     = false;
volatile bool stopAudio    = false;
String        pendingSound = "";

// Helmet mic streaming
volatile bool micStreamActive = false;
volatile int  micWsFd         = -1;
volatile int  micGainShift    = 4;

// Radar
struct RadarTarget {
  int16_t x;      // mm, negative=left, positive=right
  int16_t y;      // mm, always positive (distance)
  int16_t speed;  // cm/s, negative=approaching, positive=receding
  bool    valid;
};

volatile RadarTarget radarTargets[3] = {};
volatile int  radarTargetCount = 0;
volatile int  radarWsFd        = -1;
volatile bool radarWsActive    = false;
volatile bool targetPresent    = false;
volatile unsigned long lastTargetSoundMs = 0;
#define TARGET_SOUND_COOLDOWN_MS 5000

// Cert storage
static std::string certPem;
static std::string keyPem;

httpd_handle_t httpsServer = nullptr;
Preferences prefs;

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, COLOR_ORDER + NEO_KHZ800);

/* ================= HTML ================= */
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cylon Helmet</title>
  <style>
    body {
      background: #111;
      color: #ff3300;
      font-family: Arial, sans-serif;
      text-align: center;
      padding: 20px;
    }
    h1 { font-size: 2em; letter-spacing: 4px; text-transform: uppercase; }
    h2 { font-size: 1.2em; letter-spacing: 2px; color: #ff6644; margin-top: 30px; }
    .control { margin: 20px auto; max-width: 400px; }
    label { display: block; margin-bottom: 6px; font-size: 1.1em; }
    input[type=range] { width: 100%; accent-color: #ff3300; }
    .value { font-size: 1.2em; color: #ff6644; margin-top: 4px; }
    .btn {
      background: #ff3300;
      color: #111;
      border: none;
      padding: 12px 40px;
      font-size: 1.2em;
      border-radius: 6px;
      margin-top: 15px;
      cursor: pointer;
      letter-spacing: 2px;
      display: inline-block;
    }
    .btn:hover { background: #ff6644; }
    .btn-sound {
      background: #1a0000;
      color: #ff3300;
      border: 1px solid #ff3300;
      padding: 12px 20px;
      font-size: 1em;
      border-radius: 6px;
      margin: 8px;
      cursor: pointer;
      letter-spacing: 1px;
    }
    .btn-sound:hover { background: #ff3300; color: #111; }
    .btn-stop {
      background: #330000;
      color: #ff3300;
      border: 1px solid #ff0000;
      padding: 10px 30px;
      font-size: 1em;
      border-radius: 6px;
      margin: 8px;
      cursor: pointer;
    }
    .btn-stop:hover { background: #ff0000; color: #111; }
    .btn-mic {
      background: #1a0000;
      color: #ff3300;
      border: 2px solid #ff3300;
      padding: 15px 40px;
      font-size: 1.1em;
      border-radius: 6px;
      margin: 8px;
      cursor: pointer;
      letter-spacing: 2px;
    }
    .btn-mic.active { background: #ff3300; color: #111; }
    .btn-listen {
      background: #001a00;
      color: #00ff88;
      border: 2px solid #00ff88;
      padding: 15px 40px;
      font-size: 1.1em;
      border-radius: 6px;
      margin: 8px;
      cursor: pointer;
      letter-spacing: 2px;
    }
    .btn-listen.active { background: #00ff88; color: #111; }
    .btn-radar {
      background: #001a1a;
      color: #00ffff;
      border: 2px solid #00ffff;
      padding: 15px 40px;
      font-size: 1.1em;
      border-radius: 6px;
      margin: 8px;
      cursor: pointer;
      letter-spacing: 2px;
    }
    .btn-radar.active { background: #00ffff; color: #111; }
    .btn-led {
      background: #ff3300;
      color: #111;
      border: none;
      padding: 12px 40px;
      font-size: 1.2em;
      border-radius: 6px;
      margin-top: 15px;
      cursor: pointer;
      letter-spacing: 2px;
      display: inline-block;
    }
    .btn-led:hover { background: #ff6644; }
    .btn-led.off {
      background: #1a0000;
      color: #ff3300;
      border: 1px solid #ff3300;
    }
    .btn-led.off:hover { background: #330000; }
    hr { border-color: #ff3300; opacity: 0.3; margin: 30px 0; }
    .status { font-size: 0.9em; color: #ff6644; margin-top: 8px; }
    .status-green { font-size: 0.9em; color: #00ff88; margin-top: 8px; }
    .status-cyan  { font-size: 0.9em; color: #00ffff; margin-top: 8px; }
    .fx-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      max-width: 400px;
      margin: 0 auto;
    }
    .fx-box {
      background: #1a0000;
      border: 1px solid #ff3300;
      border-radius: 6px;
      padding: 10px;
    }
    .fx-box label { font-size: 0.85em; margin-bottom: 4px; }
    .fx-box .value { font-size: 1em; margin-top: 2px; }
    .mic-control {
      margin: 10px auto;
      max-width: 400px;
      background: #001a00;
      border: 1px solid #00ff88;
      border-radius: 6px;
      padding: 10px;
    }
    .mic-control label { color: #00ff88; font-size: 0.95em; margin-bottom: 4px; }
    .mic-control input[type=range] { accent-color: #00ff88; }
    .mic-control .value { color: #00ff88; font-size: 1em; margin-top: 2px; }
    /* DRADIS */
    #dradisCanvas {
      display: block;
      margin: 16px auto 0 auto;
    }
    .dradis-info {
      color: #00ffff;
      font-size: 0.85em;
      font-family: monospace;
      margin-top: 8px;
      min-height: 60px;
    }
  </style>
</head>
<body>
  <h1>&#9651; Cylon Control</h1>

  <div class="control">
    <label>Brightness</label>
    <input type="range" min="10" max="255" value="180" id="brightness"
      oninput="document.getElementById('bval').innerText=this.value">
    <div class="value" id="bval"></div>
  </div>

  <div class="control">
    <label>Min Delay (Center Speed)</label>
    <input type="range" min="1" max="50" value="15" id="mindelay"
      oninput="document.getElementById('mnval').innerText=this.value">
    <div class="value" id="mnval"></div>
  </div>

  <div class="control">
    <label>Max Delay (Edge Speed)</label>
    <input type="range" min="20" max="200" value="135" id="maxdelay"
      oninput="document.getElementById('mxval').innerText=this.value">
    <div class="value" id="mxval"></div>
  </div>

  <div class="control">
    <label>Volume</label>
    <input type="range" min="0" max="32767" value="2000" id="volume"
      oninput="document.getElementById('vval').innerText=this.value">
    <div class="value" id="vval"></div>
  </div>

  <button class="btn" onclick="sendSettings()">APPLY</button>
  <br>
  <button class="btn-led" id="ledBtn" onclick="toggleLed()">&#9679; LED ON</button>

  <hr>

  <h2>&#9651; SOUNDBOARD</h2>
  <button class="btn-sound" onclick="playSound('byc')">BY YOUR COMMAND</button>
  <button class="btn-sound" onclick="playSound('scan')">SCAN FOR IDENTIFICATION</button>
  <button class="btn-sound" onclick="playSound('survivors')">LEAVE NO SURVIVORS</button>
  <br>
  <button class="btn-stop" onclick="stopSound()">&#9632; STOP</button>

  <hr>

  <h2>&#9651; CYLON VOICE</h2>

  <div class="fx-grid">
    <div class="fx-box">
      <label>Ring Mod Hz</label>
      <input type="range" min="10" max="200" value="80" id="ringFreq"
        oninput="document.getElementById('rfval').innerText=this.value; updateFX()">
      <div class="value" id="rfval">80</div>
    </div>
    <div class="fx-box">
      <label>Distortion</label>
      <input type="range" min="0" max="200" value="120" id="distAmount"
        oninput="document.getElementById('dval').innerText=this.value; updateFX()">
      <div class="value" id="dval">120</div>
    </div>
    <div class="fx-box">
      <label>Peak Freq (Hz)</label>
      <input type="range" min="500" max="5000" value="2500" id="peakFreq"
        oninput="document.getElementById('pfval').innerText=this.value; updateFX()">
      <div class="value" id="pfval">2500</div>
    </div>
    <div class="fx-box">
      <label>Peak Gain (dB)</label>
      <input type="range" min="0" max="30" value="18" id="peakGain"
        oninput="document.getElementById('pgval').innerText=this.value; updateFX()">
      <div class="value" id="pgval">18</div>
    </div>
    <div class="fx-box">
      <label>Peak 2 Freq (Hz)</label>
      <input type="range" min="1000" max="8000" value="4000" id="peak2Freq"
        oninput="document.getElementById('p2fval').innerText=this.value; updateFX()">
      <div class="value" id="p2fval">4000</div>
    </div>
    <div class="fx-box">
      <label>Peak 2 Gain (dB)</label>
      <input type="range" min="0" max="30" value="10" id="peak2Gain"
        oninput="document.getElementById('p2gval').innerText=this.value; updateFX()">
      <div class="value" id="p2gval">10</div>
    </div>
    <div class="fx-box">
      <label>Highpass (Hz)</label>
      <input type="range" min="50" max="1000" value="300" id="hpFreq"
        oninput="document.getElementById('hpval').innerText=this.value; updateFX()">
      <div class="value" id="hpval">300</div>
    </div>
    <div class="fx-box">
      <label>Lowpass (Hz)</label>
      <input type="range" min="1000" max="8000" value="4000" id="lpFreq"
        oninput="document.getElementById('lpval').innerText=this.value; updateFX()">
      <div class="value" id="lpval">4000</div>
    </div>
  </div>

  <br>
  <button class="btn-mic" id="micBtn" onclick="toggleMic()">&#9671; ACTIVATE VOICE</button>
  <div class="status" id="micStatus">Voice inactive</div>

  <hr>

  <h2>&#9651; HELMET MIC</h2>

  <div class="mic-control">
    <label>Mic Gain</label>
    <input type="range" min="1" max="8" value="4" id="micGain"
      oninput="document.getElementById('mgval').innerText=gainLabel(this.value); setMicGain(this.value)">
    <div class="value" id="mgval">16x</div>
  </div>

  <button class="btn-listen" id="listenBtn" onclick="toggleListen()">&#9671; LISTEN</button>
  <div class="status-green" id="listenStatus">Helmet mic off</div>

  <hr>

  <h2>&#9651; DRADIS</h2>
  <button class="btn-radar" id="radarBtn" onclick="toggleRadar()">&#9671; ACTIVATE DRADIS</button>
  <div class="status-cyan" id="radarStatus">DRADIS offline</div>
  <canvas id="dradisCanvas" width="320" height="175"></canvas>
  <div class="dradis-info" id="dradisInfo">-- NO CONTACTS --</div>

  <script>
    window.onload = function() {
      fetch('/state').then(r => r.json()).then(s => {
        document.getElementById('brightness').value = s.brightness;
        document.getElementById('bval').innerText   = s.brightness;
        document.getElementById('mindelay').value   = s.mindelay;
        document.getElementById('mnval').innerText  = s.mindelay;
        document.getElementById('maxdelay').value   = s.maxdelay;
        document.getElementById('mxval').innerText  = s.maxdelay;
        document.getElementById('volume').value     = s.volume;
        document.getElementById('vval').innerText   = s.volume;
        document.getElementById('micGain').value    = s.micgain;
        document.getElementById('mgval').innerText  = gainLabel(s.micgain);
        updateLedBtn(s.ledon === 1);
      });
      drawDradisIdle();
    }

    function updateLedBtn(on) {
      const btn = document.getElementById('ledBtn');
      if (on) {
        btn.classList.remove('off');
        btn.innerHTML = '&#9679; LED ON';
      } else {
        btn.classList.add('off');
        btn.innerHTML = '&#9632; LED OFF';
      }
    }

    function toggleLed() {
      fetch('/ledtoggle').then(r => r.text()).then(state => {
        updateLedBtn(state === 'ON');
      });
    }

    function gainLabel(shift) {
      const gain = Math.round(Math.pow(2, 8 - parseInt(shift)));
      return gain + 'x';
    }

    function setMicGain(val) {
      fetch(`/micgain?v=${val}`).then(r => r.text()).then(t => console.log(t));
    }

    function sendSettings() {
      const b  = document.getElementById('brightness').value;
      const mn = document.getElementById('mindelay').value;
      const mx = document.getElementById('maxdelay').value;
      const v  = document.getElementById('volume').value;
      fetch(`/set?brightness=${b}&mindelay=${mn}&maxdelay=${mx}&volume=${v}`)
        .then(r => r.text()).then(t => console.log(t));
    }

    function playSound(name) {
      fetch(`/sound?name=${name}`).then(r => r.text()).then(t => console.log(t));
    }

    function stopSound() {
      fetch('/stop').then(r => r.text()).then(t => console.log(t));
    }

    // ===== CYLON VOICE =====
    let micActive  = false;
    let audioCtx   = null;
    let micStream  = null;
    let processor  = null;
    let ws         = null;
    let carrier    = null;
    let distortion = null;
    let peak       = null;
    let peak2      = null;
    let highpass   = null;
    let lowpass    = null;

    function makeDistortionCurve(amount) {
      const samples = 256;
      const curve = new Float32Array(samples);
      for (let i = 0; i < samples; i++) {
        const x = (i * 2) / samples - 1;
        curve[i] = (Math.PI + amount) * x / (Math.PI + amount * Math.abs(x));
      }
      return curve;
    }

    function updateFX() {
      if (carrier)    carrier.frequency.value        = parseFloat(document.getElementById('ringFreq').value);
      if (distortion) distortion.curve               = makeDistortionCurve(parseFloat(document.getElementById('distAmount').value));
      if (peak)     { peak.frequency.value           = parseFloat(document.getElementById('peakFreq').value);
                      peak.gain.value                = parseFloat(document.getElementById('peakGain').value); }
      if (peak2)    { peak2.frequency.value          = parseFloat(document.getElementById('peak2Freq').value);
                      peak2.gain.value               = parseFloat(document.getElementById('peak2Gain').value); }
      if (highpass)   highpass.frequency.value       = parseFloat(document.getElementById('hpFreq').value);
      if (lowpass)    lowpass.frequency.value        = parseFloat(document.getElementById('lpFreq').value);
    }

    async function toggleMic() {
      if (!micActive) { await startMic(); } else { stopMic(); }
    }

    async function startMic() {
      try {
        document.getElementById('micStatus').innerText = 'Requesting mic...';
        micStream = await navigator.mediaDevices.getUserMedia({
          audio: { noiseSuppression: true, echoCancellation: true, autoGainControl: true },
          video: false
        });

        audioCtx = new AudioContext();
        const source = audioCtx.createMediaStreamSource(micStream);

        highpass = audioCtx.createBiquadFilter();
        highpass.type = 'highpass';
        highpass.frequency.value = parseFloat(document.getElementById('hpFreq').value);
        highpass.Q.value = 0.5;

        peak = audioCtx.createBiquadFilter();
        peak.type = 'peaking';
        peak.frequency.value = parseFloat(document.getElementById('peakFreq').value);
        peak.gain.value      = parseFloat(document.getElementById('peakGain').value);
        peak.Q.value = 2.5;

        peak2 = audioCtx.createBiquadFilter();
        peak2.type = 'peaking';
        peak2.frequency.value = parseFloat(document.getElementById('peak2Freq').value);
        peak2.gain.value      = parseFloat(document.getElementById('peak2Gain').value);
        peak2.Q.value = 2.0;

        carrier = audioCtx.createOscillator();
        carrier.type = 'sawtooth';
        carrier.frequency.value = parseFloat(document.getElementById('ringFreq').value);
        const ringMod = audioCtx.createGain();
        ringMod.gain.value = 1.0;
        carrier.connect(ringMod.gain);

        distortion = audioCtx.createWaveShaper();
        distortion.curve = makeDistortionCurve(parseFloat(document.getElementById('distAmount').value));
        distortion.oversample = '4x';

        lowpass = audioCtx.createBiquadFilter();
        lowpass.type = 'lowpass';
        lowpass.frequency.value = parseFloat(document.getElementById('lpFreq').value);
        lowpass.Q.value = 1.0;

        const finalBand = audioCtx.createBiquadFilter();
        finalBand.type = 'bandpass';
        finalBand.frequency.value = 2500;
        finalBand.Q.value = 1.5;

        source.connect(highpass);
        highpass.connect(peak);
        peak.connect(peak2);
        peak2.connect(ringMod);
        ringMod.connect(distortion);
        distortion.connect(lowpass);
        lowpass.connect(finalBand);

        processor = audioCtx.createScriptProcessor(2048, 1, 1);
        finalBand.connect(processor);
        processor.connect(audioCtx.destination);

        ws = new WebSocket('wss://cylon.local/audio');
        ws.binaryType = 'arraybuffer';

        ws.onopen = () => {
          document.getElementById('micStatus').innerText = 'Voice active - Cylon mode';
          document.getElementById('micBtn').classList.add('active');
          document.getElementById('micBtn').innerHTML = '&#9679; DEACTIVATE VOICE';
          micActive = true;
          carrier.start();
        };

        ws.onerror = () => {
          document.getElementById('micStatus').innerText = 'Connection error';
          stopMic();
        };

        processor.onaudioprocess = (e) => {
          if (ws && ws.readyState === WebSocket.OPEN) {
            const float32 = e.inputBuffer.getChannelData(0);
            const ulaw = new Uint8Array(float32.length);
            for (let i = 0; i < float32.length; i++) {
              let s = Math.round(Math.max(-1, Math.min(1, float32[i])) * 32767);
              let sign = 0;
              if (s < 0) { sign = 0x80; s = -s; }
              if (s > 32635) s = 32635;
              s += 132;
              let exp = 7;
              for (let m = 0x4000; (s & m) === 0 && exp > 0; exp--, m >>= 1);
              ulaw[i] = (~(sign | (exp << 4) | ((s >> (exp + 3)) & 0x0F))) & 0xFF;
            }
            ws.send(ulaw.buffer);
          }
        };
      } catch(e) {
        document.getElementById('micStatus').innerText = 'Error: ' + e.message;
      }
    }

    function stopMic() {
      if (processor)  { processor.disconnect(); processor = null; }
      if (audioCtx)   { audioCtx.close(); audioCtx = null; }
      if (micStream)  { micStream.getTracks().forEach(t => t.stop()); micStream = null; }
      if (ws)         { ws.close(); ws = null; }
      carrier = null; distortion = null; peak = null;
      peak2 = null; highpass = null; lowpass = null;
      micActive = false;
      document.getElementById('micBtn').classList.remove('active');
      document.getElementById('micBtn').innerHTML = '&#9671; ACTIVATE VOICE';
      document.getElementById('micStatus').innerText = 'Voice inactive';
    }

    // ===== HELMET MIC LISTEN =====
    let listenActive = false;
    let listenWs     = null;
    let listenCtx    = null;

    async function toggleListen() {
      if (!listenActive) { await startListen(); } else { stopListen(); }
    }

    async function startListen() {
      listenCtx = new AudioContext({ sampleRate: 16000 });
      listenWs  = new WebSocket('wss://cylon.local/mic');
      listenWs.binaryType = 'arraybuffer';

      listenWs.onopen = () => {
        document.getElementById('listenStatus').innerText = 'Listening...';
        document.getElementById('listenBtn').classList.add('active');
        document.getElementById('listenBtn').innerHTML = '&#9679; STOP LISTENING';
        listenActive = true;
      };

      listenWs.onerror = () => {
        document.getElementById('listenStatus').innerText = 'Connection error';
        stopListen();
      };

      listenWs.onmessage = (event) => {
        const ulaw    = new Uint8Array(event.data);
        const float32 = new Float32Array(ulaw.length);
        for (let i = 0; i < ulaw.length; i++) {
          let u    = ~ulaw[i];
          let sign = u & 0x80;
          let exp  = (u >> 4) & 0x07;
          let mant = u & 0x0F;
          let s    = (((mant << 3) + 0x84) << exp) - 0x84;
          float32[i] = (sign ? -s : s) / 32768.0;
        }
        const buffer = listenCtx.createBuffer(1, float32.length, 16000);
        buffer.copyToChannel(float32, 0);
        const src = listenCtx.createBufferSource();
        src.buffer = buffer;
        src.connect(listenCtx.destination);
        src.start();
      };
    }

    function stopListen() {
      if (listenWs)  { listenWs.close(); listenWs = null; }
      if (listenCtx) { listenCtx.close(); listenCtx = null; }
      listenActive = false;
      document.getElementById('listenBtn').classList.remove('active');
      document.getElementById('listenBtn').innerHTML = '&#9671; LISTEN';
      document.getElementById('listenStatus').innerText = 'Helmet mic off';
    }

    // ===== DRADIS =====
    // Sensor sits at bottom-center; FOV is upper 180° semicircle.
    // Sweep uses math angles: 0=right, π/2=up, π=left (Y negated for canvas).
    const DRADIS_RANGE_MM = 6000;
    const canvas   = document.getElementById('dradisCanvas');
    const ctx      = canvas.getContext('2d');
    const CX       = canvas.width  / 2;  // 160
    const SENSOR_Y = canvas.height - 12; // sensor origin near bottom
    const RADIUS   = 155;                // 5px margin from sides

    let radarActive = false;
    let radarWs     = null;
    // sweepAngle: math angle, π=left → 0=right (decrements each frame)
    let sweepAngle  = Math.PI / 2;
    let sweepAnimId = null;
    let lastTargets = [];
    let blipTrails  = [];

    // Upper-semicircle clip path (sensor at bottom-center)
    function dradisClipPath() {
      ctx.beginPath();
      ctx.arc(CX, SENSOR_Y, RADIUS, Math.PI, 0, false); // clockwise = upper arc
      ctx.closePath(); // straight line closes the flat base
    }

    function drawDradisRings() {
      ctx.strokeStyle = '#004433';
      ctx.lineWidth = 1;
      for (let r = 1; r <= 4; r++) {
        ctx.beginPath();
        ctx.arc(CX, SENSOR_Y, RADIUS * r / 4, Math.PI, 0, false);
        ctx.stroke();
      }
      // Flat base line
      ctx.beginPath();
      ctx.moveTo(CX - RADIUS, SENSOR_Y);
      ctx.lineTo(CX + RADIUS, SENSOR_Y);
      ctx.stroke();
      // Center vertical axis
      ctx.strokeStyle = '#003322';
      ctx.beginPath();
      ctx.moveTo(CX, SENSOR_Y);
      ctx.lineTo(CX, SENSOR_Y - RADIUS);
      ctx.stroke();
    }

    function drawDradisLabels() {
      ctx.fillStyle = '#006644';
      ctx.font = '10px monospace';
      ctx.textAlign = 'center';
      for (let r = 1; r <= 4; r++) {
        const dist = Math.round(DRADIS_RANGE_MM * r / 4 / 1000 * 10) / 10;
        ctx.fillText(dist + 'm', CX + 4, SENSOR_Y - RADIUS * r / 4 - 3);
      }
    }

    function drawDradisBorder() {
      ctx.strokeStyle = '#00ffff44';
      ctx.lineWidth = 2;
      dradisClipPath();
      ctx.stroke();
    }

    function drawDradisIdle() {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      dradisClipPath();
      ctx.fillStyle = '#000d0d';
      ctx.fill();
      drawDradisRings();
      drawDradisLabels();
      ctx.fillStyle = '#004433';
      ctx.font = '11px monospace';
      ctx.textAlign = 'center';
      ctx.fillText('DRADIS OFFLINE', CX, SENSOR_Y - RADIUS * 0.5);
      // Sensor dot
      ctx.fillStyle = '#006644';
      ctx.beginPath();
      ctx.arc(CX, SENSOR_Y, 3, 0, Math.PI * 2);
      ctx.fill();
      drawDradisBorder();
    }

    function drawDradisFrame() {
      ctx.clearRect(0, 0, canvas.width, canvas.height);

      // Background fill
      dradisClipPath();
      ctx.fillStyle = '#000d0d';
      ctx.fill();

      // Clip to semicircle for sweep + blips
      ctx.save();
      dradisClipPath();
      ctx.clip();

      // Sweep trail: fan of sector slices behind the sweep line
      // Trail spans 90° behind current sweep angle (higher math angles = leftward)
      const trailSpan  = Math.PI * 0.5;
      const trailSteps = 50;
      for (let i = 0; i < trailSteps; i++) {
        const a1    = sweepAngle + trailSpan * i / trailSteps;
        const a2    = sweepAngle + trailSpan * (i + 1) / trailSteps;
        const alpha = (1 - i / trailSteps) * 0.22;
        ctx.beginPath();
        ctx.moveTo(CX, SENSOR_Y);
        for (let j = 0; j <= 3; j++) {
          const a = a1 + (a2 - a1) * j / 3;
          ctx.lineTo(CX + Math.cos(a) * RADIUS, SENSOR_Y - Math.sin(a) * RADIUS);
        }
        ctx.closePath();
        ctx.fillStyle = `rgba(0,255,180,${alpha})`;
        ctx.fill();
      }

      // Sweep line
      ctx.strokeStyle = 'rgba(0,255,180,0.9)';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(CX, SENSOR_Y);
      ctx.lineTo(
        CX + Math.cos(sweepAngle) * RADIUS,
        SENSOR_Y - Math.sin(sweepAngle) * RADIUS
      );
      ctx.stroke();

      // Fading blip trails
      blipTrails = blipTrails.filter(b => b.age < 60);
      for (const b of blipTrails) {
        const alpha = 1 - b.age / 60;
        ctx.beginPath();
        ctx.arc(b.cx, b.cy, 5, 0, Math.PI * 2);
        ctx.fillStyle = `rgba(0,255,150,${alpha * 0.4})`;
        ctx.fill();
        b.age++;
      }

      // Live target blips
      // X: negative=left, positive=right; Y: distance (always positive = upward)
      for (const t of lastTargets) {
        if (!t.valid) continue;
        const px = CX + (t.x / DRADIS_RANGE_MM) * RADIUS;
        const py = SENSOR_Y - (t.y / DRADIS_RANGE_MM) * RADIUS;
        const grd = ctx.createRadialGradient(px, py, 0, px, py, 12);
        grd.addColorStop(0, 'rgba(0,255,150,0.9)');
        grd.addColorStop(1, 'rgba(0,255,150,0)');
        ctx.beginPath();
        ctx.arc(px, py, 12, 0, Math.PI * 2);
        ctx.fillStyle = grd;
        ctx.fill();
        ctx.beginPath();
        ctx.arc(px, py, 4, 0, Math.PI * 2);
        ctx.fillStyle = '#00ffaa';
        ctx.fill();
        blipTrails.push({ cx: px, cy: py, age: 0 });
      }

      ctx.restore();

      // Grid, labels, sensor dot and border drawn on top (outside clip)
      drawDradisRings();
      drawDradisLabels();
      ctx.fillStyle = '#00ffff';
      ctx.beginPath();
      ctx.arc(CX, SENSOR_Y, 3, 0, Math.PI * 2);
      ctx.fill();
      drawDradisBorder();

      // Advance sweep: π (left) → 0 (right), then reset
      sweepAngle -= 0.018;
      if (sweepAngle < 0) sweepAngle = Math.PI;

      sweepAnimId = requestAnimationFrame(drawDradisFrame);
    }

    function toggleRadar() {
      if (!radarActive) { startRadar(); } else { stopRadar(); }
    }

    function startRadar() {
      radarWs = new WebSocket('wss://cylon.local/radar');
      radarWs.binaryType = 'arraybuffer';

      radarWs.onopen = () => {
        document.getElementById('radarStatus').innerText = 'DRADIS active - scanning...';
        document.getElementById('radarBtn').classList.add('active');
        document.getElementById('radarBtn').innerHTML = '&#9679; DEACTIVATE DRADIS';
        radarActive = true;
        sweepAngle = Math.PI; // start at left edge
        blipTrails = [];
        sweepAnimId = requestAnimationFrame(drawDradisFrame);
      };

      radarWs.onerror = () => {
        document.getElementById('radarStatus').innerText = 'Connection error';
        stopRadar();
      };

      radarWs.onclose = () => {
        stopRadar();
      };

      radarWs.onmessage = (event) => {
        // Binary frame: 1 byte count, then up to 3 x 6 bytes (x,y,speed each int16 LE)
        const data = new DataView(event.data);
        const count = data.getUint8(0);
        lastTargets = [];
        let infoLines = [];
        for (let i = 0; i < count; i++) {
          const offset = 1 + i * 6;
          const x     = data.getInt16(offset,     true); // mm
          const y     = data.getInt16(offset + 2, true); // mm
          const speed = data.getInt16(offset + 4, true); // cm/s
          lastTargets.push({ x, y, speed, valid: true });
          const dist  = Math.round(Math.sqrt(x*x + y*y));
          const dir   = speed < 0 ? 'APPROACHING' : speed > 0 ? 'RECEDING' : 'STATIC';
          infoLines.push(`TGT ${i+1}: ${(dist/1000).toFixed(2)}m  ${Math.abs(speed)}cm/s  ${dir}`);
        }
        document.getElementById('dradisInfo').innerText =
          count > 0 ? infoLines.join('\n') : '-- NO CONTACTS --';
      };
    }

    function stopRadar() {
      if (radarWs)     { radarWs.close(); radarWs = null; }
      if (sweepAnimId) { cancelAnimationFrame(sweepAnimId); sweepAnimId = null; }
      radarActive = false;
      lastTargets = [];
      blipTrails  = [];
      document.getElementById('radarBtn').classList.remove('active');
      document.getElementById('radarBtn').innerHTML = '&#9671; ACTIVATE DRADIS';
      document.getElementById('radarStatus').innerText = 'DRADIS offline';
      document.getElementById('dradisInfo').innerText = '-- NO CONTACTS --';
      drawDradisIdle();
    }
  </script>
</body>
</html>
)rawliteral";

/* ================= WAV HELPERS ================= */
static uint32_t wavDataOffset(File &file) {
  file.seek(12);
  while (file.available() >= 8) {
    char id[4];
    file.read((uint8_t*)id, 4);
    uint32_t size = 0;
    file.read((uint8_t*)&size, 4);
    if (memcmp(id, "data", 4) == 0) return file.position();
    file.seek(file.position() + size);
  }
  return 44;
}

/* ================= WAV PLAYER ================= */
void playWavFile(const char* filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) { Serial.printf("Failed to open %s\n", filename); return; }
  file.seek(wavDataOffset(file));
  const int bufSize = 512;
  int16_t stereo[bufSize * 2];
  uint8_t raw[bufSize * 2];
  int16_t lastSample = 0;
  stopAudio = false;
  while (file.available() && !stopAudio) {
    int bytesRead = file.read(raw, sizeof(raw));
    if (bytesRead <= 0) break;
    int samples = bytesRead / 2;
    for (int i = 0; i < samples; i++) {
      int16_t sample = (int16_t)(raw[i*2] | (raw[i*2+1] << 8));
      int16_t scaled = (int16_t)((sample * audioVolume) / 32767);
      stereo[i*2]   = scaled;
      stereo[i*2+1] = scaled;
    }
    lastSample = stereo[(samples - 1) * 2];
    size_t bytesWritten;
    i2s_write(I2S_NUM_0, stereo, samples * 4, &bytesWritten, portMAX_DELAY);
  }
  file.close();
  const int fadeLen = 256;
  int16_t fadeBuf[fadeLen * 2];
  for (int i = 0; i < fadeLen; i++) {
    int16_t s = (int16_t)((int32_t)lastSample * (fadeLen - 1 - i) / fadeLen);
    fadeBuf[i*2]   = s;
    fadeBuf[i*2+1] = s;
  }
  size_t bw;
  i2s_write(I2S_NUM_0, fadeBuf, sizeof(fadeBuf), &bw, portMAX_DELAY);
}

/* ================= I2S SETUP ================= */
void setupI2S() {
  i2s_config_t dac_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = 44100,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
    .tx_desc_auto_clear   = true
  };
  i2s_pin_config_t dac_pins = {
    .bck_io_num   = I2S_BCK,
    .ws_io_num    = I2S_LRCK,
    .data_out_num = I2S_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &dac_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &dac_pins);

  i2s_config_t mic_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = 16000,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 16,
    .dma_buf_len          = 128,
    .use_apll             = false
  };
  i2s_pin_config_t mic_pins = {
    .bck_io_num   = I2S_MIC_SCK,
    .ws_io_num    = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_1, &mic_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &mic_pins);
}

/* ================= μLAW ================= */
static inline uint8_t linearToUlaw(int16_t sample) {
  int sign = 0, s = sample;
  if (s < 0) { sign = 0x80; s = -s; }
  if (s > 32635) s = 32635;
  s += 132;
  int exp = 7;
  for (int m = 0x4000; (s & m) == 0 && exp > 0; exp--, m >>= 1);
  return (~(sign | (exp << 4) | ((s >> (exp + 3)) & 0x0F))) & 0xFF;
}

static inline int16_t ulawToLinear(uint8_t ulaw) {
  ulaw = ~ulaw;
  int sign = ulaw & 0x80, exp = (ulaw >> 4) & 0x07, mant = ulaw & 0x0F;
  int s = (((mant << 3) + 0x84) << exp) - 0x84;
  return (int16_t)(sign ? -s : s);
}

/* ================= HTTP HANDLERS ================= */
static esp_err_t rootHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, index_html, strlen(index_html));
  return ESP_OK;
}

static esp_err_t stateHandler(httpd_req_t *req) {
  char json[160];
  snprintf(json, sizeof(json),
    "{\"brightness\":%d,\"mindelay\":%d,\"maxdelay\":%d,\"volume\":%d,\"micgain\":%d,\"ledon\":%d}",
    (int)stripBrightness, (int)minDelay, (int)maxDelay, (int)audioVolume, (int)micGainShift, ledEnabled ? 1 : 0);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, strlen(json));
  return ESP_OK;
}

static esp_err_t setHandler(httpd_req_t *req) {
  char buf[200];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char val[32];
    if (httpd_query_key_value(buf, "brightness", val, sizeof(val)) == ESP_OK) {
      stripBrightness = atoi(val); strip.setBrightness(stripBrightness);
    }
    if (httpd_query_key_value(buf, "mindelay", val, sizeof(val)) == ESP_OK) minDelay = atoi(val);
    if (httpd_query_key_value(buf, "maxdelay", val, sizeof(val)) == ESP_OK) maxDelay = atoi(val);
    if (httpd_query_key_value(buf, "volume",   val, sizeof(val)) == ESP_OK) audioVolume = atoi(val);
    prefs.putInt("brightness", (int)stripBrightness);
    prefs.putInt("mindelay",   (int)minDelay);
    prefs.putInt("maxdelay",   (int)maxDelay);
    prefs.putInt("volume",     (int)audioVolume);
  }
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t micGainHandler(httpd_req_t *req) {
  char buf[64];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(buf, "v", val, sizeof(val)) == ESP_OK) {
      int g = atoi(val);
      if (g >= 1 && g <= 8) { micGainShift = g; prefs.putInt("micgain", g); }
    }
  }
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t soundHandler(httpd_req_t *req) {
  char buf[100];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char name[32];
    if (httpd_query_key_value(buf, "name", name, sizeof(name)) == ESP_OK) {
      stopAudio = true; pendingSound = String(name);
    }
  }
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t ledToggleHandler(httpd_req_t *req) {
  ledEnabled = !ledEnabled;
  const char* resp = ledEnabled ? "ON" : "OFF";
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t stopHandler(httpd_req_t *req) {
  stopAudio = true; pendingSound = ""; playTone = false;
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t wsAudioHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) { Serial.println("Voice WS connected"); return ESP_OK; }
  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = nullptr;
  memset(&ws_pkt, 0, sizeof(ws_pkt));
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) return ret;
  if (ws_pkt.len) {
    buf = (uint8_t*)malloc(ws_pkt.len);
    if (!buf) return ESP_ERR_NO_MEM;
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret == ESP_OK) {
      int samples = ws_pkt.len;
      int16_t* stereo = (int16_t*)malloc(samples * 4);
      if (stereo) {
        for (int i = 0; i < samples; i++) {
          int16_t s = ulawToLinear(buf[i]);
          int16_t scaled = (int16_t)((s * audioVolume) / 32767);
          stereo[i*2] = stereo[i*2+1] = scaled;
        }
        size_t bw;
        i2s_write(I2S_NUM_0, stereo, samples * 4, &bw, portMAX_DELAY);
        free(stereo);
      }
    }
    free(buf);
  }
  return ret;
}

static esp_err_t wsMicHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    micWsFd = httpd_req_to_sockfd(req);
    micStreamActive = true;
    Serial.println("Helmet mic WS connected");
    return ESP_OK;
  }
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(ws_pkt));
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;
  httpd_ws_recv_frame(req, &ws_pkt, 0);
  return ESP_OK;
}

static esp_err_t wsRadarHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    radarWsFd = httpd_req_to_sockfd(req);
    radarWsActive = true;
    Serial.println("DRADIS WS connected");
    return ESP_OK;
  }
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(ws_pkt));
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;
  httpd_ws_recv_frame(req, &ws_pkt, 0);
  return ESP_OK;
}

/* ================= HTTPS SERVER ================= */
void startHTTPS() {
  File certFile = LittleFS.open("/cert.pem", "r");
  File keyFile  = LittleFS.open("/key.pem", "r");
  if (!certFile || !keyFile) { Serial.println("Failed to open cert files!"); return; }
  certPem = std::string(certFile.readString().c_str());
  keyPem  = std::string(keyFile.readString().c_str());
  certFile.close(); keyFile.close();
  Serial.println("Certs loaded");

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert     = (const uint8_t*)certPem.c_str();
  conf.servercert_len = certPem.size() + 1;
  conf.prvtkey_pem    = (const uint8_t*)keyPem.c_str();
  conf.prvtkey_len    = keyPem.size() + 1;
  conf.httpd.max_uri_handlers = 16;
  conf.httpd.stack_size = 10240;

  if (httpd_ssl_start(&httpsServer, &conf) != ESP_OK) {
    Serial.println("HTTPS start failed"); return;
  }
  Serial.println("HTTPS server started");

  httpd_uri_t uriRoot    = { "/",        HTTP_GET, rootHandler,    nullptr };
  httpd_uri_t uriSet     = { "/set",     HTTP_GET, setHandler,     nullptr };
  httpd_uri_t uriSound   = { "/sound",   HTTP_GET, soundHandler,   nullptr };
  httpd_uri_t uriStop    = { "/stop",    HTTP_GET, stopHandler,    nullptr };
  httpd_uri_t uriState   = { "/state",   HTTP_GET, stateHandler,   nullptr };
  httpd_uri_t uriMicGain = { "/micgain", HTTP_GET, micGainHandler, nullptr };
  httpd_uri_t uriWsAudio  = { "/audio",     HTTP_GET, wsAudioHandler,  nullptr, true };
  httpd_uri_t uriWsMic    = { "/mic",       HTTP_GET, wsMicHandler,    nullptr, true };
  httpd_uri_t uriWsRadar  = { "/radar",     HTTP_GET, wsRadarHandler,  nullptr, true };
  httpd_uri_t uriLedToggle = { "/ledtoggle", HTTP_GET, ledToggleHandler, nullptr };

  httpd_register_uri_handler(httpsServer, &uriRoot);
  httpd_register_uri_handler(httpsServer, &uriSet);
  httpd_register_uri_handler(httpsServer, &uriSound);
  httpd_register_uri_handler(httpsServer, &uriStop);
  httpd_register_uri_handler(httpsServer, &uriState);
  httpd_register_uri_handler(httpsServer, &uriMicGain);
  httpd_register_uri_handler(httpsServer, &uriLedToggle);
  httpd_register_uri_handler(httpsServer, &uriWsAudio);
  httpd_register_uri_handler(httpsServer, &uriWsMic);
  httpd_register_uri_handler(httpsServer, &uriWsRadar);
}

/* ================= AUDIO TASK ================= */
void audioTask(void *pvParameters) {
  while (true) {
    if (pendingSound != "") {
      String sound = pendingSound; pendingSound = "";
      playWavFile(("/" + sound + ".wav").c_str());
    } else if (playTone) {
      static float phase = 0;
      float inc = 2.0f * M_PI * 440.0f / 44100.0f;
      int16_t buf[64];
      for (int i = 0; i < 64; i++) {
        buf[i] = (int16_t)(sinf(phase) * audioVolume);
        phase += inc;
        if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
      }
      size_t bw;
      i2s_write(I2S_NUM_0, buf, sizeof(buf), &bw, portMAX_DELAY);
    } else {
      int16_t silence[64] = {0};
      size_t bw;
      i2s_write(I2S_NUM_0, silence, sizeof(silence), &bw, portMAX_DELAY);
      vTaskDelay(1);
    }
  }
}

/* ================= MIC TASK ================= */
void micTask(void *pvParameters) {
  const int samples = 256;
  int32_t raw[samples];
  uint8_t ulaw[samples];
  while (true) {
    if (!micStreamActive || micWsFd < 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_1, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
    if (bytesRead == 0) continue;
    int count = bytesRead / 4;
    int shift = micGainShift;
    for (int i = 0; i < count; i++) {
      int32_t s32 = raw[i] >> 8;
      int32_t boosted = s32 >> shift;
      int16_t s16 = (int16_t)(std::max(-32768, std::min(32767, (int)boosted)));
      ulaw[i] = linearToUlaw(s16);
    }
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.payload = ulaw;
    ws_pkt.len     = count;
    ws_pkt.type    = HTTPD_WS_TYPE_BINARY;
    if (httpd_ws_send_frame_async(httpsServer, micWsFd, &ws_pkt) != ESP_OK) {
      micStreamActive = false; micWsFd = -1;
      Serial.println("Helmet mic client disconnected");
    }
  }
}

/* ================= RADAR TASK ================= */
// LD2450 frame: AA FF 03 00 | t1(6) | t2(6) | t3(6) | 55 CC  = 30 bytes total
void radarTask(void *pvParameters) {
  Serial2.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  Serial.println("LD2450 UART started");

  uint8_t frameBuf[30];
  int framePos = 0;
  bool inFrame = false;

  // Binary packet sent to browser: 1 byte count + up to 3 * 6 bytes (x,y,speed int16 LE)
  uint8_t outBuf[1 + 3 * 6];

  while (true) {
    while (Serial2.available()) {
      uint8_t b = Serial2.read();

      // Look for frame header: AA FF 03 00
      if (!inFrame) {
        // Shift bytes looking for header
        for (int i = 0; i < 3; i++) frameBuf[i] = frameBuf[i+1];
        frameBuf[3] = b;
        if (frameBuf[0] == 0xAA && frameBuf[1] == 0xFF &&
            frameBuf[2] == 0x03 && frameBuf[3] == 0x00) {
          inFrame  = true;
          framePos = 4;
        }
      } else {
        frameBuf[framePos++] = b;
        if (framePos == 30) {
          inFrame = false;
          framePos = 0;
          // Verify footer
          if (frameBuf[28] == 0x55 && frameBuf[29] == 0xCC) {
            int count = 0;
            bool anyTarget = false;
            outBuf[0] = 0; // placeholder for count

            for (int t = 0; t < 3; t++) {
              int base = 4 + t * 6;
              int16_t x     = (int16_t)(frameBuf[base]     | (frameBuf[base+1] << 8));
              int16_t y     = (int16_t)(frameBuf[base+2]   | (frameBuf[base+3] << 8));
              int16_t speed = (int16_t)(frameBuf[base+4]   | (frameBuf[base+5] << 8));

              // LD2450 encodes sign in MSB differently — bit 15 is sign for X
              // X: bit15=1 means negative (left), clear it to get magnitude
              // Y: always positive distance
              if (x & 0x8000) { x = -(x & 0x7FFF); }
              // Y just needs the magnitude (already positive)
              y = y & 0x7FFF;

              if (x == 0 && y == 0) continue; // no target in this slot

              // Store in volatile struct for other tasks
              portENTER_CRITICAL(&radarMux);
              radarTargets[count].x     = x;
              radarTargets[count].y     = y;
              radarTargets[count].speed = speed;
              radarTargets[count].valid = true;
              portEXIT_CRITICAL(&radarMux);

              // Pack into output buffer
              int outBase = 1 + count * 6;
              outBuf[outBase]     = x & 0xFF;
              outBuf[outBase + 1] = (x >> 8) & 0xFF;
              outBuf[outBase + 2] = y & 0xFF;
              outBuf[outBase + 3] = (y >> 8) & 0xFF;
              outBuf[outBase + 4] = speed & 0xFF;
              outBuf[outBase + 5] = (speed >> 8) & 0xFF;

              count++;
              anyTarget = true;
            }

            // Clear remaining slots
            for (int t = count; t < 3; t++) radarTargets[t].valid = false;
            radarTargetCount = count;
            outBuf[0] = (uint8_t)count;

            // Trigger "scan" sound if new targets detected (with cooldown)
            unsigned long now = millis();
            if (anyTarget && !targetPresent) {
              if (now - lastTargetSoundMs > TARGET_SOUND_COOLDOWN_MS) {
                stopAudio = true;
                pendingSound = "scan";
                lastTargetSoundMs = now;
              }
            }
            targetPresent = anyTarget;

            // Send to DRADIS WebSocket client
            if (radarWsActive && radarWsFd >= 0) {
              httpd_ws_frame_t ws_pkt;
              memset(&ws_pkt, 0, sizeof(ws_pkt));
              ws_pkt.payload = outBuf;
              ws_pkt.len     = 1 + count * 6;
              ws_pkt.type    = HTTPD_WS_TYPE_BINARY;
              if (httpd_ws_send_frame_async(httpsServer, radarWsFd, &ws_pkt) != ESP_OK) {
                radarWsActive = false; radarWsFd = -1;
                Serial.println("DRADIS client disconnected");
              }
            }
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/* ================= LED TASK ================= */
void ledTask(void *pvParameters) {
  while (true) {
    if (!ledEnabled) {
      strip.clear();
      strip.show();
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    for (int pos = 0; pos < LED_COUNT; pos++) {
      if (!ledEnabled) break;
      drawCylonEye(pos, 255, trailLength, eyeBoostFactor, 1);
      strip.show();
      vTaskDelay(pdMS_TO_TICKS(dynamicDelay(pos)));
    }
    vTaskDelay(pdMS_TO_TICKS(edgePauseDelay));
    for (int pos = LED_COUNT - 2; pos > 0; pos--) {
      if (!ledEnabled) break;
      drawCylonEye(pos, 255, trailLength, eyeBoostFactor, -1);
      strip.show();
      vTaskDelay(pdMS_TO_TICKS(dynamicDelay(pos)));
    }
    vTaskDelay(pdMS_TO_TICKS(edgePauseDelay));
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.setBrightness(stripBrightness);
  strip.clear();
  strip.show();
  updateDecayLUT();

  prefs.begin("cylon", false);
  minDelay        = prefs.getInt("mindelay",   (int)minDelay);
  maxDelay        = prefs.getInt("maxdelay",   (int)maxDelay);
  stripBrightness = prefs.getInt("brightness", (int)stripBrightness);
  audioVolume     = prefs.getInt("volume",     (int)audioVolume);
  micGainShift    = prefs.getInt("micgain",    (int)micGainShift);
  strip.setBrightness(stripBrightness);

  if (!LittleFS.begin(true)) { Serial.println("LittleFS mount failed"); return; }
  Serial.println("LittleFS mounted");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("FILE: %s  SIZE: %d\n", file.name(), file.size());
    file = root.openNextFile();
  }

  setupI2S();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  if (MDNS.begin("cylon")) {
    Serial.println("mDNS: https://cylon.local");
    MDNS.addService("https", "tcp", 443);
  }

  startHTTPS();

  xTaskCreatePinnedToCore(ledTask,   "LEDTask",   4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(micTask,   "MicTask",   8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(radarTask, "RadarTask", 4096, NULL, 1, NULL, 0);
}

/* ================= LOOP ================= */
void loop() { vTaskDelay(portMAX_DELAY); }

/* ================= HELPERS ================= */
void updateDecayLUT() {
  float lut[16];
  for (int t = 1; t < 16; t++) lut[t] = powf(trailDecay, t);
  portENTER_CRITICAL(&ledMux);
  for (int t = 1; t < 16; t++) decayLUT[t] = lut[t];
  portEXIT_CRITICAL(&ledMux);
}

int dynamicDelay(int eyePos) {
  float mid  = (LED_COUNT - 1) / 2.0;
  float dist = abs(eyePos - mid) / mid;
  return (int)(minDelay + dist * dist * (maxDelay - minDelay));
}

void drawCylonEye(int eyePos, uint8_t redValue, int trailLen, float boost, int direction) {
  strip.clear();
  for (int t = 1; t <= trailLen; t++) {
    int idx = eyePos - t * direction;
    if (idx >= 0 && idx < LED_COUNT) {
      uint8_t br = (uint8_t)(redValue * decayLUT[t]);
      strip.setPixelColor(idx, br, 0, 0, 0);
    }
  }
  int eb = redValue * boost;
  if (eb > 255) eb = 255;
  strip.setPixelColor(eyePos, (uint8_t)eb, 0, 0, 0);
}
