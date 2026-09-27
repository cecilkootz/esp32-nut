#ifndef DIAGNOSTIC_LED_H
#define DIAGNOSTIC_LED_H

#include <Arduino.h>

// Pin predefinito per il LED integrato della ESP32-S3-DevKitC-1
#define LED_BUILTIN_PIN 48
#define NEOPIXEL_POWER_PIN 38

// Costanti per i periodi di lampeggio (millisecondi)
static const unsigned long LED_BLINK_SLOW_MS = 1000;  // Lampeggio lento (CONNECTING)
static const unsigned long LED_BLINK_FAST_MS = 125;   // Lampeggio veloce (ERROR)

constexpr uint32_t ledColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

#define COLOR_CONNECTING ledColor(255, 255, 0)
#define COLOR_OPERATIONAL ledColor(0, 255, 0)
#define COLOR_ERROR ledColor(255, 0, 0)
#define COLOR_AP_MODE_BLUE ledColor(0, 0, 255)
#define COLOR_AP_MODE_RED ledColor(255, 0, 0)
#define COLOR_OFF 0

// Stati diagnostici del LED
enum LedState {
    CONNECTING,   // Lampeggio lento — connessione in corso
    OPERATIONAL,  // LED verde flash 200ms ogni 4s — funzionamento normale
    ERROR,        // Lampeggio veloce rosso — condizione di errore
    AP_MODE       // Animazione 8s (4s fast blue/red, 4s off) — modalità configurazione
};

class DiagnosticLED {
public:
    DiagnosticLED();

    // Inizializza il pin del LED; default = LED_BUILTIN_PIN (GPIO 47)
    void begin(uint8_t pin = LED_BUILTIN_PIN);

    // Imposta lo stato corrente del LED
    void setState(LedState state);

    // Aggiorna il pattern di lampeggio — chiamare nel loop principale
    void update();

    // Restituisce lo stato corrente
    LedState getState() const;

    // Restituisce l'ultimo colore impostato
    uint32_t getCurrentColor() const;

private:
    static constexpr size_t FRAME_SYMBOLS = 25;  // 24 GRB bits + latch

    uint8_t _pin;
    LedState _state;
    bool _ledOn;
    bool _rmtReady;
    unsigned long _previousMillis;
    uint32_t _currentColor;
    // Not a local: rmtWrite() can time out before the driver has finished reading it.
    rmt_data_t _frame[FRAME_SYMBOLS];

    void setPixelColorAndShow(uint32_t color);
    void writeFrame(uint32_t color);
};

#endif // DIAGNOSTIC_LED_H
