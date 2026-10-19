// Mini TV Simpson - ESP32-2432S028 (CYD)
// Menu de archivos .avi en la microSD + reproduccion MJPEG (video) + PCM u8 mono 22050 Hz (audio por DAC GPIO26)
//
// Librerias (Gestor de librerias): "GFX Library for Arduino" (moononournation) y "JPEGDEC" (bitbank2)
// Core ESP32 de Espressif: version 3.x (necesita driver/dac_continuous.h)
// Placa en el IDE: "ESP32 Dev Module"
//
// Convertir los videos con:
//   ffmpeg -y -i in.mp4 -ac 1 -ar 22050 -af loudnorm -c:a pcm_u8 -c:v mjpeg -q:v 7 -vf "fps=15,scale=320:240:flags=lanczos" S01E01.avi

#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include "driver/dac_continuous.h"
#include "freertos/stream_buffer.h"

// ---------- Configuracion ----------
#define AUTOPLAY_FIRST 0      // 1 = reproduce el primer .avi sin menu (modo prueba); 0 = menu
#define AUDIO_RATE 22050      // debe coincidir con -ar del ffmpeg
#define VOLUME_DEFAULT 5      // volumen inicial, 0-10 (pasos de 10 %)
#define VOLUME_MAX_GAIN 2.0   // ganancia con el volumen al 100 % (1.0 = el audio tal cual; mas puede distorsionar)
#define FRAME_MAX (40 * 1024) // tamano maximo de un frame JPEG
#define VIDEO_FPS 15          // fps de los .avi (debe coincidir con fps= del ffmpeg)
#define DISPLAY_SPI_HZ 40000000 // reloj SPI de la pantalla; 80 MHz daba lineas y franjas corruptas con el dibujo en paralelo
#define AUDIO_TARGET 2048     // bytes maximos en el buffer de audio (limita el desfase imagen-sonido)
#define SKIP_BYTES 300        // si hay menos audio que esto en cola, se salta el fotograma
#define MAX_FILES 64

// Colores RGB565 (la GFX Library 1.6.9 no los expone con estos nombres)
#define BLACK 0x0000
#define WHITE 0xFFFF
#define RED 0xF800

#define PIN_BL 21
#define PIN_BTN_NEXT 22       // Boton 1: siguiente
#define PIN_BTN_PLAY 27       // Boton 2: play / volver al menu

#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

// Touch resistivo XPT2046 (bus propio, se lee por software). Pines tipicos de la CYD: verificar.
#define T_CLK 25
#define T_MOSI 32
#define T_MISO 39
#define T_CS 33
#define T_IRQ 36
// El mapeo del touch se calibra solo con 3 puntos la primera vez (se guarda en la memoria de la
// placa). Para recalibrar, tocar la pantalla durante el primer segundo despues de encender.
// TOUCH_DEBUG 1 imprime las lecturas por serie y marca un punto rojo en el menu.
#define TOUCH_DEBUG 1

// Barra de controles (al medio de la pantalla, fondo al 50 %) y menu tactil
#define OV_H 56
#define OV_Y (240 - OV_H)     // pegada al borde inferior
#define OV_TIMEOUT 5000       // ms sin tocar para ocultar la barra
#define MENU_PER_PAGE 9
#define MENU_ROW_Y0 30
#define MENU_ROW_H 22

// ---------- Pantalla (ILI9341 en HSPI) ----------
// Si los colores salen invertidos, cambia el ultimo parametro (IPS) de false a true.
// Si la imagen sale girada, prueba rotation 3 en vez de 1.
Arduino_DataBus *bus = new Arduino_ESP32SPI(2 /* DC */, 15 /* CS */, 14 /* SCK */, 13 /* MOSI */, 12 /* MISO */, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED /* RST */, 1 /* rotation */, false /* IPS */);

SPIClass sdSPI(VSPI);
JPEGDEC jpeg;

uint8_t *frameBuf = nullptr;
volatile uint32_t gDrawUs = 0;  // microsegundos gastados dibujando en pantalla (tarea del core 0)
dac_continuous_handle_t dacH;

volatile int gVolLevel = VOLUME_DEFAULT;  // 0-10 (pasos de 10 %)
volatile bool ovVisible = false;          // barra de controles visible
Arduino_Canvas *ovCanvas = nullptr;
uint16_t *ovFb = nullptr;                 // pixeles de la barra (0 = transparente), orden nativo
static uint8_t silence[256];              // audio en silencio (128) para la pausa
volatile int gRawX = 0, gRawY = 0;        // ultima lectura cruda del touch (depuracion)

String files[MAX_FILES];
int nFiles = 0;
int sel = 0;

// ---------- Utilidades ----------
void msg(const String &s, uint16_t color = WHITE) {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(color);
  gfx->setCursor(8, 100);
  gfx->println(s);
}

bool pressed(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(25);
    if (digitalRead(pin) == LOW) {
      while (digitalRead(pin) == LOW) delay(5);
      return true;
    }
  }
  return false;
}

uint32_t le32(const uint8_t *b) {
  return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

bool readExact(File &f, void *buf, size_t n) {
  return f.read((uint8_t *)buf, n) == n;
}

// ---------- Audio ----------
// El audio va por una tarea en el core 0 que escribe al DAC en bloques FIJOS de 512 bytes
// (con trozos de tamano irregular el DAC se quedaba parado). El video corre en el core 1 y
// deposita el audio en un stream buffer; AUDIO_TARGET limita cuanto audio se adelanta.
#define AUDIO_BLOCK 512
StreamBufferHandle_t audioBuf;
uint32_t gDacTimeouts = 0;

void audioTask(void *) {
  static uint8_t blk[AUDIO_BLOCK];
  for (;;) {
    size_t have = 0;
    while (have < AUDIO_BLOCK)
      have += xStreamBufferReceive(audioBuf, blk + have, AUDIO_BLOCK - have, pdMS_TO_TICKS(50));
    const int g = (int)(gVolLevel * 256 * VOLUME_MAX_GAIN / 10);  // 256 = ganancia 1.0
    for (size_t i = 0; i < AUDIO_BLOCK; i++) {
      int s = 128 + (((int)blk[i] - 128) * g >> 8);
      blk[i] = (uint8_t)constrain(s, 0, 255);  // recorte: con ganancia > 1 los picos no deben desbordar
    }
    if (dac_continuous_write(dacH, blk, AUDIO_BLOCK, NULL, 100) != ESP_OK) gDacTimeouts++;
  }
}

void audioWrite(const uint8_t *data, size_t n) {
  while (xStreamBufferBytesAvailable(audioBuf) > AUDIO_TARGET) vTaskDelay(1);  // el audio marca el ritmo
  size_t sent = 0;
  while (sent < n) {
    size_t s = xStreamBufferSend(audioBuf, data + sent, n - sent, pdMS_TO_TICKS(200));
    if (!s) {
      gDacTimeouts++;
      break;
    }
    sent += s;
  }
}

void audioInit() {
  dac_continuous_config_t cfg = {};
  cfg.chan_mask = DAC_CHANNEL_MASK_CH1;  // ESP32 clasico: canal 1 = GPIO26
  cfg.desc_num = 4;
  cfg.buf_size = 2048;
  cfg.freq_hz = AUDIO_RATE;
  cfg.offset = 0;
  cfg.clk_src = DAC_DIGI_CLK_SRC_DEFAULT;
  cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;
  ESP_ERROR_CHECK(dac_continuous_new_channels(&cfg, &dacH));
  ESP_ERROR_CHECK(dac_continuous_enable(dacH));
  audioBuf = xStreamBufferCreate(16 * 1024, 1);
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 3, nullptr, 0);
}

// ---------- Video ----------
// El core 1 decodifica y copia cada bloque a una cola; una tarea en el core 0 los dibuja por SPI
// mientras se decodifica el siguiente. Solo esa tarea toca la pantalla durante la reproduccion.
#define DRAW_SLOTS 10
struct DrawBlk {
  int16_t x, y, w, h;
  uint16_t px[2048];  // JPEGDEC entrega bloques de 2048 pixeles como maximo
};
static DrawBlk drawBlks[DRAW_SLOTS];
QueueHandle_t qFree, qFull;  // indices de bloques libres / listos para dibujar

static inline uint16_t bs(uint16_t v) { return (v >> 8) | (v << 8); }

// Mezcla la barra de controles dentro del bloque: pixel de la barra si no es 0, y si es 0
// deja el video al ~62 % de su brillo (1/2 + 1/8), es decir, la barra es ~38 % opaca. Los bloques
// de video van en big-endian y la barra en orden nativo.
void ovBlend(int bx, int by, int bw, int bh, uint16_t *px) {
  int y0 = max(by, OV_Y), y1 = min(by + bh, OV_Y + OV_H);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *p = px + (yy - by) * bw;
    const uint16_t *o = ovFb + (yy - OV_Y) * 320 + bx;
    for (int xx = 0; xx < bw; xx++) {
      if (o[xx]) p[xx] = bs(o[xx]);
      else {
        uint16_t v = bs(p[xx]);
        p[xx] = bs(((v >> 1) & 0x7BEF) + ((v >> 3) & 0x18E3));
      }
    }
  }
}

int jpegDraw(JPEGDRAW *p) {
  uint8_t i;
  xQueueReceive(qFree, &i, portMAX_DELAY);
  DrawBlk &b = drawBlks[i];
  b.x = p->x;
  b.y = p->y;
  b.w = p->iWidth;
  b.h = p->iHeight;
  memcpy(b.px, p->pPixels, (size_t)p->iWidth * p->iHeight * 2);
  xQueueSend(qFull, &i, portMAX_DELAY);
  return 1;
}

void drawTask(void *) {
  for (;;) {
    uint8_t i;
    xQueueReceive(qFull, &i, portMAX_DELAY);
    uint32_t a = micros();
    DrawBlk &b = drawBlks[i];
    if (ovVisible && ovFb) ovBlend(b.x, b.y, b.w, b.h, b.px);
    gfx->draw16bitBeRGBBitmap(b.x, b.y, b.px, b.w, b.h);
    gDrawUs += micros() - a;
    xQueueSend(qFree, &i, portMAX_DELAY);
  }
}

void drawInit() {
  qFree = xQueueCreate(DRAW_SLOTS, sizeof(uint8_t));
  qFull = xQueueCreate(DRAW_SLOTS, sizeof(uint8_t));
  for (uint8_t i = 0; i < DRAW_SLOTS; i++) xQueueSend(qFree, &i, 0);
  xTaskCreatePinnedToCore(drawTask, "draw", 4096, nullptr, 2, nullptr, 0);
}

void drawWait() {  // esperar a que se dibuje todo lo encolado
  while (uxQueueMessagesWaiting(qFree) < DRAW_SLOTS) delay(1);
}

void drawFrame(size_t size) {
  if (jpeg.openRAM(frameBuf, size, jpegDraw)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    int x = (gfx->width() - jpeg.getWidth()) / 2;
    int y = (gfx->height() - jpeg.getHeight()) / 2;
    jpeg.decode(x, y, 0);
    jpeg.close();
  }
}

// ---------- Touch (XPT2046 por software, bus propio) ----------
void touchInit() {
  pinMode(T_CLK, OUTPUT);
  pinMode(T_MOSI, OUTPUT);
  pinMode(T_CS, OUTPUT);
  pinMode(T_MISO, INPUT);
  pinMode(T_IRQ, INPUT);
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_CLK, LOW);
}

// Convierte una orden (0xD0 = X, 0x90 = Y): 8 bits de orden y 16 de respuesta (busy + 12 bits + 3).
uint16_t xptRead(uint8_t cmd) {
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_MOSI, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
  }
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(T_CLK, HIGH);
    v = (v << 1) | digitalRead(T_MISO);  // se muestrea en el flanco de subida (SPI modo 0)
    digitalWrite(T_CLK, LOW);
  }
  digitalWrite(T_CS, HIGH);
  return (v >> 3) & 0x0FFF;
}

// Lectura cruda promediada (0-4095 por eje). false si no hay toque o la lectura es dudosa.
bool touchRaw(int &rx, int &ry) {
  if (digitalRead(T_IRQ) != LOW) return false;
  long sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 4; i++) {
    int x = xptRead(0xD0), y = xptRead(0x90);
    if (x > 100 && x < 4000 && y > 100 && y < 4000) {
      sx += x;
      sy += y;
      n++;
    }
  }
  if (n < 2) return false;
  rx = sx / n;
  ry = sy / n;
  gRawX = rx;
  gRawY = ry;
  return true;
}

// Calibracion: pantalla = factor * (eje crudo) + desplazamiento, con los ejes intercambiados o no.
Preferences prefs;
int calSwap = 0;
float calMx = 1, calBx = 0, calMy = 1, calBy = 0;

// Puntos de calibracion: (20,20), (300,20) y (20,220). ax = eje crudo que varia con x, ay con y.
void calCompute(int swap, int ax1, int ax2, int ay1, int ay3) {
  calSwap = swap;
  calMx = 280.0f / (ax2 - ax1);
  calBx = 20 - calMx * ax1;
  calMy = 200.0f / (ay3 - ay1);
  calBy = 20 - calMy * ay1;
}

// true si hay un toque; devuelve la posicion en pixeles de pantalla (0-319, 0-239).
bool touchGet(int &x, int &y) {
  int rx, ry;
  if (!touchRaw(rx, ry)) return false;
  int ax = calSwap ? ry : rx, ay = calSwap ? rx : ry;
  x = constrain((int)(calMx * ax + calBx), 0, 319);
  y = constrain((int)(calMy * ay + calBy), 0, 239);
  return true;
}

bool calPoint(int sx, int sy, int &rx, int &ry) {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE);
  gfx->setCursor(40, 104);
  gfx->println("Toca la cruz roja");
  gfx->drawLine(sx - 12, sy, sx + 12, sy, RED);
  gfx->drawLine(sx, sy - 12, sx, sy + 12, RED);
  while (digitalRead(T_IRQ) == LOW) delay(10);  // esperar a que suelte
  while (digitalRead(T_IRQ) != LOW) delay(10);  // esperar el toque
  delay(150);
  long a = 0, b = 0;
  int n = 0;
  for (int i = 0; i < 10; i++) {
    int x, y;
    if (touchRaw(x, y)) {
      a += x;
      b += y;
      n++;
    }
    delay(10);
  }
  while (digitalRead(T_IRQ) == LOW) delay(10);
  if (n < 3) return false;
  rx = a / n;
  ry = b / n;
  return true;
}

void touchCalibrate() {
  for (;;) {
    int r[3][2];
    if (!calPoint(20, 20, r[0][0], r[0][1])) continue;
    if (!calPoint(300, 20, r[1][0], r[1][1])) continue;
    if (!calPoint(20, 220, r[2][0], r[2][1])) continue;
    int swap = abs(r[1][1] - r[0][1]) > abs(r[1][0] - r[0][0]);  // el eje que cambia de izq. a der. es x
    int ax1 = swap ? r[0][1] : r[0][0], ax2 = swap ? r[1][1] : r[1][0];
    int ay1 = swap ? r[0][0] : r[0][1], ay3 = swap ? r[2][0] : r[2][1];
    Serial.printf("calibracion: swap=%d ax1=%d ax2=%d ay1=%d ay3=%d\n", swap, ax1, ax2, ay1, ay3);
    if (abs(ax2 - ax1) < 800 || abs(ay3 - ay1) < 800) {
      msg("Calibracion mala", RED);
      delay(1200);
      continue;
    }
    calCompute(swap, ax1, ax2, ay1, ay3);
    prefs.putInt("swap", swap);
    prefs.putInt("ax1", ax1);
    prefs.putInt("ax2", ax2);
    prefs.putInt("ay1", ay1);
    prefs.putInt("ay3", ay3);
    msg("Touch calibrado", 0x07E0);
    delay(800);
    return;
  }
}

// Carga la calibracion guardada o la hace. Tocar la pantalla en el primer segundo fuerza recalibrar.
void touchBegin() {
  prefs.begin("touch2", false);  // "touch" tenia una calibracion hecha con la lectura defectuosa
  bool saved = prefs.isKey("ax1");
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFE0);
  gfx->setCursor(8, 100);
  gfx->println("SIMPSONS TV");
  gfx->setTextSize(1);
  gfx->setTextColor(WHITE);
  gfx->setCursor(8, 130);
  gfx->println("Toca la pantalla para recalibrar el touch");
  bool recal = false;
  uint32_t t = millis();
  while (millis() - t < 1000) {
    if (digitalRead(T_IRQ) == LOW) {
      recal = true;
      break;
    }
    delay(10);
  }
  if (saved && !recal) {
    calCompute(prefs.getInt("swap"), prefs.getInt("ax1"), prefs.getInt("ax2"), prefs.getInt("ay1"), prefs.getInt("ay3"));
  } else {
    touchCalibrate();
  }
}

// true una sola vez por pulsacion (al apoyar el dedo), con un antirrebote de 250 ms.
bool touchTap(int &x, int &y) {
  static bool down = false;
  static uint32_t lastTap = 0;
  if (digitalRead(T_IRQ) != LOW) {
    down = false;
    return false;
  }
  if (down) return false;
  down = true;
  if (millis() - lastTap < 250) return false;
  delay(12);  // dejar que la lectura se estabilice
  if (!touchGet(x, y)) return false;
  lastTap = millis();
  return true;
}

// ---------- Barra de controles ----------
void ovInit() {
  ovCanvas = new Arduino_Canvas(320, OV_H, nullptr);
  if (ovCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) ovFb = ovCanvas->getFramebuffer();
  memset(silence, 128, sizeof(silence));
}

// Cuatro botones de 80 px: menu | pausa o play | menos | mas. 0 = transparente.
void ovRender(bool paused) {
  if (!ovFb) return;
  Arduino_Canvas *c = ovCanvas;
  c->fillScreen(0);
  for (int k = 0; k < 3; k++) c->fillRect(27, 17 + k * 9, 26, 5, WHITE);  // menu
  if (paused) {
    c->fillTriangle(80 + 28, 15, 80 + 28, 44, 80 + 52, 29, WHITE);  // play
  } else {
    c->fillRect(80 + 29, 16, 8, 26, WHITE);  // pausa
    c->fillRect(80 + 43, 16, 8, 26, WHITE);
  }
  char s[8];
  snprintf(s, sizeof(s), "%d%%", gVolLevel * 10);
  c->setTextSize(1);
  c->setTextColor(WHITE);
  c->setCursor(240 - (int)strlen(s) * 3, 2);
  c->print(s);
  c->fillRect(160 + 29, 28, 22, 5, WHITE);  // menos
  c->fillRect(240 + 29, 28, 22, 5, WHITE);  // mas
  c->fillRect(240 + 38, 19, 5, 22, WHITE);
}

// Vuelve a dibujar el ultimo fotograma (frameBuf aun lo tiene) para refrescar la barra en pausa.
void redrawFrame(size_t sz) {
  if (!sz) return;
  drawFrame(sz);
  drawWait();
}

// Atiende el touch durante la reproduccion. true = se pidio volver al menu.
bool ctrlPoll(bool &paused, uint32_t &ovTs, size_t lastSz) {
  int x, y;
  uint32_t now = millis();
  if (touchTap(x, y)) {
#if TOUCH_DEBUG
    Serial.printf("tap pantalla=(%d,%d) barra=%d\n", x, y, (int)ovVisible);
#endif
    if (!ovVisible) {  // el primer toque solo muestra la barra
      ovRender(paused);
      ovVisible = true;
      ovTs = now;
      if (paused) redrawFrame(lastSz);
    } else {
      ovTs = now;
      if (y >= OV_Y && y < OV_Y + OV_H) {
        int btn = x / 80;
        if (btn == 0) return true;
        if (btn == 1) paused = !paused;
        if (btn == 2 && gVolLevel > 0) gVolLevel = gVolLevel - 1;
        if (btn == 3 && gVolLevel < 10) gVolLevel = gVolLevel + 1;
        ovRender(paused);
        if (paused) redrawFrame(lastSz);
      }
    }
  }
  if (ovVisible && (now - ovTs) > OV_TIMEOUT) {
    ovVisible = false;
    if (paused) redrawFrame(lastSz);
  }
  return false;
}

// Lectura de la SD con buffer: evita una llamada a f.read por cada cabecera de 8 bytes.
// Solo vale para lectura secuencial; la posicion logica es brPos().
#define SDBUF 8192
static uint8_t sdBuf[SDBUF];
static size_t sdPos = 0, sdLen = 0;

bool brRead(File &f, void *dst, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  size_t got = 0;
  while (got < n) {
    if (sdPos == sdLen) {
      if (n - got >= SDBUF) {  // bloque grande: directo, sin pasar por el buffer
        size_t r = f.read(d + got, n - got);
        if (!r) return false;
        got += r;
        continue;
      }
      sdLen = f.read(sdBuf, SDBUF);
      sdPos = 0;
      if (!sdLen) return false;
    }
    size_t c = min(n - got, sdLen - sdPos);
    memcpy(d + got, sdBuf + sdPos, c);
    sdPos += c;
    got += c;
  }
  return true;
}

void brSkip(File &f, size_t n) {
  size_t avail = sdLen - sdPos;
  if (n <= avail) {
    sdPos += n;
    return;
  }
  n -= avail;
  sdPos = sdLen = 0;  // buffer agotado: la posicion del archivo ya es la logica
  f.seek(f.position() + n);
}

uint32_t brPos(File &f) {
  return f.position() - (sdLen - sdPos);
}

void playAvi(const String &path) {
  File f = SD.open(path);
  if (!f) {
    msg("No se pudo abrir", RED);
    delay(1500);
    return;
  }
  uint8_t h[12];
  if (!readExact(f, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) {
    msg("AVI invalido", RED);
    f.close();
    delay(1500);
    return;
  }

  // Buscar la lista "movi" (entrando en los LIST de cabecera)
  uint8_t c[8];
  bool inMovi = false;
  while (readExact(f, c, 8)) {
    uint32_t sz = le32(c + 4);
    if (!memcmp(c, "LIST", 4)) {
      uint8_t t[4];
      if (!readExact(f, t, 4)) break;
      if (!memcmp(t, "movi", 4)) {
        inMovi = true;
        break;
      }
      continue;
    }
    f.seek(f.position() + sz + (sz & 1));
  }
  if (!inMovi) {
    msg("Sin datos movi", RED);
    f.close();
    delay(1500);
    return;
  }

  gfx->fillScreen(BLACK);
  uint32_t frames = 0, skipped = 0;
  uint32_t t0 = millis();
  static uint8_t atmp[1024];
  // Medicion por bloques de 50 fotogramas (microsegundos): lectura SD, decodificacion y audio
  uint32_t tSd = 0, tDec = 0, tAud = 0, tWin = millis();
  // Sincronizacion: vIdx = fotogramas de video vistos, audioSent = bytes de audio entregados al DAC
  uint32_t vIdx = 0, dropped = 0, audioSent = 0;
  uint32_t lastStat = millis(), chunks = 0;  // estado cada segundo (diagnostico)
  gDacTimeouts = 0;
  sdPos = sdLen = 0;  // a partir de aqui se lee con buffer (brRead / brSkip)
  bool paused = false, toMenu = false;
  uint32_t ovTs = 0;
  size_t lastSz = 0;  // tamano del ultimo fotograma leido (sigue en frameBuf)
  ovVisible = false;

  while (brRead(f, c, 8)) {
    uint32_t sz = le32(c + 4);
    chunks++;
    if (ctrlPoll(paused, ovTs, lastSz)) break;  // "Menu"
    while (paused) {  // en pausa se alimenta silencio para que el DAC no repita audio viejo
      audioWrite(silence, sizeof(silence));
      if (ctrlPoll(paused, ovTs, lastSz)) {
        toMenu = true;
        break;
      }
    }
    if (toMenu) break;
    if (millis() - lastStat >= 1000) {
      lastStat = millis();
      Serial.printf("estado t=%lus chunks=%lu video_vistos=%lu decodificados=%lu saltados_sync=%lu dac_esperas=%lu cola_audio=%.2fs pos=%.1f%%\n",
                    (unsigned long)((millis() - t0) / 1000), (unsigned long)chunks,
                    (unsigned long)vIdx, (unsigned long)frames, (unsigned long)dropped,
                    (unsigned long)gDacTimeouts, (double)xStreamBufferBytesAvailable(audioBuf) / AUDIO_RATE,
                    100.0 * brPos(f) / f.size());
    }
    if (!memcmp(c, "LIST", 4)) {  // "rec ": entrar
      brSkip(f, 4);
      continue;
    }
    if (!memcmp(c, "idx1", 4)) break;

    if (c[0] == '0' && c[1] == '0' && c[2] == 'd' && c[3] == 'c') {  // video
      vIdx++;
      if (xStreamBufferBytesAvailable(audioBuf) < SKIP_BYTES) {  // poco audio en cola: alimentar primero
        brSkip(f, sz);
        dropped++;
      } else if (sz <= FRAME_MAX) {
        uint32_t a = micros();
        brRead(f, frameBuf, sz);
        lastSz = sz;
        uint32_t b = micros();
        drawFrame(sz);
        uint32_t d = micros();
        tSd += b - a;
        tDec += d - b;
        frames++;
        if (frames % 50 == 0) {
          uint32_t now = millis();
          Serial.printf("[%lu fr] %.1f fps (ultimos 50: %.1f fps) | ms/fr: SD %.1f, decodificar %.1f, dibujar(core0) %.1f, audio %.1f | saltados sync %lu | pos %.0f%%\n",
                        (unsigned long)frames, frames * 1000.0 / (now - t0),
                        50000.0 / (now - tWin),
                        tSd / 50000.0, tDec / 50000.0, gDrawUs / 50000.0, tAud / 50000.0,
                        (unsigned long)dropped,
                        100.0 * brPos(f) / f.size());
          tSd = tDec = tAud = 0;
          gDrawUs = 0;
          tWin = now;
        }
      } else {
        brSkip(f, sz);
        skipped++;
      }
    } else if (c[0] == '0' && c[1] == '1' && c[2] == 'w' && c[3] == 'b') {  // audio
      uint32_t left = sz;
      while (left) {
        size_t n = left > sizeof(atmp) ? sizeof(atmp) : left;
        uint32_t a = micros();
        brRead(f, atmp, n);
        audioWrite(atmp, n);
        audioSent += n;
        tAud += micros() - a;
        left -= n;
      }
    } else {
      brSkip(f, sz);
    }
    if (sz & 1) brSkip(f, 1);

    if (digitalRead(PIN_BTN_PLAY) == LOW) {  // volver al menu
      while (digitalRead(PIN_BTN_PLAY) == LOW) delay(5);
      break;
    }
  }
  f.close();
  ovVisible = false;
  drawWait();  // el menu vuelve a usar la pantalla: esperar a la tarea de dibujo

  uint32_t ms = millis() - t0;
  Serial.printf("Frames: %lu, saltados: %lu, saltados sync: %lu, tiempo: %lu ms, fps: %.1f\n",
                (unsigned long)frames, (unsigned long)skipped, (unsigned long)dropped, (unsigned long)ms,
                ms ? frames * 1000.0 / ms : 0.0);
}

// ---------- Menu ----------
void scanSD() {
  nFiles = 0;
  File root = SD.open("/");
  for (File e = root.openNextFile(); e && nFiles < MAX_FILES; e = root.openNextFile()) {
    String n = e.name();
    String l = n;
    l.toLowerCase();
    if (!e.isDirectory() && !n.startsWith(".") && l.endsWith(".avi")) files[nFiles++] = n;
    e.close();
  }
  root.close();
  for (int i = 0; i < nFiles - 1; i++)
    for (int j = i + 1; j < nFiles; j++)
      if (files[j] < files[i]) {
        String t = files[i];
        files[i] = files[j];
        files[j] = t;
      }
}

void drawMenu() {
  const int perPage = MENU_PER_PAGE;
  int page = sel / perPage;
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFE0);  // amarillo
  gfx->setCursor(8, 6);
  gfx->println("SIMPSONS TV");
  if (nFiles > perPage) {  // tocar la cabecera pasa a la pagina siguiente
    gfx->setCursor(230, 6);
    gfx->printf("%d/%d >", page + 1, (nFiles + perPage - 1) / perPage);
  }
  for (int i = page * perPage; i < nFiles && i < (page + 1) * perPage; i++) {
    String name = files[i];
    name.remove(name.length() - 4);  // sin ".avi"
    gfx->setTextColor(i == sel ? 0xFFE0 : WHITE);
    gfx->setCursor(8, 30 + (i - page * perPage) * 22);
    gfx->print(i == sel ? "> " : "  ");
    gfx->println(name);
  }
}

String pathOf(int i) {
  return files[i].startsWith("/") ? files[i] : "/" + files[i];
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);

  gfx->begin(DISPLAY_SPI_HZ);
  gfx->fillScreen(BLACK);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  frameBuf = (uint8_t *)malloc(FRAME_MAX);
  if (!frameBuf) {
    msg("Sin memoria", RED);
    while (true) delay(1000);
  }

  // La SD a 40 MHz a veces no inicia en frio: reintentar bajando la velocidad.
  const uint32_t sdSpeeds[] = {40000000, 25000000, 10000000};
  bool sdOk = false;
  for (int i = 0; i < 3 && !sdOk; i++) {
    sdSPI.end();
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdOk = SD.begin(SD_CS, sdSPI, sdSpeeds[i]);
    Serial.printf("SD intento %d a %lu Hz: %s\n", i + 1, (unsigned long)sdSpeeds[i], sdOk ? "OK" : "FALLO");
    if (!sdOk) delay(300);
  }
  if (!sdOk) {
    msg("Error en la SD", RED);
    while (true) delay(1000);
  }

  audioInit();
  drawInit();
  touchInit();
  touchBegin();
  ovInit();
  scanSD();
  Serial.printf("Archivos .avi encontrados: %d\n", nFiles);
  if (nFiles == 0) {
    msg("No hay .avi", RED);
    while (true) delay(1000);
  }
  drawMenu();
}

void loop() {
#if AUTOPLAY_FIRST
  playAvi(pathOf(0));
  delay(1000);
#else
  int tx, ty;
  if (touchTap(tx, ty)) {
#if TOUCH_DEBUG
    Serial.printf("touch raw=(%d,%d) -> pantalla=(%d,%d)\n", (int)gRawX, (int)gRawY, tx, ty);
    gfx->fillRect(tx - 4, ty - 4, 9, 9, RED);  // punto rojo donde cree que tocaste
    delay(500);
#endif
    const int page = sel / MENU_PER_PAGE;
    if (ty < MENU_ROW_Y0) {  // cabecera: pagina siguiente
      int pages = (nFiles + MENU_PER_PAGE - 1) / MENU_PER_PAGE;
      sel = ((page + 1) % pages) * MENU_PER_PAGE;
      drawMenu();
    } else {
      int row = (ty - MENU_ROW_Y0) / MENU_ROW_H;
      int idx = page * MENU_PER_PAGE + row;
      if (row < MENU_PER_PAGE && idx < nFiles) {  // tocar un capitulo lo reproduce
        sel = idx;
        drawMenu();
        playAvi(pathOf(sel));
        drawMenu();
      } else {
        drawMenu();
      }
    }
  }
  if (pressed(PIN_BTN_NEXT)) {
    sel = (sel + 1) % nFiles;
    drawMenu();
  }
  if (pressed(PIN_BTN_PLAY)) {
    playAvi(pathOf(sel));
    drawMenu();
  }
#endif
}
