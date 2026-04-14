// =============================================================================
// ble_gatt.h — Tipos compartidos e interfaz del servidor GATT BLE
//
// Este fichero define los tipos de datos compartidos entre main.cpp y
// ble_gatt.cpp, y declara las funciones publicas del modulo BLE.
//
// Interfaz GATT documentada:
//   Servicio:  TRASTERO_SVC_UUID  "1b5ab4f0-0000-1000-8000-00805f9b34fb"
//
//   Characteristic   UUID suffix  Props           Payload
//   SENSOR_INT       ...0001      READ, NOTIFY    9 B: float temp + float hum + uint8 valid
//   SENSOR_EXT       ...0002      READ, NOTIFY    9 B: igual
//   ESTADO           ...0003      READ, NOTIFY    1 B: 0=IDLE 1=EXTRACTOR 2=DEHUMID 3=SAFE
//   CMD_ACTUADOR     ...0010      WRITE           2 B: [0]=cmd [1]=reservado
//   UMBRALES         ...0011      READ, WRITE    12 B: float activar + float desactivar + float delta
//   HORA             ...0012      READ, WRITE     4 B: uint32 unix timestamp UTC
//   CMD_DISPLAY      ...0013      WRITE           1 B: 0x01=encender pantalla 30 s, 0x00=apagar
//
// Formato CMD_ACTUADOR byte [0]:
//   0 = AUTO (control automatico)
//   1 = FORZAR_EXTRACTOR
//   2 = FORZAR_DEHUMID
//   3 = FORZAR_OFF
//
// Todos los campos multibyte en little-endian (IEEE 754 para floats).
// =============================================================================

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <BLEServer.h>

// =============================================================================
// Tipos compartidos (definidos aqui, usados en main.cpp y ble_gatt.cpp)
// =============================================================================

struct SensorData {
    float temperature;
    float humidity;
    bool  valid;
    int   consecutiveFailures;
};

enum class ControlState : uint8_t {
    IDLE         = 0,
    EXTRACTOR_ON = 1,
    DEHUMID_ON   = 2,
    SAFE_OFF     = 3
};

enum class ManualOverride : uint8_t {
    NONE            = 0,
    FORCE_EXTRACTOR = 1,
    FORCE_DEHUMID   = 2,
    FORCE_OFF       = 3
};

// =============================================================================
// Funciones publicas del modulo BLE
// =============================================================================

// Inicializa el servidor GATT y arranca el advertising.
// Llamar desde setup() antes de crear la tarea.
void bleInit();

// Tarea FreeRTOS: envia notificaciones BLE cada 1 s.
// Crear con xTaskCreatePinnedToCore en Core 0.
void taskBLE(void* param);

// Devuelve true si hay un cliente BLE conectado actualmente.
bool bleIsConnected();
