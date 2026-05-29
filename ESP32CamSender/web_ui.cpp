#include "web_ui.h"
#include "config.h"
#include "camera_io.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer web_server(80);

static void appendJsonString(String& json, const char* value) {
  json += "\"";
  for (const char* c = value; *c; ++c) {
    if (*c == '"' || *c == '\\') { json += '\\'; json += *c; }
    else if (*c == '\n') json += "\\n";
    else if (*c == '\r') json += "\\r";
    else json += *c;
  }
  json += "\"";
}

static void handleSnapshot() {
  CamUiStatus s = getCamUiStatus();
  if (!s.camera_on) {
    web_server.send(409, "text/plain", "Camera idle");
    return;
  }
  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!captureFrameJpeg(&jpg, &jpg_len, kSnapshotJpegQuality, /*draw_box=*/true, /*crop_roi=*/false)) {
    web_server.send(500, "text/plain", "Snapshot failed");
    return;
  }
  web_server.sendHeader("Cache-Control", "no-store");
  web_server.setContentLength(jpg_len);
  web_server.send(200, "image/jpeg", "");
  web_server.client().write(jpg, jpg_len);
  free(jpg);
}

void startWebServer() {
  web_server.on("/", []() {
    const char* html = R"raw(
<!DOCTYPE html><html><head><title>FruitCam (Gemini)</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:Arial;background:#101820;color:#fff;text-align:center;padding:18px;margin:0;}
img{max-width:520px;width:100%;background:#000;border:1px solid #3f5566;border-radius:8px;}
button{font-size:16px;padding:10px 14px;margin:5px;border:0;border-radius:8px;color:#fff;}
.on{background:#2e7d32;}.off{background:#b71c1c;}.test{background:#1565c0;}
.panel{background:#1d2a35;border-radius:10px;padding:16px;max-width:600px;margin:0 auto;}
.label{font-size:30px;font-weight:700;margin:10px 0;}
.meta{color:#b8c1cc;font-size:14px;}
.mon{margin-top:14px;background:#0b1117;border:1px solid #31475a;border-radius:8px;padding:12px;text-align:left;font-family:monospace;font-size:13px;color:#eef6ff;}
</style></head><body>
<div class="panel"><h2>FruitCam (Gemini bridge)</h2>
<img id="p" src="/snapshot.jpg">
<div class="label" id="label">Idle</div>
<div class="meta" id="meta"></div>
<div><button class="on" onclick="cmd('/preview/start')">Preview ON</button>
<button class="off" onclick="cmd('/preview/stop')">Preview OFF</button>
<button class="test" onclick="cmd('/detect/start')">Detect Test</button>
<button class="off" onclick="cmd('/detect/stop')">Stop</button></div>
<div class="mon" id="mon">-</div></div>
<script>
async function cmd(p){try{await fetch(p+'?ts='+Date.now());}catch(e){}refresh();}
async function refresh(){try{
const r=await fetch('/status?ts='+Date.now());const s=await r.json();
document.getElementById('label').textContent=s.label;
document.getElementById('meta').textContent=(s.active?'Detecting':'Idle')+' | Preview '+(s.preview?'ON':'OFF')+' | IP '+s.ip;
document.getElementById('mon').textContent=s.monitor;
const img=document.getElementById('p');
if(s.cameraOn){img.style.display='block';img.src='/snapshot.jpg?ts='+Date.now();}else{img.style.display='none';}
}catch(e){}}
setInterval(refresh,900);refresh();
</script></body></html>
)raw";
    web_server.send(200, "text/html", html);
  });

  web_server.on("/status", []() {
    CamUiStatus s = getCamUiStatus();
    String j = "{";
    j += "\"label\":"; appendJsonString(j, s.label);
    j += ",\"confidence\":"; j += String(s.confidence, 2);
    j += ",\"monitor\":"; appendJsonString(j, s.monitor);
    j += ",\"sequence\":"; j += String(s.sequence);
    j += ",\"uptime_ms\":"; j += String(s.uptime_ms);
    j += ",\"active\":"; j += s.active ? "true" : "false";
    j += ",\"preview\":"; j += s.preview ? "true" : "false";
    j += ",\"cameraOn\":"; j += s.camera_on ? "true" : "false";
    j += ",\"ip\":"; appendJsonString(j, s.ip);
    j += "}";
    web_server.send(200, "application/json", j);
  });

  web_server.on("/snapshot.jpg", handleSnapshot);
  web_server.on("/preview/start", []() { onWebPreviewStart(); web_server.send(200, "text/plain", "ok"); });
  web_server.on("/preview/stop",  []() { onWebPreviewStop();  web_server.send(200, "text/plain", "ok"); });
  web_server.on("/detect/start",  []() { onWebDetectStart();  web_server.send(200, "text/plain", "ok"); });
  web_server.on("/detect/stop",   []() { onWebDetectStop();   web_server.send(200, "text/plain", "ok"); });

  web_server.begin();
  Serial.println("Web server started");
}

void handleWebClient() {
  web_server.handleClient();
}
