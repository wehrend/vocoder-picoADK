// 12-Band-Filterbank-Vocoder auf generischem Raspberry Pi Pico +
// PCM5102A-Breakout, mit FreeRTOS statt bare-metal while-Schleife.
//
// RECHENPFAD: Q16.16 Fixed-Point (siehe fixed_point.h/biquad_fixed.h/
// vocoder_band_fixed.h), NICHT float. Grund: eine erste float-Version
// war auf dem FPU-losen RP2040 bei 44.1kHz/12 Bändern um Faktor ~8.6x
// zu langsam - siehe DEVLOG für die Messung samt der Erkenntnis, dass
// -O3 gegenüber -O0 dabei NICHTS half (Softfloat-Bibliotheksaufrufe
// werden durch Optimierung nicht schneller). Nur die Filter-Koeffizienten
// (setBandpass, Attack/Release) werden weiterhin einmalig in float
// berechnet - das läuft nicht im Sample-Hot-Path.
//
// AUFBAU:
// - Modulator (Stimme, Mic-Amp) -> 12 parallele Analyse-Bandpässe ->
//   Gleichrichter -> Attack/Release -> 12 Hüllkurven (siehe vocoder_band_fixed.h)
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
#include "pico/time.h"
#include "hardware/adc.h"
#include "fixed_point.h"
#include "biquad_fixed.h"
#include "vocoder_band_fixed.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

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
constexpr float kBandQ = 2.0f; // von 4.0 gesenkt - schmalere Bänder klingeln länger/reagieren träger, siehe DEVLOG
// Tiefstes -> höchstes Band: langsam/schmal -> schnell (siehe init_vocoder_bands_fixed)
constexpr float kAttackMsLow   = 8.0f;
constexpr float kAttackMsHigh  = 1.5f;
constexpr float kReleaseMsLow  = 150.0f;
constexpr float kReleaseMsHigh = 40.0f;
// Platzhalter wie schon bei der 1-Band-Stufe - hängt von Mic-Gain und
// Abstand zum Mic ab, auf echter Hardware experimentell nachjustieren
// (z.B. testweise printf auf die Summe vor der Skalierung).
constexpr float kMakeupGain = 8.0f;

// Von 44.1kHz auf 22.05kHz gesenkt - verdoppelt das Zeitbudget pro
// Sample. Filterbank bleibt gueltig: hoechste Bandfrequenz ist 8kHz,
// klar unter der neuen Nyquist-Grenze von 11.025kHz. Siehe DEVLOG fuer
// die Messung, die diesen Schritt noetig gemacht hat (auch nach
// Fixed-Point-Umstellung + Multiplikations-Reduktion allein reichte es
// laut Abschaetzung noch nicht ganz).
constexpr uint32_t kSampleRateHz  = 22050;
constexpr uint32_t kBufferSamples = 256;

VocoderBandFixed bands[kNumBands];
q16 g_makeupGainQ16 = 0; // wird einmalig in audioTask() gesetzt (float_to_q16 gehoert nicht in den Hot Path)

// Sägezahn-Wavetable für den Carrier, jetzt direkt in Q16.16 statt
// int16 - erspart eine Konversion pro Sample im Hot Path.
constexpr int kCarrierTableSize = 512;
q16 carrierTable[kCarrierTableSize];

void build_carrier_table() {
    for (int i = 0; i < kCarrierTableSize; ++i) {
        float phase = (float)i / (float)kCarrierTableSize; // 0..1
        float saw = 2.0f * phase - 1.0f;                   // -1..1 Rampe
        carrierTable[i] = float_to_q16(saw);
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

// Wie read_adc_normalized(), aber direkt in Q16.16 statt float - fuer
// den Mic-Read im Sample-Hot-Path. RP2040-ADC ist 12-Bit (0..4095);
// (raw-2048)<<5 mappt das naeherungsweise auf -1.0..+1.0 in Q16.16
// (2048 = Mittelwert des 12-Bit-Bereichs, 32 = 65536/2048), komplett
// ohne Division oder float - nur Subtraktion + Shift.
inline q16 read_adc_bipolar_q16() {
    int32_t raw = (int32_t)adc_read();
    return (raw - 2048) << 5;
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
        // laengst vorher (1x pro Audio-Puffer, ca. alle 11.6ms bei 256
        // Samples/22.05kHz).
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
    init_vocoder_bands_fixed(bands, kNumBands, kBandFreqLowHz, kBandFreqHighHz,
                              kBandQ, kAttackMsLow, kAttackMsHigh,
                              kReleaseMsLow, kReleaseMsHigh, (float)kSampleRateHz);
    // float_to_q16() nur hier beim einmaligen Setup, nicht im Hot Path.
    g_makeupGainQ16 = float_to_q16(kMakeupGain / (float)kNumBands);

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

        // --- DIAGNOSE: bei Stottern hier ansetzen, danach wieder entfernen ---
        // Vergleicht die tatsächliche Verarbeitungszeit pro Puffer mit dem
        // dafür verfügbaren Zeitbudget (256 Samples bei 22.05kHz = 11610us).
        // Zwei mögliche Ergebnisse:
        //   - avgUs/maxUs liegen NAHE oder ÜBER kBufferBudgetUs
        //     -> CPU-Budget ist tatsächlich das Problem (siehe README)
        //   - avgUs/maxUs liegen KOMFORTABEL darunter
        //     -> das Stottern kommt von woanders (z.B. I2S-IRQ wird durch
        //        FreeRTOS-kritische-Abschnitte verzögert), CPU ist nicht
        //        der Flaschenhals
        constexpr uint32_t kBufferBudgetUs = (kBufferSamples * 1000000ull) / kSampleRateHz;
        static uint32_t sBufferCount = 0;
        static uint64_t sSumUs = 0;
        static uint32_t sMaxUs = 0;
        // Feinere Aufteilung: wie viel von der Zeit geht in adc_read()
        // vs. in die 24 Biquad-Aufrufe (Analyse+Synthese)? Klärt, ob die
        // ADC-Konversion selbst der Flaschenhals ist (siehe Diagnose-
        // Ausgabe: "adc=" vs "bands=").
        static uint64_t sAdcUs = 0;
        static uint64_t sBandsUs = 0;
        uint64_t loopStartUs = time_us_64();
        // --- Ende Diagnose-Setup ---

        for (uint32_t i = 0; i < buf->max_sample_count; ++i) {
            // WICHTIG (wie schon bei der 1-Band-Stufe): Mic-Read +
            // Analyse/Synthese müssen PRO SAMPLE laufen, nicht einmal pro
            // Puffer - sonst filtern die Bandpässe nur 1 von 256 Samples.
            uint64_t tAdcStart = time_us_64();
            q16 micQ16 = read_adc_bipolar_q16();
            uint64_t tAdcEnd = time_us_64();

            q16 carrierQ16 = carrierTable[(phase >> 16) & (kCarrierTableSize - 1)];

            q16 mixed = 0;
            for (int b = 0; b < kNumBands; ++b) {
                bands[b].analyze(micQ16);
                mixed += bands[b].synthesize(carrierQ16);
            }
            mixed = q16_mul(mixed, g_makeupGainQ16);
            if (mixed > kQ16One) mixed = kQ16One;
            if (mixed < -kQ16One) mixed = -kQ16One;
            uint64_t tBandsEnd = time_us_64();

            sAdcUs += (tAdcEnd - tAdcStart);
            sBandsUs += (tBandsEnd - tAdcEnd);

            // Q16.16 (~+-1.0) -> int16 PCM (~+-32767), per Ganzzahl-
            // Multiplikation+Shift statt float-Division.
            int32_t s32 = (int32_t)(((int64_t)mixed * 32767) >> kQ16Frac);
            if (s32 > 32767) s32 = 32767;
            if (s32 < -32768) s32 = -32768;
            int16_t s = (int16_t)s32;
            samples[2 * i]     = s; // links
            samples[2 * i + 1] = s; // rechts
            phase += phaseInc;
        }

        buf->sample_count = buf->max_sample_count;
        give_audio_buffer(pool, buf);

        // --- DIAGNOSE (Fortsetzung von oben) ---
        uint32_t elapsedUs = (uint32_t)(time_us_64() - loopStartUs);
        sSumUs += elapsedUs;
        if (elapsedUs > sMaxUs) sMaxUs = elapsedUs;
        if (++sBufferCount >= 100) {
            // __DATE__/__TIME__ werden vom Compiler bei JEDEM Build neu
            // eingesetzt (Zeitpunkt der Kompilierung dieser Datei) - so
            // lässt sich zweifelsfrei prüfen, ob das Board wirklich die
            // zuletzt gebaute Firmware fährt, ganz ohne picotool/USB.
            printf("Diagnose[%s %s]: avg=%luus max=%luus budget=%luus adc=%luus bands=%luus (100 Puffer)\n",
                   __DATE__, __TIME__,
                   (unsigned long)(sSumUs / sBufferCount), (unsigned long)sMaxUs,
                   (unsigned long)kBufferBudgetUs,
                   (unsigned long)(sAdcUs / sBufferCount), (unsigned long)(sBandsUs / sBufferCount));
            sBufferCount = 0;
            sSumUs = 0;
            sMaxUs = 0;
            sAdcUs = 0;
            sBandsUs = 0;
        }
        // --- Ende Diagnose ---
    }
}

} // namespace

// FreeRTOS ruft diese beiden Hooks aus dem Kernel heraus auf (tasks.c
// bzw. heap_4.c), erwartet aber, dass die Anwendung sie bereitstellt -
// aktiviert über configCHECK_FOR_STACK_OVERFLOW=2 und
// configUSE_MALLOC_FAILED_HOOK=1 in FreeRTOSConfig.h. Ohne diese beiden
// Funktionen bricht der Link mit "undefined reference" ab.
// extern "C", weil sie aus reinem C-Code (tasks.c/heap_4.c) aufgerufen
// werden - ohne extern "C" würde der C++-Compiler den Funktionsnamen
// mit anderer Signatur "mangeln" und der Linker fände sie trotz
// vorhandener Definition nicht.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    panic("Stack-Overflow in Task: %s", pcTaskName);
}

extern "C" void vApplicationMallocFailedHook(void) {
    panic("FreeRTOS malloc fehlgeschlagen - configTOTAL_HEAP_SIZE in FreeRTOSConfig.h zu klein?");
}

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