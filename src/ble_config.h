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
// =============================================================================

#pragma once

#include <BLEServer.h>

// Carga umbrales e intervalos desde NVS al arranque.
// Llamar desde setup() antes de bleInit() y de crear tareas.
void bleConfigLoad();

// Inicializa el servicio de configuracion sobre el servidor ya creado.
// Llamar desde bleInit() tras arrancar los otros servicios.
void bleConfigServiceInit(BLEServer* server);
