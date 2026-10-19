// Prueba de microSD en CYD: monta la tarjeta, lista la raiz y muestra el resultado.
// Pantalla: "SD OK: N MB" y cuantos .avi hay, o el error. Serie (115200): lista de archivos.
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <SD.h>

Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 15, 14, 13, 12, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1, false);

#define PIN_BL 21
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

SPIClass sdSPI(VSPI);

void show(const char *a, const char *b = "", const char *c = "") {
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(8, 30);
  gfx->println(a);
  gfx->println(b);
  gfx->println(c);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);
  gfx->begin();
  show("Probando SD...");

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 40000000)) {
    Serial.println("ERROR: no se pudo montar la SD");
    show("ERROR: no monta SD");
    return;
  }

  uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("SD montada. Tamano: %llu MB\n", (unsigned long long)mb);

  int avi = 0, total = 0;
  File root = SD.open("/");
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    String n = e.name();
    String l = n;
    l.toLowerCase();
    bool isAvi = !e.isDirectory() && l.endsWith(".avi");
    Serial.printf("%s %s %lu bytes\n", e.isDirectory() ? "[DIR]" : "     ", n.c_str(), (unsigned long)e.size());
    total++;
    if (isAvi) avi++;
    e.close();
  }
  root.close();

  Serial.printf("Total en raiz: %d, .avi: %d\n", total, avi);
  char l1[40], l2[40];
  snprintf(l1, sizeof(l1), "SD OK: %llu MB", (unsigned long long)mb);
  snprintf(l2, sizeof(l2), "%d archivos .avi", avi);
  show(l1, l2, avi ? "" : "No hay .avi en la raiz");
}

void loop() {}
