// Envelope-Follower-Vocoderstufe (1/16 eines vollen Vocoders), portiert
// vom PicoADK auf einen GENERISCHEN Raspberry Pi Pico mit externem
// PCM5100A/PCM5102A-Breakout am I2S-Bus.
//
// UNTERSCHIEDE ZUR PICOADK-VERSION:
// 1. I2S-Pins sind hier frei wählbar (unten via CMake-Defines), da kein
//    fest verdrahteter interner DAC mehr vorliegt.
// 2. Kein ADC128S102/SPI mehr für Poti+Mic - der RP2040 hat 3 eigene
//    ADC-Kanäle (GPIO26-28), zwei davon reichen für Poti + Mic.
// 3. KEIN XSMT/DEMP-GPIO-Handling mehr nötig - das übernehmen bei den
//    meisten Billig-Breakouts feste Jumper/Lötbrücken auf dem Modul
//    selbst (siehe Checkliste unten in main()).

// Envelope-Follower-Logik (Bandpass -> Gleichrichter -> Attack/Release)
// und Biquad-Koeffizienten sind 1:1 vom PicoADK-Code übernommen - reine
// DSP-Mathematik, unabhängig von der Hardware-Plattform.

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/adc.h"
#include "biquad.h"
#include <cmath>
#include <cstdint>

namespace {

// RP2040-ADC-Kanäle: ADC0=GPIO26, ADC1=GPIO27, ADC2=GPIO28.
// Poti-Schleifer -> GPIO26 (Außenbeine -> 3V3/GND).
// Mic-Amp-Ausgang (MAX9814 oder MAX4466) -> GPIO27 - beide Module
// liefern schon einen ADC-tauglichen Bias, kein externes Bias-Netzwerk
// nötig.
constexpr uint POT_ADC_GPIO     = 26;
constexpr uint8_t POT_ADC_CHANNEL = 0;
constexpr uint MIC_ADC_GPIO     = 27;
constexpr uint8_t MIC_ADC_CHANNEL = 1;

// Frequenzbereich, den das Poti abdeckt (linear gemappt)
constexpr float kMinToneHz = 80.0f;
constexpr float kMaxToneHz = 1500.0f;

// Analyse-Bandpass für den Envelope-Follower: Mittenfrequenz im
// Bereich, wo Stimme energiereich ist. Q moderat, damit auch normales
// Sprechen (nicht nur ein reiner Ton) eine Hüllkurve liefert.
constexpr float kEnvelopeBandHz = 1000.0f;
constexpr float kEnvelopeQ = 1.5f;

constexpr uint32_t kSampleRateHz = 44100;
constexpr uint32_t kBufferSamples = 256;

// Wavetable statt sinf() pro Sample - schont die CPU (RP2040 hat keine FPU).
constexpr int kTableSize = 512;
int16_t sineTable[kTableSize];

void build_sine_table() {
    for (int i = 0; i < kTableSize; ++i) {
        float phase = (float)i / (float)kTableSize;
        // ~-6dBFS Headroom, damit nichts hart am Anschlag clippt
        sineTable[i] = (int16_t)(sinf(2.0f * (float)M_PI * phase) * 16000.0f);
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

    audio_buffer_pool_t *pool = audio_new_producer_pool(&producerFormat, 3, kBufferSamples);

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

// Nativer RP2040-ADC: 12-bit, 0..4095. Deutlich simpler als das
// ADC128S102-SPI-Protokoll vom PicoADK - kein Kommando-Framing, kein
// Pipeline-Delay. WICHTIG: adc_select_input() nur beim tatsächlichen
// Kanalwechsel aufrufen, nicht vor jedem einzelnen Read - der Mic-Kanal
// bleibt über den gesamten Sample-Loop eines Puffers aktiv ausgewählt,
// nur einmal pro Puffer wird kurz auf den Poti-Kanal gewechselt.
float read_adc_normalized() {
    uint16_t raw = adc_read();
    return (float)raw / 4095.0f;
}

// Einzelner Envelope-Follower: Bandpass -> Gleichrichter -> Attack/
// Release-Tiefpass. Das ist im Kern schon 1/16 eines Vocoder-Bands.
struct EnvelopeFollower {
    Biquad bandpass;
    float envelope = 0.0f;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    void init(float bandHz, float Q, float attackMs, float releaseMs, float sampleRate) {
        bandpass.setBandpass(bandHz, Q, sampleRate);
        attackCoeff  = expf(-1.0f / (0.001f * attackMs  * sampleRate));
        releaseCoeff = expf(-1.0f / (0.001f * releaseMs * sampleRate));
    }

    float process(float x) {
        float filtered = bandpass.process(x);
        float rectified = fabsf(filtered);
        float coeff = (rectified > envelope) ? attackCoeff : releaseCoeff;
        envelope = coeff * envelope + (1.0f - coeff) * rectified;
        return envelope;
    }
};

} // namespace

int main() {
    stdio_init_all();

    build_sine_table();

    // WICHTIG, unbedingt vor dem ersten Test prüfen:
    // Anders als beim PicoADK (wo XSMT/DEMP per Software-GPIO gesteuert
    // werden mussten) haben die meisten generischen PCM5102A-Breakout-
    // Module dafür feste Jumper/Lötbrücken AUF DEM MODUL SELBST:
    //   XSMT  -> auf HIGH/"un-mute" stellen
    //   FMT   -> auf LOW/"I2S" stellen (nicht Left-Justified)
    //   DEMP  -> auf LOW/"off" stellen
    //   FLT   -> auf LOW/"normal latency" stellen
    //   SCK   -> auf LOW/"kein externer Master-Clock" stellen
    // Falls dein Modul diese Signale stattdessen als GPIO-Pins herausführt,
    // müsstest du sie wie beim PicoADK-Code per gpio_init/gpio_put selbst
    // auf die obigen Pegel legen.

    audio_buffer_pool_t *pool = setup_audio();
    setup_adc();

    EnvelopeFollower envFollower;
    envFollower.init(kEnvelopeBandHz, kEnvelopeQ, /*attackMs=*/5.0f, /*releaseMs=*/120.0f, (float)kSampleRateHz);

    // Phasenakkumulator in Wavetable-Schritten (Fixed-Point, 16.16)
    uint32_t phase = 0;
    float smoothedToneHz = 440.0f; // Startwert, bis der erste Poti-Read kommt

    while (true) {
        // Poti (Frequenz) einmal pro Puffer lesen reicht - ändert sich
        // langsam. Kurzer Kanalwechsel, dann sofort zurück auf Mic für
        // den Sample-Loop.
        adc_select_input(POT_ADC_CHANNEL);
        float potNorm = read_adc_normalized();
        float targetHz = kMinToneHz + potNorm * (kMaxToneHz - kMinToneHz);
        smoothedToneHz += (targetHz - smoothedToneHz) * 0.2f;
        uint32_t phaseInc = (uint32_t)((smoothedToneHz * kTableSize / (float)kSampleRateHz) * 65536.0f);

        adc_select_input(MIC_ADC_CHANNEL);

        audio_buffer_t *buf = take_audio_buffer(pool, true);
        int16_t *samples = (int16_t *)buf->buffer->bytes;

        for (uint32_t i = 0; i < buf->max_sample_count; ++i) {
            // WICHTIG: Mic-Sample + Bandpass + Hüllkurve müssen PRO
            // SAMPLE laufen, nicht einmal pro Puffer - sonst filtert
            // der Bandpass nur 1 von 256 Samples und die "Hüllkurve"
            // wird bedeutungslos (massives Unter-Abtasten).
            float micNorm = read_adc_normalized(); // 0..1, Bias inklusive
            float micBipolar = (micNorm - 0.5f) * 2.0f; // Bandpass entfernt den Rest-Bias
            float envelope = envFollower.process(micBipolar);
            // Hüllkurve ist roh oft sehr leise/laut je nach Mic-Gain -
            // grobe Normalisierung, bei Bedarf Faktor anpassen/messen.
            float scaledEnvelope = envelope * 6.0f;
            if (scaledEnvelope > 1.0f) scaledEnvelope = 1.0f;

            int16_t s = (int16_t)(sineTable[(phase >> 16) & (kTableSize - 1)] * scaledEnvelope);
            samples[2 * i]     = s; // links
            samples[2 * i + 1] = s; // rechts
            phase += phaseInc;
        }

        buf->sample_count = buf->max_sample_count;
        give_audio_buffer(pool, buf);
    }

    return 0;
}