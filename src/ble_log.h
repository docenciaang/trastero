// =============================================================================
// ble_log.h — Servicio BLE de descarga del registro de eventos
//
// Servicio BLE de log:
//   UUID servicio:  1b5ab4f0-0001-1000-8000-000000000000
//
//   Characteristic   UUID suffix  Props    Payload
//   LOG_COUNT        ...0001      READ     4 B: uint32 LE — nº registros almacenados
//   LOG_CTRL         ...0002      WRITE    1 B: 0x01=iniciar descarga, 0x02=borrar log
//   LOG_DATA         ...0003      NOTIFY   chunks de texto CSV hasta "---EOF---\r\n"
// =============================================================================

#pragma once

#include <BLEServer.h>
#include <BLECharacteristic.h>

// Puntero a la characteristic LOG_DATA (definido en ble_log.cpp,
// necesario en event_log.cpp para llamar a notify() desde taskEventLog).
extern BLECharacteristic* charLogData;

// Inicializa el servicio BLE de log sobre el servidor ya creado.
// Llamar desde bleInit() después de svc->start().
void bleLogServiceInit(BLEServer* server);
