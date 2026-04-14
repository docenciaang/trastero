# Trastero — BLE GATT API Reference

Referencia completa para implementar un cliente BLE que se conecte al dispositivo **"Trastero"**.

---

## Conexión

| Parámetro | Valor |
|---|---|
| Nombre del dispositivo | `Trastero` |
| Tipo de conexión | BLE (Bluetooth Low Energy) |
| Rol del dispositivo | GATT Server (periférico) |
| MTU recomendada | 23 bytes (defecto BLE) — negociar mayor para acelerar descarga CSV |

El dispositivo anuncia los tres UUID de servicio en el paquete de advertising. Se puede filtrar por nombre `"Trastero"` o por cualquiera de los UUID de servicio.

Tras la desconexión el dispositivo reinicia el advertising automáticamente.

---

## Convenciones

- **Byte order**: little-endian en todos los campos multi-byte.
- **Floats**: IEEE 754 de 32 bits, little-endian.
- **Timestamps**: `uint32` segundos desde epoch UTC (Unix timestamp). Valor `0` indica que el reloj no ha sido configurado.
- Las UUIDs siguen el patrón `1b5ab4f0-SSSS-1000-8000-00000000CCCC` donde `SSSS` es el identificador de servicio y `CCCC` el de characteristic.

---

## Servicio Principal

**UUID**: `1b5ab4f0-0000-1000-8000-000000000000`

### SENSOR_INT — Sensor interior

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0000-1000-8000-000000000001` |
| Propiedades | READ, NOTIFY |
| Tamaño | 9 bytes |

Payload:

| Bytes | Tipo | Descripción |
|---|---|---|
| `[0..3]` | `float` | Temperatura en °C |
| `[4..7]` | `float` | Humedad relativa en %RH |
| `[8]` | `uint8` | `1` = lectura válida, `0` = inválida |

Las notificaciones se envían automáticamente cada **1 segundo** cuando el CCCD está activado (`0x0001`).

Ejemplo de lectura (Python, struct):
```python
import struct
temp, hum, valid = struct.unpack_from('<ffB', data)
```

---

### SENSOR_EXT — Sensor exterior

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0000-1000-8000-000000000002` |
| Propiedades | READ, NOTIFY |
| Tamaño | 9 bytes |

Mismo formato que `SENSOR_INT`.

---

### ESTADO — Estado de la máquina de control

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0000-1000-8000-000000000003` |
| Propiedades | READ, NOTIFY |
| Tamaño | 1 byte |

Payload:

| Valor | Estado | Descripción |
|---|---|---|
| `0x00` | `IDLE` | Humedad OK — actuadores apagados |
| `0x01` | `EXTRACTOR_ON` | Extractor en marcha |
| `0x02` | `DEHUMID_ON` | Deshumidificador Peltier en marcha |
| `0x03` | `SAFE_OFF` | Fallo de sensor — todo apagado por seguridad |

Notificaciones cada **1 segundo** cuando el CCCD está activado.

---

### CMD_ACTUADOR — Override manual de actuadores

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0000-1000-8000-000000000010` |
| Propiedades | WRITE |
| Tamaño | 2 bytes |

Payload:

| Byte | Tipo | Descripción |
|---|---|---|
| `[0]` | `uint8` | Comando (ver tabla) |
| `[1]` | `uint8` | Reservado — enviar `0x00` |

Comandos:

| Valor byte `[0]` | Efecto |
|---|---|
| `0x00` | `AUTO` — devuelve el control a los sensores |
| `0x01` | `FORCE_EXTRACTOR` — activa el extractor independientemente de los sensores |
| `0x02` | `FORCE_DEHUMID` — activa el deshumidificador independientemente de los sensores |
| `0x03` | `FORCE_OFF` — apaga ambos actuadores independientemente de los sensores |

> El override se mantiene hasta que se escriba `0x00` (AUTO). No persiste tras un reinicio del dispositivo.

---

### HORA — Reloj del sistema

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0000-1000-8000-000000000012` |
| Propiedades | READ, WRITE |
| Tamaño | 4 bytes |

Payload:

| Bytes | Tipo | Descripción |
|---|---|---|
| `[0..3]` | `uint32` | Segundos desde epoch UTC (Unix timestamp) |

- **READ**: devuelve la hora actual del sistema.
- **WRITE**: establece la hora del sistema. Necesario para que los registros de log tengan timestamp correcto.

Ejemplo de sincronización (Python):
```python
import struct, time
ts = int(time.time())
data = struct.pack('<I', ts)
# escribir data en la characteristic HORA
```

---

### CMD_DISPLAY — Control de la pantalla OLED

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0000-1000-8000-000000000013` |
| Propiedades | WRITE |
| Tamaño | 1 byte |

Payload:

| Valor | Efecto |
|---|---|
| `0x01` | Enciende la pantalla durante 30 segundos |
| `0x00` | Apaga la pantalla inmediatamente |

---

## Servicio de Log

**UUID**: `1b5ab4f0-0001-1000-8000-000000000000`

Permite consultar y descargar el histórico de transiciones de estado almacenado en flash NVS. Capacidad: **100 registros** (buffer circular — los más antiguos se sobreescriben).

### LOG_COUNT — Número de registros almacenados

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0001-1000-8000-000000000001` |
| Propiedades | READ |
| Tamaño | 4 bytes |

Payload: `uint32` LE con el número de registros actualmente almacenados (0–100).

---

### LOG_CTRL — Control del log

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0001-1000-8000-000000000002` |
| Propiedades | WRITE |
| Tamaño | 1 byte |

Comandos:

| Valor | Efecto |
|---|---|
| `0x01` | Inicia la descarga del log completo como CSV por `LOG_DATA` |
| `0x02` | Borra el log (irreversible) — `LOG_COUNT` vuelve a 0 |

---

### LOG_DATA — Datos CSV del log

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0001-1000-8000-000000000003` |
| Propiedades | NOTIFY |
| Tamaño | variable (chunks de texto) |

Para iniciar una descarga:
1. Activar notificaciones en `LOG_DATA` (escribir `0x0001` en CCCD).
2. Escribir `0x01` en `LOG_CTRL`.
3. Recibir chunks de texto (notificaciones sucesivas) y concatenarlos.
4. La descarga termina cuando se recibe una notificación que contiene `---EOF---`.

Si el cliente se desconecta durante la descarga, la transferencia se cancela. Al reconectar se puede iniciar de nuevo.

#### Formato del CSV recibido

```
sep=;
Fecha;Hora;Estado_Anterior;Estado_Nuevo;Temp_Int;Hum_Int;Val_Int;Temp_Ext;Hum_Ext;Val_Ext
2025-07-01;14:32:05;IDLE;EXTRACTOR;24.3;71.2;1;19.8;55.0;1
2025-07-01;15:10:22;EXTRACTOR;IDLE;24.1;64.8;1;19.9;56.1;1
2025-07-01;15:11:03;IDLE;DESHUMID;24.5;72.0;1;19.7;72.8;1
2025-07-01;23:58:44;DESHUMID;FALLO;--;--;0;--;--;0
---EOF---
```

Columnas:

| Columna | Tipo | Descripción |
|---|---|---|
| `Fecha` | `YYYY-MM-DD` | Fecha UTC de la transición |
| `Hora` | `HH:MM:SS` | Hora UTC de la transición |
| `Estado_Anterior` | string | Estado antes del cambio |
| `Estado_Nuevo` | string | Estado después del cambio |
| `Temp_Int` | float | Temperatura interior en °C |
| `Hum_Int` | float | Humedad interior en %RH |
| `Val_Int` | `0`/`1` | `1` = lectura interior válida |
| `Temp_Ext` | float | Temperatura exterior en °C |
| `Hum_Ext` | float | Humedad exterior en %RH |
| `Val_Ext` | `0`/`1` | `1` = lectura exterior válida |

Nombres de estado en el CSV:

| Nombre CSV | Significado |
|---|---|
| `IDLE` | Reposo |
| `EXTRACTOR` | Extractor en marcha |
| `DESHUMID` | Deshumidificador en marcha |
| `FALLO` | SAFE_OFF — fallo de sensor |

> Si el reloj no estaba configurado (`HORA` no se escribió), la fecha aparece como `1970-01-01;00:00:00`.
> Los campos de sensores inválidos se muestran como `--`.

La primera línea `sep=;` permite abrir el fichero directamente en Excel con locale europeo (coma decimal) sin pasos de importación.

---

## Servicio de Configuración

**UUID**: `1b5ab4f0-0002-1000-8000-000000000000`

Permite leer y modificar en tiempo real los umbrales de humedad y los intervalos de muestreo. Los cambios son inmediatos y se aplican en la siguiente iteración del bucle de control. **No persisten tras un reinicio** del dispositivo.

### UMBRALES — Umbrales de humedad

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0002-1000-8000-000000000001` |
| Propiedades | READ, WRITE |
| Tamaño | 12 bytes |

Payload:

| Bytes | Tipo | Descripción | Defecto |
|---|---|---|---|
| `[0..3]` | `float` | Humedad de activación (%) | `70.0` |
| `[4..7]` | `float` | Humedad de desactivación (%) | `65.0` |
| `[8..11]` | `float` | Delta mínimo interior−exterior para elegir extractor (%) | `10.0` |

Restricciones (si no se cumplen, el write se descarta):
- `activación > desactivación`
- `delta > 0`

Ejemplo:
```python
import struct
# Leer
act, des, delta = struct.unpack_from('<fff', data)

# Escribir umbrales: activar=72%, desactivar=67%, delta=8%
data = struct.pack('<fff', 72.0, 67.0, 8.0)
```

---

### INTERVALOS — Intervalos de muestreo

| Campo | Valor |
|---|---|
| UUID | `1b5ab4f0-0002-1000-8000-000000000002` |
| Propiedades | READ, WRITE |
| Tamaño | 20 bytes |

Payload (5 × `uint32` LE, en **segundos**):

| Bytes | Tipo | Estado | Defecto | Descripción |
|---|---|---|---|---|
| `[0..3]` | `uint32` | `IDLE` | `600` | Intervalo entre lecturas en reposo |
| `[4..7]` | `uint32` | `EXTRACTOR_ON` | `120` | Intervalo con extractor activo |
| `[8..11]` | `uint32` | `DEHUMID_ON` | `300` | Intervalo con deshumidificador activo |
| `[12..15]` | `uint32` | `SAFE_OFF` | `30` | Intervalo en modo fallo de sensor |
| `[16..19]` | `uint32` | Confirmación | `30` | Lectura de confirmación tras cambio de estado |

Restricciones (si no se cumplen, el write se descarta):
- Todos los valores > `0`
- `SAFE_OFF` ≥ `5`

Ejemplo:
```python
import struct
# Leer
idle, ext, deh, safe, conf = struct.unpack_from('<IIIII', data)

# Escribir: acortar idle a 5 min, resto igual
data = struct.pack('<IIIII', 300, 120, 300, 30, 30)
```

---

## Secuencia de uso típica

```
1. Escanear dispositivos BLE → filtrar por nombre "Trastero"
2. Conectar
3. Descubrir servicios y characteristics
4. Sincronizar reloj:
     escribir timestamp actual en HORA (servicio 0000, char ...0012)
5. Activar notificaciones en SENSOR_INT, SENSOR_EXT, ESTADO
     → recibir actualizaciones cada 1 s
6. (Opcional) Leer o modificar UMBRALES e INTERVALOS (servicio 0002)
7. (Opcional) Descargar histórico:
     a. Activar notificaciones en LOG_DATA (servicio 0001, char ...0003)
     b. Leer LOG_COUNT para saber cuántos registros hay
     c. Escribir 0x01 en LOG_CTRL
     d. Acumular chunks hasta recibir "---EOF---"
     e. Guardar el CSV
8. Desconectar → el dispositivo reinicia advertising automáticamente
```

---

## Códigos de error / comportamiento ante escrituras inválidas

| Situación | Comportamiento del dispositivo |
|---|---|
| Write en `CMD_ACTUADOR` con byte `[0]` > 3 | Ignorado silenciosamente |
| Write en `UMBRALES` con `activación ≤ desactivación` o `delta ≤ 0` | Ignorado; imprime aviso por Serial |
| Write en `INTERVALOS` con algún valor `0` o `SAFE_OFF < 5` | Ignorado; imprime aviso por Serial |
| Write en `HORA` con payload < 4 bytes | Ignorado |
| Write en `LOG_CTRL` con valor distinto de `0x01` / `0x02` | Ignorado |
| Desconexión durante descarga CSV | Stream cancelado; al reconectar se puede reiniciar |
