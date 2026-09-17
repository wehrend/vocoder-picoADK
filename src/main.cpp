// 12-Band-Filterbank-Vocoder auf generischem Raspberry Pi Pico +
// PCM5102A-Breakout, jetzt mit FreeRTOS statt bare-metal while-Schleife.
//
// AUFBAU:
// - Modulator (Stimme, Mic-Amp) -> 12 parallele Analyse-Bandpässe ->
//   Gleichrichter -> Attack/Release -> 12 Hüllkurven (siehe vocoder_band.h)
// - Carrier (Sägezahn aus Wavetable, tonhöhengesteuert per Poti) -> 12
//   parallele Synthese-Bandpässe (gleiche Mittenfrequenzen wie oben) ->
//   je mit der zugehörigen Hüllkurve skaliert -> aufsummiert -> Ausgang
//
// WARUM SÄGEZAHN STATT SINUS ALS CARRIER (Unterschied zur vorigen
// Envelope-Follower-Stufe): Ein Sinus hat nur bei einer einzigen Frequenz
// Energie. Ein Vocoder braucht aber einen Carrier mit Obertönen in JEDEM
// der 12 Analysebänder, sonst bleiben die oberen Bänder stumm. Ein
// Sägezahn liefert diese Obertöne (naive Wavetable, daher etwas Aliasing
// bei hohen Grundfrequenzen - für einen ersten funktionalen Vocoder
// akzeptiert, siehe "Nächster Schritt" im README).
//
// WARUM DER CARRIER-FREQUENZBEREICH NIEDRIGER LIEGT ALS BEIM ALTEN
// SINUS-TEST (80-400Hz statt 80-1500Hz): Je höher die Grundfrequenz,
// desto WENIGER Obertöne fallen unterhalb der oberen Analysegrenze
// (8kHz) - bei z.B. 1000Hz Grundfrequenz gäbe es nur 8 Obertöne bis
// 8kHz, bei 100Hz sind es 80. Für gute Bandabdeckung bleibt der Carrier
// bewusst tief.
//
// FREERTOS-AUFTEILUNG:
// - audioTask (hohe Priorität): exakt das, was vorher in der
//   while-Schleife lief - I2S-Ausgabe, Mic-Read + 12x Analyse/Synthese
//   PRO SAMPLE. Bleibt alleiniger Besitzer des ADCs (siehe unten, WICHTIG).
// - controlTask (niedrige Priorität): glättet/mapped den rohen Poti-Wert
//   auf eine Ziel-Carrier-Frequenz. Läuft mit fester, langsamer Rate statt
//   im Audio-Hot-Path - Vorbereitung für später mehr Nicht-Audio-Logik
//   (Taster, Anzeige, ...), ohne die Echtzeit-Schleife zu belasten.
//
// WICHTIG - GETEILTE ADC-PERIPHERIE: Poti und Mic hängen am selben
// RP2040-ADC (nur ein Hardware-Mux mit "welcher Kanal ist aktuell
// ausgewählt"-Zustand). Würde der controlTask selbst adc_select_input()/
// adc_read() aufrufen, könnte er dem audioTask mitten im Sample-Loop den
// Kanal wegreißen - der Mic-Read würde dann plötzlich vom Poti-Kanal
// lesen, ohne dass das an der Fehlermeldung sichtbar wäre (nur an
// kaputtem Audio). Deshalb: NUR audioTask fasst den ADC an. Der rohe
// Poti-Wert wird 1x pro Puffer in eine Queue geschrieben, die der
// controlTask konsumiert; das Ergebnis (Ziel-Carrier-Frequenz) kommt
// über eine zweite Queue zurück. Damit gibt es nur einen ADC-Besitzer,
// aber die Glättungs-/Mapping-Logik läuft trotzdem als eigener,
// niedrigpriorer Task.

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/adc.h"
#include "biquad.h"
#include "vocoder_band.h"
#include <cmath>
#include <cstdint>

namespace {

// --- Pin-/ADC-Zuordnung: unverändert zur vorigen Stufe ---
constexpr uint POT_ADC_GPIO       = 26;
constexpr uint8_t POT_ADC_CHANNEL = 0;
constexpr uint MIC_ADC_GPIO       = 27;
constexpr uint8_t MIC_ADC_CHANNEL = 1;

// Carrier-Tonhöhenbereich - bewusst tief, siehe Erklärung oben.
constexpr float kMinCarrierHz = 80.0f;
constexpr float kMaxCarrierHz = 400.0f;

// --- Vocoder-Filterbank-Parameter ---
constexpr int kNumBands = 12;
constexpr float kBandFreqLowHz  = 100.0f;
constexpr float kBandFreqHighHz = 8000.0f;
constexpr float kBandQ = 4.0f; // konstantes Q bei log-Staffelung = konstante relative Bandbreite
constexpr float kAttackMs  = 3.0f;
constexpr float kReleaseMs = 100.0f;
// Platzhalter wie schon bei der 1-Band-Stufe - hängt von Mic-Gain und
// Abstand zum Mic ab, auf echter Hardware experimentell nachjustieren
// (z.B. testweise printf auf die Summe vor der Skalierung).
constexpr float kMakeupGain = 8.0f;

constexpr uint32_t kSampleRateHz  = 44100;
constexpr uint32_t kBufferSamples = 256;

VocoderBand bands[kNumBands];

// Sägezahn-Wavetable für den Carrier (Ersatz für die Sinus-Tabelle der
// vorigen Stufe). Naiv (nicht band-limited) - siehe Aliasing-Hinweis oben.
constexpr int kCarrierTableSize = 512;
int16_t carrierTable[kCarrierTableSize];

void build_carrier_table() {
    for (int i = 0; i < kCarrierTableSize; ++i) {
        float phase = (float)i / (float)kCarrierTableSize; // 0..1
        float saw = 2.0f * phase - 1.0f;                   // -1..1 Rampe
        carrierTable[i] = (int16_t)(saw * 16000.0f);        // ~-6dBFS Headroom
    }
}

audio_buffer_pool_t *setup_audio() {
    static audio_format_t audioFormat = {
        .sample_freq = kSampleRateHz,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };
    static audio_buffer_format_t producerFormat = {
        .format = &audioFormat,
        .sample_stride = 4, // 2 Kanäle x 16-bit
    };

    // Pool-Größe von 3 auf 4 erhöht: FreeRTOS-Scheduling-Jitter (Ticks,
    // andere Tasks) gibt etwas mehr Grund für Sicherheitsmarge als die
    // vorige bare-metal-Schleife. Bei Knacken auf Hardware zuerst hier
    // weiter erhöhen, siehe README-Troubleshooting.
    audio_buffer_pool_t *pool = audio_new_producer_pool(&producerFormat, 4, kBufferSamples);

    audio_i2s_config_t i2sConfig = {
        .data_pin = PICO_AUDIO_I2S_DATA_PIN,
        .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
        .dma_channel = 0,
        .pio_sm = 0,
    };

    const audio_format_t *outputFormat = audio_i2s_setup(&audioFormat, &i2sConfig);
    if (!outputFormat) {
        panic("I2S-Setup fehlgeschlagen - Pins/Format pruefen");
    }

    audio_i2s_connect(pool);
    audio_i2s_set_enabled(true);
    return pool;
}

void setup_adc() {
    adc_init();
    adc_gpio_init(POT_ADC_GPIO);
    adc_gpio_init(MIC_ADC_GPIO);
}

float read_adc_normalized() {
    uint16_t raw = adc_read();
    return (float)raw / 4095.0f;
}

// Queues zur Kommunikation zwischen audioTask (ADC-Besitzer) und
// controlTask (Glättung/Mapping). Länge 1 + xQueueOverwrite: uns
// interessiert immer nur der JEWEILS AKTUELLSTE Wert, kein Backlog
// alter Poti-Stände - klassisches FreeRTOS-"Mailbox"-Pattern.
QueueHandle_t g_potRawQueue = nullptr;
QueueHandle_t g_carrierFreqQueue = nullptr;

void controlTask(void *) {
    float smoothedHz = 220.0f; // Startwert, bis der erste Wert reinkommt

    for (;;) {
        float potNorm;
        // Bis zu 50ms auf einen neuen Rohwert warten - kommt normalerweise
        // laengst vorher (1x pro Audio-Puffer, ca. alle 5.8ms bei 256
        // Samples/44.1kHz).
        if (xQueueReceive(g_potRawQueue, &potNorm, pdMS_TO_TICKS(50)) == pdTRUE) {
            float targetHz = kMinCarrierHz + potNorm * (kMaxCarrierHz - kMinCarrierHz);
            smoothedHz += (targetHz - smoothedHz) * 0.2f;
            xQueueOverwrite(g_carrierFreqQueue, &smoothedHz);
        }
        // Kein Grund, öfter als ~50x/Sekunde zu glätten - ein Poti
        // ändert sich nicht schneller, und das hält die Priorität dieses
        // Tasks bewusst niedrig/selten lauffähig.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void audioTask(void *) {
    build_carrier_table();
    init_vocoder_bands(bands, kNumBands, kBandFreqLowHz, kBandFreqHighHz,
                        kBandQ, kAttackMs, kReleaseMs, (float)kSampleRateHz);

    audio_buffer_pool_t *pool = setup_audio();
    setup_adc();

    uint32_t phase = 0;
    float carrierHz = 220.0f;

    for (;;) {
        // Poti (Carrier-Frequenz) einmal pro Puffer lesen reicht - siehe
        // Erklärung oben, warum NUR dieser Task den ADC anfasst.
        adc_select_input(POT_ADC_CHANNEL);
        float potNorm = read_adc_normalized();
        xQueueOverwrite(g_potRawQueue, &potNorm);

        // Nicht-blockierend abholen: falls der controlTask noch keinen
        // (neuen) Wert geliefert hat, einfach den letzten bekannten
        // carrierHz weiterverwenden statt die Audio-Schleife zu blockieren.
        xQueueReceive(g_carrierFreqQueue, &carrierHz, 0);
        uint32_t phaseInc = (uint32_t)((carrierHz * kCarrierTableSize / (float)kSampleRateHz) * 65536.0f);

        adc_select_input(MIC_ADC_CHANNEL);

        audio_buffer_t *buf = take_audio_buffer(pool, true);
        int16_t *samples = (int16_t *)buf->buffer->bytes;

        for (uint32_t i = 0; i < buf->max_sample_count; ++i) {
            // WICHTIG (wie schon bei der 1-Band-Stufe): Mic-Read +
            // Analyse/Synthese müssen PRO SAMPLE laufen, nicht einmal pro
            // Puffer - sonst filtern die Bandpässe nur 1 von 256 Samples.
            float micNorm = read_adc_normalized();
            float micBipolar = (micNorm - 0.5f) * 2.0f;

            int16_t carrierRaw = carrierTable[(phase >> 16) & (kCarrierTableSize - 1)];
            float carrierBipolar = (float)carrierRaw / 16000.0f;

            float mixed = 0.0f;
            for (int b = 0; b < kNumBands; ++b) {
                bands[b].analyze(micBipolar);
                mixed += bands[b].synthesize(carrierBipolar);
            }
            mixed *= kMakeupGain / (float)kNumBands;
            if (mixed > 1.0f) mixed = 1.0f;
            if (mixed < -1.0f) mixed = -1.0f;

            int16_t s = (int16_t)(mixed * 16000.0f);
            samples[2 * i]     = s; // links
            samples[2 * i + 1] = s; // rechts
            phase += phaseInc;
        }

        buf->sample_count = buf->max_sample_count;
        give_audio_buffer(pool, buf);
    }
}

} // namespace

int main() {
    stdio_init_all();

    g_potRawQueue = xQueueCreate(1, sizeof(float));
    g_carrierFreqQueue = xQueueCreate(1, sizeof(float));
    if (!g_potRawQueue || !g_carrierFreqQueue) {
        panic("Queue-Erstellung fehlgeschlagen (Heap zu klein?)");
    }

    // Stackgrößen sind in WORTEN (nicht Bytes) angegeben. 2048 Worte fuer
    // den audioTask ist grosszuegig bemessen (12 Baender x 2 Biquads +
    // verschachtelte Aufrufe) - falls configCHECK_FOR_STACK_OVERFLOW
    // anschlaegt, hier zuerst erhoehen; noch nicht auf Hardware vermessen.
    xTaskCreate(audioTask, "audio", 2048, nullptr, /*priority=*/3, nullptr);
    xTaskCreate(controlTask, "control", 512, nullptr, /*priority=*/1, nullptr);

    vTaskStartScheduler();

    // Wird nur erreicht, wenn der Scheduler aus Speichermangel gar nicht
    // erst startet.
    panic("vTaskStartScheduler() zurueckgekehrt - Heap zu klein?");
    return 0;
}
