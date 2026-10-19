# Mini TV Simpson (ESP32-2432S028 "CYD")

## Objetivo
Televisor miniatura de Los Simpson: electrónica + carcasa impresa en 3D (el 3D se hace aparte).
Funcionalidad: un aparato simple. Se enciende con un interruptor, muestra una lista de capítulos, un botón navega y otro reproduce. Todo el contenido está en una microSD.

## Cómo trabajar con el usuario (Mauro)
- Responder en español, breve y directo. Sin condescendencia.
- Preferir tablas y listas cortas. Dar feedback honesto si algo no funciona.
- El usuario es quien juzga imagen y sonido (Claude no ve la pantalla ni oye el audio). Pedir descripción concreta: colores, saltos, ruido, giro de imagen.
- Si hay una forma más rápida de hacer algo (script, herramienta, automatización), decirlo.

## Hardware
- Placa: ESP32-2432S028 (ESP32-WROOM-32, 4 MB flash, ILI9341 240x320, microSD, amplificador de audio integrado con conector de parlante JST 1.25).
- Alimentación: batería 18650 + módulo IP5306 (USB-C, salida 5 V) → interruptor → pin VIN de P1. GND común.
- Parlante: 8 Ω, 1-2 W, 28-30 mm, al conector JST de la placa.
- Nota: no conectar el USB de la placa y el USB-C del IP5306 al mismo tiempo.

### Pines (verificar con la serigrafía de la placa)
| Función | Pines |
|---|---|
| Pantalla ILI9341 (HSPI) | DC 2, CS 15, SCK 14, MOSI 13, MISO 12, backlight 21 |
| microSD (VSPI) | CS 5, SCK 18, MISO 19, MOSI 23 |
| Audio (DAC) | GPIO26 (ESP32 clásico: canal 1 del DAC) |
| Botón 1 (siguiente) | IO22, pull-up interno, el otro lado a GND |
| Botón 2 (play / volver al menú) | IO27, pull-up interno, el otro lado a GND |
| Touch XPT2046 (bus propio, por software) | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |

## Video: formato de los capítulos
MJPEG 320x240 a 15 fps con audio PCM u8 mono a 22050 Hz, en contenedor AVI. Nombres como `S01E01.avi`. Base: repo `moononournation/aviPlayer`.

```bash
ffmpeg -y -i capitulo.mp4 -ac 1 -ar 22050 -af loudnorm -c:a pcm_u8 -c:v mjpeg -q:v 7 \
  -vf "fps=15,scale=320:240:flags=lanczos" S01E01.avi
```

- `scale=320:240` asume fuente 4:3. Si el mp4 es 16:9, usar `scale=-1:240,crop=320:240` (recorta costados) o preguntar al usuario.
- Si el video salta: subir `-q:v` a 9 o bajar a 12 fps.
- Tamaño estimado: 200-300 MB por capítulo de ~22 min (estimación, medirlo).
- Nunca sobrescribir ni borrar los mp4 originales. Escribir los `.avi` en una carpeta aparte.
- Para el lote: script que recorre la carpeta de mp4 y genera los `.avi` con el comando de arriba.

## Firmware
- Archivo: `mini_tv_simpsons/mini_tv_simpsons.ino` (la carpeta del sketch debe llamarse igual). Pruebas de puesta en marcha en `pruebas/`.
- Librerías: "GFX Library for Arduino" (moononournation) y "JPEGDEC" (bitbank2).
- Core: esp32 de Espressif **3.x** (usa `driver/dac_continuous.h`). Placa: `esp32:esp32:esp32` (ESP32 Dev Module).
- Funcionamiento: parsea el AVI a mano (chunks `00dc` video, `01wb` audio), decodifica JPEG con JPEGDEC. Core 1: lee la SD (con buffer de 8 KB), decodifica y manda el audio a un stream buffer. Core 0: una tarea dibuja los bloques con Arduino_GFX y otra escribe el audio al DAC en bloques fijos de 512 bytes (con tamaños irregulares el DAC se atoraba). El buffer de audio limita el desfase imagen-sonido y, si queda casi vacío, se omite la decodificación de un fotograma.
- Pantalla a 40 MHz (`DISPLAY_SPI_HZ`): a 80 MHz aparecen líneas verticales y franjas corruptas.
- Touch XPT2046 leído por software (bus propio), con calibración de 3 puntos guardada en la memoria de la placa. Barra de controles (menú, pausa/play, −, +) mezclada en el core 0 dentro de cada bloque de video; se oculta a los 5 s. Volumen 0-10 con ganancia máxima `VOLUME_MAX_GAIN` (2.0).
- `AUTOPLAY_FIRST 1` reproduce el primer .avi sin menú (modo prueba). Con `0` (valor actual) arranca en el menú táctil.
- `TOUCH_DEBUG 1` imprime lecturas por serie y marca un punto rojo en el menú; ponerlo en 0 para uso normal.

### Comandos útiles (arduino-cli)
```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "GFX Library for Arduino" JPEGDEC
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli board list
arduino-cli upload -p <PUERTO> --fqbn esp32:esp32:esp32 .
arduino-cli monitor -p <PUERTO> -c baudrate=115200
```
El puerto depende del sistema (COMx en Windows, /dev/ttyUSB0 o /dev/ttyACM0 en Linux, /dev/cu.usbserial-* en Mac). Si la subida falla, mantener presionado BOOT al iniciar la carga.

**En esta placa la subida a 921600 baudios falla** ("Unable to verify flash chip connection"). Usar `--upload-property upload.speed=115200` (o Upload Speed 115200 en el IDE). El Monitor Serie del IDE bloquea el puerto: cerrarlo antes de subir. arduino-cli exige que la carpeta del sketch se llame `mini_tv_simpsons`.

## Estado actual
**v1.0** (oct 2026): compila, corre en la placa y está probado en un capítulo (~2 min de reproducción continua; un capítulo completo de 20 min no se ha verificado).
- Video a **15,0 fps** sostenidos, audio continuo y sincronía perfecta, imagen limpia a 40 MHz.
- Menú táctil y barra de controles táctiles funcionando (menú, pausa/play, volumen ±10 %).
- 3 capítulos de prueba en la SD (`35x01_1080p.avi` ... `35x03_1080p.avi`, ya a 320x240).
- Volumen: sigue sonando bajo; con ganancia 2x mejoró algo. Opción pendiente: reconvertir con `loudnorm=I=-14:TP=-1.5`.
- Pendiente: botones físicos sin soldar (el código ya los soporta).
- Pendiente: script de conversión por lote de todos los capítulos.
- Pendiente: verificar un capítulo completo (final del capítulo, posible chasquido, fps en escenas pesadas).
- Pendiente: "Error en la SD" apareció una vez en el arranque y no se repitió; hay reintentos a 40/25/10 MHz.

## Riesgos conocidos
1. **Audio por DAC (más incierto):** el repo de referencia usa I2S, esta placa saca el sonido por el DAC interno. Se usa `dac_continuous` a 22050 Hz (el mínimo del DAC continuo ronda los 19,6 kHz). Calidad esperada: mono 8 bits, tipo TV vieja. Posible chasquido al terminar un capítulo.
2. **Fluidez:** sin PSRAM, el tope realista es ~15 fps. El Monitor Serie imprime fps reales al terminar cada capítulo.
3. **Variantes de placa:** algunas CYD usan ST7789 en vez de ILI9341, o necesitan colores invertidos (parámetro IPS) o rotation 3.
4. **SD:** FAT32, ≤ 32 GB. Los archivos que empiezan con "." se ignoran (macOS crea `._*`).
5. **IP5306:** puede apagarse solo con poco consumo; si pasa, cambiar por TP4056 + boost MT3608.

## Próximos pasos sugeridos
1. Script de conversión por lote (mp4 → .avi) sin tocar los originales.
2. Probar un capítulo completo y revisar el final y los fps en escenas pesadas.
3. Soldar los botones físicos y probarlos junto al touch.
4. `TOUCH_DEBUG 0` para uso normal.
