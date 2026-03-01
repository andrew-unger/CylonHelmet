#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include <esp_https_server.h>
#include <math.h>

/* ================= USER CONFIG ================= */
#define LED_PIN          4
#define LED_COUNT        32
#define COLOR_ORDER      NEO_GRBW

const char* ssid     = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

#define I2S_BCK   5
#define I2S_LRCK  6
#define I2S_DOUT  7
/* ============================================== */

// LED runtime values
int minDelay         = 15;
int maxDelay         = 135;
int stripBrightness  = 180;
int trailLength      = 7;
float trailDecay     = 0.3;
float eyeBoostFactor = 1.7;
int edgePauseDelay   = 10;

// Audio
volatile int  audioVolume  = 2000;
volatile bool playTone     = false;
volatile bool stopAudio    = false;
String        pendingSound = "";

// Cert storage
static std::string certPem;
static std::string keyPem;

httpd_handle_t httpsServer = nullptr;

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
    hr { border-color: #ff3300; opacity: 0.3; margin: 30px 0; }
    .status { font-size: 0.9em; color: #ff6644; margin-top: 8px; }
    .fx-control { margin: 10px auto; max-width: 400px; }
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
  <div class="status" id="micStatus">Voice inactive
  </div>

  <script>
    window.onload = function() {
      document.getElementById('bval').innerText  = document.getElementById('brightness').value;
      document.getElementById('mnval').innerText = document.getElementById('mindelay').value;
      document.getElementById('mxval').innerText = document.getElementById('maxdelay').value;
      document.getElementById('vval').innerText  = document.getElementById('volume').value;
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
      fetch(`/sound?name=${name}`)
        .then(r => r.text()).then(t => console.log(t));
    }

    function stopSound() {
      fetch('/stop')
        .then(r => r.text()).then(t => console.log(t));
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
          audio: {
            noiseSuppression: true,
            echoCancellation: true,
            autoGainControl: true
          },
          video: false
        });

        audioCtx = new AudioContext();
        const source = audioCtx.createMediaStreamSource(micStream);

        // Highpass - remove low rumble
        highpass = audioCtx.createBiquadFilter();
        highpass.type = 'highpass';
        highpass.frequency.value = parseFloat(document.getElementById('hpFreq').value);
        highpass.Q.value = 0.5;

        // Peak 1 - nasal midrange boost
        peak = audioCtx.createBiquadFilter();
        peak.type = 'peaking';
        peak.frequency.value = parseFloat(document.getElementById('peakFreq').value);
        peak.gain.value      = parseFloat(document.getElementById('peakGain').value);
        peak.Q.value = 2.5;

        // Peak 2 - telephone/radio quality boost
        peak2 = audioCtx.createBiquadFilter();
        peak2.type = 'peaking';
        peak2.frequency.value = parseFloat(document.getElementById('peak2Freq').value);
        peak2.gain.value      = parseFloat(document.getElementById('peak2Gain').value);
        peak2.Q.value = 2.0;

        // Ring modulator
        carrier = audioCtx.createOscillator();
        carrier.type = 'sawtooth';
        carrier.frequency.value = parseFloat(document.getElementById('ringFreq').value);
        const ringMod = audioCtx.createGain();
        ringMod.gain.value = 1.0;
        carrier.connect(ringMod.gain);

        // Distortion
        distortion = audioCtx.createWaveShaper();
        distortion.curve = makeDistortionCurve(parseFloat(document.getElementById('distAmount').value));
        distortion.oversample = '4x';

        // Lowpass - cut harsh highs
        lowpass = audioCtx.createBiquadFilter();
        lowpass.type = 'lowpass';
        lowpass.frequency.value = parseFloat(document.getElementById('lpFreq').value);
        lowpass.Q.value = 1.0;

        // Final bandpass - tighten the sound
        const finalBand = audioCtx.createBiquadFilter();
        finalBand.type = 'bandpass';
        finalBand.frequency.value = 2500;
        finalBand.Q.value = 1.5;

        // Chain: source → highpass → peak → peak2 → ringMod → distortion → lowpass → finalBand → output
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
            const int16 = new Int16Array(float32.length);
            for (let i = 0; i < float32.length; i++) {
              int16[i] = Math.max(-32768, Math.min(32767, float32[i] * 32767));
            }
            ws.send(int16.buffer);
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
  </script>
</body>
</html>
)rawliteral";

/* ================= WAV PLAYER ================= */
void playWavFile(const char* filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) {
    Serial.printf("Failed to open %s\n", filename);
    return;
  }
  file.seek(44);
  const int bufSize = 512;
  int16_t stereo[bufSize * 2];
  uint8_t raw[bufSize * 2];
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
    size_t bytesWritten;
    i2s_write(I2S_NUM_0, stereo, samples * 4, &bytesWritten, portMAX_DELAY);
  }
  file.close();
  int16_t silence[64] = {0};
  size_t bytesWritten;
  i2s_write(I2S_NUM_0, silence, sizeof(silence), &bytesWritten, portMAX_DELAY);
}

/* ================= I2S SETUP ================= */
void setupI2S() {
  i2s_config_t i2s_config = {
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
  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCK,
    .ws_io_num    = I2S_LRCK,
    .data_out_num = I2S_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

/* ================= HTTP HANDLERS ================= */
static std::string buildPage() {
  std::string html(index_html);
  auto replace = [&](const std::string& from, const std::string& to) {
    size_t pos = html.find(from);
    if (pos != std::string::npos) html.replace(pos, from.size(), to);
  };
  replace("value=\"2000\" id=\"volume\"",    "value=\"" + std::to_string(audioVolume)     + "\" id=\"volume\"");
  replace("value=\"180\" id=\"brightness\"", "value=\"" + std::to_string(stripBrightness) + "\" id=\"brightness\"");
  replace("value=\"15\" id=\"mindelay\"",    "value=\"" + std::to_string(minDelay)        + "\" id=\"mindelay\"");
  replace("value=\"135\" id=\"maxdelay\"",   "value=\"" + std::to_string(maxDelay)        + "\" id=\"maxdelay\"");
  return html;
}

static esp_err_t rootHandler(httpd_req_t *req) {
  std::string page = buildPage();
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, page.c_str(), page.size());
  return ESP_OK;
}

static esp_err_t setHandler(httpd_req_t *req) {
  char buf[200];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char val[32];
    if (httpd_query_key_value(buf, "brightness", val, sizeof(val)) == ESP_OK) {
      stripBrightness = atoi(val);
      strip.setBrightness(stripBrightness);
    }
    if (httpd_query_key_value(buf, "mindelay", val, sizeof(val)) == ESP_OK)
      minDelay = atoi(val);
    if (httpd_query_key_value(buf, "maxdelay", val, sizeof(val)) == ESP_OK)
      maxDelay = atoi(val);
    if (httpd_query_key_value(buf, "volume", val, sizeof(val)) == ESP_OK)
      audioVolume = atoi(val);
  }
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t soundHandler(httpd_req_t *req) {
  char buf[100];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char name[32];
    if (httpd_query_key_value(buf, "name", name, sizeof(name)) == ESP_OK) {
      stopAudio = true;
      pendingSound = String(name);
    }
  }
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t stopHandler(httpd_req_t *req) {
  stopAudio    = true;
  pendingSound = "";
  playTone     = false;
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t wsAudioHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    Serial.println("WebSocket handshake");
    return ESP_OK;
  }
  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = nullptr;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;

  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) return ret;

  if (ws_pkt.len) {
    buf = (uint8_t*)malloc(ws_pkt.len);
    if (!buf) return ESP_ERR_NO_MEM;
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret == ESP_OK) {
      int samples = ws_pkt.len / 2;
      int16_t* src = (int16_t*)buf;
      int16_t stereo[samples * 2];
      for (int i = 0; i < samples; i++) {
        int16_t scaled = (int16_t)((src[i] * audioVolume) / 32767);
        stereo[i*2]   = scaled;
        stereo[i*2+1] = scaled;
      }
      size_t bytesWritten;
      i2s_write(I2S_NUM_0, stereo, samples * 4, &bytesWritten, portMAX_DELAY);
    }
    free(buf);
  }
  return ret;
}

/* ================= HTTPS SERVER ================= */
void startHTTPS() {
  File certFile = LittleFS.open("/cert.pem", "r");
  File keyFile  = LittleFS.open("/key.pem", "r");
  if (!certFile || !keyFile) {
    Serial.println("Failed to open cert files!");
    return;
  }
  certPem = std::string(certFile.readString().c_str());
  keyPem  = std::string(keyFile.readString().c_str());
  certFile.close();
  keyFile.close();
  Serial.println("Certs loaded");

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert     = (const uint8_t*)certPem.c_str();
  conf.servercert_len = certPem.size() + 1;
  conf.prvtkey_pem    = (const uint8_t*)keyPem.c_str();
  conf.prvtkey_len    = keyPem.size() + 1;
  conf.httpd.max_uri_handlers = 10;
  conf.httpd.stack_size = 10240;

  esp_err_t ret = httpd_ssl_start(&httpsServer, &conf);
  if (ret != ESP_OK) {
    Serial.printf("HTTPS start failed: %d\n", ret);
    return;
  }
  Serial.println("HTTPS server started");

  httpd_uri_t uriRoot  = { "/",      HTTP_GET, rootHandler,    nullptr };
  httpd_uri_t uriSet   = { "/set",   HTTP_GET, setHandler,     nullptr };
  httpd_uri_t uriSound = { "/sound", HTTP_GET, soundHandler,   nullptr };
  httpd_uri_t uriStop  = { "/stop",  HTTP_GET, stopHandler,    nullptr };
  httpd_uri_t uriWs    = { "/audio", HTTP_GET, wsAudioHandler, nullptr, true };

  httpd_register_uri_handler(httpsServer, &uriRoot);
  httpd_register_uri_handler(httpsServer, &uriSet);
  httpd_register_uri_handler(httpsServer, &uriSound);
  httpd_register_uri_handler(httpsServer, &uriStop);
  httpd_register_uri_handler(httpsServer, &uriWs);
}

/* ================= AUDIO TASK (Core 1) ================= */
void audioTask(void *pvParameters) {
  while (true) {
    if (pendingSound != "") {
      String sound = pendingSound;
      pendingSound = "";
      String path = "/" + sound + ".wav";
      playWavFile(path.c_str());
    } else if (playTone) {
      static float phase = 0;
      float phaseIncrement = 2.0f * M_PI * 440.0f / 44100.0f;
      int16_t buffer[64];
      for (int i = 0; i < 64; i++) {
        int16_t sample = (int16_t)(sinf(phase) * audioVolume);
        buffer[i] = sample;
        phase += phaseIncrement;
        if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
      }
      size_t bytesWritten;
      i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytesWritten, portMAX_DELAY);
    } else {
      int16_t silence[64] = {0};
      size_t bytesWritten;
      i2s_write(I2S_NUM_0, silence, sizeof(silence), &bytesWritten, portMAX_DELAY);
      vTaskDelay(1);
    }
  }
}

/* ================= LED TASK (Core 0) ================= */
void ledTask(void *pvParameters) {
  while (true) {
    for (int pos = 0; pos < LED_COUNT; pos++) {
      drawCylonEye(pos, 255, trailLength, trailDecay, eyeBoostFactor, 1);
      strip.show();
      vTaskDelay(pdMS_TO_TICKS(dynamicDelay(pos)));
    }
    vTaskDelay(pdMS_TO_TICKS(edgePauseDelay));
    for (int pos = LED_COUNT - 2; pos > 0; pos--) {
      drawCylonEye(pos, 255, trailLength, trailDecay, eyeBoostFactor, -1);
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

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }
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
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("cylon")) {
    Serial.println("mDNS started - https://cylon.local");
    MDNS.addService("https", "tcp", 443);
  }

  startHTTPS();

  xTaskCreatePinnedToCore(ledTask,   "LEDTask",   4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 1, NULL, 1);
}

/* ================= LOOP ================= */
void loop() {
  vTaskDelay(portMAX_DELAY);
}

/* ================= FUNCTIONS ================= */
int dynamicDelay(int eyePos) {
  float midpoint = (LED_COUNT - 1) / 2.0;
  float distanceFromCenter = abs(eyePos - midpoint) / midpoint;
  float delayValue = minDelay + distanceFromCenter * distanceFromCenter * (maxDelay - minDelay);
  return (int)delayValue;
}

void drawCylonEye(int eyePos, uint8_t redValue, int trailLen, float decay,
                  float boost, int direction) {
  strip.clear();
  for (int t = 1; t <= trailLen; t++) {
    int ledIndex = eyePos - t * direction;
    if (ledIndex >= 0 && ledIndex < LED_COUNT) {
      float factor = pow(decay, t);
      uint8_t brightness = redValue * factor;
      strip.setPixelColor(ledIndex, brightness, 0, 0, 0);
    }
  }
  int eyeBrightness = redValue * boost;
  if (eyeBrightness > 255) eyeBrightness = 255;
  strip.setPixelColor(eyePos, eyeBrightness, 0, 0, 0);
}