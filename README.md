# RELOJNTP

Reloj sincronizado vía NTP que muestra la hora (HH:MM) en una matriz de LEDs de 16x32 píxeles con interfaz HUB12, sobre un ESP32 (PlatformIO, framework Arduino).

## Funcionalidades

### Hora NTP
- Sincronización con `pool.ntp.org` (zona horaria configurable en `GMT_OFFSET_SEC`).
- Re-sincronización automática cada 30 minutos y tras cada reconexión WiFi.
- La última hora sincronizada se guarda en NVS y se restaura al arrancar, de modo que el reloj muestra hora aproximada aun sin red.
- Si no hay hora NTP válida, lleva la cuenta manualmente hasta lograr sincronizar.
- Los dos puntos (`:`) parpadean cada segundo.

### WiFi con WiFiManager
- Configuración de red mediante portal cautivo (AP `HUB12-Clock`, portal en `192.168.4.1`), no bloqueante.
- Reconexión automática con re-sincronización NTP.

### Arranque de bajo consumo
- Al encender, la matriz permanece apagada y solo parpadea **un LED en el centro** (0.5 s) mientras se establece la conexión WiFi, evitando el pico de corriente de tener el panel encendido durante el arranque.

### Barrido del panel por interrupciones de timer
- El refresco de la matriz HUB12 lo realiza la ISR de un timer hardware (timer 0) cada 500 µs, independiente del `loop()` — sin parpadeos aunque el programa esté ocupado.

### Control de brillo (botón pin 18)
- Tres niveles: bajo (~20%), medio (~50%) y alto (100%), implementados como PWM sobre la línea OE con un segundo timer (timer 1, one-shot) que apaga OE dentro de cada scan — sin flicker visible.
- Pulsante a GND detectado por interrupción de flanco de bajada, con antirrebote de 200 ms.
- El nivel elegido se persiste en NVS y se restaura al encender.

### Tres fuentes de dígitos (botón pin 19)
- Fuentes definidas en `src/fonts.h` en formato unificado (un `uint16_t` por columna, bit R = fila R):
  1. **5x7** original
  2. **Arial Narrow 9pt** (5x9)
  3. **Carlito 10pt** (6x9)
- El texto se centra automáticamente según la métrica de la fuente.
- Pulsante a GND con interrupción y antirrebote; la selección se persiste en NVS.

### Animaciones de cambio de dígito (botón pin 17)
Un segundo antes de que cambie algún dígito (en el segundo 59) se renderizan la hora actual y la siguiente, y se muestran cuadros intermedios cada 100 ms. Solo se animan las posiciones que cambian. Tres tipos seleccionables:

1. **Scroll vertical** — las filas del nuevo dígito entran desde abajo desplazando a las del antiguo.
2. **Scroll horizontal** — las columnas del nuevo dígito entran desde la derecha desplazando a las del antiguo.
3. **Disolución** — los píxeles del dígito antiguo se apagan en orden aleatorio y luego se encienden aleatoriamente los del nuevo; los píxeles comunes permanecen fijos.

Pulsante a GND con interrupción y antirrebote; la selección se persiste en NVS.

### Persistencia en NVS (namespace `clock`)
| Clave        | Contenido                          |
|--------------|------------------------------------|
| `brightness` | Último nivel de brillo (0-2)       |
| `font`       | Última fuente seleccionada (0-2)   |
| `anim`       | Última animación seleccionada (0-2)|
| `lastSync`   | Última hora NTP sincronizada       |

## Hardware

### Panel
- Matriz de LEDs 16x32 con interfaz HUB12 (barrido 1/4).

### Conexiones ESP32
| Pin | Señal HUB12 / Función        |
|-----|------------------------------|
| 27  | OE (output enable)           |
| 26  | DATA                         |
| 25  | CLK                          |
| 33  | LOAD / LATCH                 |
| 14  | A (dirección de fila)        |
| 13  | B (dirección de fila)        |
| 18  | Pulsante de brillo (a GND)   |
| 19  | Pulsante de fuente (a GND)   |
| 17  | Pulsante de animación (a GND)|

Los pulsantes usan el pull-up interno (`INPUT_PULLUP`) y se detectan por interrupción de flanco de bajada con antirrebote de 200 ms.

## Compilación y subida

```bash
pio run                # compilar
pio run -t upload      # subir al ESP32
pio device monitor     # monitor serie (115200 baud)
```

## Estructura del código

- `src/main.cpp` — programa principal: driver HUB12, timers/ISRs, WiFi/NTP, botones, dibujo y animaciones.
- `src/fonts.h` — definición de las tres fuentes de dígitos (`'0'..'9'`, `':'`) y la estructura `Font`.
- `platformio.ini` — entorno `esp32dev`, dependencia `tzapu/WiFiManager`.
