// Hola mundo para CYD (ESP32-2432S028)
// Prueba: serial (115200) + pantalla ILI9341. Sin SD, sin audio.

#include <Arduino_GFX_Library.h>

Arduino_DataBus *bus = new Arduino_ESP32SPI(2 /* DC */, 15 /* CS */, 14 /* SCK */, 13 /* MOSI */, 12 /* MISO */, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED /* RST */, 1 /* rotation */, false /* IPS */);

#define PIN_BL 21

int n = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("hola mundo: serial OK");

  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(3);
  gfx->setCursor(20, 100);
  gfx->println("HOLA MUNDO");
  Serial.println("hola mundo: pantalla OK");
}

void loop() {
  Serial.printf("hola mundo %d\n", n);
  gfx->fillRect(20, 160, 200, 30, 0x0000);
  gfx->setCursor(20, 160);
  gfx->printf("contador: %d", n);
  n++;
  delay(1000);
}
