// Prueba de audio aislada: tono continuo ~441 Hz por el DAC (GPIO26), sin AVI.
// Sirve para comprobar el parlante y el conector antes de tocar el video.
#include <Arduino_GFX_Library.h>
#include "driver/dac_continuous.h"
#include <math.h>

#define BLACK 0x0000
#define WHITE 0xFFFF
#define PIN_BL 21
#define AUDIO_RATE 22050
#define VOLUME 180  // 0-256

Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 15, 14, 13, 12, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1, false);
dac_continuous_handle_t dacH;
uint8_t toneBuf[50 * 10];  // 50 muestras por ciclo (22050 / 50 = 441 Hz), 10 ciclos por bloque

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);
  gfx->begin();
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, 100);
  gfx->println("TONO 441 Hz");

  for (int i = 0; i < 50 * 10; i++) {
    int s = 128 + (int)(100.0 * sin(2.0 * M_PI * (i % 50) / 50.0));
    s = 128 + (((s - 128) * VOLUME) >> 8);
    toneBuf[i] = (uint8_t)s;
  }

  dac_continuous_config_t cfg = {};
  cfg.chan_mask = DAC_CHANNEL_MASK_CH1;  // GPIO26
  cfg.desc_num = 4;
  cfg.buf_size = 2048;
  cfg.freq_hz = AUDIO_RATE;
  cfg.offset = 0;
  cfg.clk_src = DAC_DIGI_CLK_SRC_DEFAULT;
  cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;
  esp_err_t e1 = dac_continuous_new_channels(&cfg, &dacH);
  esp_err_t e2 = dac_continuous_enable(dacH);
  Serial.printf("DAC init: new=%d enable=%d\n", (int)e1, (int)e2);
}

void loop() {
  dac_continuous_write(dacH, toneBuf, sizeof(toneBuf), NULL, -1);
}
