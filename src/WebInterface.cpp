#include "WebInterface.h"
#include "HardwareDriver.h"
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

// 1. 定义全局标志位
volatile bool webControlActive = false;
volatile bool webStopActive = false;

// 2. 定义私有服务器对象
static WebServer server(80);

// 3. 系统状态缓存
static WebStatus_t currentStatus = {false, "待机", 0, 0, 0, "", "", false, false, false, false, false, 0.0, POS_UNKNOWN, false, 0, 0, 0};

const String INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>小净 控制中心</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:linear-gradient(180deg,#0a0e1a 0%,#101828 50%,#0a0e1a 100%);min-height:100vh;font-family:'Segoe UI','Microsoft YaHei',sans-serif;color:#e0eaff;padding:12px;display:flex;justify-content:center}
.wrap{width:100%;max-width:420px}
h1{font-size:20px;font-weight:700;color:#00d4ff;text-align:center;padding:8px 0 2px;text-shadow:0 0 16px rgba(0,212,255,0.3)}
.sub{text-align:center;color:#607090;font-size:11px;margin-bottom:12px}
.glass{background:rgba(26,42,64,0.55);backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);border:1px solid rgba(58,90,128,0.35);border-radius:14px;padding:12px 14px;margin-bottom:8px;box-shadow:0 2px 16px rgba(0,0,0,0.25),inset 0 1px 0 rgba(255,255,255,0.04)}
.glass h2{font-size:13px;font-weight:600;margin-bottom:8px;display:flex;align-items:center;gap:6px}
.glass h2 .dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0;font-size:13px}
.lbl{color:#607090}.val{color:#e0eaff;font-weight:500}.val.cyan{color:#00d4ff}.val.green{color:#44ff44}.val.amber{color:#ffaa44}
.sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:5px}
.sensor-item{display:flex;align-items:center;gap:5px;font-size:12px}
.sensor-dot{width:10px;height:10px;border-radius:50%;background:#1a2a40;border:1px solid rgba(58,90,128,0.3);transition:all 0.3s}
.sensor-dot.on{background:#44ff44;box-shadow:0 0 8px rgba(68,255,68,0.5);border-color:#44ff44}
.sensor-dot.blue{background:#4488ff;box-shadow:0 0 8px rgba(68,136,255,0.5);border-color:#4488ff}
.status-big{text-align:center;font-size:20px;font-weight:700;padding:6px 0}
.bar-wrap{height:22px;border-radius:11px;background:rgba(26,42,64,0.6);border:1px solid rgba(58,90,128,0.3);overflow:hidden;margin:6px 0;position:relative}
.bar-fill{height:100%;border-radius:11px;background:linear-gradient(90deg,rgba(0,180,255,0.7),rgba(0,212,255,0.9));box-shadow:0 0 10px rgba(0,212,255,0.25);transition:width 0.8s ease}
.bar-pct{position:absolute;right:8px;top:0;line-height:22px;font-size:12px;font-weight:700;color:#e0eaff;text-shadow:0 1px 2px rgba(0,0,0,0.5)}
.queue-text{color:#607090;font-size:11px;margin-top:4px;line-height:1.5}
.btns{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:8px}
.btn{border:none;border-radius:14px;padding:16px;font-size:16px;font-weight:700;cursor:pointer;width:100%;letter-spacing:1px}
.btn:active{transform:scale(0.97)}
.btn-start{background:linear-gradient(135deg,#1a6b4a,#22c55e);color:#fff;box-shadow:0 4px 20px rgba(34,197,94,0.3)}
.btn-stop{background:linear-gradient(135deg,#7f1d1d,#ef4444);color:#fff;box-shadow:0 4px 20px rgba(239,68,68,0.3)}
.foot{text-align:center;color:#3a5a80;font-size:11px;padding:10px 0}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}
.pulse{animation:pulse 1.5s ease-in-out infinite}
.toast{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);background:rgba(0,0,0,0.85);color:#fff;padding:12px 24px;border-radius:10px;font-size:14px;z-index:99;display:none}
</style>
</head>
<body>
<div class="wrap">
<h1>小净智能洗衣管家</h1>
<div class="sub">V3.2 · 液态玻璃控制面板</div>
<div class="glass">
  <div class="row"><span class="lbl">当前状态</span><span id="badge" class="val green">待机</span></div>
  <div id="stageBig" class="status-big cyan">等待唤醒</div>
  <div class="bar-wrap"><div id="progBar" class="bar-fill" style="width:0%"></div><span id="progPct" class="bar-pct">0%</span></div>
  <div class="row"><span class="lbl">当前步骤</span><span id="curStep" class="val">待机</span></div>
  <div class="row"><span class="lbl">已用时间</span><span id="elapsed" class="val">00:00</span></div>
  <div id="queueLine" class="queue-text"></div>
</div>
<div class="glass">
  <h2><span class="dot" style="background:#00d4ff"></span> 传感器</h2>
  <div class="sensor-grid">
    <div class="sensor-item"><div id="s0" class="sensor-dot"></div><span class="lbl">0° 接近</span></div>
    <div class="sensor-item"><div id="s90" class="sensor-dot"></div><span class="lbl">90° 接近</span></div>
    <div class="sensor-item"><div id="s180" class="sensor-dot"></div><span class="lbl">180° 接近</span></div>
    <div class="sensor-item"><div id="sbldc" class="sensor-dot"></div><span class="lbl">BLDC 归位</span></div>
    <div class="sensor-item"><div id="swater" class="sensor-dot"></div><span class="lbl">水位满</span></div>
    <div class="sensor-item"><span class="lbl">浊度</span><span id="turb" class="val" style="font-size:12px">0 NTU</span></div>
  </div>
</div>
<div class="glass">
  <h2><span class="dot" style="background:#ffaa44"></span> 电机</h2>
  <div class="row"><span class="lbl">外筒姿态</span><span id="posture" class="val amber">--</span></div>
  <div class="row"><span class="lbl">BLDC 电机</span><span id="bldc" class="val green">停止</span></div>
</div>
<div class="glass">
  <h2><span class="dot" style="background:#44ff44"></span> 系统</h2>
  <div class="row"><span class="lbl">WiFi</span><span id="wifi" class="val">-- dBm</span></div>
  <div class="row"><span class="lbl">堆内存</span><span id="heap" class="val">-- KB</span></div>
</div>
<div class="btns">
  <button class="btn btn-start" id="btnStart" onclick="startWash()">开始洗涤</button>
  <button class="btn btn-stop" onclick="stopWash()">紧急停止</button>
</div>
<div class="foot">小净 · <span id="ip">--</span></div>
</div>
<div id="toast" class="toast"></div>
<script>
var W=false;
var errCount=0;
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.style.display='block';setTimeout(function(){t.style.display='none';},1500);}
function api(u,c){var x=new XMLHttpRequest();x.open('GET',u,true);x.timeout=5000;
  x.onload=function(){errCount=0;if(c)c(x.responseText);};
  x.onerror=function(){errCount++;if(errCount>=3)toast('网络不稳定');};
  x.ontimeout=function(){errCount++;};
  x.send();}
function poll(){
  api('/api/status',function(t){
    try{var d=JSON.parse(t);}catch(e){return;}
    W=d.isWashing;
    var b=document.getElementById('badge'),s=document.getElementById('stageBig');
    if(W){b.className='val amber pulse';b.textContent='洗涤中';s.className='status-big amber';s.textContent=d.currentStepName||'执行中';}
    else{b.className='val green';b.textContent='待机';s.className='status-big cyan';s.textContent='等待唤醒';}
    var p=0;if(d.totalSteps>0)p=Math.round((d.totalSteps-d.queueRemaining)/d.totalSteps*100);
    document.getElementById('progBar').style.width=p+'%';
    document.getElementById('progPct').textContent=p+'%';
    document.getElementById('curStep').textContent=d.currentStepName||'待机';
    var m=Math.floor(d.elapsedTime/60),sc=d.elapsedTime%60;
    document.getElementById('elapsed').textContent=(m<10?'0':'')+m+':'+(sc<10?'0':'')+sc;
    var ql=document.getElementById('queueLine');
    if(W&&d.queueRemaining>0){ql.textContent='后续('+d.queueRemaining+'步): '+(d.queuePreview||'');ql.style.color='#607090';}
    else if(W){ql.textContent='最后一步';ql.style.color='#44ff44';}
    else{ql.textContent='';}
    document.getElementById('s0').className='sensor-dot'+(d.ips_0?' on':'');
    document.getElementById('s90').className='sensor-dot'+(d.ips_90?' on':'');
    document.getElementById('s180').className='sensor-dot'+(d.ips_180?' on':'');
    document.getElementById('sbldc').className='sensor-dot'+(d.ips_bldc?' on':'');
    document.getElementById('swater').className='sensor-dot'+(d.waterFull?' blue':'');
    document.getElementById('turb').textContent=d.turbidity.toFixed(1)+' NTU';
    var pm={0:'--',1:'0 deg',2:'90 deg',3:'180 deg'};
    document.getElementById('posture').textContent=pm[d.posture]||'--';
    document.getElementById('bldc').textContent=d.blcdRunning?'运行中':'停止';
    document.getElementById('bldc').className=d.blcdRunning?'val amber':'val green';
    document.getElementById('wifi').textContent=d.wifiRSSI+' dBm';
    document.getElementById('heap').textContent=(d.freeHeap/1024).toFixed(0)+' KB';
    var bs=document.getElementById('btnStart');
    if(bs){bs.textContent=W?'洗涤中...':'开始洗涤';bs.disabled=W;bs.style.opacity=W?'0.5':'1';}
  });
}
api('/api/ip',function(t){document.getElementById('ip').textContent=t;});
poll();setInterval(poll,3000);
function startWash(){toast('启动中...');api('/start',function(){setTimeout(poll,500);});}
function stopWash(){if(confirm('确定停止？')){toast('停止中...');api('/stop',function(){setTimeout(poll,500);});}}
</script>
</body>
</html>
)rawliteral";

// 5. Web 核心任务函数 (私有)
static void webServerTask(void *pvParameters) {
    // 挂载 mDNS: http://xiaojing.local
    for (int retry = 0; retry < 3; retry++) {
        if (MDNS.begin("xiaojing")) {
            MDNS.addService("http", "tcp", 80);
            Serial.println("🌐 [Web] mDNS 服务挂载成功: http://xiaojing.local");
            break;
        }
        Serial.printf("⚠️ [Web] mDNS 挂载失败，重试 %d/3...\n", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 打印当前 IP 地址
    Serial.print("🌐 [Web] HTTP 服务器已启动，您也可以直接访问 IP: http://");
    Serial.println(WiFi.localIP());

    // 主页面
    server.on("/", []() {
        Serial.println("🌐 [Web] 收到 / 请求");
        server.send(200, "text/html", INDEX_HTML);
    });

    // 启动洗涤
    server.on("/start", []() {
        Serial.println("🌐 [Web] 收到 /start 请求");
        webControlActive = true;
        server.send(200, "text/plain", "Start OK");
    });

    // 停止洗涤
    server.on("/stop", []() {
        Serial.println("🌐 [Web] 收到 /stop 请求");
        webStopActive = true;
        server.send(200, "text/plain", "Stop OK");
    });

    // API: 获取系统状态（snprintf手拼JSON，省掉ArduinoJson的堆分配）
    server.on("/api/status", []() {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"isWashing\":%s,\"currentStepName\":\"%s\",\"elapsedTime\":%lu,"
            "\"queueRemaining\":%d,\"totalSteps\":%d,\"queuePreview\":\"%s\","
            "\"ips_0\":%s,\"ips_90\":%s,\"ips_180\":%s,\"ips_bldc\":%s,"
            "\"waterFull\":%s,\"turbidity\":%.1f,\"posture\":%d,\"blcdRunning\":%s,"
            "\"freeHeap\":%lu,\"freePsram\":%lu,\"wifiRSSI\":%d}",
            currentStatus.isWashing ? "true" : "false",
            currentStatus.currentStepName ? currentStatus.currentStepName : "待机",
            currentStatus.elapsedTime,
            currentStatus.queueRemaining,
            currentStatus.totalSteps,
            currentStatus.queuePreview ? currentStatus.queuePreview : "",
            currentStatus.ips_0 ? "true" : "false",
            currentStatus.ips_90 ? "true" : "false",
            currentStatus.ips_180 ? "true" : "false",
            currentStatus.ips_bldc ? "true" : "false",
            currentStatus.waterFull ? "true" : "false",
            currentStatus.turbidity,
            (int)currentStatus.posture,
            currentStatus.blcdRunning ? "true" : "false",
            currentStatus.freeHeap,
            currentStatus.freePsram,
            currentStatus.wifiRSSI
        );
        server.send(200, "application/json", buf);
    });

    // API: 获取IP地址
    server.on("/api/ip", []() {
        server.send(200, "text/plain", WiFi.localIP().toString());
    });

    server.begin();

    while (true) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// 6. 网页子系统初始化接口 (公有)
void WEB_Init() {
    xTaskCreatePinnedToCore(webServerTask, "webServer", 8192, NULL, 1, NULL, 0);
}

// 7. 更新Web状态
void WEB_UpdateStatus(const WebStatus_t* status) {
    currentStatus = *status;
}