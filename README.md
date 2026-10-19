# Mini TV Simpson

Televisor en miniatura de Los Simpson con una ESP32-2432S028 ("CYD"). Un interruptor lo enciende,
la pantalla táctil muestra la lista de capítulos y la reproducción tiene controles táctiles.
Todo el contenido está en una microSD.

**Versión 1.0:** video MJPEG 320x240 a **15 fps** con audio sincronizado, menú táctil y barra de controles.

## Hardware

| Pieza | Detalle |
|---|---|
| Placa | ESP32-2432S028 (ESP32-WROOM-32, 4 MB, ILI9341 240x320, microSD, amplificador de audio) |
| Alimentación | Batería 18650 + módulo IP5306 → interruptor → VIN. No conectar el USB de la placa y el USB-C del IP5306 a la vez |
| Parlante | 8 Ω, 1-2 W, 28-30 mm, al conector JST de la placa |
| SD | FAT32, hasta 32 GB. Los `.avi` van sueltos en la raíz |

## Controles (pantalla táctil)

- **Menú:** tocar un capítulo para reproducirlo. Tocar la cabecera cambia de página.
- **Durante la reproducción:** tocar la pantalla muestra una barra inferior con **menú**, **pausa/play**, **−** y **+** (volumen en pasos de 10 %). Se oculta sola a los 5 s.
- **Primer arranque:** pide tocar 3 cruces para calibrar el touch (se guarda). Para recalibrar, tocar la pantalla durante el primer segundo tras encender.

## Compilar y subir

Requiere `arduino-cli` (o el IDE), el core **esp32 de Espressif 3.x** y las librerías "GFX Library for Arduino" y "JPEGDEC".

```bash
arduino-cli lib install "GFX Library for Arduino" JPEGDEC
arduino-cli compile --fqbn esp32:esp32:esp32 mini_tv_simpsons
arduino-cli upload -p <PUERTO> --fqbn esp32:esp32:esp32 --upload-property upload.speed=115200 mini_tv_simpsons
```

La subida a 921600 baudios falla en esta placa: usar 115200. Cerrar el Monitor Serie antes de subir.

## Preparar los videos

MJPEG 320x240 a 15 fps con audio PCM u8 mono a 22050 Hz, en AVI:

```bash
ffmpeg -y -i capitulo.mp4 -ac 1 -ar 22050 -af loudnorm -c:a pcm_u8 -c:v mjpeg -q:v 7 \
  -vf "fps=15,scale=320:240:flags=lanczos" S01E01.avi
```

Si el volumen sale bajo, se puede normalizar más fuerte (`loudnorm=I=-14:TP=-1.5`).

## Cómo funciona

- Parsea el AVI a mano y decodifica JPEG con JPEGDEC.
- **Core 1:** lee la SD, decodifica y manda el audio. **Core 0:** dibuja en la pantalla y escribe el audio al DAC.
- El audio va en bloques fijos de 512 bytes (con tamaños irregulares el DAC se atoraba).
- La pantalla corre a 40 MHz: a 80 MHz aparecen líneas corruptas.
- Si el audio se atrasa, se omite la decodificación de algún fotograma para mantener el sonido continuo.

## Estructura

```
mini_tv_simpsons/   firmware (el sketch debe estar en una carpeta con su nombre)
pruebas/            sketches de puesta en marcha: hola_mundo, prueba_sd, prueba_tono
CLAUDE.md           notas del proyecto
```

## Pendiente

- Botones físicos (IO22 siguiente, IO27 play/menú): el código ya los soporta, falta soldarlos.
- Script de conversión por lote de todos los capítulos.
- Una vez el "Error en la SD" apareció en el arranque y no se repitió; el firmware reintenta a 40, 25 y 10 MHz.
