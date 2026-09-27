/**
 * DiagnosticLED.cpp
 *
 * Implementazione della classe DiagnosticLED.
 * Gestisce il LED diagnostico con pattern di lampeggio non-bloccanti
 * basati su millis(), pilotando il WS2812 tramite RMT.
 */

#include "DiagnosticLED.h"

namespace {

const uint8_t BRIGHTNESS = 10;

const uint32_t RMT_RESOLUTION_HZ = 10000000;  // 100 ns ticks

// WS2812 800 kHz bit timings, in ticks
const uint16_t T0H = 4, T0L = 8, T1H = 8, T1L = 4;

// 300 us low (2 x 1500 ticks) ends every frame, so the pixel latches before TX-done and
// a back-to-back write cannot merge into the same frame.
const uint16_t LATCH_HALF_TICKS = 1500;

// A frame takes ~330 us. The core sets TX-done from the RMT ISR via the FreeRTOS timer
// queue and ignores a full queue, so an unbounded wait can block the loop task forever.
const uint32_t WRITE_TIMEOUT_MS = 10;

uint8_t scale(uint32_t c) {
    return (uint8_t)(((c & 0xFF) * (BRIGHTNESS + 1)) >> 8);
}

}  // namespace

// Costruttore — inizializza i membri a valori predefiniti
DiagnosticLED::DiagnosticLED()
    : _pin(LED_BUILTIN_PIN),
      _state(CONNECTING),
      _ledOn(false),
      _rmtReady(false),
      _previousMillis(0),
      _currentColor(0) {}

void DiagnosticLED::writeFrame(uint32_t color) {
    if (!_rmtReady) return;

    const uint8_t grb[] = {scale(color >> 8), scale(color >> 16), scale(color)};
    rmt_data_t* s = _frame;
    for (uint8_t value : grb) {
        for (uint8_t mask = 0x80; mask; mask >>= 1) {
            const bool one = value & mask;
            s->level0 = 1;
            s->duration0 = one ? T1H : T0H;
            s->level1 = 0;
            s->duration1 = one ? T1L : T0L;
            s++;
        }
    }
    s->level0 = 0;
    s->duration0 = LATCH_HALF_TICKS;
    s->level1 = 0;
    s->duration1 = LATCH_HALF_TICKS;

    // Best effort: a dropped frame only leaves the LED showing its previous colour.
    rmtWrite(_pin, _frame, FRAME_SYMBOLS, WRITE_TIMEOUT_MS);
}

void DiagnosticLED::setPixelColorAndShow(uint32_t color) {
    if (color == _currentColor) return;
    _currentColor = color;
    writeFrame(color);
}

void DiagnosticLED::begin(uint8_t pin) {
    _pin = pin;

    // Assicurati che l'alimentazione del NeoPixel sia attiva
    pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
    digitalWrite(NEOPIXEL_POWER_PIN, HIGH);

    _rmtReady = rmtInit(_pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_RESOLUTION_HZ);
    _currentColor = COLOR_OFF;
    writeFrame(COLOR_OFF); // Spento inizialmente

    _ledOn = false;
    _state = CONNECTING;
    _previousMillis = millis();
}

// Cambia lo stato corrente e resetta il timer del pattern
void DiagnosticLED::setState(LedState state) {
    if (_state == state) return;  // Nessun cambiamento necessario

    _state = state;
    _previousMillis = millis();
    _ledOn = false;
    setPixelColorAndShow(COLOR_OFF);
}

// Aggiorna il pattern di lampeggio in base allo stato corrente
void DiagnosticLED::update() {
    unsigned long currentMillis = millis();

    switch (_state) {
        case CONNECTING:
            if (currentMillis - _previousMillis >= LED_BLINK_SLOW_MS / 2) {
                _previousMillis = currentMillis;
                _ledOn = !_ledOn;
                setPixelColorAndShow(_ledOn ? COLOR_CONNECTING : COLOR_OFF);
            }
            break;

        case OPERATIONAL:
            if (_ledOn) {
                if (currentMillis - _previousMillis >= 100) {
                    _previousMillis = currentMillis;
                    _ledOn = false;
                    setPixelColorAndShow(COLOR_OFF);
                }
            } else {
                if (currentMillis - _previousMillis >= 5000) {
                    _previousMillis = currentMillis;
                    _ledOn = true;
                    setPixelColorAndShow(COLOR_OPERATIONAL);
                }
            }
            break;

        case AP_MODE:
            {
                unsigned long phase = (currentMillis - _previousMillis) % 8000;
                if (phase < 4000) {
                    if ((phase / 125) % 2 == 0) {
                        setPixelColorAndShow(COLOR_AP_MODE_BLUE);
                    } else {
                        setPixelColorAndShow(COLOR_AP_MODE_RED);
                    }
                } else {
                    setPixelColorAndShow(COLOR_OFF);
                }
            }
            break;

        case ERROR:
            if (currentMillis - _previousMillis >= LED_BLINK_FAST_MS / 2) {
                _previousMillis = currentMillis;
                _ledOn = !_ledOn;
                setPixelColorAndShow(_ledOn ? COLOR_ERROR : COLOR_OFF);
            }
            break;
    }
}

// Restituisce lo stato corrente del LED
LedState DiagnosticLED::getState() const {
    return _state;
}

// Restituisce l'ultimo colore impostato
uint32_t DiagnosticLED::getCurrentColor() const {
    return _currentColor;
}
