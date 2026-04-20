// =============================================================================
// ble_config.h — Servicio BLE de configuracion del sistema
//
// Servicio UUID: 1b5ab4f0-0002-1000-8000-000000000000
//
//   Characteristic   UUID suffix  Props         Payload
//   UMBRALES         ...0001      READ, WRITE   12 B: float activar + float desactivar + float delta
//   INTERVALOS       ...0002      READ, WRITE   20 B: 5 × uint32 LE (segundos)
//                                               [0..3]  idle
//                                               [4..7]  extractor activo
//                                               [8..11] deshumidificador activo
//                                               [12..15] safe-off (fallo sensor)
//                                               [16..19] confirmacion (post-cambio)
//   SCHEDULE         ...0003      READ, WRITE   18 B: uint8 enabled + uint8 count + 4×SchedulePeriod
// =============================================================================

#pragma once

#include <BLEServer.h>

// =============================================================================
// Structs de planificacion diaria
// =============================================================================

struct __attribute__((packed)) SchedulePeriod {
    uint8_t startHour;   // 0-23
    uint8_t startMin;    // 0-59
    uint8_t endHour;     // 0-23
    uint8_t endMin;      // 0-59
};

struct __attribute__((packed)) Schedule {
    uint8_t        enabled;    // 0=deshabilitada, 1=habilitada
    uint8_t        count;      // 0-4 periodos activos
    SchedulePeriod periods[4]; // siempre 4 entradas; las no usadas son ceros
};
// sizeof(Schedule) == 18 bytes

// Carga umbrales e intervalos desde NVS al arranque.
// Llamar desde setup() antes de bleInit() y de crear tareas.
void bleConfigLoad();

// Lee el motivo de reset, actualiza el contador en NVS y muestra por Serial.
// Llamar desde setup() justo despues de bleConfigLoad().
void resetInfoInit();

// Inicializa el servicio de configuracion sobre el servidor ya creado.
// Llamar desde bleInit() tras arrancar los otros servicios.
void bleConfigServiceInit(BLEServer* server);

// Acceso de solo lectura a los datos de reset (para la pantalla OLED).
uint32_t    resetInfoGetCount();
uint8_t     resetInfoGetReason();
const char* resetInfoReasonStr(uint8_t reason);
