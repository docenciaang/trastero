Bootstrap program for Wemos Lolin ESP32 OLED board (and clones)
--

This program allows you a quick start with the Wemos Lolin ESP32 OLED, which features a 0.96" display. Documentation online about the pinout is sketchy and, in some cases, wrong. 

The important thing is that to communicate with the display, you need to use I2C, talking to the SSD1306 controller. The I2C address and I2C pins are:

```C++
#define I2C_DISPLAY_ADDR    0x3C
#define SDA                 5
#define SCL                 4
```

Here's a photo of a Lolin clone running the "Hello World" code:

![ESP32 with the OLED display](imgs/ESP32_photo.jpeg)

Here's the pinout of the module for future reference:

![ESP32 with the OLED display](imgs/ESP32_pinout.jpeg)

Please also check this documentation:
* [Random Nerd Tutorials -- WEMOS LOLIN](https://randomnerdtutorials.com/esp32-built-in-oled-ssd1306/)
* [ESP32-WROOM-32 with 0.96" OLED by Melife](https://www.technologyx2.com/blog_hightech/2020/5/24/research-esp32esp-wroom-32-development-board-with-096-oled-by-melife)

## Control de Humedad — Esquema de conexiones

### Componentes

| Componente               | Cantidad |
|--------------------------|----------|
| Wemos Lolin ESP32 (OLED integrado) | 1 |
| Sensor DHT21 (AM2301)    | 2        |
| Módulo relé 2 canales (activo LOW) | 1 |
| Extractor de aire        | 1        |
| Deshumidificador Peltier | 1        |

### Tabla de pines

| Señal               | GPIO ESP32 | Notas                        |
|---------------------|-----------|------------------------------|
| OLED SDA            | GPIO 5    | Integrado en la placa        |
| OLED SCL            | GPIO 4    | Integrado en la placa        |
| DHT21 (AM2301) interior DATA | GPIO 13   | Pull-up 10 kΩ a 3.3 V       |
| DHT21 (AM2301) exterior DATA | GPIO 14   | Pull-up 10 kΩ a 3.3 V       |
| Relé extractor IN   | GPIO 26   | Activo LOW                   |
| Relé deshumid. IN   | GPIO 25   | Activo LOW                   |
| Botón pantalla      | GPIO 12   | Pull-up interno; GND al pulsar |

### Diagrama de bloques

```
                        ┌─────────────────────────────────┐
                        │      Wemos Lolin ESP32           │
                        │                                  │
   DHT21 (AM2301) (interior) ────┤ GPIO13          GPIO26 ├──── Relé CH1 ──► Extractor
                        │                                  │
   DHT21 (AM2301) (exterior) ────┤ GPIO14          GPIO25 ├──── Relé CH2 ──► Deshumidificador
                        │                                  │
              Botón ────┤ GPIO12                           │
                        │                                  │
                        │   GPIO4/5 (I2C)                  │
                        │      │                           │
                        │   OLED SSD1306 (integrado)       │
                        └─────────────────────────────────┘
```

### Cableado de los sensores DHT21 (AM2301)

Cada DHT21 (AM2301) se conecta del mismo modo:

```
DHT21 (AM2301)
 ┌───┐
 │ 1 ├──── VCC (3.3 V)
 │   │         │
 │ 2 ├──── DATA ──┬──► GPIO13 (interior) / GPIO14 (exterior)
 │   │            │
 │   │          10 kΩ (pull-up a 3.3 V)
 │ 3 │  (no conectar)
 │ 4 ├──── GND
 └───┘
```

### Cableado del botón de pantalla

El botón activa la pantalla OLED durante 30 segundos. Usa el pull-up interno del ESP32,
por lo que no necesita resistencia externa.

```
ESP32          Botón
GPIO12 ────── [  ]
GND    ────── [  ]
```

Al pulsar, GPIO12 cae a GND (flanco descendente). En reposo permanece en HIGH gracias al pull-up interno.

### Cableado del módulo de relé (2 canales)

```
Módulo relé          ESP32
┌──────────┐
│ VCC      ├──── 5 V  (o 3.3 V según módulo)
│ GND      ├──── GND
│ IN1      ├──── GPIO26  (extractor)
│ IN2      ├──── GPIO25  (deshumidificador)
│          │
│ COM1/NO1 ├──── Fase ──► Extractor
│ COM2/NO2 ├──── Fase ──► Deshumidificador
└──────────┘
```

> **Nota de seguridad:** Los relés conmutan la línea de red (230 V AC). Utiliza siempre
> un módulo con optoacoplador y asegúrate de aislar correctamente los bornes de alta tensión.

## Lógica de control

### Máquina de estados

El sistema tiene cuatro estados posibles:

| Estado | Extractor | Deshumidificador | Condición de entrada |
|---|---|---|---|
| `IDLE` | OFF | OFF | Humedad interior ≤ 65 % |
| `EXTRACTOR_ON` | ON | OFF | Humedad interior > 70 % **y** exterior ≥ 10 % menor |
| `DEHUMID_ON` | OFF | ON | Humedad interior > 70 % y exterior no ayuda |
| `SAFE_OFF` | OFF | OFF | ≥ 5 lecturas consecutivas fallidas en cualquier sensor |

### Criterio de selección de actuador

Cuando la humedad interior supera el 70 %:

```
humedad_interior - humedad_exterior >= 10 %
        ↓ SÍ                  ↓ NO
  EXTRACTOR_ON          DEHUMID_ON
  (ventilar)            (Peltier)
```

El extractor tiene prioridad porque es más eficiente energéticamente.
Si el aire exterior no está suficientemente seco, se recurre al deshumidificador.

### Histéresis

La banda entre 65 % y 70 % evita el encendido/apagado repetitivo (*chatter*):

```
         ┌─ activa dispositivo
  70 % ──┤
         │  (banda: mantiene estado actual)
  65 % ──┤
         └─ apaga dispositivo
```

### Seguridad

Si cualquier sensor acumula 5 lecturas fallidas consecutivas, ambos relés
se apagan inmediatamente (`SAFE_OFF`). La recuperación es automática en
cuanto vuelve a haber una lectura válida.

---

## Ciclos de lectura adaptativos

Los cambios de humedad en un trastero son lentos (escala de minutos/horas),
por lo que la frecuencia de lectura se adapta al estado actual para ahorrar
energía y reducir el desgaste de los sensores:

| Estado | Intervalo de lectura | Motivo |
|---|---|---|
| `IDLE` | **10 min** | Humedad estable; solo vigilar que no supere 70 % |
| `EXTRACTOR_ON` | **2 min** | Detectar cuándo la humedad baja a 65 % |
| `DEHUMID_ON` | **5 min** | La célula Peltier es más lenta |
| `SAFE_OFF` | **30 s** | Recuperación rápida del fallo de sensor |

Adicionalmente, cada vez que el sistema cambia de estado se realiza una
**lectura de confirmación a los 30 segundos** para verificar que el
actuador está teniendo efecto antes de entrar en el intervalo largo.

### Comparativa de lecturas diarias

| Situación | Sin adaptar (2 s fijo) | Con intervalos adaptativos |
|---|---|---|
| Día sin problemas (IDLE todo el día) | 43.200 | **144** |
| Extractor activo 2 horas | 43.200 | ~220 |

---

## Arquitectura software

El firmware usa **FreeRTOS** con tres tareas concurrentes:

```
Core 1  taskSensors  prio 3  Lee DHT21 (AM2301) con intervalos adaptativos
Core 1  taskControl  prio 2  Evalúa estado y activa/desactiva relés
Core 0  taskDisplay  prio 1  Refresca la pantalla OLED cada 500 ms
```

Los datos compartidos (`interior`, `exterior`, `currentState`) están
protegidos por un mutex. Cuando `taskControl` cambia el estado, notifica
a `taskSensors` para que se despierte antes de que expire su intervalo
largo y realice la lectura de confirmación.

Para añadir WiFi o Bluetooth en el futuro basta con crear una tarea
adicional en Core 0 que lea los datos compartidos tomando el mismo mutex.

---

License
---
[Apache 2.0](LICENSE.txt)
