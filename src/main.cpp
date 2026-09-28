// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>

#include "Config.h"
#include "Version.h"

namespace {

constexpr size_t MAX_PROFILE_POINTS = 24;
constexpr size_t MAX_SAVED_PROFILES = 8;
constexpr time_t VALID_EPOCH = 1700000000;
constexpr const char* RELEASE_API_URL = "https://api.github.com/repos/opencreationsdk/openkiln/releases/latest";
constexpr uint32_t UPDATE_CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;

struct ProfilePoint {
  float minute;
  float temperatureC;
};

enum class RunState { IDLE, RUNNING, PAUSED, COMPLETE, FAULT };

SPIClass thermoSpi(VSPI);
WebServer server(80);
Preferences preferences;
WiFiClient mqttNetwork;
PubSubClient mqttClient(mqttNetwork);

ProfilePoint profile[MAX_PROFILE_POINTS];
size_t profileCount = 0;
String activeProfileName = "Default";
RunState runState = RunState::IDLE;

float currentTemperatureC = NAN;
float targetTemperatureC = 0.0f;
float outputDuty = 0.0f;
float integralTerm = 0.0f;
float previousError = 0.0f;
float heatingRateCPerHour = 0.0f;
float previousRateTemperature = NAN;
uint32_t previousRateMs = 0;

double profileElapsedSeconds = 0.0;
uint32_t lastSensorReadMs = 0;
uint32_t lastGoodSensorMs = 0;
uint32_t lastControlMs = 0;
uint32_t ssrWindowStartMs = 0;
uint32_t lastCheckpointMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t lastMqttPublishMs = 0;
bool sensorHealthy = false;
String faultMessage;


struct RuntimeSettings {
  String wifiSsid, wifiPassword, apName, apPassword, webUser, webPassword;
  bool mqttEnabled, mqttControl, mqttStart;
  String mqttHost, mqttUser, mqttPassword, mqttTopic;
  uint16_t mqttPort;
  bool influxEnabled; String influxUrl, influxOrg, influxBucket, influxToken; uint32_t influxInterval;
  bool pushoverEnabled; String pushoverToken, pushoverUser;
  String ntpServer, timezone, hostname, language;
} settings;
uint32_t lastInfluxMs=0;
uint32_t lastUpdateCheckMs=0;
bool firstUpdateCheckDone=false;
String latestFirmwareVersion;
String latestFirmwareUrl;
String latestFirmwarePage;
String updateCheckError;
bool firmwareUpdateAvailable=false;
volatile bool firmwareInstallRunning=false;
volatile int firmwareInstallProgress=0;
String firmwareInstallStage="idle";
String firmwareInstallMessage;

String prefStr(const char* key,const char* fallback="") { return preferences.getString(key,fallback); }
void loadSettings(){
  settings.wifiSsid=prefStr("wifiSsid",WIFI_SSID); settings.wifiPassword=prefStr("wifiPass",WIFI_PASSWORD);
  settings.apName=prefStr("apName",AP_NAME); settings.apPassword=prefStr("apPass",AP_PASSWORD);
  settings.webUser=prefStr("webUser",WEB_USER); settings.webPassword=prefStr("webPass",WEB_PASSWORD);
  settings.mqttEnabled=preferences.getBool("mqttEn",strlen(MQTT_HOST)>0); settings.mqttHost=prefStr("mqttHost",MQTT_HOST);
  settings.mqttPort=preferences.getUShort("mqttPort",MQTT_PORT); settings.mqttUser=prefStr("mqttUser",MQTT_USER); settings.mqttPassword=prefStr("mqttPass",MQTT_PASSWORD);
  settings.mqttTopic=prefStr("mqttTopic",MQTT_TOPIC); settings.mqttControl=preferences.getBool("mqttCtl",MQTT_ALLOW_CONTROL); settings.mqttStart=preferences.getBool("mqttStart",MQTT_ALLOW_REMOTE_START);
  settings.influxEnabled=preferences.getBool("influxEn",false); settings.influxUrl=prefStr("influxUrl",""); settings.influxOrg=prefStr("influxOrg",""); settings.influxBucket=prefStr("influxBucket",""); settings.influxToken=prefStr("influxToken",""); settings.influxInterval=preferences.getUInt("influxInt",30);
  settings.pushoverEnabled=preferences.getBool("pushEn",false); settings.pushoverToken=prefStr("pushToken",""); settings.pushoverUser=prefStr("pushUser","");
  settings.ntpServer=prefStr("ntp","pool.ntp.org"); settings.timezone=prefStr("tz","CET-1CEST,M3.5.0,M10.5.0/3");
  settings.hostname=prefStr("hostname","kiln"); if(settings.hostname.isEmpty()) settings.hostname="kiln";
  settings.language=prefStr("language","en"); if(settings.language!="da") settings.language="en";
}
String urlEncode(const String& v){String o; char b[4]; for(char c:v){if(isalnum((unsigned char)c)||c=='-'||c=='_'||c=='.'||c=='~')o+=c;else{snprintf(b,sizeof(b),"%%%02X",(unsigned char)c);o+=b;}}return o;}
void sendPushover(const String& title,const String& message){
  if(!settings.pushoverEnabled||WiFi.status()!=WL_CONNECTED||settings.pushoverToken.isEmpty()||settings.pushoverUser.isEmpty())return;
  WiFiClientSecure net; net.setInsecure(); HTTPClient http; if(!http.begin(net,"https://api.pushover.net/1/messages.json"))return;
  http.addHeader("Content-Type","application/x-www-form-urlencoded");
  String body="token="+urlEncode(settings.pushoverToken)+"&user="+urlEncode(settings.pushoverUser)+"&title="+urlEncode(title)+"&message="+urlEncode(message);
  int code=http.POST(body); Serial.printf("Pushover HTTP %d\n",code); http.end();
}
void writeInflux(){
  if(!settings.influxEnabled||WiFi.status()!=WL_CONNECTED||settings.influxUrl.isEmpty()||settings.influxOrg.isEmpty()||settings.influxBucket.isEmpty()||settings.influxToken.isEmpty())return;
  String url=settings.influxUrl+"/api/v2/write?org="+urlEncode(settings.influxOrg)+"&bucket="+urlEncode(settings.influxBucket)+"&precision=s";
  HTTPClient http; if(!http.begin(url))return; http.addHeader("Authorization","Token "+settings.influxToken); http.addHeader("Content-Type","text/plain");
  String line="kiln,profile="+activeProfileName; line.replace(" ","\\ "); line+=" temperature="+(isfinite(currentTemperatureC)?String(currentTemperatureC,2):String("0"))+",target="+String(targetTemperatureC,2)+",duty="+String(outputDuty*100.0f,1)+",elapsed_minutes="+String(profileElapsedSeconds/60.0,2)+",sensor_ok="+String(sensorHealthy?"true":"false");
  int code=http.POST(line); Serial.printf("Influx HTTP %d\n",code); http.end();
}

void forceHeatOff();

String jsonStringField(const String& json, const String& key, int start=0){
  String needle = "\"" + key + "\"";
  int k = json.indexOf(needle, start);
  if(k < 0) return "";
  int colon = json.indexOf(':', k + needle.length());
  if(colon < 0) return "";
  int q1 = json.indexOf('\"', colon + 1);
  if(q1 < 0) return "";
  int q2 = q1 + 1;
  while(true){
    q2 = json.indexOf('\"', q2);
    if(q2 < 0) return "";
    if(q2 == q1 + 1 || json[q2-1] != '\\') break;
    q2++;
  }
  return json.substring(q1 + 1, q2);
}
int compareVersions(String a,String b){
  a.trim();b.trim(); if(a.startsWith("v")||a.startsWith("V"))a.remove(0,1); if(b.startsWith("v")||b.startsWith("V"))b.remove(0,1);
  for(int part=0;part<4;part++){int da=a.indexOf('.'),db=b.indexOf('.');String sa=da<0?a:a.substring(0,da),sb=db<0?b:b.substring(0,db);long va=sa.toInt(),vb=sb.toInt();if(va<vb)return -1;if(va>vb)return 1;if(da<0)a="0";else a=a.substring(da+1);if(db<0)b="0";else b=b.substring(db+1);}return 0;
}
bool checkFirmwareUpdate(){
  if(WiFi.status()!=WL_CONNECTED){updateCheckError="No Internet connection";return false;}
  WiFiClientSecure net; net.setInsecure(); HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if(!http.begin(net,RELEASE_API_URL)){updateCheckError="Unable to contact update service";return false;}
  http.addHeader("User-Agent","OpenKiln/"+String(OPENKILN_VERSION)); http.addHeader("Accept","application/vnd.github+json");
  int code=http.GET(); if(code!=HTTP_CODE_OK){updateCheckError="Update service HTTP "+String(code);http.end();return false;}
  String body=http.getString(); http.end(); String tag=jsonStringField(body,"tag_name"); String page=jsonStringField(body,"html_url");
  int pos=0; String assetUrl; while(true){int n=body.indexOf("\"name\"",pos);if(n<0)break;String name=jsonStringField(body,"name",n);if(name=="firmware.bin"){assetUrl=jsonStringField(body,"browser_download_url",n);break;}pos=n+6;}
  if(tag.isEmpty()){updateCheckError="Latest release has no version tag";return false;} if(assetUrl.isEmpty()){updateCheckError="Latest release has no firmware.bin asset";latestFirmwareVersion=tag;firmwareUpdateAvailable=false;return false;}
  latestFirmwareVersion=tag; latestFirmwareUrl=assetUrl; latestFirmwarePage=page; firmwareUpdateAvailable=compareVersions(String(OPENKILN_VERSION),tag)<0; updateCheckError="";
  Serial.printf("Firmware check: installed %s, latest %s, update=%s\\n",OPENKILN_VERSION,tag.c_str(),firmwareUpdateAvailable?"yes":"no"); return true;
}
bool installFirmwareUpdateBlocking(String& error){
  if(runState==RunState::RUNNING||runState==RunState::PAUSED){error="Stop the kiln before installing firmware";return false;}
  if(!firmwareUpdateAvailable||latestFirmwareUrl.isEmpty()){error="No firmware update is available";return false;}
  if(WiFi.status()!=WL_CONNECTED){error="No Internet connection";return false;}
  forceHeatOff();
  firmwareInstallStage="downloading"; firmwareInstallMessage="Downloading firmware"; firmwareInstallProgress=1;
  WiFiClientSecure net; net.setInsecure(); HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if(!http.begin(net,latestFirmwareUrl)){error="Unable to open firmware download";return false;}
  http.addHeader("User-Agent","OpenKiln/"+String(OPENKILN_VERSION));
  int code=http.GET();
  if(code!=HTTP_CODE_OK){error="Firmware download HTTP "+String(code);http.end();return false;}
  int len=http.getSize();
  if(!Update.begin(len>0?(size_t)len:UPDATE_SIZE_UNKNOWN)){error="Not enough OTA space";http.end();return false;}
  WiFiClient* stream=http.getStreamPtr();
  uint8_t buf[1024]; size_t written=0;
  while(http.connected() && (len<0 || written<(size_t)len)){
    size_t avail=stream->available();
    if(avail){
      size_t want=avail>sizeof(buf)?sizeof(buf):avail;
      int got=stream->readBytes(buf,want);
      if(got<=0) break;
      size_t w=Update.write(buf,(size_t)got);
      if(w!=(size_t)got){error="Firmware write failed";Update.abort();http.end();return false;}
      written+=w;
      if(len>0){int pct=(int)((written*90ULL)/(size_t)len); if(pct<1)pct=1;if(pct>90)pct=90;firmwareInstallProgress=pct;}
    } else { delay(1); }
  }
  firmwareInstallStage="installing"; firmwareInstallMessage="Installing firmware"; firmwareInstallProgress=95;
  bool ok=Update.end(true); http.end();
  if(!ok){error="Firmware installation failed: "+String(Update.errorString());return false;}
  if(len>0&&written!=(size_t)len){error="Incomplete firmware download";return false;}
  firmwareInstallProgress=100; return true;
}
void firmwareUpdateTask(void*){
  String error;
  bool ok=installFirmwareUpdateBlocking(error);
  if(!ok){firmwareInstallStage="error";firmwareInstallMessage=error;firmwareInstallRunning=false;vTaskDelete(nullptr);return;}
  firmwareInstallStage="rebooting"; firmwareInstallMessage="Update installed. Rebooting OpenKiln"; firmwareInstallProgress=100;
  delay(1800); ESP.restart();
}


const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OpenKiln</title><style>:root{--background:#151515;--panel:#222321;--border:#454641;--text:#f2f1eb;--muted:#aaa99f;--profile:#8aa000;--danger:#b74a4a}*{box-sizing:border-box}body{margin:0;background:var(--background);color:var(--text);font-family:Arial,sans-serif}.wrapper{max-width:1500px;margin:auto;padding:25px}.header{display:flex;justify-content:space-between;gap:15px;align-items:end;margin-bottom:20px}.nav{display:flex;gap:8px;flex-wrap:wrap}.nav a,button{border:1px solid var(--border);border-radius:9px;padding:10px 14px;background:#30312e;color:var(--text);font-weight:700;text-decoration:none;cursor:pointer}.nav a.active,button.primary{background:var(--profile);border-color:var(--profile);color:#111}button.danger{background:#482828;border-color:#784040}button:disabled{opacity:.4}h1{margin:0;font-size:38px}.muted,.label{color:var(--muted)}.label{font-size:13px;margin-bottom:6px}.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-bottom:15px}.card,.panel{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:18px}.value{font-size:clamp(24px,3vw,40px);font-weight:bold}.unit{color:var(--muted);font-size:15px}.actions{display:flex;gap:9px;flex-wrap:wrap;margin-top:14px}.graph{height:500px;margin-top:15px;background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:12px}.graph canvas{width:100%;height:100%;display:block}.bar{height:10px;background:#171817;border-radius:8px;overflow:hidden;margin-top:8px}.fill{height:100%;background:var(--profile)}select,input{width:100%;background:#181918;color:var(--text);border:1px solid var(--border);border-radius:8px;padding:10px;font:inherit}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}.steps{width:100%;border-collapse:collapse}.steps th,.steps td{text-align:left;padding:6px}.steps th{color:var(--muted);font-size:12px}.notice{margin-top:10px;color:var(--muted);min-height:20px}.fault{color:#e27676}.section{margin-top:14px}.switch{width:auto}.statusgrid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.pill{padding:8px;border:1px solid var(--border);border-radius:8px;color:var(--muted)}details.advanced{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:0 18px 18px}details.advanced summary{cursor:pointer;font-weight:700;font-size:20px;padding:18px 0;list-style:none}details.advanced summary::-webkit-details-marker{display:none}details.advanced[open] summary{border-bottom:1px solid var(--border);margin-bottom:14px}@media(max-width:900px){.cards,.grid2,.statusgrid{grid-template-columns:1fr 1fr}.graph{height:390px}}@media(max-width:600px){.cards,.grid2,.statusgrid{grid-template-columns:1fr}.wrapper{padding:14px}.header{align-items:flex-start;flex-direction:column}}</style></head><body><div class="wrapper"><div class="header"><div><h1>OpenKiln</h1><div class="muted">Profil: <span id="activeProfile">--</span></div></div><div class="nav"><a class="active" href="/">Ovn</a><a href="/profiles">Profiler</a><a href="/setup">Setup</a></div></div>
<div class="cards"><div class="card"><div class="label">Status</div><div class="value" id="state">--</div></div><div class="card"><div class="label">Temperatur</div><span class="value" id="temp">--.-</span> <span class="unit">°C</span></div><div class="card"><div class="label">Target</div><span class="value" id="target">--.-</span> <span class="unit">°C</span></div><div class="card"><div class="label">Profil tid</div><span class="value" id="elapsed">0.0</span> <span class="unit">min</span></div></div>
<div class="panel"><div class="grid2"><div><div class="label">Vælg profil</div><select id="profileSelect" onchange="selectProfile(this.value)"></select></div><div><div class="label">SSR output: <span id="duty">0</span>%</div><div class="bar"><div class="fill" id="fill"></div></div></div></div><div class="actions"><button class="primary" onclick="cmd('start')">Start</button><button onclick="cmd('pause')">Pause</button><button onclick="cmd('resume')">Resume</button><button class="danger" onclick="cmd('stop')">STOP</button><button onclick="cmd('clear')">Clear fault</button></div><div class="notice fault" id="fault"></div></div>
<div class="panel section"><div class="actions"><button id="checkUpdateBtn" onclick="checkForUpdate()">Check for update</button></div><div id="updateCheckMsg" class="notice"></div></div><div id="updateBox" class="panel section" style="display:none"><h2 id="updateTitle">New firmware available</h2><div id="updateText" class="notice"></div><div class="actions"><button class="primary" onclick="installFirmwareUpdate()">Install update</button><button onclick="dismissUpdate()">Later</button></div></div><div class="graph"><canvas id="profileGraph"></canvas></div><div class="notice" id="clock"></div></div><script>
let graphData=[]; async function req(p,o){let r=await fetch(p,o),t=await r.text();if(!r.ok)throw Error(t);try{return JSON.parse(t)}catch{return t}} function form(d){return{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d)}}
function draw(){const c=profileGraph,r=c.getBoundingClientRect(),q=devicePixelRatio||1;c.width=r.width*q;c.height=r.height*q;let x=c.getContext('2d');x.setTransform(q,0,0,q,0,0);let w=r.width,h=r.height,p={l:58,r:18,t:18,b:40},d=graphData,maxM=Math.max(60,...d.map(v=>v[0])),maxT=Math.max(200,...d.map(v=>v[1])),ym=Math.ceil((maxT+50)/100)*100;x.clearRect(0,0,w,h);x.strokeStyle='rgba(255,255,255,.13)';x.fillStyle='#aaa99f';x.font='12px Arial';for(let i=0;i<=5;i++){let yy=p.t+(h-p.t-p.b)*i/5;x.beginPath();x.moveTo(p.l,yy);x.lineTo(w-p.r,yy);x.stroke();x.fillText(Math.round(ym*(1-i/5))+'°',8,yy+4)}for(let i=0;i<=6;i++){let xx=p.l+(w-p.l-p.r)*i/6;x.beginPath();x.moveTo(xx,p.t);x.lineTo(xx,h-p.b);x.stroke();x.fillText(Math.round(maxM*i/6)+' min',Math.max(p.l,xx-15),h-12)}if(d.length){x.strokeStyle='#8aa000';x.lineWidth=3;x.beginPath();d.forEach((v,i)=>{let xx=p.l+v[0]/maxM*(w-p.l-p.r),yy=p.t+(1-v[1]/ym)*(h-p.t-p.b);i?x.lineTo(xx,yy):x.moveTo(xx,yy)});x.stroke()}}
async function loadProfiles(){let ps=await req('/api/profiles');profileSelect.innerHTML=ps.map(p=>`<option value="${p.slot}" ${p.active?'selected':''}>${p.name}</option>`).join('');let a=ps.find(p=>p.active)||ps[0];if(a)await loadGraph(a.slot)} async function loadGraph(slot){let p=await req('/api/profiles?slot='+slot);graphData=p.data;draw()} async function selectProfile(slot){try{await req('/api/profiles',form({action:'select',slot}));await loadGraph(slot);refresh()}catch(e){alert(e.message);loadProfiles()}} async function cmd(c){try{await req('/api/control',form({cmd:c}));refresh()}catch(e){alert(e.message)}}
async function refresh(){try{let s=await req('/api/status');state.textContent=s.state;temp.textContent=s.temp==null?'--.-':Number(s.temp).toFixed(1);target.textContent=Number(s.target).toFixed(1);elapsed.textContent=Number(s.elapsed).toFixed(1);duty.textContent=Number(s.duty).toFixed(0);fill.style.width=s.duty+'%';fault.textContent=s.fault||'';activeProfile.textContent=s.profile;clock.textContent=s.time_valid?'Tid: '+s.local_time:'Tid ikke synkroniseret (NTP kræver internet)';if(s.update_available&&!sessionStorage.getItem('dismissUpdate:'+s.latest_version)){updateBox.style.display='block';updateText.textContent='Installed '+s.firmware_version+' — available '+s.latest_version;}else updateBox.style.display='none'}catch(e){}} async function checkForUpdate(){checkUpdateBtn.disabled=true;updateCheckMsg.textContent='Checking for firmware update...';try{let r=await req('/api/update/check',{method:'POST'});if(r.available){sessionStorage.removeItem('dismissUpdate:'+r.latest);updateCheckMsg.textContent='New firmware '+r.latest+' is available.';}else{updateCheckMsg.textContent='Firmware is up to date. Latest version: '+r.latest;}await refresh();}catch(e){updateCheckMsg.textContent='Update check failed: '+e.message;}finally{checkUpdateBtn.disabled=false}} function dismissUpdate(){sessionStorage.setItem('dismissUpdate:'+((updateText.textContent.match(/available (.+)$/)||[])[1]||''),'1');updateBox.style.display='none'}async function installFirmwareUpdate(){if(!confirm('Install the new firmware now? OpenKiln will reboot.'))return;try{updateText.textContent='Firmware update started. Open Setup to follow progress.';await req('/api/update/install',{method:'POST'});}catch(e){alert(e.message);refresh()}} window.addEventListener('resize',draw);loadProfiles();refresh();setInterval(refresh,2000);</script><script>
(async()=>{try{const r=await fetch('/api/settings');if(!r.ok)return;const s=await r.json();document.documentElement.lang=s.language||'en';if(s.language!=='en')return;const m={
'Ovn':'Oven','Profiler':'Profiles','Profil:':'Profile:','Temperatur':'Temperature','Mål':'Target','Tid':'Elapsed','Effekt':'Output','Start':'Start','Pause':'Pause','Fortsæt':'Resume','Stop':'Stop','Vælg profil':'Select profile','Gemte profiler':'Saved profiles','+ Ny profil':'+ New profile','Profil editor':'Profile editor','Navn':'Name','Minut':'Minute','Temperatur °C':'Temperature °C','Slope °C/h':'Slope °C/h','+ Punkt':'+ Point','Gem':'Save','Slet':'Delete','Profiler kan kun ændres når ovnen er stoppet.':'Profiles can only be changed while the kiln is stopped.','Setup':'Setup','Gem indstillinger':'Save settings','Genstart OpenKiln':'Reboot OpenKiln','Dansk':'Danish','Nedkøling':'Cooling'};
const w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);let n;while(n=w.nextNode()){const t=n.nodeValue.trim();if(m[t])n.nodeValue=n.nodeValue.replace(t,m[t]);}
}catch(e){}})();
</script></body></html>)HTML";
const char PROFILES_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Profiler</title><style>:root{--background:#151515;--panel:#222321;--border:#454641;--text:#f2f1eb;--muted:#aaa99f;--profile:#8aa000;--danger:#b74a4a}*{box-sizing:border-box}body{margin:0;background:var(--background);color:var(--text);font-family:Arial,sans-serif}.wrapper{max-width:1500px;margin:auto;padding:25px}.header{display:flex;justify-content:space-between;gap:15px;align-items:end;margin-bottom:20px}.nav{display:flex;gap:8px;flex-wrap:wrap}.nav a,button{border:1px solid var(--border);border-radius:9px;padding:10px 14px;background:#30312e;color:var(--text);font-weight:700;text-decoration:none;cursor:pointer}.nav a.active,button.primary{background:var(--profile);border-color:var(--profile);color:#111}button.danger{background:#482828;border-color:#784040}button:disabled{opacity:.4}h1{margin:0;font-size:38px}.muted,.label{color:var(--muted)}.label{font-size:13px;margin-bottom:6px}.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-bottom:15px}.card,.panel{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:18px}.value{font-size:clamp(24px,3vw,40px);font-weight:bold}.unit{color:var(--muted);font-size:15px}.actions{display:flex;gap:9px;flex-wrap:wrap;margin-top:14px}.graph{height:500px;margin-top:15px;background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:12px}.graph canvas{width:100%;height:100%;display:block}.bar{height:10px;background:#171817;border-radius:8px;overflow:hidden;margin-top:8px}.fill{height:100%;background:var(--profile)}select,input{width:100%;background:#181918;color:var(--text);border:1px solid var(--border);border-radius:8px;padding:10px;font:inherit}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}.steps{width:100%;border-collapse:collapse}.steps th,.steps td{text-align:left;padding:6px}.steps th{color:var(--muted);font-size:12px}.notice{margin-top:10px;color:var(--muted);min-height:20px}.fault{color:#e27676}.section{margin-top:14px}.switch{width:auto}.statusgrid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.pill{padding:8px;border:1px solid var(--border);border-radius:8px;color:var(--muted)}@media(max-width:900px){.cards,.grid2,.statusgrid{grid-template-columns:1fr 1fr}.graph{height:390px}}@media(max-width:600px){.cards,.grid2,.statusgrid{grid-template-columns:1fr}.wrapper{padding:14px}.header{align-items:flex-start;flex-direction:column}}</style></head><body><div class="wrapper"><div class="header"><h1>Profiler</h1><div class="nav"><a href="/">Ovn</a><a class="active" href="/profiles">Profiler</a><a href="/setup">Setup</a></div></div><div class="grid2"><div class="panel"><h2>Gemte profiler</h2><div id="list"></div><div class="actions"><button class="primary" onclick="fresh()">+ Ny profil</button></div></div><div class="panel"><h2>Profil editor</h2><div class="label">Navn</div><input id="profileName" maxlength="32"><div class="graph" style="height:340px"><canvas id="g"></canvas></div><table class="steps"><thead><tr><th>Minut</th><th>Temperatur °C</th><th>Slope °C/h</th><th></th></tr></thead><tbody id="rows"></tbody></table><div class="actions"><button onclick="add()">+ Punkt</button><button class="primary" onclick="save()">Gem</button><button onclick="activate()">Vælg profil</button><button class="danger" onclick="del()">Slet</button></div><div id="note" class="notice">Profiler kan kun ændres når ovnen er stoppet.</div></div></div></div><script>
let slot=-1,ps=[];
async function req(p,o){let r=await fetch(p,o),t=await r.text();if(!r.ok)throw Error(t);try{return JSON.parse(t)}catch{return t}}
function form(d){return{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d)}}
function data(){return[...rows.children].map(r=>[+r.querySelector('.minute').value,+r.querySelector('.temperature').value])}
function slopeFor(index){let rr=[...rows.children];if(index<=0)return 0;let a=rr[index-1],b=rr[index],dm=+b.querySelector('.minute').value-+a.querySelector('.minute').value,dt=+b.querySelector('.temperature').value-+a.querySelector('.temperature').value;return dm>0?dt/dm*60:0}
function updateSlopes(){[...rows.children].forEach((r,i)=>{let s=r.querySelector('.slope');if(i===0){s.value='';s.placeholder='Start';s.disabled=true}else{s.disabled=false;s.value=slopeFor(i).toFixed(1)}})}
function applySlope(r){let rr=[...rows.children],i=rr.indexOf(r);if(i<=0)return;let prev=rr[i-1],s=+r.querySelector('.slope').value,t0=+prev.querySelector('.temperature').value,t1=+r.querySelector('.temperature').value,m0=+prev.querySelector('.minute').value;if(!Number.isFinite(s)||Math.abs(s)<0.001)return;let minutes=(t1-t0)/s*60;if(minutes>0&&Number.isFinite(minutes))r.querySelector('.minute').value=(m0+minutes).toFixed(1);updateSlopes();draw()}
function row(m,t){let r=document.createElement('tr');r.innerHTML=`<td><input class="minute" type="number" min="0" step="0.1" value="${m}"></td><td><input class="temperature" type="number" step="1" value="${t}"></td><td><input class="slope" type="number" step="1" placeholder="Start"></td><td><button onclick="this.closest('tr').remove();updateSlopes();draw()">×</button></td>`;r.querySelector('.minute').oninput=()=>{updateSlopes();draw()};r.querySelector('.temperature').oninput=()=>{updateSlopes();draw()};r.querySelector('.slope').onchange=()=>applySlope(r);rows.appendChild(r);updateSlopes();draw()}
function set(n,d){profileName.value=(n===undefined||n===null)?'':String(n);rows.innerHTML='';d.forEach(v=>row(v[0],v[1]));updateSlopes();draw()}
function fresh(){slot=-1;set('',[[0,20],[60,600]]);render();name.focus()}
function add(){let d=data(),z=d[d.length-1]||[0,20];row(z[0]+30,z[1])}
function draw(){const c=g,r=c.getBoundingClientRect(),q=devicePixelRatio||1;c.width=r.width*q;c.height=r.height*q;let x=c.getContext('2d');x.setTransform(q,0,0,q,0,0);let w=r.width,h=r.height,p={l:55,r:15,t:15,b:38},d=data().sort((a,b)=>a[0]-b[0]),mm=Math.max(60,...d.map(v=>v[0])),mt=Math.max(200,...d.map(v=>v[1])),ym=Math.ceil((mt+50)/100)*100;x.clearRect(0,0,w,h);x.strokeStyle='rgba(255,255,255,.13)';x.fillStyle='#aaa99f';x.font='12px Arial';for(let i=0;i<=5;i++){let yy=p.t+(h-p.t-p.b)*i/5;x.beginPath();x.moveTo(p.l,yy);x.lineTo(w-p.r,yy);x.stroke();x.fillText(Math.round(ym*(1-i/5))+'°',5,yy+4)}for(let i=0;i<=6;i++){let xx=p.l+(w-p.l-p.r)*i/6;x.beginPath();x.moveTo(xx,p.t);x.lineTo(xx,h-p.b);x.stroke();x.fillText(Math.round(mm*i/6)+'m',xx-8,h-10)}x.strokeStyle='#8aa000';x.lineWidth=3;x.beginPath();d.forEach((v,i)=>{let xx=p.l+v[0]/mm*(w-p.l-p.r),yy=p.t+(1-v[1]/ym)*(h-p.t-p.b);i?x.lineTo(xx,yy):x.moveTo(xx,yy)});x.stroke()}
async function load(openActive=true){ps=await req('/api/profiles');render();if(openActive){let a=ps.find(p=>p.active)||ps[0];if(a)await openp(a.slot);else fresh()}}
function render(){list.innerHTML=ps.map(p=>`<button style="width:100%;margin:4px 0;text-align:left" class="${p.slot===slot||p.active?'primary':''}" onclick="openp(${p.slot})">${p.active?'● ':''}${p.name||'(unnamed)'}</button>`).join('')}
async function openp(s){let p=await req('/api/profiles?slot='+s);slot=s;set(p.name,p.data);render()}
async function save(){try{let profileNameValue=String(profileName.value||'').trim();if(!profileNameValue)return alert('Profile name is required');let r=await req('/api/profiles',form({action:'save',slot:String(slot),name:profileNameValue,data:JSON.stringify(data())}));slot=Number(r.slot);note.textContent='Gemt';await load(false);await openp(slot)}catch(e){alert(e.message)}}
async function activate(){if(slot<0)return alert('Gem først');try{await req('/api/profiles',form({action:'select',slot:String(slot)}));note.textContent='Profil valgt';await load(false);await openp(slot)}catch(e){alert(e.message)}}
async function del(){if(slot<0||!confirm('Slet profil?'))return;try{await req('/api/profiles',form({action:'delete',slot:String(slot)}));slot=-1;await load(true)}catch(e){alert(e.message)}}
function dismissUpdate(){sessionStorage.setItem('dismissUpdate:'+((updateText.textContent.match(/available (.+)$/)||[])[1]||''),'1');updateBox.style.display='none'}async function installFirmwareUpdate(){if(!confirm('Install the new firmware now? OpenKiln will reboot.'))return;try{updateText.textContent='Firmware update started. Open Setup to follow progress.';await req('/api/update/install',{method:'POST'});}catch(e){alert(e.message);refresh()}} window.addEventListener('resize',draw);load(true);</script><script>
(async()=>{try{const r=await fetch('/api/settings');if(!r.ok)return;const s=await r.json();document.documentElement.lang=s.language||'en';if(s.language!=='en')return;const m={
'Ovn':'Oven','Profiler':'Profiles','Profil:':'Profile:','Temperatur':'Temperature','Mål':'Target','Tid':'Elapsed','Effekt':'Output','Start':'Start','Pause':'Pause','Fortsæt':'Resume','Stop':'Stop','Vælg profil':'Select profile','Gemte profiler':'Saved profiles','+ Ny profil':'+ New profile','Profil editor':'Profile editor','Navn':'Name','Minut':'Minute','Temperatur °C':'Temperature °C','Slope °C/h':'Slope °C/h','+ Punkt':'+ Point','Gem':'Save','Slet':'Delete','Profiler kan kun ændres når ovnen er stoppet.':'Profiles can only be changed while the kiln is stopped.','Setup':'Setup','Gem indstillinger':'Save settings','Genstart OpenKiln':'Reboot OpenKiln','Dansk':'Danish','Nedkøling':'Cooling'};
const w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);let n;while(n=w.nextNode()){const t=n.nodeValue.trim();if(m[t])n.nodeValue=n.nodeValue.replace(t,m[t]);}
}catch(e){}})();
</script></body></html>)HTML";
const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Setup</title><style>:root{--background:#151515;--panel:#222321;--border:#454641;--text:#f2f1eb;--muted:#aaa99f;--profile:#8aa000;--danger:#b74a4a}*{box-sizing:border-box}body{margin:0;background:var(--background);color:var(--text);font-family:Arial,sans-serif}.wrapper{max-width:1500px;margin:auto;padding:25px}.header{display:flex;justify-content:space-between;gap:15px;align-items:end;margin-bottom:20px}.nav{display:flex;gap:8px;flex-wrap:wrap}.nav a,button{border:1px solid var(--border);border-radius:9px;padding:10px 14px;background:#30312e;color:var(--text);font-weight:700;text-decoration:none;cursor:pointer}.nav a.active,button.primary{background:var(--profile);border-color:var(--profile);color:#111}button.danger{background:#482828;border-color:#784040}button:disabled{opacity:.4}h1{margin:0;font-size:38px}.muted,.label{color:var(--muted)}.label{font-size:13px;margin-bottom:6px}.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-bottom:15px}.card,.panel{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:18px}.value{font-size:clamp(24px,3vw,40px);font-weight:bold}.unit{color:var(--muted);font-size:15px}.actions{display:flex;gap:9px;flex-wrap:wrap;margin-top:14px}.graph{height:500px;margin-top:15px;background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:12px}.graph canvas{width:100%;height:100%;display:block}.bar{height:10px;background:#171817;border-radius:8px;overflow:hidden;margin-top:8px}.fill{height:100%;background:var(--profile)}select,input{width:100%;background:#181918;color:var(--text);border:1px solid var(--border);border-radius:8px;padding:10px;font:inherit}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}.steps{width:100%;border-collapse:collapse}.steps th,.steps td{text-align:left;padding:6px}.steps th{color:var(--muted);font-size:12px}.notice{margin-top:10px;color:var(--muted);min-height:20px}.fault{color:#e27676}.section{margin-top:14px}.switch{width:auto}.statusgrid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.pill{padding:8px;border:1px solid var(--border);border-radius:8px;color:var(--muted)}@media(max-width:900px){.cards,.grid2,.statusgrid{grid-template-columns:1fr 1fr}.graph{height:390px}}@media(max-width:600px){.cards,.grid2,.statusgrid{grid-template-columns:1fr}.wrapper{padding:14px}.header{align-items:flex-start;flex-direction:column}}</style></head><body><div class="wrapper"><div class="header"><h1>Setup</h1><div class="nav"><a href="/">Ovn</a><a href="/profiles">Profiler</a><a class="active" href="/setup">Setup</a></div></div><form id="f"><div class="grid2">
<div class="panel"><h2>Wi‑Fi / Access</h2><div class="label">Hostname (.local)</div><input name="hostname" placeholder="kiln"><div class="notice">Local address: http://&lt;hostname&gt;.local — default http://kiln.local</div><div class="label section">Language</div><select name="language"><option value="en">English</option><option value="da">Dansk</option></select><div class="label section">Wi‑Fi SSID</div><input name="wifi_ssid"><div class="label section">Wi‑Fi password</div><input type="password" name="wifi_password" placeholder="Leave blank to keep current"><div class="label section">AP name</div><input name="ap_name"><div class="label section">AP password</div><input type="password" name="ap_password" placeholder="Leave blank to keep current"><div class="label section">Web username</div><input name="web_user"><div class="label section">Web password</div><input type="password" name="web_password" placeholder="Leave blank to keep current"></div>


<div class="panel"><h2>Time</h2><div class="label">NTP server</div><input name="ntp_server"><div class="label section">Timezone</div><select name="timezone"><option value="CET-1CEST,M3.5.0,M10.5.0/3">Denmark (Europe/Copenhagen)</option><option value="GMT0BST,M3.5.0/1,M10.5.0">United Kingdom (Europe/London)</option><option value="UTC0">UTC</option><option value="EST5EDT,M3.2.0/2,M11.1.0/2">US Eastern</option><option value="CST6CDT,M3.2.0/2,M11.1.0/2">US Central</option><option value="MST7MDT,M3.2.0/2,M11.1.0/2">US Mountain</option><option value="PST8PDT,M3.2.0/2,M11.1.0/2">US Pacific</option></select><div class="notice">Denmark is the default and automatically follows summer/winter time.</div></div></div><details class="advanced section"><summary>Advanced ▸</summary><div class="notice">Advanced integration and maintenance settings.</div><div class="grid2 section"><div class="panel"><h2>MQTT</h2><label><input class="switch" type="checkbox" name="mqtt_enabled"> Enabled</label><div class="label section">Host</div><input name="mqtt_host"><div class="label section">Port</div><input type="number" name="mqtt_port"><div class="label section">Username</div><input name="mqtt_user"><div class="label section">Password</div><input type="password" name="mqtt_password" placeholder="Leave blank to keep current"><div class="label section">Topic prefix</div><input name="mqtt_topic"><label><input class="switch" type="checkbox" name="mqtt_control"> Allow remote pause/resume</label><br><label><input class="switch" type="checkbox" name="mqtt_start"> Allow remote START</label></div><div class="panel"><h2>InfluxDB 2.x</h2><label><input class="switch" type="checkbox" name="influx_enabled"> Enabled</label><div class="label section">URL (e.g. http://192.168.1.10:8086)</div><input name="influx_url"><div class="label section">Organisation</div><input name="influx_org"><div class="label section">Bucket</div><input name="influx_bucket"><div class="label section">Token</div><input type="password" name="influx_token" placeholder="Leave blank to keep current"><div class="label section">Write interval seconds</div><input type="number" name="influx_interval"></div><div class="panel"><h2>Pushover</h2><label><input class="switch" type="checkbox" name="pushover_enabled"> Enabled</label><div class="label section">Application API token</div><input type="password" name="pushover_token" placeholder="Leave blank to keep current"><div class="label section">User/group key</div><input type="password" name="pushover_user" placeholder="Leave blank to keep current"><div class="notice">Notifications: firing started, complete, stopped and fault.</div></div></div><div class="panel section"><h2>Manual firmware upload / OTA</h2><div class="notice">Upload a PlatformIO firmware.bin manually. Heating is forced OFF during update.</div><input id="fw" type="file" accept=".bin"><div class="actions"><button type="button" onclick="ota()">Upload firmware</button></div><div id="otaMsg" class="notice"></div></div></details><div class="actions"><button class="primary" type="submit">Save settings</button><button type="button" onclick="reboot()">Reboot OpenKiln</button></div><div id="msg" class="notice"></div></form>
<div class="panel section"><h2>Firmware</h2><div class="label">Current firmware version</div><div class="value" id="currentFirmwareVersion" style="font-size:26px">--</div><div class="actions"><button type="button" id="setupCheckUpdateBtn" onclick="setupCheckForUpdate()">Check for update</button></div><div id="setupUpdateMsg" class="notice"></div><div id="setupProgressWrap" style="display:none;margin-top:12px"><div style="height:16px;background:#333;border:1px solid #555;border-radius:8px;overflow:hidden"><div id="setupProgressBar" style="height:100%;width:0%;background:#8aa000;transition:width .25s"></div></div><div id="setupProgressText" class="notice">Preparing update...</div></div><div id="setupInstallBox" style="display:none"><div class="actions"><button type="button" class="primary" onclick="setupInstallFirmwareUpdate()">Install update</button></div></div></div></div><script>
async function req(p,o){let r=await fetch(p,o),t=await r.text();if(!r.ok)throw Error(t);try{return JSON.parse(t)}catch{return t}}async function load(){let s=await req('/api/settings');for(let [k,v] of Object.entries(s)){let e=f.elements[k];if(!e)continue;if(e.type==='checkbox')e.checked=!!v;else e.value=v??''}await loadFirmwareStatus()}async function loadFirmwareStatus(){try{let s=await req('/api/status');currentFirmwareVersion.textContent=s.firmware_version||'--';if(s.update_available){setupUpdateMsg.textContent='New firmware '+s.latest_version+' is available.';setupInstallBox.style.display='block'}else{setupInstallBox.style.display='none'}}catch(e){setupUpdateMsg.textContent='Unable to read firmware status: '+e.message}}async function setupCheckForUpdate(){setupCheckUpdateBtn.disabled=true;setupUpdateMsg.textContent='Checking for firmware update...';setupInstallBox.style.display='none';try{let r=await req('/api/update/check',{method:'POST'});if(r.available){setupUpdateMsg.textContent='New firmware '+r.latest+' is available. Installed: '+currentFirmwareVersion.textContent;setupInstallBox.style.display='block'}else{setupUpdateMsg.textContent='Firmware is up to date. Latest version: '+r.latest}}catch(e){setupUpdateMsg.textContent='Update check failed: '+e.message}finally{setupCheckUpdateBtn.disabled=false}}async function setupInstallFirmwareUpdate(){if(!confirm('Install the new firmware now? OpenKiln will reboot.'))return;setupInstallBox.style.display='none';setupProgressWrap.style.display='block';setupProgressBar.style.width='0%';setupProgressText.textContent='Preparing firmware update...';try{await req('/api/update/install',{method:'POST'});pollUpdateStatus()}catch(e){setupUpdateMsg.textContent='Update failed: '+e.message;setupProgressWrap.style.display='none'}}async function pollUpdateStatus(){try{let u=await req('/api/update/status');setupProgressBar.style.width=(u.progress||0)+'%';let labels={starting:'Preparing update...',downloading:'Downloading firmware...',installing:'Installing firmware — do not turn off OpenKiln.',rebooting:'Rebooting OpenKiln...',error:'Update failed'};setupProgressText.textContent=(labels[u.stage]||u.message||'Updating...')+(u.stage==='downloading'?' '+(u.progress||0)+'%':'');if(u.stage==='error'){setupUpdateMsg.textContent='Update failed: '+u.message;return}if(u.stage==='rebooting'){setupProgressText.textContent='Rebooting OpenKiln... Waiting to reconnect.';setTimeout(waitForOpenKiln,2500);return}setTimeout(pollUpdateStatus,500)}catch(e){setTimeout(waitForOpenKiln,1500)}}async function waitForOpenKiln(){setupProgressText.textContent='Waiting for OpenKiln to reconnect...';try{let s=await req('/api/status');currentFirmwareVersion.textContent=s.firmware_version||'--';setupProgressBar.style.width='100%';setupProgressText.textContent='Update complete. Running firmware: '+currentFirmwareVersion.textContent;setupUpdateMsg.textContent='Firmware updated successfully.';setupCheckUpdateBtn.disabled=false}catch(e){setTimeout(waitForOpenKiln,1500)}}f.onsubmit=async e=>{e.preventDefault();try{await req('/api/settings',{method:'POST',body:new URLSearchParams(new FormData(f))});msg.textContent='Saved. Network and integration changes apply after reboot.'}catch(x){alert(x.message)}};async function reboot(){if(confirm('Reboot OpenKiln?'))await req('/api/reboot',{method:'POST'})}async function ota(){if(!fw.files.length)return alert('Choose firmware.bin');let d=new FormData();d.append('firmware',fw.files[0]);otaMsg.textContent='Uploading...';try{await req('/update',{method:'POST',body:d});otaMsg.textContent='Update complete. OpenKiln is rebooting.'}catch(e){otaMsg.textContent=e.message}}load();</script><script>
(async()=>{try{const r=await fetch('/api/settings');if(!r.ok)return;const s=await r.json();document.documentElement.lang=s.language||'en';if(s.language!=='en')return;const m={
'Ovn':'Oven','Profiler':'Profiles','Profil:':'Profile:','Temperatur':'Temperature','Mål':'Target','Tid':'Elapsed','Effekt':'Output','Start':'Start','Pause':'Pause','Fortsæt':'Resume','Stop':'Stop','Vælg profil':'Select profile','Gemte profiler':'Saved profiles','+ Ny profil':'+ New profile','Profil editor':'Profile editor','Navn':'Name','Minut':'Minute','Temperatur °C':'Temperature °C','Slope °C/h':'Slope °C/h','+ Punkt':'+ Point','Gem':'Save','Slet':'Delete','Profiler kan kun ændres når ovnen er stoppet.':'Profiles can only be changed while the kiln is stopped.','Setup':'Setup','Gem indstillinger':'Save settings','Genstart OpenKiln':'Reboot OpenKiln','Dansk':'Danish','Nedkøling':'Cooling'};
const w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);let n;while(n=w.nextNode()){const t=n.nodeValue.trim();if(m[t])n.nodeValue=n.nodeValue.replace(t,m[t]);}
}catch(e){}})();
</script></body></html>)HTML";

const char* stateName(RunState state) {
  switch (state) {
    case RunState::IDLE: return "IDLE";
    case RunState::RUNNING: return "RUNNING";
    case RunState::PAUSED: return "PAUSED";
    case RunState::COMPLETE: return "COMPLETE";
    case RunState::FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

void setRelay(bool on) {
  digitalWrite(PIN_SSR, (on == SSR_ACTIVE_HIGH) ? HIGH : LOW);
}

void forceHeatOff() {
  outputDuty = 0.0f;
  setRelay(false);
}

void enterFault(const String& message) {
  forceHeatOff();
  runState = RunState::FAULT;
  faultMessage = message;
  Serial.printf("FAULT: %s\n", message.c_str());
  sendPushover("Kiln fault", message);
}

uint8_t spiReadRegister(uint8_t address) {
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_THERMO_CS, LOW);
  thermoSpi.transfer(address & 0x7F);
  uint8_t value = thermoSpi.transfer(0x00);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();
  return value;
}

void spiWriteRegister(uint8_t address, uint8_t value) {
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_THERMO_CS, LOW);
  thermoSpi.transfer(address | 0x80);
  thermoSpi.transfer(value);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();
}

bool beginThermocouple() {
  pinMode(PIN_THERMO_CS, OUTPUT);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.begin(PIN_THERMO_SCK, PIN_THERMO_MISO, PIN_THERMO_MOSI, PIN_THERMO_CS);
  if (THERMOCOUPLE_CHIP == ThermocoupleChip::MAX31856) {
    // CR0: automatic conversions and local mains-frequency rejection.
    spiWriteRegister(0x00, static_cast<uint8_t>(0x80 | (MAINS_IS_50_HZ ? 0x01 : 0x00)));
    // CR1: 16-sample averaging plus thermocouple type.
    spiWriteRegister(0x01, static_cast<uint8_t>(0x40 | (MAX31856_TYPE & 0x0F)));
  }
  return true;
}

bool readMax31855(float& temperature, String& error) {
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_THERMO_CS, LOW);
  delayMicroseconds(2);
  uint32_t raw = 0;
  for (int i = 0; i < 4; ++i) raw = (raw << 8) | thermoSpi.transfer(0x00);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();

  if (raw & 0x00010000UL) {
    if (raw & 0x1) error = "Thermocouple open";
    else if (raw & 0x2) error = "Thermocouple shorted to ground";
    else if (raw & 0x4) error = "Thermocouple shorted to supply";
    else error = "MAX31855 fault";
    return false;
  }
  int16_t signedValue = static_cast<int16_t>((raw >> 18) & 0x3FFF);
  if (signedValue & 0x2000) signedValue |= 0xC000;
  temperature = signedValue * 0.25f;
  return isfinite(temperature);
}

bool readMax31856(float& temperature, String& error) {
  uint8_t status = spiReadRegister(0x0F);
  if (status != 0) {
    error = "MAX31856 fault 0x" + String(status, HEX);
    return false;
  }
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_THERMO_CS, LOW);
  thermoSpi.transfer(0x0C);
  uint32_t raw = (static_cast<uint32_t>(thermoSpi.transfer(0)) << 16) |
                 (static_cast<uint32_t>(thermoSpi.transfer(0)) << 8) |
                 thermoSpi.transfer(0);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();
  int32_t signedValue = static_cast<int32_t>(raw >> 5);
  if (signedValue & 0x40000) signedValue |= ~0x7FFFF;
  temperature = signedValue * 0.0078125f;
  return isfinite(temperature);
}

bool readThermocouple(float& temperature, String& error) {
  bool ok = THERMOCOUPLE_CHIP == ThermocoupleChip::MAX31855
                ? readMax31855(temperature, error)
                : readMax31856(temperature, error);
  temperature += THERMOCOUPLE_OFFSET_C;
  if (ok && (temperature < -100.0f || temperature > 1800.0f)) {
    error = "Implausible temperature";
    return false;
  }
  return ok;
}

String defaultProfile() {
  return "0,20\n10,100\n20,100\n30,20\n";
}

bool parseProfile(const String& text, ProfilePoint* destination, size_t& count, String& error) {
  count = 0;
  int start = 0;
  float previousMinute = -1.0f;
  while (start < static_cast<int>(text.length())) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.isEmpty() || line.startsWith("#")) continue;
    int comma = line.indexOf(',');
    if (comma < 1 || count >= MAX_PROFILE_POINTS) {
      error = "Invalid line or too many profile points";
      return false;
    }
    String minuteText = line.substring(0, comma);
    String tempText = line.substring(comma + 1);
    char* minuteEnd = nullptr;
    char* tempEnd = nullptr;
    float minute = strtof(minuteText.c_str(), &minuteEnd);
    float temperature = strtof(tempText.c_str(), &tempEnd);
    while (*minuteEnd == ' ' || *minuteEnd == '\t') ++minuteEnd;
    while (*tempEnd == ' ' || *tempEnd == '\t' || *tempEnd == '\r') ++tempEnd;
    if (*minuteEnd != '\0' || *tempEnd != '\0' || !isfinite(minute) ||
        !isfinite(temperature) || minute < 0 || minute <= previousMinute ||
        temperature < -50 || temperature >= EMERGENCY_SHUTOFF_C) {
      error = "Each line needs increasing minutes and a safe Celsius temperature";
      return false;
    }
    destination[count++] = {minute, temperature};
    previousMinute = minute;
  }
  if (count < 2 || destination[0].minute != 0.0f) {
    error = "Profile needs at least two points and must start at minute 0";
    return false;
  }
  return true;
}

String profileNameKey(size_t slot) { return "pn" + String(slot); }
String profileDataKey(size_t slot) { return "pd" + String(slot); }

String profileTextFromJsonArray(const String& json, String& error) {
  String text;
  int pos = 0;
  while (true) {
    int open = json.indexOf('[', pos);
    if (open < 0) break;
    if (open == 0) { pos = 1; continue; }
    int comma = json.indexOf(',', open + 1);
    int close = json.indexOf(']', comma + 1);
    if (comma < 0 || close < 0) break;
    String a = json.substring(open + 1, comma); a.trim();
    String b = json.substring(comma + 1, close); b.trim();
    if (a.length() && b.length()) text += a + "," + b + "\n";
    pos = close + 1;
  }
  ProfilePoint candidate[MAX_PROFILE_POINTS]; size_t count = 0;
  if (!parseProfile(text, candidate, count, error)) return "";
  return text;
}

String profileDataJson(const String& text) {
  ProfilePoint points[MAX_PROFILE_POINTS]; size_t count = 0; String error;
  if (!parseProfile(text, points, count, error)) return "[]";
  String json = "[";
  for (size_t i=0;i<count;i++) { if(i) json += ","; json += "["+String(points[i].minute,1)+","+String(points[i].temperatureC,1)+"]"; }
  json += "]"; return json;
}

bool activateProfileSlot(size_t slot, String& error) {
  if (slot >= MAX_SAVED_PROFILES) { error="Invalid profile slot"; return false; }
  String name=preferences.getString(profileNameKey(slot).c_str(), "");
  String text=preferences.getString(profileDataKey(slot).c_str(), "");
  if(name.isEmpty()||text.isEmpty()){error="Profile not found";return false;}
  ProfilePoint candidate[MAX_PROFILE_POINTS]; size_t count=0;
  if(!parseProfile(text,candidate,count,error)) return false;
  memcpy(profile,candidate,sizeof(ProfilePoint)*count); profileCount=count;
  activeProfileName=name; preferences.putUInt("activeSlot",slot); preferences.putString("profile",text);
  return true;
}

String loadProfile() {
  // Migrate the original single profile into slot 0 on first boot after upgrade.
  if (preferences.getString("pn0", "").isEmpty()) {
    String legacy=preferences.getString("profile", ""); if(legacy.isEmpty()) legacy=defaultProfile();
    preferences.putString("pn0", "Default"); preferences.putString("pd0", legacy); preferences.putUInt("activeSlot",0);
  }
  size_t slot=preferences.getUInt("activeSlot",0); String error;
  if(!activateProfileSlot(slot,error)) activateProfileSlot(0,error);
  return preferences.getString("profile",defaultProfile());
}

float targetForElapsed(double seconds) {
  if (profileCount == 0) return 0.0f;
  float minute = seconds / 60.0;
  if (minute <= profile[0].minute) return profile[0].temperatureC;
  for (size_t i = 1; i < profileCount; ++i) {
    if (minute <= profile[i].minute) {
      float span = profile[i].minute - profile[i - 1].minute;
      float fraction = (minute - profile[i - 1].minute) / span;
      return profile[i - 1].temperatureC +
             fraction * (profile[i].temperatureC - profile[i - 1].temperatureC);
    }
  }
  return profile[profileCount - 1].temperatureC;
}

void checkpoint(bool running) {
  preferences.putBool("running", running);
  preferences.putDouble("elapsed", profileElapsedSeconds);
  preferences.putULong64("epoch", static_cast<uint64_t>(time(nullptr)));
}

void startProfile() {
  if (!sensorHealthy) {
    enterFault("Cannot start: thermocouple is not healthy");
    return;
  }
  profileElapsedSeconds = 0;
  integralTerm = 0;
  previousError = 0;
  faultMessage = "";
  runState = RunState::RUNNING;
  checkpoint(true);
}

void stopProfile() {
  forceHeatOff();
  runState = RunState::IDLE;
  integralTerm = 0;
  checkpoint(false);
}

void updateSensor(uint32_t now) {
  if (now - lastSensorReadMs < SENSOR_INTERVAL_MS) return;
  lastSensorReadMs = now;
  float value = NAN;
  String error;
  if (readThermocouple(value, error)) {
    currentTemperatureC = value;
    sensorHealthy = true;
    lastGoodSensorMs = now;
    if (currentTemperatureC >= EMERGENCY_SHUTOFF_C) enterFault("Emergency temperature limit reached");
  } else {
    sensorHealthy = false;
    if (runState == RunState::RUNNING) enterFault(error);
  }

  if (sensorHealthy && (previousRateMs == 0 || now - previousRateMs >= 10000)) {
    if (isfinite(previousRateTemperature)) {
      heatingRateCPerHour = (currentTemperatureC - previousRateTemperature) *
                            3600000.0f / static_cast<float>(now - previousRateMs);
    }
    previousRateTemperature = currentTemperatureC;
    previousRateMs = now;
  }
}

void updateController(uint32_t now, float dtSeconds) {
  if (runState != RunState::RUNNING) {
    forceHeatOff();
    return;
  }
  if (!sensorHealthy || now - lastGoodSensorMs > SENSOR_TIMEOUT_MS) {
    enterFault("Temperature sensor timeout");
    return;
  }

  targetTemperatureC = targetForElapsed(profileElapsedSeconds);
  bool caughtUp = fabsf(currentTemperatureC - targetTemperatureC) <= CATCH_UP_WINDOW_C;
  if (!KILN_MUST_CATCH_UP || caughtUp) profileElapsedSeconds += dtSeconds;
  if (profileElapsedSeconds >= profile[profileCount - 1].minute * 60.0) {
    runState = RunState::COMPLETE;
    forceHeatOff();
    checkpoint(false);
    sendPushover("Kiln complete", activeProfileName + " is complete");
    return;
  }

  float error = targetTemperatureC - currentTemperatureC;
  if (error > PID_CONTROL_WINDOW_C) {
    integralTerm = 0;
    outputDuty = currentTemperatureC < THROTTLE_BELOW_C ? THROTTLE_MAX_DUTY : 1.0f;
  } else if (error < -PID_CONTROL_WINDOW_C) {
    integralTerm = 0;
    outputDuty = 0.0f;
  } else {
    float derivative = (error - previousError) / max(dtSeconds, 0.001f);
    float candidateIntegral = constrain(integralTerm + PID_KI * error * dtSeconds,
                                        -INTEGRAL_LIMIT, INTEGRAL_LIMIT);
    float candidateOutput = PID_KP * error + candidateIntegral + PID_KD * derivative;
    outputDuty = constrain(candidateOutput, 0.0f, 1.0f);
    // Conditional integration prevents wind-up while saturated in the wrong direction.
    if ((candidateOutput >= 0.0f && candidateOutput <= 1.0f) ||
        (candidateOutput > 1.0f && error < 0) || (candidateOutput < 0.0f && error > 0)) {
      integralTerm = candidateIntegral;
    }
  }
  previousError = error;
}

void updateRelay(uint32_t now) {
  if (now - ssrWindowStartMs >= SSR_WINDOW_MS) {
    ssrWindowStartMs += ((now - ssrWindowStartMs) / SSR_WINDOW_MS) * SSR_WINDOW_MS;
  }
  bool on = runState == RunState::RUNNING && sensorHealthy &&
            (now - ssrWindowStartMs) < static_cast<uint32_t>(outputDuty * SSR_WINDOW_MS);
  setRelay(on);
}

String jsonEscape(const String& input) {
  String result;
  result.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '"' || c == '\\') result += '\\';
    if (c == '\n') result += "\\n";
    else if (c != '\r') result += c;
  }
  return result;
}

bool authenticate() {
  if (server.authenticate(settings.webUser.c_str(), settings.webPassword.c_str())) return true;
  server.requestAuthentication();
  return false;
}

bool executeCommand(const String& command, bool remote, String& error) {
  if (remote) {
    if (command == "start" && !settings.mqttStart) {
      error = "Remote start is disabled";
      return false;
    }
    if (command != "start" && command != "stop" && !settings.mqttControl) {
      error = "Remote control is disabled";
      return false;
    }
  }

  if (command == "start" && (runState == RunState::IDLE || runState == RunState::COMPLETE)) {
    startProfile();
    if (runState == RunState::RUNNING) sendPushover("Kiln started", activeProfileName);
    if (runState == RunState::FAULT) {
      error = faultMessage;
      return false;
    }
  } else if (command == "pause" && runState == RunState::RUNNING) {
    runState = RunState::PAUSED;
    forceHeatOff();
    checkpoint(true);
  } else if (command == "resume" && runState == RunState::PAUSED && sensorHealthy) {
    runState = RunState::RUNNING;
  } else if (command == "stop") {
    bool wasActive = runState == RunState::RUNNING || runState == RunState::PAUSED;
    stopProfile();
    if (wasActive) sendPushover("Kiln stopped", activeProfileName);
  } else if (command == "clear" && runState == RunState::FAULT && sensorHealthy &&
             currentTemperatureC < EMERGENCY_SHUTOFF_C) {
    faultMessage = "";
    runState = RunState::IDLE;
    checkpoint(false);
  } else {
    error = "Command is not valid in the current state";
    return false;
  }
  return true;
}

void publishMqttStatus();

void installWebRoutes() {
  server.on("/", HTTP_GET, [] {
    if (!authenticate()) return;
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/profiles", HTTP_GET, [] { if (!authenticate()) return; server.send_P(200,"text/html",PROFILES_HTML); });
  server.on("/setup", HTTP_GET, [] { if (!authenticate()) return; server.send_P(200,"text/html",SETUP_HTML); });

  server.on("/api/status", HTTP_GET, [] {
    if (!authenticate()) return;
    String json = "{\"state\":\"" + String(stateName(runState)) + "\",\"temp\":";
    json += isfinite(currentTemperatureC) ? String(currentTemperatureC, 1) : String("null");
    json += ",\"target\":" + String(targetTemperatureC, 1);
    json += ",\"rate\":" + String(heatingRateCPerHour, 1);
    json += ",\"duty\":" + String(outputDuty * 100.0f, 1);
    json += ",\"elapsed\":" + String(profileElapsedSeconds / 60.0, 1);
    json += ",\"fault\":\"" + jsonEscape(faultMessage) + "\"";
    json += ",\"profile\":\"" + jsonEscape(activeProfileName) + "\"";
    json += ",\"firmware_version\":\""+String(OPENKILN_VERSION)+"\"";
    json += ",\"update_available\":"+String(firmwareUpdateAvailable?"true":"false");
    json += ",\"latest_version\":\""+jsonEscape(latestFirmwareVersion)+"\"";
    json += ",\"update_error\":\""+jsonEscape(updateCheckError)+"\"";
    time_t nowTime=time(nullptr); bool valid=nowTime>=VALID_EPOCH; json += ",\"time_valid\":"+String(valid?"true":"false");
    if(valid){struct tm tmv; localtime_r(&nowTime,&tmv); char tb[32]; strftime(tb,sizeof(tb),"%Y-%m-%d %H:%M:%S",&tmv); json += ",\"local_time\":\""+String(tb)+"\"";}
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/profiles", HTTP_GET, [] {
    if (!authenticate()) return;
    if (server.hasArg("slot")) {
      int slot=server.arg("slot").toInt();
      if(slot<0||slot>=static_cast<int>(MAX_SAVED_PROFILES)){server.send(400,"text/plain","Invalid slot");return;}
      String name=preferences.getString(profileNameKey(slot).c_str(),"");
      String data=preferences.getString(profileDataKey(slot).c_str(),"");
      if(name.isEmpty()||data.isEmpty()){server.send(404,"text/plain","Profile not found");return;}
      String json="{\"slot\":"+String(slot)+",\"name\":\""+jsonEscape(name)+"\",\"data\":"+profileDataJson(data)+"}";
      server.send(200,"application/json",json); return;
    }
    size_t active=preferences.getUInt("activeSlot",0); String json="["; bool first=true;
    for(size_t i=0;i<MAX_SAVED_PROFILES;i++){
      String name=preferences.getString(profileNameKey(i).c_str(),""); if(name.isEmpty()) continue;
      if(!first) json+=","; first=false;
      json+="{\"slot\":"+String(i)+",\"name\":\""+jsonEscape(name)+"\",\"active\":"+String(i==active?"true":"false")+"}";
    }
    json+="]"; server.send(200,"application/json",json);
  });

  server.on("/api/profiles", HTTP_POST, [] {
    if (!authenticate()) return;
    if (runState == RunState::RUNNING || runState == RunState::PAUSED) {server.send(409,"text/plain","Stop the kiln before changing profiles");return;}
    String action=server.arg("action"); int slot=server.arg("slot").toInt();
    if(action=="save"){
      if(slot<0||slot>=static_cast<int>(MAX_SAVED_PROFILES)){
        slot=-1; for(size_t i=0;i<MAX_SAVED_PROFILES;i++) if(preferences.getString(profileNameKey(i).c_str(),"").isEmpty()){slot=i;break;}
        if(slot<0){server.send(409,"text/plain","Profile library is full (maximum 8)");return;}
      }
      String name=server.arg("name"); name.trim(); if(name.isEmpty()){server.send(400,"text/plain","Profile name is required");return;} if(name.length()>32) name=name.substring(0,32);
      String error; String text=profileTextFromJsonArray(server.arg("data"),error); if(text.isEmpty()){server.send(400,"text/plain",error);return;}
      preferences.putString(profileNameKey(slot).c_str(),name); preferences.putString(profileDataKey(slot).c_str(),text);
      server.send(200,"application/json","{\"success\":true,\"slot\":"+String(slot)+"}"); return;
    }
    if(slot<0||slot>=static_cast<int>(MAX_SAVED_PROFILES)){server.send(400,"text/plain","Invalid slot");return;}
    if(action=="select"){
      String error; if(!activateProfileSlot(slot,error)){server.send(400,"text/plain",error);return;}
      publishMqttStatus(); server.send(200,"application/json","{\"success\":true}"); return;
    }
    if(action=="delete"){
      if(static_cast<size_t>(slot)==preferences.getUInt("activeSlot",0)){server.send(409,"text/plain","Select another profile before deleting the active profile");return;}
      preferences.remove(profileNameKey(slot).c_str()); preferences.remove(profileDataKey(slot).c_str()); server.send(200,"application/json","{\"success\":true}"); return;
    }
    server.send(400,"text/plain","Unknown action");
  });

  server.on("/api/profile", HTTP_GET, [] {
    if (!authenticate()) return;
    server.send(200, "text/plain", preferences.getString("profile", defaultProfile()));
  });

  server.on("/api/profile", HTTP_POST, [] {
    if (!authenticate()) return;
    if (runState == RunState::RUNNING || runState == RunState::PAUSED) {
      server.send(409, "text/plain", "Stop the kiln before changing its profile");
      return;
    }
    String text = server.arg("data");
    ProfilePoint candidate[MAX_PROFILE_POINTS];
    size_t candidateCount = 0;
    String error;
    if (!parseProfile(text, candidate, candidateCount, error)) {
      server.send(400, "text/plain", error);
      return;
    }
    preferences.putString("profile", text);
    size_t activeSlot = preferences.getUInt("activeSlot", 0);
    preferences.putString(profileDataKey(activeSlot).c_str(), text);
    memcpy(profile, candidate, sizeof(ProfilePoint) * candidateCount);
    profileCount = candidateCount;
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/settings", HTTP_GET, [] {
    if(!authenticate())return;
    String j="{";
    j+="\"hostname\":\""+jsonEscape(settings.hostname)+"\",\"language\":\""+jsonEscape(settings.language)+"\",\"wifi_ssid\":\""+jsonEscape(settings.wifiSsid)+"\",\"ap_name\":\""+jsonEscape(settings.apName)+"\",\"web_user\":\""+jsonEscape(settings.webUser)+"\",";
    j+="\"mqtt_enabled\":"+String(settings.mqttEnabled?"true":"false")+",\"mqtt_host\":\""+jsonEscape(settings.mqttHost)+"\",\"mqtt_port\":"+String(settings.mqttPort)+",\"mqtt_user\":\""+jsonEscape(settings.mqttUser)+"\",\"mqtt_topic\":\""+jsonEscape(settings.mqttTopic)+"\",\"mqtt_control\":"+String(settings.mqttControl?"true":"false")+",\"mqtt_start\":"+String(settings.mqttStart?"true":"false")+",";
    j+="\"influx_enabled\":"+String(settings.influxEnabled?"true":"false")+",\"influx_url\":\""+jsonEscape(settings.influxUrl)+"\",\"influx_org\":\""+jsonEscape(settings.influxOrg)+"\",\"influx_bucket\":\""+jsonEscape(settings.influxBucket)+"\",\"influx_interval\":"+String(settings.influxInterval)+",";
    j+="\"pushover_enabled\":"+String(settings.pushoverEnabled?"true":"false")+",\"ntp_server\":\""+jsonEscape(settings.ntpServer)+"\",\"timezone\":\""+jsonEscape(settings.timezone)+"\"}";
    server.send(200,"application/json",j);
  });
  server.on("/api/settings", HTTP_POST, [] {
    if(!authenticate())return; if(runState==RunState::RUNNING){server.send(409,"text/plain","Stop kiln before changing setup");return;}
    auto put=[&](const char* arg,const char* key,String& target){if(server.hasArg(arg)){target=server.arg(arg);preferences.putString(key,target);}};
    put("hostname","hostname",settings.hostname); settings.hostname.toLowerCase(); settings.hostname.replace(" ", "-"); if(settings.hostname.isEmpty()) settings.hostname="kiln"; preferences.putString("hostname",settings.hostname); put("language","language",settings.language); if(settings.language!="da") settings.language="en"; preferences.putString("language",settings.language);
    put("wifi_ssid","wifiSsid",settings.wifiSsid); if(server.hasArg("wifi_password")&&!server.arg("wifi_password").isEmpty()){settings.wifiPassword=server.arg("wifi_password");preferences.putString("wifiPass",settings.wifiPassword);}
    put("ap_name","apName",settings.apName); if(server.hasArg("ap_password")&&!server.arg("ap_password").isEmpty()){settings.apPassword=server.arg("ap_password");preferences.putString("apPass",settings.apPassword);}
    put("web_user","webUser",settings.webUser); if(server.hasArg("web_password")&&!server.arg("web_password").isEmpty()){settings.webPassword=server.arg("web_password");preferences.putString("webPass",settings.webPassword);}
    settings.mqttEnabled=server.hasArg("mqtt_enabled");preferences.putBool("mqttEn",settings.mqttEnabled);put("mqtt_host","mqttHost",settings.mqttHost);settings.mqttPort=server.arg("mqtt_port").toInt();if(!settings.mqttPort)settings.mqttPort=1883;preferences.putUShort("mqttPort",settings.mqttPort);put("mqtt_user","mqttUser",settings.mqttUser);if(server.hasArg("mqtt_password")&&!server.arg("mqtt_password").isEmpty()){settings.mqttPassword=server.arg("mqtt_password");preferences.putString("mqttPass",settings.mqttPassword);}put("mqtt_topic","mqttTopic",settings.mqttTopic);settings.mqttControl=server.hasArg("mqtt_control");settings.mqttStart=server.hasArg("mqtt_start");preferences.putBool("mqttCtl",settings.mqttControl);preferences.putBool("mqttStart",settings.mqttStart);
    settings.influxEnabled=server.hasArg("influx_enabled");preferences.putBool("influxEn",settings.influxEnabled);put("influx_url","influxUrl",settings.influxUrl);put("influx_org","influxOrg",settings.influxOrg);put("influx_bucket","influxBucket",settings.influxBucket);if(server.hasArg("influx_token")&&!server.arg("influx_token").isEmpty()){settings.influxToken=server.arg("influx_token");preferences.putString("influxToken",settings.influxToken);}long influxInterval = server.arg("influx_interval").toInt(); if (influxInterval < 5L) influxInterval = 5L; settings.influxInterval = (uint32_t)influxInterval; preferences.putUInt("influxInt", settings.influxInterval);
    settings.pushoverEnabled=server.hasArg("pushover_enabled");preferences.putBool("pushEn",settings.pushoverEnabled);if(server.hasArg("pushover_token")&&!server.arg("pushover_token").isEmpty()){settings.pushoverToken=server.arg("pushover_token");preferences.putString("pushToken",settings.pushoverToken);}if(server.hasArg("pushover_user")&&!server.arg("pushover_user").isEmpty()){settings.pushoverUser=server.arg("pushover_user");preferences.putString("pushUser",settings.pushoverUser);}put("ntp_server","ntp",settings.ntpServer);put("timezone","tz",settings.timezone);
    server.send(200,"application/json","{\"success\":true,\"reboot_required\":true}");
  });
  server.on("/api/reboot",HTTP_POST,[]{if(!authenticate())return;if(runState==RunState::RUNNING){server.send(409,"text/plain","Stop kiln before reboot");return;}server.send(200,"application/json","{\"success\":true}");delay(300);ESP.restart();});
  server.on("/update",HTTP_POST,[]{if(!authenticate())return;bool ok=!Update.hasError();server.send(ok?200:500,"text/plain",ok?"OK - rebooting":"Update failed");if(ok){delay(500);ESP.restart();}},[]{
    if(!server.authenticate(settings.webUser.c_str(),settings.webPassword.c_str()))return; HTTPUpload& up=server.upload();
    if(up.status==UPLOAD_FILE_START){if(runState==RunState::RUNNING||runState==RunState::PAUSED){Update.abort();return;}forceHeatOff();Update.begin(UPDATE_SIZE_UNKNOWN);}
    else if(up.status==UPLOAD_FILE_WRITE){if(!Update.hasError())Update.write(up.buf,up.currentSize);}
    else if(up.status==UPLOAD_FILE_END){if(!Update.hasError())Update.end(true);}
  });

  server.on("/api/update/check",HTTP_POST,[]{if(!authenticate())return;if(runState==RunState::RUNNING||runState==RunState::PAUSED){server.send(409,"text/plain","Update checks are deferred while the kiln is firing");return;}bool ok=checkFirmwareUpdate();server.send(ok?200:502,ok?"application/json":"text/plain",ok?String("{\"success\":true,\"latest\":\"")+jsonEscape(latestFirmwareVersion)+"\",\"available\":"+(firmwareUpdateAvailable?"true":"false")+"}":updateCheckError);});
  server.on("/api/update/install",HTTP_POST,[]{if(!authenticate())return;if(firmwareInstallRunning){server.send(409,"text/plain","Firmware update already in progress");return;}if(runState==RunState::RUNNING||runState==RunState::PAUSED){server.send(409,"text/plain","Stop the kiln before installing firmware");return;}firmwareInstallRunning=true;firmwareInstallProgress=0;firmwareInstallStage="starting";firmwareInstallMessage="Preparing firmware update";server.send(202,"application/json","{\"success\":true,\"started\":true}");xTaskCreatePinnedToCore(firmwareUpdateTask,"fw-update",8192,nullptr,1,nullptr,0);});
  server.on("/api/update/status",HTTP_GET,[]{if(!authenticate())return;String j="{\"running\":"+String(firmwareInstallRunning?"true":"false")+",\"progress\":"+String(firmwareInstallProgress)+",\"stage\":\""+jsonEscape(firmwareInstallStage)+"\",\"message\":\""+jsonEscape(firmwareInstallMessage)+"\"}";server.send(200,"application/json",j);});

  server.on("/api/control", HTTP_POST, [] {
    if (!authenticate()) return;
    String command = server.arg("cmd");
    String error;
    if (!executeCommand(command, false, error)) {
      server.send(409, "text/plain", error);
      return;
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
}

void connectNetwork() {
  WiFi.mode(WIFI_AP_STA); bool connected=false;
  if(!settings.wifiSsid.isEmpty()){WiFi.begin(settings.wifiSsid.c_str(),settings.wifiPassword.c_str());Serial.printf("Connecting to %s",settings.wifiSsid.c_str());for(int i=0;i<30&&WiFi.status()!=WL_CONNECTED;i++){delay(500);Serial.print('.');}connected=WiFi.status()==WL_CONNECTED;}
  String apPass=settings.apPassword; if(apPass.length()<8)apPass="kilnsetup"; WiFi.softAP(settings.apName.c_str(),apPass.c_str()); Serial.printf("\\nAP: http://%s\\n",WiFi.softAPIP().toString().c_str());
  if(connected){Serial.printf("Wi-Fi: http://%s\\n",WiFi.localIP().toString().c_str());setenv("TZ",settings.timezone.c_str(),1);tzset();configTime(0,0,settings.ntpServer.c_str(),"time.cloudflare.com");}
}

String mqttTopic(const char* suffix) {
  return settings.mqttTopic + "/" + suffix;
}

void publishMqttStatus() {
  if (!mqttClient.connected()) return;
  String json = "{\"state\":\"" + String(stateName(runState)) + "\",\"temperature_c\":";
  json += isfinite(currentTemperatureC) ? String(currentTemperatureC, 2) : String("null");
  json += ",\"target_c\":" + String(targetTemperatureC, 2);
  json += ",\"heating_rate_c_h\":" + String(heatingRateCPerHour, 1);
  json += ",\"duty_percent\":" + String(outputDuty * 100.0f, 1);
  json += ",\"elapsed_minutes\":" + String(profileElapsedSeconds / 60.0, 2);
  json += ",\"sensor_ok\":" + String(sensorHealthy ? "true" : "false");
  json += ",\"fault\":\"" + jsonEscape(faultMessage) + "\"";
    json += ",\"profile\":\"" + jsonEscape(activeProfileName) + "\"}";
  mqttClient.publish(mqttTopic("status").c_str(), json.c_str(), true);
  String profileJson = "{\"name\":\"" + jsonEscape(activeProfileName) + "\",\"data\":" + profileDataJson(preferences.getString("profile", defaultProfile())) + "}";
  mqttClient.publish("kiln/profile_json", profileJson.c_str(), true);
  mqttClient.publish(mqttTopic("state").c_str(), stateName(runState), true);
  mqttClient.publish(mqttTopic("temperature_c").c_str(),
                     isfinite(currentTemperatureC) ? String(currentTemperatureC, 2).c_str() : "nan", true);
  mqttClient.publish(mqttTopic("target_c").c_str(), String(targetTemperatureC, 2).c_str(), true);
  mqttClient.publish(mqttTopic("duty_percent").c_str(), String(outputDuty * 100.0f, 1).c_str(), true);
  mqttClient.publish(mqttTopic("fault").c_str(), faultMessage.c_str(), true);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != mqttTopic("command")) return;
  String command;
  command.reserve(length);
  for (unsigned int i = 0; i < length; ++i) command += static_cast<char>(payload[i]);
  command.trim();
  command.toLowerCase();
  String error;
  bool ok = executeCommand(command, true, error);
  String result = ok ? "ok:" + command : "error:" + error;
  mqttClient.publish(mqttTopic("command_result").c_str(), result.c_str(), false);
  publishMqttStatus();
}

void maintainMqtt(uint32_t now) {
  if (!settings.mqttEnabled || settings.mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  if (!mqttClient.connected()) {
    if (now - lastMqttAttemptMs < 5000) return;
    lastMqttAttemptMs = now;
    uint64_t chipId = ESP.getEfuseMac();
    String clientId = "esp32-kiln-" + String(static_cast<uint32_t>(chipId), HEX);
    String availability = mqttTopic("availability");
    bool connected;
    if (!settings.mqttUser.isEmpty()) {
      connected = mqttClient.connect(clientId.c_str(), settings.mqttUser.c_str(), settings.mqttPassword.c_str(),
                                     availability.c_str(), 1, true, "offline");
    } else {
      connected = mqttClient.connect(clientId.c_str(), availability.c_str(), 1, true, "offline");
    }
    if (!connected) {
      Serial.printf("MQTT connection failed, state=%d\n", mqttClient.state());
      return;
    }
    mqttClient.publish(availability.c_str(), "online", true);
    mqttClient.subscribe(mqttTopic("command").c_str(), 1);
    publishMqttStatus();
  }
  mqttClient.loop();
  if (now - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS) {
    lastMqttPublishMs = now;
    publishMqttStatus();
  }
}

void tryAutomaticRestart() {
  if (!AUTO_RESTART || !preferences.getBool("running", false)) return;
  time_t saved = static_cast<time_t>(preferences.getULong64("epoch", 0));
  for (int i = 0; i < 20 && time(nullptr) < VALID_EPOCH; ++i) delay(250);
  time_t now = time(nullptr);
  if (saved >= VALID_EPOCH && now >= saved &&
      static_cast<uint32_t>(now - saved) <= AUTO_RESTART_WINDOW_SECONDS) {
    profileElapsedSeconds = preferences.getDouble("elapsed", 0.0);
    runState = RunState::PAUSED;
    faultMessage = "Power interruption recovered; inspect kiln, then press Resume";
  } else {
    checkpoint(false);
  }
}

}  // namespace


String openKilnWifiStatusJson() {
  String j = "{";
  j += "\"connected\":";
  j += (WiFi.status() == WL_CONNECTED ? "true" : "false");
  j += ",\"ssid\":\"";
  if (WiFi.status() == WL_CONNECTED) j += WiFi.SSID();
  j += "\",\"ip\":\"";
  if (WiFi.status() == WL_CONNECTED) j += WiFi.localIP().toString();
  j += "\",\"rssi\":";
  j += String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  j += "}";
  return j;
}


const char OPENKILN_MODERN_SETUP_PAGE[] PROGMEM = R"OPENKILNHTML(
<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenKiln · Setup</title>
<style>
:root{--blue:#1769ff;--ink:#111827;--muted:#64748b;--line:#dbe3ec;--bg:#f6f9fc;--card:#fff}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--bg);color:var(--ink)}
header{height:76px;background:#fff;border-bottom:1px solid var(--line);display:flex;align-items:center;padding:0 30px;gap:44px;position:sticky;top:0;z-index:20}
.brand{display:flex;align-items:center;gap:11px;font-size:27px;font-weight:800;white-space:nowrap}.brand svg{width:31px;height:39px}
.nav{display:flex;align-items:stretch;gap:32px;height:100%;flex:1}.nav a{display:flex;align-items:center;color:#334155;text-decoration:none;padding:0 3px;border-bottom:3px solid transparent}.nav a.active{color:var(--blue);font-weight:700;border-color:var(--blue)}
.wifi{position:relative;width:34px;height:34px;display:grid;place-items:center;cursor:default}.wifi svg{width:29px;height:29px;stroke:#16a34a}.wifi.off svg{stroke:#94a3b8}.tip{display:none;position:absolute;right:-8px;top:38px;background:#17202d;color:white;padding:12px 15px;border-radius:9px;min-width:220px;font-size:14px;line-height:1.6;box-shadow:0 8px 25px #0003}.wifi:hover .tip{display:block}
main{max-width:1420px;margin:0 auto;padding:0 28px 44px}h1{font-size:38px;margin:0 0 28px}.grid{display:grid;grid-template-columns:1.15fr .85fr;gap:22px}.card{background:#fff;border:1px solid var(--line);border-radius:16px;padding:26px;box-shadow:0 3px 12px #0f172a0a}.title{display:flex;align-items:center;gap:11px;font-size:26px;font-weight:800;border-bottom:1px solid var(--line);padding-bottom:18px;margin-bottom:20px}.ico{width:27px;height:27px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.row{display:grid;grid-template-columns:190px 1fr;align-items:center;gap:18px;margin:14px 0}.row input,.row select,.field{width:100%;padding:13px 15px;border:1px solid #cbd5e1;border-radius:9px;font-size:16px;background:#fff}.sep{height:1px;background:var(--line);margin:24px 0}
.btn{border:1px solid var(--blue);background:#fff;color:var(--blue);font-weight:700;border-radius:9px;padding:12px 18px;cursor:pointer}.btn.primary{background:var(--blue);color:#fff}.btn:disabled{opacity:.5}.status{color:var(--muted);font-size:14px;margin-top:7px}.apline{display:flex;gap:9px;align-items:center}.dot{width:12px;height:12px;border-radius:50%;background:#94a3b8}.dot.on{background:#22c55e}
details.advanced>summary{display:flex;align-items:center;gap:11px;font-size:26px;font-weight:800;cursor:pointer;list-style:none;border-bottom:1px solid var(--line);padding-bottom:18px;margin-bottom:8px}details summary::-webkit-details-marker{display:none}
.chev{margin-left:auto;transition:.2s}.advanced[open]>.advhead .chev{transform:rotate(180deg)}
.advitem{border:1px solid var(--line);border-radius:12px;margin-top:14px;overflow:hidden}.advitem>summary{display:flex;align-items:center;gap:14px;padding:18px;cursor:pointer;font-size:18px;font-weight:750;list-style:none}.advitem>summary .chev{margin-left:auto}.advitem[open]>summary .chev{transform:rotate(90deg)}.advbody{border-top:1px solid var(--line);padding:16px 18px;background:#fbfdff}.advbody label{display:block;font-size:13px;color:var(--muted);margin:10px 0 5px}.advbody input,.advbody select{width:100%;padding:11px 12px;border:1px solid #cbd5e1;border-radius:8px;background:white}
.progress{height:12px;background:#e5e7eb;border-radius:99px;overflow:hidden;margin:10px 0}.progress>div{height:100%;width:0;background:var(--blue);transition:width .2s}
@media(max-width:900px){header{padding:0 14px;gap:14px}.nav{display:none}main{padding:0 14px 30px}.grid{grid-template-columns:1fr}.row{grid-template-columns:1fr;gap:5px}}
</style></head><body>
<header>
<div class="brand">
<svg viewBox="0 0 32 40" aria-hidden="true"><path fill="#ff7a00" d="M18 1c2 9-5 10-2 18 1-5 5-7 6-12 7 7 10 15 8 22-2 7-8 11-15 11C7 40 1 34 2 26 3 19 9 16 9 9c5 4 6 8 5 12 4-5 1-12 4-20z"/><path fill="#ffd43b" d="M17 18c1 5-3 6-1 10 1-3 3-4 4-7 4 4 5 8 4 12-1 4-4 6-8 6-5 0-8-3-8-7 0-4 4-6 4-10 3 2 3 5 3 7 2-3 1-7 2-11z"/></svg>
OpenKiln</div>
<nav class="nav"><a href="/">Dashboard</a><a href="/profiles">Program</a><a href="/">Status</a><a href="/system">System</a><a href="/setup" class="active">Setup</a></nav>
<div id="wifi" class="wifi off">
<svg viewBox="0 0 24 24"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10.5 10.5 0 0 1 14 0M8.5 15.5a5.3 5.3 0 0 1 7 0M12 19h.01"/></svg>
<div id="wifiTip" class="tip">Wi‑Fi disconnected</div></div>
</header>
<main><h1>Setup</h1><div class="grid">
<section class="card">
<div class="title"><svg class="ico" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 .6 1.7 1.7 0 0 0-.4 1.1V21h-4v-.1A1.7 1.7 0 0 0 8.6 19.4a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-.6-1 1.7 1.7 0 0 0-1.1-.4H3v-4h.1A1.7 1.7 0 0 0 4.6 8.6a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-.6 1.7 1.7 0 0 0 .4-1.1V3h4v.1A1.7 1.7 0 0 0 15.4 4.6a1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.4 9c.13.37.34.7.6 1 .3.27.7.4 1.1.4h.1v4h-.1a1.7 1.7 0 0 0-1.7.6z"/></svg>General</div>
<div class="row"><label>Wi‑Fi SSID</label><input id="ssid"></div>
<div class="row"><label>Wi‑Fi Password</label><input id="wifiPassword" type="password"></div>
<div class="row"><label>Hostname</label><input id="hostname" value="kiln"></div>
<div class="row"><label>Language</label><select id="language"><option value="en">English</option><option value="da">Dansk</option></select></div>
<div class="row"><label>Access</label><select id="access"><option value="none">No login</option><option value="login">Login required</option></select></div>
<div class="sep"></div>
<div class="row"><label>Firmware version</label><div><strong id="fwVersion">)OPENKILNHTML" OPENKILN_VERSION R"OPENKILNHTML(</strong> &nbsp;<button id="checkUpdate" class="btn">↻ Check for update</button><div id="updateStatus" class="status"></div><div id="progressWrap" class="progress" hidden><div id="progressBar"></div></div></div></div>
<div class="sep"></div>
<div class="title" style="font-size:20px;border:0;padding:0;margin:0 0 12px"><svg class="ico" viewBox="0 0 24 24"><path d="M12 20v-8M8.5 20h7M5.6 16.4a9 9 0 0 1 0-8.8M18.4 7.6a9 9 0 0 1 0 8.8M8.5 13.5a4 4 0 0 1 0-3M15.5 10.5a4 4 0 0 1 0 3"/><circle cx="12" cy="8" r="1"/></svg>Access Point</div>
<div class="row"><label>Status</label><div><div class="apline"><span id="apDot" class="dot"></span><strong id="apStatus">Disabled</strong></div><div id="apInfo" class="status">AP is disabled while connected to local Wi‑Fi.</div></div></div>
<div class="row"><span></span><button id="apButton" class="btn primary">Enable Access Point</button></div>
</section>

<section class="card">
<details class="advanced" open><summary class="advhead">
<svg class="ico" viewBox="0 0 24 24"><path d="M12 22a2 2 0 0 0 2-2h-4a2 2 0 0 0 2 2zM18 8a6 6 0 1 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9"/></svg>
Advanced <span class="chev">⌃</span></summary>

<details class="advitem"><summary><svg class="ico" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>Timezone<span class="chev">›</span></summary><div class="advbody">
<select id="timezone"><option value="Europe/Copenhagen">Denmark (Europe/Copenhagen)</option><option value="Europe/London">United Kingdom</option><option value="Europe/Berlin">Central Europe</option><option value="UTC">UTC</option></select>
<div class="status">Denmark automatically handles CET/CEST daylight saving time.</div></div></details>

<details class="advitem"><summary><svg class="ico" viewBox="0 0 24 24"><circle cx="6" cy="12" r="2"/><circle cx="18" cy="6" r="2"/><circle cx="18" cy="18" r="2"/><path d="m8 11 8-4M8 13l8 4"/></svg>MQTT<span class="chev">›</span></summary><div class="advbody">
<label>Broker / Host</label><input id="mqttHost"><label>Port</label><input id="mqttPort" type="number" value="1883"><label>Username</label><input id="mqttUser"><label>Password</label><input id="mqttPassword" type="password"></div></details>

<details class="advitem"><summary><svg class="ico" viewBox="0 0 24 24"><ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v7c0 1.7 3.6 3 8 3s8-1.3 8-3V5M4 12v7c0 1.7 3.6 3 8 3s8-1.3 8-3v-7"/></svg>InfluxDB<span class="chev">›</span></summary><div class="advbody">
<label>URL / Host</label><input id="influxHost"><label>Database / Bucket</label><input id="influxDb"><label>Token / Password</label><input id="influxToken" type="password"></div></details>

<details class="advitem"><summary><svg class="ico" viewBox="0 0 24 24"><path d="M18 8a6 6 0 1 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9"/><path d="M10 21h4"/></svg>Pushover<span class="chev">›</span></summary><div class="advbody">
<label>User key</label><input id="pushoverUser"><label>Application token</label><input id="pushoverToken" type="password"></div></details>

<details class="advitem"><summary><svg class="ico" viewBox="0 0 24 24"><path d="M12 16V4M8 8l4-4 4 4M5 14v5h14v-5"/></svg>Firmware Update<span class="chev">›</span></summary><div class="advbody">
<input type="file" id="firmwareFile" accept=".bin"><div style="height:10px"></div><button id="manualUpload" class="btn">Upload firmware</button><div id="manualStatus" class="status"></div></div></details>
</details></section></div></main>

<script>
const $=id=>document.getElementById(id);
async function getJSON(url,opt){const r=await fetch(url,Object.assign({cache:'no-store'},opt||{}));if(!r.ok)throw new Error(r.status);return r.json()}
async function wifiStatus(){try{const d=await getJSON('/api/wifi-status');$('wifi').classList.toggle('off',!d.connected);$('wifiTip').innerHTML=d.connected?`<b>Wi‑Fi connected</b><br>SSID: ${d.ssid||'—'}<br>IP: ${d.ip||'—'}<br>Signal: ${d.rssi||0} dBm`:'Wi‑Fi disconnected';}catch(e){}}
async function toggleAP(){const on=$('apDot').classList.contains('on');try{await getJSON(on?'/api/ap/disable':'/api/ap/enable',{method:'POST'});setAP(!on,!on?'Enabled manually':'Disabled')}catch(e){$('apInfo').textContent='Unable to change Access Point state.'}}
function setAP(on,label){$('apDot').classList.toggle('on',on);$('apStatus').textContent=label;$('apButton').textContent=on?'Disable Access Point':'Enable Access Point'}
$('apButton').onclick=toggleAP;
$('checkUpdate').onclick=async()=>{const b=$('checkUpdate');b.disabled=true;$('updateStatus').textContent='Checking for update…';try{const d=await getJSON('/api/update/check',{method:'POST'});$('updateStatus').textContent=d.message||d.status||'Update check complete.'}catch(e){$('updateStatus').textContent='Update check failed.'}b.disabled=false};
$('manualUpload').onclick=async()=>{const f=$('firmwareFile').files[0];if(!f)return $('manualStatus').textContent='Select firmware.bin first.';$('manualStatus').textContent='Uploading firmware…';const fd=new FormData();fd.append('firmware',f);try{const r=await fetch('/update',{method:'POST',body:fd});$('manualStatus').textContent=r.ok?'Firmware uploaded. OpenKiln is rebooting…':'Firmware upload failed.'}catch(e){$('manualStatus').textContent='Connection closed while OpenKiln rebooted.'}};
wifiStatus();setInterval(wifiStatus,10000);
</script></body></html>
)OPENKILNHTML";


const char OPENKILN_SYSTEM_PAGE[] PROGMEM = R"SYSHTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenKiln · System</title>
<style>
:root{--blue:#1769ff;--ink:#111827;--muted:#64748b;--line:#dbe3ec;--bg:#f6f9fc}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:var(--bg);color:var(--ink)}
header{height:76px;background:#fff;border-bottom:1px solid var(--line);display:flex;align-items:center;padding:0 30px;gap:44px}.brand{display:flex;align-items:center;gap:11px;font-size:27px;font-weight:800}.brand svg{width:31px;height:39px}.nav{display:flex;gap:32px;height:100%;align-items:center}.nav a{text-decoration:none;color:#334155}.nav .active{color:var(--blue);font-weight:700}
main{max-width:1200px;margin:34px auto;padding:0 24px}h1{font-size:38px}.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:20px}.card{background:#fff;border:1px solid var(--line);border-radius:16px;padding:24px;box-shadow:0 3px 12px #0f172a0a}.card h2{margin:0 0 18px}.bar{height:14px;background:#e5e7eb;border-radius:99px;overflow:hidden;margin:12px 0}.bar div{height:100%;width:0;background:var(--blue);transition:width .35s}.big{font-size:34px;font-weight:800}.muted{color:var(--muted)}.kv{display:grid;grid-template-columns:1fr auto;gap:10px;padding:9px 0;border-bottom:1px solid #eef2f7}.kv:last-child{border:0}.wide{grid-column:1/-1}
@media(max-width:760px){.grid{grid-template-columns:1fr}.wide{grid-column:auto}.nav{display:none}}
</style></head><body>
<header><div class="brand"><svg viewBox="0 0 32 40"><path fill="#ff7a00" d="M18 1c2 9-5 10-2 18 1-5 5-7 6-12 7 7 10 15 8 22-2 7-8 11-15 11C7 40 1 34 2 26 3 19 9 16 9 9c5 4 6 8 5 12 4-5 1-12 4-20z"/><path fill="#ffd43b" d="M17 18c1 5-3 6-1 10 1-3 3-4 4-7 4 4 5 8 4 12-1 4-4 6-8 6-5 0-8-3-8-7 0-4 4-6 4-10 3 2 3 5 3 7 2-3 1-7 2-11z"/></svg>OpenKiln</div>
<nav class="nav"><a href="/">Dashboard</a><a href="/profiles">Program</a><a href="/">Status</a><a href="/system" class="active">System</a><a href="/setup">Setup</a></nav></header>
<main><h1>System</h1><div class="grid">
<section class="card"><h2>RAM / Heap</h2><div class="big"><span id="heapPct">—</span>% used</div><div class="bar"><div id="heapBar"></div></div><div class="kv"><span>Total</span><b id="heapTotal">—</b></div><div class="kv"><span>Used</span><b id="heapUsed">—</b></div><div class="kv"><span>Free</span><b id="heapFree">—</b></div><div class="kv"><span>Minimum free since boot</span><b id="heapMin">—</b></div><div class="kv"><span>Largest free block</span><b id="heapMax">—</b></div></section>
<section class="card"><h2>Flash</h2><div class="big"><span id="flashPct">—</span>% used</div><div class="bar"><div id="flashBar"></div></div><div class="kv"><span>Flash size</span><b id="flashTotal">—</b></div><div class="kv"><span>Firmware sketch</span><b id="sketchUsed">—</b></div><div class="kv"><span>Free sketch space</span><b id="sketchFree">—</b></div></section>
<section class="card wide"><h2>OpenKiln</h2><div class="kv"><span>Firmware</span><b id="version">—</b></div><div class="kv"><span>Uptime</span><b id="uptime">—</b></div><div class="kv"><span>CPU frequency</span><b id="cpu">—</b></div><div class="kv"><span>Wi‑Fi SSID</span><b id="ssid">—</b></div><div class="kv"><span>IP address</span><b id="ip">—</b></div><div class="kv"><span>Wi‑Fi signal</span><b id="rssi">—</b></div></section>
</div><p class="muted">Live values update every 5 seconds.</p></main>
<script>
const $=x=>document.getElementById(x), kb=n=>(n/1024).toFixed(1)+' KB', mb=n=>(n/1048576).toFixed(2)+' MB';
function uptime(sec){let d=Math.floor(sec/86400);sec%=86400;let h=Math.floor(sec/3600);sec%=3600;let m=Math.floor(sec/60);return (d?d+'d ':'')+h+'h '+m+'m '+(sec%60)+'s'}
async function load(){try{let r=await fetch('/api/system',{cache:'no-store'}),d=await r.json();let used=d.heapTotal-d.heapFree,p=d.heapTotal?Math.round(used*100/d.heapTotal):0;$('heapPct').textContent=p;$('heapBar').style.width=p+'%';$('heapTotal').textContent=kb(d.heapTotal);$('heapUsed').textContent=kb(used);$('heapFree').textContent=kb(d.heapFree);$('heapMin').textContent=kb(d.heapMinFree);$('heapMax').textContent=kb(d.heapLargest);
let fp=d.flashSize?Math.round(d.sketchSize*100/d.flashSize):0;$('flashPct').textContent=fp;$('flashBar').style.width=fp+'%';$('flashTotal').textContent=mb(d.flashSize);$('sketchUsed').textContent=mb(d.sketchSize);$('sketchFree').textContent=mb(d.freeSketch);$('version').textContent=d.version;$('uptime').textContent=uptime(d.uptime);$('cpu').textContent=d.cpuMHz+' MHz';$('ssid').textContent=d.ssid||'Not connected';$('ip').textContent=d.ip||'—';$('rssi').textContent=d.connected?d.rssi+' dBm':'—'}catch(e){}}
load();setInterval(load,5000);
</script></body></html>
)SYSHTML";

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SSR, OUTPUT);
  setRelay(false);  // Fail-safe state before doing anything else.
  preferences.begin("kiln", false);
  loadSettings();
  loadProfile();
  beginThermocouple();
  connectNetwork();
  mqttClient.setServer(settings.mqttHost.c_str(), settings.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(512);
  installWebRoutes();
  
  server.on("/api/wifi-status", HTTP_GET, []() {
    server.send(200, "application/json", openKilnWifiStatusJson());
  });

  
  server.on("/api/ap/enable", HTTP_POST, []() {
    WiFi.mode(WIFI_AP_STA);
    bool ok = WiFi.softAP("OpenKiln-Setup");
    server.send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true,\"enabled\":true}" : "{\"ok\":false}");
  });

  server.on("/api/ap/disable", HTTP_POST, []() {
    WiFi.softAPdisconnect(true);
    if (WiFi.status() == WL_CONNECTED) WiFi.mode(WIFI_STA);
    server.send(200, "application/json", "{\"ok\":true,\"enabled\":false}");
  });

  
  server.on("/setup", HTTP_GET, []() {
    server.send_P(200, "text/html", OPENKILN_MODERN_SETUP_PAGE);
  });

  
  server.on("/system", HTTP_GET, []() {
    server.send_P(200, "text/html", OPENKILN_SYSTEM_PAGE);
  });

  server.on("/api/system", HTTP_GET, []() {
    String j = "{";
    j += "\"heapTotal\":" + String(ESP.getHeapSize());
    j += ",\"heapFree\":" + String(ESP.getFreeHeap());
    j += ",\"heapMinFree\":" + String(ESP.getMinFreeHeap());
    j += ",\"heapLargest\":" + String(ESP.getMaxAllocHeap());
    j += ",\"flashSize\":" + String(ESP.getFlashChipSize());
    j += ",\"sketchSize\":" + String(ESP.getSketchSize());
    j += ",\"freeSketch\":" + String(ESP.getFreeSketchSpace());
    j += ",\"cpuMHz\":" + String(ESP.getCpuFreqMHz());
    j += ",\"uptime\":" + String(millis() / 1000UL);
    j += ",\"version\":\"" OPENKILN_VERSION "\"";
    j += ",\"connected\":";
    j += (WiFi.status() == WL_CONNECTED ? "true" : "false");
    j += ",\"ssid\":\"";
    if (WiFi.status() == WL_CONNECTED) j += WiFi.SSID();
    j += "\",\"ip\":\"";
    if (WiFi.status() == WL_CONNECTED) j += WiFi.localIP().toString();
    j += "\",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    j += "}";
    server.send(200, "application/json", j);
  });

  server.begin();
  tryAutomaticRestart();
  ssrWindowStartMs = millis();
  Serial.println("Kiln controller ready");
}

void loop() {
  const uint32_t now = millis();
  server.handleClient();
  maintainMqtt(now);
  if(runState!=RunState::RUNNING&&runState!=RunState::PAUSED&&WiFi.status()==WL_CONNECTED){
    bool due=(!firstUpdateCheckDone&&now>=30000UL)||(firstUpdateCheckDone&&(uint32_t)(now-lastUpdateCheckMs)>=UPDATE_CHECK_INTERVAL_MS);
    if(due){lastUpdateCheckMs=now;firstUpdateCheckDone=true;checkFirmwareUpdate();}
  }
  if(settings.influxEnabled && now-lastInfluxMs >= settings.influxInterval*1000UL){lastInfluxMs=now;writeInflux();}
  updateSensor(now);
  if (now - lastControlMs >= CONTROL_INTERVAL_MS) {
    const float controlDt = lastControlMs == 0 ? CONTROL_INTERVAL_MS / 1000.0f
                                               : (now - lastControlMs) / 1000.0f;
    lastControlMs = now;
    updateController(now, controlDt);
  }
  updateRelay(now);
  if ((runState == RunState::RUNNING || runState == RunState::PAUSED) &&
      now - lastCheckpointMs >= CHECKPOINT_INTERVAL_MS) {
    lastCheckpointMs = now;
    checkpoint(true);
  }
  delay(2);
}
