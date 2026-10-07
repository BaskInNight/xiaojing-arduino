#include "DisplayDriver.h"
#include "WashExecutor.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "custom_font_cjk.h"

// ==========================================
// 液态玻璃配色方案
// ==========================================
#define CLR_BG_DEEP     0x0a0e1a   // 最深背景
#define CLR_GLASS_BG    0x1a2a40   // 玻璃卡片背景
#define CLR_GLASS_BORDER 0x3a5a80  // 玻璃边缘光
#define CLR_GLOW_CYAN   0x00d4ff   // 主强调色
#define CLR_GLOW_WHITE  0xe0eaff   // 柔光白
#define CLR_TEXT_DIM     0x607090  // 暗文字

// ==========================================
// 硬件对象
// ==========================================
static TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[320 * 12];

// ==========================================
// 页面与全局状态
// ==========================================
static lv_obj_t* pages[PAGE_COUNT] = {NULL, NULL, NULL};
static int currentPage = PAGE_STATUS;
static int pageBeforeVoice = PAGE_STATUS;

static VoiceState_t currentVoiceState = VOICE_IDLE;
static uint32_t voiceStateTime = 0;

// ==========================================
// UI 对象
// ==========================================
// Page 1
static lv_obj_t* card_p1;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_posture;
static lv_obj_t* lbl_water;
static lv_obj_t* lbl_turbidity;
static lv_obj_t* lbl_ips;
static lv_obj_t* lbl_wifi;
static lv_obj_t* lbl_wash_state;
static lv_obj_t* lbl_hint_1;

// Page 2
static lv_obj_t* bar_progress;
static lv_obj_t* lbl_progress_pct;
static lv_obj_t* lbl_step_name;
static lv_obj_t* lbl_queue;
static lv_obj_t* lbl_posture_2;
static lv_obj_t* lbl_hint_2;

// Page 3
static lv_obj_t* circle_outer;
static lv_obj_t* circle_inner;
static lv_obj_t* lbl_voice_state;
static lv_obj_t* lbl_voice_text;
static lv_obj_t* lbl_hint_3;
static lv_anim_t breath_anim;
static bool anim_running = false;

// ==========================================
// LVGL flush 回调
// ==========================================
static void disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

// ==========================================
// 液态玻璃辅助函数
// ==========================================

// 创建玻璃卡片容器
static lv_obj_t* create_glass_card(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(CLR_GLASS_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(CLR_GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// 创建玻璃风标签
static lv_obj_t* create_glass_label(lv_obj_t* parent, const char* text, lv_color_t color, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

// UTF-8 安全截断：最多 maxChars 个 Unicode 字符，超长加 "..."
static void truncateUtf8(char* dst, size_t dstSize, const char* src, int maxChars) {
    int chars = 0;
    const char* p = src;
    const char* lastSafe = src;
    while (*p && chars < maxChars) {
        unsigned char c = (unsigned char)*p;
        int len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (p + len > src + dstSize - 4) break;  // 空间不够
        lastSafe = p + len;
        p += len;
        chars++;
    }
    size_t copyLen = (size_t)(lastSafe - src);
    if (*p != '\0') {
        // 截断了，加 "..."
        if (copyLen + 4 > dstSize) copyLen = dstSize - 4;
        memcpy(dst, src, copyLen);
        strcpy(dst + copyLen, "...");
    } else {
        memcpy(dst, src, copyLen);
        dst[copyLen] = '\0';
    }
}

// 底部引导栏
static lv_obj_t* create_hint_label(lv_obj_t* parent) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, &custom_font_cjk, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_DIM), 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    return lbl;
}

static void update_hint(lv_obj_t* lbl, bool isWashing, int page, int total) {
    if (isWashing) {
        lv_label_set_text_fmt(lbl, "下挥停止  EC11停止          %d/%d", page, total);
    } else {
        lv_label_set_text_fmt(lbl, "上挥启动  EC11启动          %d/%d", page, total);
    }
}

// ==========================================
// 创建 Page 1: 系统状态总览
// ==========================================
static void build_page_status() {
    lv_obj_t* scr = pages[PAGE_STATUS];
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG_DEEP), 0);

    // 标题 — 悬浮在卡片外
    lv_obj_t* title = create_glass_label(scr, "小净智能洗衣机", lv_color_hex(CLR_GLOW_CYAN), &custom_font_cjk);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

    // BLE — 悬浮在右上角
    lbl_ble_status = create_glass_label(scr, LV_SYMBOL_WIFI " BLE", lv_color_hex(0xff4444), &lv_font_montserrat_16);
    lv_obj_align(lbl_ble_status, LV_ALIGN_TOP_RIGHT, -12, 9);

    // 玻璃卡片：传感器数据
    card_p1 = create_glass_card(scr, 8, 34, 304, 100);

    // 姿态 + 水位
    lbl_posture = create_glass_label(card_p1, "姿态: --", lv_color_hex(CLR_GLOW_WHITE), &custom_font_cjk);
    lv_obj_align(lbl_posture, LV_ALIGN_TOP_LEFT, 4, 2);

    lbl_water = create_glass_label(card_p1, "水位: --", lv_color_hex(CLR_GLOW_WHITE), &custom_font_cjk);
    lv_obj_align(lbl_water, LV_ALIGN_TOP_RIGHT, -4, 2);

    // 浊度
    lbl_turbidity = create_glass_label(card_p1, "浊度: -- NTU", lv_color_hex(CLR_GLOW_WHITE), &custom_font_cjk);
    lv_obj_align(lbl_turbidity, LV_ALIGN_TOP_LEFT, 4, 28);

    // IPS
    lbl_ips = create_glass_label(card_p1, "IPS: 0:--  90:--  180:--  BLDC:--", lv_color_hex(0x8899bb), &custom_font_cjk);
    lv_obj_align(lbl_ips, LV_ALIGN_TOP_LEFT, 4, 58);

    // 玻璃卡片：连接状态
    lv_obj_t* card_conn = create_glass_card(scr, 8, 140, 304, 50);

    lbl_wifi = create_glass_label(card_conn, "WiFi: --", lv_color_hex(CLR_GLOW_WHITE), &custom_font_cjk);
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, 4, 0);

    lbl_wash_state = create_glass_label(card_conn, "洗涤: 空闲", lv_color_hex(0x44ff44), &custom_font_cjk);
    lv_obj_align(lbl_wash_state, LV_ALIGN_RIGHT_MID, -4, 0);

    // 引导
    lbl_hint_1 = create_hint_label(scr);
}

// ==========================================
// 创建 Page 2: 洗涤进度
// ==========================================
static void build_page_wash() {
    lv_obj_t* scr = pages[PAGE_WASH];
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG_DEEP), 0);

    // 标题
    lv_obj_t* title = create_glass_label(scr, "洗涤进度", lv_color_hex(CLR_GLOW_CYAN), &custom_font_cjk);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 进度百分比 — 与进度条垂直居中对齐
    lbl_progress_pct = create_glass_label(scr, "0%", lv_color_hex(CLR_GLOW_WHITE), &custom_font_cjk);
    lv_obj_align(lbl_progress_pct, LV_ALIGN_TOP_LEFT, 250, 38);

    // 水平进度条 — 玻璃风格
    bar_progress = lv_bar_create(scr);
    lv_obj_set_size(bar_progress, 220, 22);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x1a2a40), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, 11, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_progress, lv_color_hex(CLR_GLASS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(bar_progress, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_progress, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(CLR_GLOW_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 11, LV_PART_INDICATOR);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_LEFT, 16, 38);

    // 玻璃卡片：洗涤详情
    lv_obj_t* card_info = create_glass_card(scr, 12, 76, 296, 100);

    // 当前步骤名（大字）
    lbl_step_name = create_glass_label(card_info, "当前: 待机", lv_color_hex(CLR_GLOW_WHITE), &custom_font_cjk);
    lv_obj_align(lbl_step_name, LV_ALIGN_TOP_LEFT, 4, 4);

    // 姿态
    lbl_posture_2 = create_glass_label(card_info, "姿态: --", lv_color_hex(0x8899bb), &custom_font_cjk);
    lv_obj_align(lbl_posture_2, LV_ALIGN_TOP_RIGHT, -4, 8);

    // 队列预览（小字，显示后续步骤流程）
    lbl_queue = create_glass_label(card_info, "", lv_color_hex(0x607090), &custom_font_cjk);
    lv_obj_set_width(lbl_queue, 272);
    lv_label_set_long_mode(lbl_queue, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_queue, LV_ALIGN_TOP_LEFT, 4, 36);

    // 引导
    lbl_hint_2 = create_hint_label(scr);
}

// ==========================================
// 呼吸圈动画
// ==========================================
static void breath_size_cb(void* obj, int32_t val) {
    lv_obj_set_size((lv_obj_t*)obj, val, val);
    lv_obj_align((lv_obj_t*)obj, LV_ALIGN_TOP_MID, 0, 38 + (70 - val) / 2);
    // 内圈跟随外圈圆心，保持同心
    lv_obj_align(circle_inner, LV_ALIGN_TOP_MID, 0, 38 + (val - 52) / 2);
    int32_t opa = 200 - (val - 70) * 2;
    if (opa < 30) opa = 30;
    lv_obj_set_style_opa((lv_obj_t*)obj, opa, 0);
}

static void start_breath_anim() {
    if (anim_running) return;
    anim_running = true;
    lv_anim_init(&breath_anim);
    lv_anim_set_var(&breath_anim, circle_outer);
    lv_anim_set_values(&breath_anim, 70, 115);
    lv_anim_set_time(&breath_anim, 1200);
    lv_anim_set_playback_time(&breath_anim, 1200);
    lv_anim_set_repeat_count(&breath_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&breath_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&breath_anim, breath_size_cb);
    lv_anim_start(&breath_anim);
}

static void stop_breath_anim() {
    if (!anim_running) return;
    anim_running = false;
    lv_anim_del(circle_outer, breath_size_cb);
    lv_obj_set_size(circle_outer, 70, 70);
    lv_obj_align(circle_outer, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_opa(circle_outer, LV_OPA_TRANSP, 0);
}

static void set_circle_color(lv_color_t color) {
    lv_obj_set_style_bg_color(circle_outer, color, 0);
    lv_obj_set_style_bg_color(circle_inner, color, 0);
}

// ==========================================
// 创建 Page 3: 语音助手
// ==========================================
static void build_page_voice() {
    lv_obj_t* scr = pages[PAGE_VOICE];
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG_DEEP), 0);

    // 标题
    create_glass_label(scr, "小净 语音助手", lv_color_hex(CLR_GLOW_CYAN), &custom_font_cjk);
    lv_obj_align(lv_obj_get_child(scr, -1), LV_ALIGN_TOP_MID, 0, 8);

    // 外圈呼吸光（初始透明）
    circle_outer = lv_obj_create(scr);
    lv_obj_set_size(circle_outer, 70, 70);
    lv_obj_set_style_radius(circle_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle_outer, lv_color_hex(CLR_GLOW_CYAN), 0);
    lv_obj_set_style_bg_opa(circle_outer, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(circle_outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(circle_outer, 0, 0);
    lv_obj_align(circle_outer, LV_ALIGN_TOP_MID, 0, 38);

    // 内圈 — 玻璃风：半透明背景 + 亮边
    circle_inner = lv_obj_create(scr);
    lv_obj_set_size(circle_inner, 52, 52);
    lv_obj_set_style_radius(circle_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle_inner, lv_color_hex(0x333355), 0);
    lv_obj_set_style_bg_opa(circle_inner, LV_OPA_50, 0);
    lv_obj_set_style_border_color(circle_inner, lv_color_hex(0x5577aa), 0);
    lv_obj_set_style_border_opa(circle_inner, LV_OPA_60, 0);
    lv_obj_set_style_border_width(circle_inner, 1, 0);
    lv_obj_align(circle_inner, LV_ALIGN_TOP_MID, 0, 47);

    // 状态文字
    lbl_voice_state = create_glass_label(scr, "等待唤醒", lv_color_hex(CLR_TEXT_DIM), &custom_font_cjk);
    lv_obj_align(lbl_voice_state, LV_ALIGN_TOP_MID, 0, 108);

    // 回复文本
    lbl_voice_text = create_glass_label(scr, "", lv_color_hex(0x99aabb), &custom_font_cjk);
    lv_obj_set_width(lbl_voice_text, 280);
    lv_label_set_long_mode(lbl_voice_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_voice_text, LV_ALIGN_TOP_MID, 0, 130);

    // 引导
    lbl_hint_3 = create_hint_label(scr);
}

// ==========================================
// 公开 API
// ==========================================

void DISP_Init() {
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(CLR_BG_DEEP);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 320 * 12);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 320;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    pages[PAGE_STATUS] = lv_obj_create(NULL);
    pages[PAGE_WASH]   = lv_obj_create(NULL);
    pages[PAGE_VOICE]  = lv_obj_create(NULL);

    build_page_status();
    build_page_wash();
    build_page_voice();

    lv_scr_load(pages[PAGE_STATUS]);
    Serial.println("✅ [显示] 液态玻璃风格 初始化完成");
}

VoiceState_t DISP_GetVoiceState() { return currentVoiceState; }

void DISP_Handler() {
    lv_timer_handler();
    uint32_t elapsed = millis() - voiceStateTime;
    if (currentVoiceState == VOICE_REPLY && elapsed > 6000) {
        // AI 回复显示 6 秒后自动切到 DONE
        currentVoiceState = VOICE_DONE;
        voiceStateTime = millis();
        stop_breath_anim();
        set_circle_color(lv_color_hex(0x333355));
    } else if (currentVoiceState == VOICE_DONE && elapsed > 2000) {
        currentVoiceState = VOICE_IDLE;
        if (currentPage == PAGE_VOICE) {
            lv_scr_load(pages[pageBeforeVoice]);
            currentPage = pageBeforeVoice;
        }
    }
}

void DISP_SwitchPage(bool next) {
    if (next) currentPage = (currentPage + 1) % PAGE_COUNT;
    else      currentPage = (currentPage - 1 + PAGE_COUNT) % PAGE_COUNT;
    lv_scr_load(pages[currentPage]);
}

void DISP_UpdateStatus(bool bleConnected, bool wifiConnected, bool isWashing) {
    if (currentPage != PAGE_STATUS) return;

    lv_label_set_text(lbl_ble_status, LV_SYMBOL_WIFI " BLE");
    lv_obj_set_style_text_color(lbl_ble_status,
        bleConnected ? lv_color_hex(0x44ff44) : lv_color_hex(0xff4444), 0);

    DrumPosture posture = EXE_GetCurrentPosture();
    const char* pStr = "--";
    if (posture == POS_0)   pStr = "0";
    if (posture == POS_90)  pStr = "90";
    if (posture == POS_180) pStr = "180";
    lv_label_set_text_fmt(lbl_posture, "姿态: %s deg", pStr);

    bool wf = DRV_IsWaterFull();
    lv_label_set_text_fmt(lbl_water, "水位: %s", wf ? "满" : "空");
    lv_obj_set_style_text_color(lbl_water, wf ? lv_color_hex(0x44ff44) : lv_color_hex(0xffaa44), 0);

    lv_label_set_text_fmt(lbl_turbidity, "浊度: %.0f NTU", DRV_GetTurbidity());

    SensorState_t s = DRV_GetSensorState();
    lv_label_set_text_fmt(lbl_ips, "IPS: 0:%s  90:%s  180:%s  BLDC:%s",
        s.ips_0 ? "ON" : "--", s.ips_90 ? "ON" : "--",
        s.ips_180 ? "ON" : "--", s.ips_bldc ? "ON" : "--");

    lv_label_set_text_fmt(lbl_wifi, "WiFi: %s", wifiConnected ? "已连接" : "未连接");
    lv_obj_set_style_text_color(lbl_wifi, wifiConnected ? lv_color_hex(0x44ff44) : lv_color_hex(0xff4444), 0);

    lv_label_set_text_fmt(lbl_wash_state, "洗涤: %s", isWashing ? "进行中" : "空闲");
    lv_obj_set_style_text_color(lbl_wash_state, isWashing ? lv_color_hex(0xffaa44) : lv_color_hex(0x44ff44), 0);

    update_hint(lbl_hint_1, isWashing, 1, PAGE_COUNT);
}

void DISP_UpdateWashProgress(int progress, const char* stepName, int queueSize, DrumPosture posture, bool isWashing) {
    if (currentPage != PAGE_WASH) return;

    lv_bar_set_value(bar_progress, progress, LV_ANIM_ON);
    lv_label_set_text_fmt(lbl_progress_pct, "%d%%", progress);

    // 当前步骤名
    const char* curStep = EXE_GetCurrentStepName();
    lv_label_set_text_fmt(lbl_step_name, "当前: %s", curStep);

    // 姿态
    const char* pStr = "--";
    if (posture == POS_0)   pStr = "0";
    if (posture == POS_90)  pStr = "90";
    if (posture == POS_180) pStr = "180";
    lv_label_set_text_fmt(lbl_posture_2, "姿态: %s deg", pStr);

    // 队列预览
    if (queueSize > 0) {
        char preview[64];
        EXE_GetQueuePreview(preview, sizeof(preview));
        lv_label_set_text_fmt(lbl_queue, "后续(%d步): %s", queueSize, preview);
    } else {
        lv_label_set_text(lbl_queue, "队列为空");
    }

    update_hint(lbl_hint_2, isWashing, 2, PAGE_COUNT);
}

void DISP_SetVoiceState(VoiceState_t state, const char* text) {
    currentVoiceState = state;
    voiceStateTime = millis();

    if (state == VOICE_AWAKE && currentPage != PAGE_VOICE) {
        pageBeforeVoice = currentPage;
        lv_scr_load(pages[PAGE_VOICE]);
        currentPage = PAGE_VOICE;
    }

    switch (state) {
        case VOICE_IDLE:
            stop_breath_anim();
            set_circle_color(lv_color_hex(0x333355));
            lv_obj_set_style_bg_opa(circle_inner, LV_OPA_50, 0);
            lv_obj_set_style_border_color(circle_inner, lv_color_hex(0x5577aa), 0);
            lv_label_set_text(lbl_voice_state, "等待唤醒");
            lv_obj_set_style_text_color(lbl_voice_state, lv_color_hex(CLR_TEXT_DIM), 0);
            lv_label_set_text(lbl_voice_text, "说 \"小净\" 唤醒助手");
            lv_obj_set_style_text_color(lbl_voice_text, lv_color_hex(0x556680), 0);
            break;

        case VOICE_AWAKE:
            set_circle_color(lv_color_hex(CLR_GLOW_CYAN));
            lv_obj_set_style_bg_opa(circle_inner, LV_OPA_60, 0);
            lv_obj_set_style_border_color(circle_inner, lv_color_hex(CLR_GLOW_CYAN), 0);
            start_breath_anim();
            lv_label_set_text(lbl_voice_state, "已唤醒");
            lv_obj_set_style_text_color(lbl_voice_state, lv_color_hex(CLR_GLOW_CYAN), 0);
            lv_label_set_text(lbl_voice_text, "请说出您的需求");
            lv_obj_set_style_text_color(lbl_voice_text, lv_color_hex(0x8899bb), 0);
            break;

        case VOICE_LISTENING:
            set_circle_color(lv_color_hex(CLR_GLOW_CYAN));
            start_breath_anim();
            lv_label_set_text(lbl_voice_state, "正在倾听...");
            lv_obj_set_style_text_color(lbl_voice_state, lv_color_hex(CLR_GLOW_CYAN), 0);
            break;

        case VOICE_THINKING:
            set_circle_color(lv_color_hex(0xffaa44));
            start_breath_anim();
            lv_label_set_text(lbl_voice_state, "思考中...");
            lv_obj_set_style_text_color(lbl_voice_state, lv_color_hex(0xffaa44), 0);
            lv_label_set_text(lbl_voice_text, "");
            break;

        case VOICE_REPLY: {
            stop_breath_anim();
            set_circle_color(lv_color_hex(0x44ff44));
            lv_obj_set_style_bg_opa(circle_inner, LV_OPA_60, 0);
            lv_obj_set_style_border_color(circle_inner, lv_color_hex(0x44ff44), 0);
            lv_label_set_text(lbl_voice_state, "小净回复");
            lv_obj_set_style_text_color(lbl_voice_state, lv_color_hex(0x44ff44), 0);
            if (text) {
                static char trunc_buf[200];
                truncateUtf8(trunc_buf, sizeof(trunc_buf), text, 40);
                lv_label_set_text_fmt(lbl_voice_text, "\"%s\"", trunc_buf);
            }
            lv_obj_set_style_text_color(lbl_voice_text, lv_color_hex(0x44ff44), 0);
            break;
        }

        case VOICE_DONE:
            stop_breath_anim();
            set_circle_color(lv_color_hex(0x333355));
            break;
    }

    if (state == VOICE_IDLE || state == VOICE_DONE) {
        update_hint(lbl_hint_3, EXE_IsWashing(), 3, PAGE_COUNT);
    } else {
        lv_label_set_text(lbl_hint_3, "语音交互中...          3/3");
    }
}

const char* DISP_GetCmdName(WashCmd_t cmd) {
    switch (cmd) {
        case CMD_WATER_IN:       return "进水";
        case CMD_WASH_PULSATOR:  return "波轮洗涤";
        case CMD_TILT_BARREL:    return "换筒";
        case CMD_WASH_DRUM:      return "滚筒洗涤";
        case CMD_SPIN_DRY:       return "脱水";
        case CMD_HOME_BLDC:      return "BLDC归位";
        case CMD_TURBIDITY_CHECK: return "浊度检测";
        default:                 return "待机";
    }
}
