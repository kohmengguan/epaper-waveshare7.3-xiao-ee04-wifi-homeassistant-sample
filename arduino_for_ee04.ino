#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include "driver/rtc_io.h"

// ── Verified pins for EE04──────────────────────────────────────────────────────
#define EPD_CS    44
#define EPD_RST   38
#define EPD_DC    10
#define EPD_BUSY   4
#define EPD_SCK    7
#define EPD_MOSI   9
#define EPD_PWR   43
// 4 hours in microseconds
#define TIME_TO_SLEEP  14400000000ULL 

// EE04 S3 Button GPIOs
#define KEY1 GPIO_NUM_2
#define KEY2 GPIO_NUM_3
#define KEY3 GPIO_NUM_5

// ── WiFi & HA ─────────────────────────────────────────────────────────────────
const char* ssid     = "wifi ssid";
const char* password = "wifi password";
const char* server   = "ha ip"; 
const int   port     = 8123;
const char* token    = "your_ha_token";

// ── Geometry ──────────────────────────────────────────────────────────────────
#define EPD_W        800
#define EPD_H        480
#define ROW_BYTES    400

// ── SPI helpers ───────────────────────────────────────────────────────────────
void sendCmd(uint8_t cmd) {
    digitalWrite(EPD_DC, LOW);
    digitalWrite(EPD_CS, LOW);
    SPI.transfer(cmd);
    digitalWrite(EPD_CS, HIGH);
}
void sendData(uint8_t d) {
    digitalWrite(EPD_DC, HIGH);
    digitalWrite(EPD_CS, LOW);
    SPI.transfer(d);
    digitalWrite(EPD_CS, HIGH);
}
void sendBuf(const uint8_t* buf, size_t len) {
    digitalWrite(EPD_DC, HIGH);
    digitalWrite(EPD_CS, LOW);
    SPI.writeBytes(buf, len);
    digitalWrite(EPD_CS, HIGH);
}

// BUSY HIGH = ready, LOW = working  (from Hilko's lcd_chkstatus)
void waitBusy(const char* tag = "") {
    delay(100);
    Serial.printf("[BUSY] %s ", tag);
    unsigned long t = millis();
    while (digitalRead(EPD_BUSY) == LOW) {
        delay(10);
        if (millis() - t > 60000) { Serial.println("TIMEOUT"); return; }
    }
    Serial.printf("ready (%lu ms)\n", millis() - t);
}

void epd_init() {
    Serial.println("[INIT] start");
    digitalWrite(EPD_PWR, HIGH); delay(100);
    digitalWrite(EPD_RST, LOW);  delay(10);
    digitalWrite(EPD_RST, HIGH); delay(10);
    waitBusy("reset");

    sendCmd(0xAA);                                              // CMDH
    sendData(0x49); sendData(0x55); sendData(0x20);
    sendData(0x08); sendData(0x09); sendData(0x18);

    sendCmd(0x01); sendData(0x3F);                             // PWRR
    sendCmd(0x00); sendData(0x5F); sendData(0x69);             // PSR
    sendCmd(0x03);                                             // POFS
    sendData(0x00); sendData(0x54); sendData(0x00); sendData(0x44);
    sendCmd(0x06);                                             // BTST1
    sendData(0x40); sendData(0x1F); sendData(0x1F); sendData(0x2C);
    sendCmd(0x08);                                             // BTST2
    sendData(0x6F); sendData(0x1F); sendData(0x17); sendData(0x49);
    sendCmd(0x09);                                             // BTST3
    sendData(0x6F); sendData(0x1F); sendData(0x1F); sendData(0x22);
    sendCmd(0x30); sendData(0x08);                             // PLL
    sendCmd(0x50); sendData(0x3F);                             // CDI
    sendCmd(0x60); sendData(0x02); sendData(0x00);             // TCON
    sendCmd(0x61);                                             // TRES 800x480
    sendData(0x03); sendData(0x20);
    sendData(0x01); sendData(0xE0);
    sendCmd(0xE7); sendData(0x01);                             // T_VDCS
    sendCmd(0xE3); sendData(0x2F);                             // PWS

    sendCmd(0x04);                                             // PON
    waitBusy("power_on");
    Serial.println("[INIT] done");
}

void epd_sleep() {
    sendCmd(0x02); sendData(0x00);
    waitBusy("power_off");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Stream image using HTTPClient (handles chunked transfer, reconnects, etc.)
//  output.bin = 192,000 bytes, pre-packed 4-bit nibbles, 400 bytes per row.
//  Bytes are sent straight to SPI with no conversion.
// ─────────────────────────────────────────────────────────────────────────────
void streamToEPD() {
    HTTPClient http;
    http.setTimeout(15000);
    String url = String("http://") + server + ":" + port + "/local/eink_frame/output.bin";
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + token);

    int code = http.GET();
    Serial.printf("[HTTP] GET → %d  size=%d\n", code, http.getSize());
    if (code != 200) { http.end(); return; }

    WiFiClient* stream = http.getStreamPtr();
    stream->setTimeout(10000);  // blocking readBytes timeout in ms

    sendCmd(0x10);  // DTM — data start transmission

    uint8_t row[ROW_BYTES];
    int rowsDone = 0;
    int totalReceived = 0;

    for (int y = 0; y < EPD_H; y++) {
        size_t got = stream->readBytes(row, ROW_BYTES);
        if (got != ROW_BYTES) {
            Serial.printf("[STREAM] short read at row %d: got %d\n", y, (int)got);
            http.end();
            return;
        }
        // Pre-packed nibbles go straight to SPI — no conversion
        sendBuf(row, ROW_BYTES);
        totalReceived += got;
        rowsDone++;
        if (y % 60 == 0) Serial.printf("  row %d / %d  (%d bytes)\n", y, EPD_H, totalReceived);
    }

    http.end();
    Serial.printf("[STREAM] done — %d rows, %d bytes\n", rowsDone, totalReceived);
}

void setup() {
    Serial.begin(115200);
    unsigned long startTimestamp = millis();
    while (!Serial && (millis() - startTimestamp < 3000));
    Serial.println("\n--- EE04 HA Streamer ---");

    pinMode(EPD_PWR,  OUTPUT); digitalWrite(EPD_PWR, LOW);
    pinMode(EPD_CS,   OUTPUT); digitalWrite(EPD_CS,  HIGH);
    pinMode(EPD_DC,   OUTPUT); digitalWrite(EPD_DC,  HIGH);
    pinMode(EPD_RST,  OUTPUT); digitalWrite(EPD_RST, HIGH);
    pinMode(EPD_BUSY, INPUT);

    SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));

    // 1. WiFi
    WiFi.begin(ssid, password);
    Serial.print("[WiFi] connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\n[WiFi] connected");

    // 2. Trigger HA dithering script
    {
        HTTPClient h;
        h.begin(String("http://") + server + ":" + port + "/api/services/pyscript/process_next_image");
        h.addHeader("Authorization", String("Bearer ") + token);
        h.addHeader("Content-Type", "application/json");
        int rc = h.POST("{}");
        Serial.printf("[HA] trigger → %d\n", rc);
        h.end();
    }

    // Poll output.bin size until it's exactly 192,000 bytes (fully written)
    // Avoids the race condition of a fixed delay — works regardless of HA processing speed
    Serial.println("[WAIT] polling for output.bin...");
    {
        const int EXPECTED_BYTES = 192000;
        const int POLL_INTERVAL_MS = 1000;
        const int MAX_WAIT_MS = 60000;
        unsigned long started = millis();
        bool ready = false;

        while (millis() - started < MAX_WAIT_MS) {
            HTTPClient h;
            h.begin(String("http://") + server + ":" + port + "/local/eink_frame/output.bin");
            h.addHeader("Authorization", String("Bearer ") + token);
            int rc = h.GET();
            int sz = h.getSize();
            h.end();
            Serial.printf("  [POLL] status=%d size=%d\n", rc, sz);
            if (rc == 200 && sz == EXPECTED_BYTES) {
                ready = true;
                break;
            }
            delay(POLL_INTERVAL_MS);
        }

        if (!ready) {
            Serial.println("[WAIT] timeout — output.bin not ready, aborting.");
            esp_deep_sleep_start();
        }
        Serial.println("[WAIT] output.bin ready.");
    }

    // 3. Init EPD
    epd_init();

    // 4. Stream image
    streamToEPD();

    // 5. Refresh — delay(1) before waitBusy is required (from Hilko's PIC_display)
    Serial.println("[EPD] refresh (~30s)...");
    sendCmd(0x12); sendData(0x00);
    delay(1);
    waitBusy("refresh");

    // 6. Power off + sleep
    epd_sleep();
    digitalWrite(EPD_PWR, LOW);
    SPI.endTransaction();

    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP);
    uint64_t wakeup_pin_mask = (1ULL << KEY1) | (1ULL << KEY2) | (1ULL << KEY3);

    esp_sleep_enable_ext1_wakeup(wakeup_pin_mask, ESP_EXT1_WAKEUP_ANY_LOW);

    rtc_gpio_pullup_en(KEY1);
    rtc_gpio_pullup_en(KEY2);
    rtc_gpio_pullup_en(KEY3);

    Serial.println("[DONE] sleeping.");
    Serial.flush();
    
    delay(100);
    esp_deep_sleep_start();
}

void loop() {}
