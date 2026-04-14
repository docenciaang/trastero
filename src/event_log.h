// =============================================================================
// event_log.h — Registro de eventos de estado en NVS + servicio BLE de descarga
//
// Cada transición de estado del controlador se guarda en NVS (Non-Volatile
// Storage) como un registro binario compacto (24 bytes). NVS es atómico por
// diseño: no hay corrupción de superbloque ante cortes de corriente. Un segundo
// servicio BLE permite descargar el historial completo como CSV (separador ';',
// compatible con Excel en locale europeo).
//
// Servicio BLE de log:
//   UUID servicio:  1b5ab4f0-0001-1000-8000-000000000000
//
//   Characteristic   UUID suffix  Props    Payload
//   LOG_COUNT        ...0001      READ     4 B: uint32 LE — nº registros almacenados
//   LOG_CTRL         ...0002      WRITE    1 B: 0x01=iniciar descarga, 0x02=borrar log
//   LOG_DATA         ...0003      NOTIFY   chunks de texto CSV hasta "---EOF---\r\n"
//
// Formato CSV:
//   Línea 1: sep=;
//   Línea 2: Fecha;Hora;Estado_Anterior;Estado_Nuevo;Temp_Int;Hum_Int;Val_Int;...
//   Datos:   YYYY-MM-DD;HH:MM:SS;IDLE;EXTRACTOR;24.3;71.2;1;19.8;55.0;1
//   Final:   ---EOF---
// =============================================================================

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ble_gatt.h"   // SensorData, ControlState

// =============================================================================
// Structs de almacenamiento
// =============================================================================

struct __attribute__((packed)) LogRecord {
    uint32_t timestamp;   // [0..3]  unix UTC; 0 = reloj no configurado
    uint8_t  prevState;   // [4]     ControlState antes de la transición
    uint8_t  newState;    // [5]     ControlState después
    uint8_t  intValid;    // [6]     1 = lectura interior válida
    uint8_t  extValid;    // [7]     1 = lectura exterior válida
    float    intTemp;     // [8..11]
    float    intHum;      // [12..15]
    float    extTemp;     // [16..19]
    float    extHum;      // [20..23]
};

struct __attribute__((packed)) LogHeader {
    uint32_t magic;       // 0xDEAD1234 — detecta primera vez / corrupción
    uint16_t capacity;    // 5460 registros
    uint16_t count;       // registros válidos actualmente (0..capacity)
    uint32_t head;        // índice del registro más antiguo
    uint32_t tail;        // índice donde se escribirá el próximo
};

// =============================================================================
// Payload de la cola inter-tarea (vive solo en RAM)
// =============================================================================

struct LogEntry {
    uint32_t     timestamp;
    ControlState prevState;
    ControlState newState;
    SensorData   intSnap;
    SensorData   extSnap;
};

// =============================================================================
// Cola que taskControl usa para enviar eventos a taskEventLog sin bloquearse
// =============================================================================

extern QueueHandle_t eventLogQueue;

// =============================================================================
// API pública
// =============================================================================

// Abre el namespace NVS, inicializa la cabecera si es necesario y crea la cola.
// Llamar desde setup() antes de bleInit() y de crear tareas.
void eventLogInit();

// Encola un LogEntry de forma no bloqueante (descarta si la cola está llena).
// Llamar desde taskControl() FUERA del dataMutex.
void eventLogEnqueue(const LogEntry& e);

// Devuelve el número de registros almacenados (lectura directa, sin mutex).
uint32_t eventLogGetCount();

// Inicia la transferencia CSV por BLE (escribe 0x01 en LOG_CTRL).
void eventLogStartStream();

// Aborta una transferencia en curso (llamar en onDisconnect).
void eventLogResetStream();

// Borra el log completo (resetea header en flash).
// Llamar desde callback BLE (Core 0), toma logMutex internamente.
void eventLogClearLog();

// Tarea FreeRTOS: drena cola → escribe en flash; gestiona streaming CSV.
// Crear con xTaskCreatePinnedToCore en Core 0, prio 1, stack 4096.
void taskEventLog(void* param);
