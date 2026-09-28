#include <WiFi.h>
#include "ap_view.h"

static const char *AP_SSID = "meter-node";
static const char *AP_PASS = "meternode";      // WPA2 needs >= 8 characters
static const uint16_t DEFAULT_MINUTES = 20;

static WiFiServer server(80);
static bool active = false;
static uint32_t stopAt = 0;

static uint8_t *frameBuf = nullptr;
static size_t frameLen = 0;
static size_t frameCap = 0;
static char statusText[96] = "";
static portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;
static ApCommandHandler cmdHandler = nullptr;
static uint32_t framesPublished = 0, framesSent = 0, reqCount = 0;

void apViewSetCommandHandler(ApCommandHandler fn) { cmdHandler = fn; }

bool apViewActive() { return active; }

void apViewBegin(uint16_t minutes) {
    if (active) return;
    if (!minutes) minutes = DEFAULT_MINUTES;

    WiFi.mode(WIFI_AP);
    // Power save off: with it on, the AP misses association attempts and phones
    // silently fail to join or drop off after a few seconds. Costs a little
    // current, which is irrelevant for a viewfinder used minutes at a time.
    WiFi.setSleep(false);
    // Channel 6 and up to 4 stations. Leaving the channel to chance sometimes
    // lands on one the phone is already busy with on another band.
    WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 4);
    server.begin();
    server.setNoDelay(true);
    active = true;
    stopAt = millis() + (uint32_t)minutes * 60000UL;

    Serial.printf("[ap] up: SSID \"%s\" pass \"%s\"  http://%s/  (%u min)\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str(), minutes);
}

void apViewEnd() {
    if (!active) return;
    server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    active = false;
    Serial.println("[ap] down");
}

void apViewPublish(const uint8_t *jpeg, size_t len, const char *status) {
    if (!active || !jpeg || !len) return;
    if (len > frameCap) {
        uint8_t *grown = (uint8_t *)ps_realloc(frameBuf, len);
        if (!grown) return;                 // keep the previous frame rather than drop the buffer
        frameBuf = grown;
        frameCap = len;
    }
    // A plain copy, not inside a critical section: this is ~10 kB and disabling
    // interrupts for that long upsets the WiFi driver. Only apViewTick reads
    // the buffer, and it runs on this same task.
    memcpy(frameBuf, jpeg, len);
    frameLen = len;
    framesPublished++;
    if (status) strncpy(statusText, status, sizeof(statusText) - 1);
}

// Kept deliberately small and dependency-free: this page is served to a phone
// with no internet, so nothing may be fetched from a CDN.
static const char PAGE[] PROGMEM =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Meter node</title><style>"
    "body{margin:0;background:#111;color:#eee;font:15px system-ui;text-align:center}"
    "#w{position:relative;display:inline-block;line-height:0;max-width:100%;"
    "touch-action:none;-webkit-user-select:none;user-select:none}"
    "img{width:100%;max-width:520px;display:block;pointer-events:none}"
    "#o{position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none}"
    "#o div{position:absolute;box-sizing:border-box}"
    ".bx{border:2px solid #3c96ff;background:rgba(60,150,255,.18)}"
    ".cl{border-left:1px dashed #3c96ff}"

    "#s{padding:6px;font:16px ui-monospace,monospace;white-space:pre-wrap}"
    "#q{padding:2px;font:13px ui-monospace,monospace;color:#8ab4f8}"
    "#b{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;padding:8px}"
    "button{padding:12px 15px;font-size:15px;border-radius:9px;border:1px solid #444;"
    "background:#222;color:#eee}button.p{background:#2a78d6;border-color:#2a78d6}"
    "</style>"
    "<div id=w><img id=i><div id=o></div></div>"
    "<div id=q>drag on the image to set the ROI</div>"
    "<div id=s>connecting…</div>"
    "<div id=b>"
    "<button id=dm>digits −</button><button id=dp>digits +</button>"
    "<button id=iv>invert</button>"
    "<button id=tm>tilt \u2212</button><button id=tp>tilt +</button>"
    "<button id=rt>rotate 90\u00b0</button>"
    "<button id=bm>dim</button><button id=bp>bright</button>"
    "<button class=p id=ap>apply ROI</button>"
    "<button id=cl>clear</button>"
    "</div>"
    "<script>"
    "var r=null,f=null,dg=3,iv=1,VB=0,NW=0,NH=0;"
    "var I=document.getElementById('i'),O=document.getElementById('o'),"
    "W=document.getElementById('w'),Q=document.getElementById('q');"
    // Chain the next request off the previous frame's load, so a slow link
    // slows the refresh rate instead of queueing requests the node cannot serve.
    "function nx(){I.src='/frame.jpg?'+Date.now()}"
    // Cache the frame size the first time one loads. Assigning src resets
    // naturalWidth to 0 until the next frame arrives, so reading it during a
    // drag intermittently yields 0 -- the scale then falls back to CSS pixels
    // and the ROI lands somewhere other than where the finger was. Pinning the
    // aspect ratio also stops the box collapsing between frames.
    "I.onload=function(){if(I.naturalWidth){NW=I.naturalWidth;NH=I.naturalHeight;"
    "if(!VB){VB=1;I.style.aspectRatio=NW+'/'+NH;d()}}setTimeout(nx,120)};"
    "I.onerror=function(){setTimeout(nx,600)};nx();"
    // Coordinates come from the image's own box, not the SVG matrix: an SVG root
    // with nothing painted in it is unreliable for hit-testing on mobile, which
    // is why the wrapper div takes the events and the SVG only draws.
    "function P(e){var t=(e.touches&&e.touches[0])||(e.changedTouches&&e.changedTouches[0])||e;"
    "var b=I.getBoundingClientRect();if(!b.width||!b.height)return null;"
    "if(!NW||!NH)return null;var nw=NW,nh=NH;"
    "var x=Math.round((t.clientX-b.left)*nw/b.width);"
    "var y=Math.round((t.clientY-b.top)*nh/b.height);"
    "x=Math.max(0,Math.min(nw-1,x));y=Math.max(0,Math.min(nh-1,y));return{x:x,y:y}}"
    // Positioned in percentages of the image box. An SVG viewBox is the usual
    // way to do this, but it silently mis-scales if the image dimensions are not
    // known when it is set -- and then the box you drag sits somewhere other
    // than the region actually being cropped. Percentages cannot drift.
    "function d(){if(!r){O.innerHTML='';"
    "Q.textContent='drag on the image to set the ROI';return}"
    "var nw=NW||1,nh=NH||1;"
    "var h='<div class=bx style=\"left:'+(100*r.x/nw)+'%;top:'+(100*r.y/nh)+"
    "'%;width:'+(100*r.w/nw)+'%;height:'+(100*r.h/nh)+'%\"></div>';"
    "for(var k=1;k<dg;k++){var cx=r.x+r.w*k/dg;"
    "h+='<div class=cl style=\"left:'+(100*cx/nw)+'%;top:'+(100*r.y/nh)+"
    "'%;height:'+(100*r.h/nh)+'%;width:0\"></div>'}"
    "O.innerHTML=h;"
    "Q.textContent='ROI '+r.w+'\u00d7'+r.h+' at '+r.x+','+r.y+'  \u00b7  '+dg+' digits'}"
    "function down(e){var p=P(e);if(!p){Q.textContent='no image size yet';return}"
    "f=p;r={x:p.x,y:p.y,w:0,h:0};d();if(e.cancelable)e.preventDefault()}"
    "function move(e){if(!f)return;var p=P(e);if(!p)return;"
    "r={x:Math.min(f.x,p.x),y:Math.min(f.y,p.y),"
    "w:Math.abs(p.x-f.x),h:Math.abs(p.y-f.y)};d();if(e.cancelable)e.preventDefault()}"
    "function up(){f=null}"
    "W.addEventListener('touchstart',down,{passive:false});"
    "W.addEventListener('touchmove',move,{passive:false});"
    "W.addEventListener('touchend',up);W.addEventListener('touchcancel',up);"
    "W.addEventListener('mousedown',down);W.addEventListener('mousemove',move);"
    "W.addEventListener('mouseup',up);"
    "function c(x){return fetch('/cmd?c='+encodeURIComponent(x)).catch(function(){})}"
    "document.getElementById('dm').onclick=function(){if(dg>1){dg--;d();c('SET digits '+dg)}};"
    "document.getElementById('dp').onclick=function(){if(dg<12){dg++;d();c('SET digits '+dg)}};"
    "document.getElementById('iv').onclick=function(){iv=1-iv;c('SET invert '+iv)};"
    // Tilt in tenths of a degree. 0.5 degree steps are about the finest worth
    // having: across a 480 px frame that is a four pixel shift at the edges.
    "var fd=0,rq=0,br=255;"
    "function setf(v){fd=Math.max(-450,Math.min(450,v));"
    "Q.textContent='tilt '+(fd/10).toFixed(1)+'\u00b0';c('SET fine '+fd)}"
    "document.getElementById('tm').onclick=function(){setf(fd-5)};"
    "document.getElementById('tp').onclick=function(){setf(fd+5)};"
    "document.getElementById('rt').onclick=function(){rq=(rq+90)%360;"
    "Q.textContent='rotate '+rq+'\u00b0 \u2014 redraw the ROI';r=null;d();"
    "c('SET rotate '+rq).then(function(){return c('SET roi_w 0')})"
    ".then(function(){return c('SET roi_h 0')})};"
    "function setb(v){br=Math.max(0,Math.min(255,v));"
    "Q.textContent='lamp '+br;c('SET bright '+br)}"
    "document.getElementById('bm').onclick=function(){setb(br-25)};"
    "document.getElementById('bp').onclick=function(){setb(br+25)};"
    "document.getElementById('cl').onclick=function(){r=null;d();"
    "c('SET roi_w 0');c('SET roi_h 0')};"
    "document.getElementById('ap').onclick=function(){if(!r){Q.textContent='draw a box first';return}"
    "Q.textContent='applying…';"
    "c('SET roi_x '+r.x).then(function(){return c('SET roi_y '+r.y)})"
    ".then(function(){return c('SET roi_w '+r.w)})"
    ".then(function(){return c('SET roi_h '+r.h)})"
    ".then(function(){return c('SET digits '+dg)})"
    ".then(function(){Q.textContent='applied '+r.w+'×'+r.h})};"
    "setInterval(function(){fetch('/status').then(function(x){return x.text()})"
    ".then(function(t){document.getElementById('s').textContent=t}).catch(function(){})},1000);"
    "</script>";

// Minimal percent-decoding: commands arrive as query strings from the phone.
static String urlDecode(const String &in) {
    String out;
    for (size_t i = 0; i < in.length(); i++) {
        char ch = in[i];
        if (ch == '+') out += ' ';
        else if (ch == '%' && i + 2 < in.length()) {
            out += (char)strtol(in.substring(i + 1, i + 3).c_str(), nullptr, 16);
            i += 2;
        } else out += ch;
    }
    return out;
}

static void serveOnce(WiFiClient &c) {
    String req = c.readStringUntil('\r');
    while (c.available()) c.read();
    // Log the request line: without it, "the page did not load" and "the page
    // was never asked for" look identical from here.
    Serial.printf("[ap] req: %s\n", req.c_str());

    if (req.indexOf("GET /frame.jpg") >= 0) {
        // One JPEG per request, pulled by the page on a timer.
        //
        // A persistent multipart stream is more efficient but depends on the
        // WiFiClient outliving the request handler, which it did not: the
        // connection was dead by the next tick and no frame was ever written.
        // Polling costs a connection per frame and is completely reliable, which
        // is the better trade for something used at the meter.
        if (!frameLen) {
            c.print(F("HTTP/1.1 503 Service Unavailable\r\n"
                      "Content-Length: 0\r\nConnection: close\r\n\r\n"));
            c.stop();
            return;
        }
        c.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                 "Content-Length: %u\r\nCache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n", (unsigned)frameLen);
        c.write(frameBuf, frameLen);
        framesSent++;
        c.stop();
        return;
    }
    if (req.indexOf("GET /cmd?c=") >= 0) {
        int a = req.indexOf("c=") + 2;
        int b = req.indexOf(' ', a);
        String line = urlDecode(req.substring(a, b < 0 ? req.length() : b));
        if (cmdHandler && line.length()) cmdHandler(line);
        c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                  "Connection: close\r\n\r\n"));
        c.print(F("ok"));
        c.stop();
        return;
    }
    if (req.indexOf("GET /status") >= 0) {
        c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n"));
        c.print(statusText);
        c.stop();
        return;
    }
    // no-store matters here: without it a phone happily re-serves the previous
    // page from cache, so a firmware fix looks like it changed nothing.
    c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
              "Cache-Control: no-store, no-cache, must-revalidate\r\n"
              "Pragma: no-cache\r\nConnection: close\r\n\r\n"));
    c.print(FPSTR(PAGE));
    c.stop();
}

void apViewTick() {
    if (!active) return;

    if ((int32_t)(millis() - stopAt) >= 0) {
        Serial.println("[ap] timeout");
        apViewEnd();
        return;
    }

    WiFiClient c = server.available();
    if (c) { reqCount++; serveOnce(c); }

    static int lastStations = -1;
    int stations = WiFi.softAPgetStationNum();
    if (stations != lastStations) {
        lastStations = stations;
        Serial.printf("[ap] stations now %d\n", stations);
    }

    static uint32_t lastLog = 0;
    if (millis() - lastLog > 5000) {
        lastLog = millis();
        Serial.printf("[ap] clients=%d requests=%lu published=%lu sent=%lu "
                      "frame=%u\n",
                      WiFi.softAPgetStationNum(), (unsigned long)reqCount,
                      (unsigned long)framesPublished, (unsigned long)framesSent,
                      (unsigned)frameLen);
    }

}
