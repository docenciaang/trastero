// =============================================================================
// ble_log.cpp — Servicio BLE de descarga del registro de eventos
//
// Expone tres characteristics sobre el servicio 1b5ab4f0-0001-...:
//   LOG_COUNT  lectura del número de registros almacenados
//   LOG_CTRL   escritura de comandos (iniciar descarga / borrar log)
//   LOG_DATA   notificaciones CSV hasta "---EOF---"
// =============================================================================

#include "ble_log.h"
#include "event_log.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =============================================================================
// UUIDs del servicio de log
// =============================================================================

#define LOG_SVC_UUID        "1b5ab4f0-0001-1000-8000-000000000000"
#define CHAR_LOG_COUNT_UUID "1b5ab4f0-0001-1000-8000-000000000001"
#define CHAR_LOG_CTRL_UUID  "1b5ab4f0-0001-1000-8000-000000000002"
#define CHAR_LOG_DATA_UUID  "1b5ab4f0-0001-1000-8000-000000000003"

// =============================================================================
// Characteristics del servicio de log
// =============================================================================

BLECharacteristic* charLogData  = nullptr;

static BLECharacteristic* charLogCount = nullptr;
static BLECharacteristic* charLogCtrl  = nullptr;

// =============================================================================
// Callbacks BLE del servicio de log
// =============================================================================

class LogCountCallback : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) override {
        uint32_t n = eventLogGetCount();
        c->setValue((uint8_t*)&n, sizeof(n));
    }
};

class LogCtrlCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (c->getLength() < 1) return;
        uint8_t cmd = c->getData()[0];
        if (cmd == 0x01) {
            eventLogStartStream();
            Serial.println("[BLE] LOG_CTRL: inicio descarga CSV");
        } else if (cmd == 0x02) {
            eventLogClearLog();
        }
    }
};

// =============================================================================
// Inicialización del servicio BLE de log
// =============================================================================

void bleLogServiceInit(BLEServer* server) {
    BLEService* logSvc = server->createService(BLEUUID(LOG_SVC_UUID), 15);

    charLogCount = logSvc->createCharacteristic(
        CHAR_LOG_COUNT_UUID,
        BLECharacteristic::PROPERTY_READ);
    charLogCount->setCallbacks(new LogCountCallback());

    charLogCtrl = logSvc->createCharacteristic(
        CHAR_LOG_CTRL_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charLogCtrl->setCallbacks(new LogCtrlCallback());

    charLogData = logSvc->createCharacteristic(
        CHAR_LOG_DATA_UUID,
        BLECharacteristic::PROPERTY_NOTIFY);
    charLogData->addDescriptor(new BLE2902());

    logSvc->start();

    // Anunciar también el UUID del servicio de log
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(LOG_SVC_UUID);

    Serial.println("[BLE] Servicio de log iniciado");
}
