#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <Preferences.h>
#include "fonts.h"

// ==================== PINES HUB12 ====================
static const int PIN_OE   = 27;
static const int PIN_DATA = 26;
static const int PIN_CLK  = 25;
static const int PIN_LOAD = 33;
static const int PIN_A    = 14;
static const int PIN_B    = 13;
static const int PIN_BTN  = 18;   // Pulsante de brillo (a GND, flanco de bajada)
static const int PIN_FONT = 19;   // Pulsante de fuente (a GND, flanco de bajada)

// ==================== SETTINGS DEL PANEL ====================
#define OE_ACTIVE_HIGH    1
#define DATA_ACTIVE_LOW   1

// ==================== CONSTANTES DEL DISPLAY ====================
#define MATRIX_WIDTH   32
#define MATRIX_HEIGHT  16
#define MATRIX_SCAN    4
#define ROWS_PER_SCAN  (MATRIX_HEIGHT / MATRIX_SCAN)
#define COL_CHUNK      8

// ==================== NTP / ZONA HORARIA ====================
const char* NTP_SERVER       = "pool.ntp.org";
const long  GMT_OFFSET_SEC   = -5 * 3600;
const int   DST_OFFSET_SEC   = 0;

// ==================== MAPEO DE FILAS ====================
static const uint8_t rowMap[MATRIX_SCAN][ROWS_PER_SCAN] = {
    {0, 4,  8, 12},
    {1, 5,  9, 13},
    {2, 6, 10, 14},
    {3, 7, 11, 15}
};

// ==================== FRAMEBUFFER ====================
static volatile uint32_t framebuffer[MATRIX_HEIGHT];

// ==================== TIMER DE BARRIDO ====================
// La ISR barre una fila (scan) cada SCAN_PERIOD_US microsegundos.
// Con 4 scans: refresco completo = 4 * SCAN_PERIOD_US -> 500 Hz a 500 us.
#define SCAN_PERIOD_US 500
static hw_timer_t* scanTimer = nullptr;
static volatile uint8_t currentScan = 0;

// ==================== BRILLO ====================
// PWM sobre OE dentro de cada scan: un segundo timer apaga OE tras
// el tiempo de duty correspondiente al nivel de brillo.
enum Brightness : uint8_t { BR_LOW = 0, BR_MED = 1, BR_HIGH = 2, BR_COUNT = 3 };
static const uint16_t dutyUs[BR_COUNT] = {
    SCAN_PERIOD_US / 5,   // bajo:  ~20%
    SCAN_PERIOD_US / 2,   // medio: ~50%
    SCAN_PERIOD_US        // alto:  100%
};
static volatile uint8_t brightness = BR_HIGH;
static hw_timer_t* oeTimer = nullptr;

// ==================== BOTÓN (ISR + antirrebote) ====================
#define BTN_DEBOUNCE_MS 200
static volatile bool btnPressed = false;
static volatile uint32_t lastBtnIsrMs = 0;

void IRAM_ATTR onBtnFalling() {
    uint32_t now = millis();
    if ((uint32_t)(now - lastBtnIsrMs) >= BTN_DEBOUNCE_MS) {
        lastBtnIsrMs = now;
        btnPressed = true;
    }
}

// ==================== BOTÓN DE FUENTE (ISR + antirrebote) ====================
static volatile bool fontBtnPressed = false;
static volatile uint32_t lastFontIsrMs = 0;
static volatile uint8_t fontIndex = 0;

void IRAM_ATTR onFontBtnFalling() {
    uint32_t now = millis();
    if ((uint32_t)(now - lastFontIsrMs) >= BTN_DEBOUNCE_MS) {
        lastFontIsrMs = now;
        fontBtnPressed = true;
    }
}

// ==================== VARIABLES GLOBALES ====================
WiFiManager wm;
Preferences prefs;

// ==================== DRIVER HUB12 ====================
inline void setData(bool bitVal) {
#if DATA_ACTIVE_LOW
    digitalWrite(PIN_DATA, bitVal ? LOW : HIGH);
#else
    digitalWrite(PIN_DATA, bitVal ? HIGH : LOW);
#endif
}

inline void setOE(bool on) {
#if OE_ACTIVE_HIGH
    digitalWrite(PIN_OE, on ? HIGH : LOW);
#else
    digitalWrite(PIN_OE, on ? LOW : HIGH);
#endif
}

inline void pulseClk() {
    digitalWrite(PIN_CLK, HIGH);
    __asm__ __volatile__ ("nop");
    digitalWrite(PIN_CLK, LOW);
}

inline void setRowAddr(uint8_t scan) {
    digitalWrite(PIN_A, (scan & 0x01) ? HIGH : LOW);
    digitalWrite(PIN_B, (scan & 0x02) ? HIGH : LOW);
}

void sendScanData(uint8_t scan) {
    for (int chunk = 0; chunk < (MATRIX_WIDTH / COL_CHUNK); chunk++) {
        for (int sub = ROWS_PER_SCAN - 1; sub >= 0; sub--) {
            uint8_t row = rowMap[scan][sub];
            int startCol = chunk * COL_CHUNK;
            uint32_t rowData = framebuffer[row];
            for (int bit = 0; bit < COL_CHUNK; bit++) {
                setData((rowData >> (startCol + bit)) & 1);
                pulseClk();
            }
        }
    }
}

void refreshScan(uint8_t scan) {
    sendScanData(scan);
    setOE(false);
    digitalWrite(PIN_LOAD, HIGH);
    setRowAddr(scan);
    digitalWrite(PIN_LOAD, LOW);
    setOE(true);
}

// ISR: barre el panel de forma autónoma, sin depender del loop()
void IRAM_ATTR onScanTimer() {
    refreshScan(currentScan);
    currentScan = (currentScan + 1) % MATRIX_SCAN;

    // Control de brillo: programa el apagado de OE según el duty activo.
    uint16_t duty = dutyUs[brightness];
    if (duty < SCAN_PERIOD_US) {
        timerAlarmWrite(oeTimer, duty, false);  // one-shot
        timerRestart(oeTimer);
        timerAlarmEnable(oeTimer);
    }
}

// ISR one-shot: fin del tiempo de encendido de OE (PWM de brillo)
void IRAM_ATTR onOeTimer() {
    setOE(false);
}

// ==================== DIBUJO ====================
inline void setPixel(int x, int y, bool on) {
    if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) return;
    if (on) framebuffer[y] |= (1UL << x);
    else    framebuffer[y] &= ~(1UL << x);
}

void clearFB() {
    for (int i = 0; i < MATRIX_HEIGHT; i++) framebuffer[i] = 0;
}

void drawChar(int x, int y, char c) {
    int idx = -1;
    if (c >= '0' && c <= '9') idx = c - '0';
    else if (c == ':') idx = 10;
    else return;

    const Font& f = fonts[fontIndex];
    const uint16_t* glyph = f.cols + idx * f.width;

    for (int col = 0; col < f.width; col++) {
        int px = x + col;
        if (px < 0 || px >= MATRIX_WIDTH) continue;
        uint16_t colData = glyph[col];
        for (int row = 0; row < f.height; row++) {
            int py = y + row;
            if (py < 0 || py >= MATRIX_HEIGHT) continue;
            if (colData & (1 << row)) setPixel(px, py, true);
        }
    }
}

void drawClock(uint8_t h, uint8_t m, bool showColon) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d%c%02d", h % 24, showColon ? ':' : ' ', m % 60);

    const Font& f = fonts[fontIndex];
    // Ancho total de "HH:MM": 5 caracteres con 1 px de separacion.
    // Si no cabe (fuente ancha), se dibujan sin separacion.
    int advance = f.width + 1;
    if (5 * advance - 1 > MATRIX_WIDTH) advance = f.width;
    int startX = (MATRIX_WIDTH - (5 * advance - 1)) / 2;
    if (startX < 0) startX = 0;
    int startY = (MATRIX_HEIGHT - f.height) / 2;
    if (startY < 0) startY = 0;

    clearFB();
    int cx = startX;
    for (const char* p = buf; *p; p++) {
        drawChar(cx, startY, *p);
        cx += advance;
    }
}

// ==================== NTP ====================
bool syncNTP() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 3000)) return false;
    
    Serial.printf("NTP OK: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    time_t now = time(nullptr);
    prefs.begin("clock", false);
    prefs.putLong("lastSync", now);
    prefs.end();
    
    return true;
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== HUB12 Reloj NTP ===");

    // Display
    pinMode(PIN_OE, OUTPUT);
    pinMode(PIN_DATA, OUTPUT);
    pinMode(PIN_CLK, OUTPUT);
    pinMode(PIN_LOAD, OUTPUT);
    pinMode(PIN_A, OUTPUT);
    pinMode(PIN_B, OUTPUT);

    setOE(true);
    digitalWrite(PIN_DATA, LOW);
    digitalWrite(PIN_CLK, LOW);
    digitalWrite(PIN_LOAD, LOW);
    digitalWrite(PIN_A, LOW);
    digitalWrite(PIN_B, LOW);

    // Matriz completamente apagada durante el arranque (ahorro de corriente).
    // Solo parpadeará 1 LED en el centro mientras no haya WiFi.
    clearFB();

    // Restaurar último brillo seleccionado desde NVS
    prefs.begin("clock", true);
    uint8_t savedBr = prefs.getUChar("brightness", BR_HIGH);
    uint8_t savedFont = prefs.getUChar("font", 0);
    prefs.end();
    if (savedBr < BR_COUNT) brightness = savedBr;
    if (savedFont < FONT_COUNT) fontIndex = savedFont;

    // Botón de brillo: interrupción por flanco de bajada (pulsante a GND)
    pinMode(PIN_BTN, INPUT_PULLUP);
    attachInterrupt(PIN_BTN, onBtnFalling, FALLING);

    // Botón de fuente: interrupción por flanco de bajada (pulsante a GND)
    pinMode(PIN_FONT, INPUT_PULLUP);
    attachInterrupt(PIN_FONT, onFontBtnFalling, FALLING);

    // Timer de apagado de OE para el PWM de brillo (one-shot, se rearma en cada scan)
    oeTimer = timerBegin(1, 80, true);
    timerAttachInterrupt(oeTimer, &onOeTimer, true);

    // Timer de barrido del panel (interrupción cada SCAN_PERIOD_US)
    scanTimer = timerBegin(0, 80, true);              // 80 MHz / 80 = 1 MHz (1 us por tick)
    timerAttachInterrupt(scanTimer, &onScanTimer, true);
    timerAlarmWrite(scanTimer, SCAN_PERIOD_US, true); // auto-reload
    timerAlarmEnable(scanTimer);

    // Restaurar hora desde NVS
    prefs.begin("clock", true);
    time_t lastSync = prefs.getLong("lastSync", 0);
    prefs.end();
    if (lastSync > 1609459200) {
        struct timeval tv = { .tv_sec = lastSync };
        settimeofday(&tv, NULL);
        Serial.println("Hora restaurada desde memoria");
    }

    // WiFiManager NO bloqueante, sin debug spam
    wm.setDebugOutput(false);  // <-- Desactiva prints internos de WiFiManager
    wm.setConfigPortalBlocking(false);
    wm.setConfigPortalTimeout(180);
    
    wm.setAPCallback([](WiFiManager *myWM) {
        Serial.println("AP: conectate a 'HUB12-Clock' y abre 192.168.4.1");
    });

    bool res = wm.autoConnect("HUB12-Clock");
    if (res) {
        Serial.print("WiFi conectado. IP: ");
        Serial.println(WiFi.localIP());
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
        delay(500);
        syncNTP();
    } else {
        Serial.println("WiFi no configurado. Portal AP activo.");
    }

    Serial.println("Setup completo.");
}

// ==================== LOOP ====================
// ==================== LOOP (CORREGIDO) ====================
void loop() {
    static uint32_t nextSecond = 0;
    static uint8_t lastHH = 255, lastMM = 255;
    static bool colonOn = false;
    static bool ntpDone = false;
    static uint32_t lastNtpTry = 0;
    static bool wifiPrev = false;
    static bool bootBlinkOn = false;
    static uint32_t nextBootBlink = 0;

    // 0) BOTÓN DE BRILLO: cicla bajo -> medio -> alto y persiste en NVS
    if (btnPressed) {
        btnPressed = false;
        brightness = (brightness + 1) % BR_COUNT;
        prefs.begin("clock", false);
        prefs.putUChar("brightness", brightness);
        prefs.end();
        Serial.printf("Brillo: %s\n",
            brightness == BR_LOW ? "BAJO" : (brightness == BR_MED ? "MEDIO" : "ALTO"));
    }

    // 0b) BOTÓN DE FUENTE: cicla entre las 3 fuentes disponibles y persiste en NVS
    if (fontBtnPressed) {
        fontBtnPressed = false;
        fontIndex = (fontIndex + 1) % FONT_COUNT;
        prefs.begin("clock", false);
        prefs.putUChar("font", fontIndex);
        prefs.end();
        nextSecond = 0;  // fuerza redibujado inmediato con la nueva fuente
        Serial.printf("Fuente: %s\n", fonts[fontIndex].name);
    }

    // 1) PARPADEO DE ARRANQUE: 1 LED en el centro de la matriz
    //    mientras no haya WiFi (mínimo consumo de corriente).
    bool wifiNow = (WiFi.status() == WL_CONNECTED);
    if (!wifiNow) {
        ntpDone = false;
        uint32_t ms0 = millis();
        if ((int32_t)(ms0 - nextBootBlink) >= 0) {
            nextBootBlink = ms0 + 500;
            bootBlinkOn = !bootBlinkOn;
            clearFB();
            if (bootBlinkOn) setPixel(MATRIX_WIDTH / 2, MATRIX_HEIGHT / 2, true);
        }
        wm.process();
        wifiPrev = false;
        return;
    }

    // 2) WiFiManager
    wm.process();

    // 3) Detectar reconexión WiFi
    if (!wifiPrev) {
        Serial.println("WiFi reconectado.");
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
        if (syncNTP()) ntpDone = true;
    }
    wifiPrev = wifiNow;

    // 4) Primer intento NTP si nunca se hizo
    if (wifiNow && !ntpDone && (millis() - lastNtpTry > 10000)) {
        lastNtpTry = millis();
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
        if (syncNTP()) ntpDone = true;
    }

    // 5) Re-sincronización cada 30 minutos
    if (wifiNow && ntpDone && (millis() - lastNtpTry > 30UL * 60UL * 1000UL)) {
        lastNtpTry = millis();
        if (!syncNTP()) {
            lastNtpTry -= 30UL * 60UL * 1000UL;
            lastNtpTry += 10000UL;
        }
    }

    // 6) ACTUALIZAR RELOJ (una vez por segundo)
    uint32_t ms = millis();
    if ((int32_t)(ms - nextSecond) >= 0) {
        nextSecond = ms + 1000;
        colonOn = !colonOn;

        uint8_t h, m, s;
        struct tm ti;
        if (getLocalTime(&ti, 50)) {
            h = ti.tm_hour;
            m = ti.tm_min;
            s = ti.tm_sec;
            ntpDone = true;
        } else {
            static uint8_t manualHH = 0, manualMM = 0, manualSS = 0;
            static bool manualInit = false;
            if (!manualInit) {
                manualInit = true;
                if (getLocalTime(&ti, 50)) {
                    manualHH = ti.tm_hour;
                    manualMM = ti.tm_min;
                    manualSS = ti.tm_sec;
                }
            }
            manualSS++;
            if (manualSS >= 60) { manualSS = 0; manualMM++; }
            if (manualMM >= 60) { manualMM = 0; manualHH++; }
            if (manualHH >= 24) manualHH = 0;
            h = manualHH; m = manualMM; s = manualSS;
        }

        // CORRECCIÓN: Redibujar SIEMPRE cada segundo para que los dos puntos parpadeen
        lastHH = h; lastMM = m;
        drawClock(h, m, colonOn);

        // Serial solo cuando cambia el minuto (para no spammear)
        if (s == 0) {
            Serial.printf("Hora: %02d:%02d | WiFi:%s | NTP:%s\n",
                h, m,
                wifiNow ? "OK" : "OFF",
                ntpDone ? "SYNC" : "PEND");
        }
    }
}