// Hello-World für Audio auf dem PicoADK, jetzt erweitert um einen
// Envelope-Follower: das Poti steuert weiter die Carrier-Frequenz, das
// Mic-Signal (über Bandpass + Gleichrichter + Tiefpass) steuert jetzt
// die Lautstärke - das ist im Kern schon 1/16 eines Vocoders.

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "biquad.h"
#include <cmath>
#include <cstdint>

namespace {

// Laut DatanoiseTV/PicoADK-Hardware README ("Internal Signals"):
// GPIO25 = PCM5100A XSMT (Mute/Unmute), GPIO23 = PCM5100A DEMP.
// XSMT muss von der Software auf High gesetzt werden, sonst bleibt
// der DAC-Ausgang stumm geschaltet - das ist NICHT der Default-Zustand!
constexpr uint XSMT_PIN = 25;
constexpr uint DEMP_PIN = 23;

// Onboard-ADC128S102 (8-Kanal, 12-Bit, bis 1 MS/s) an SPI1.
constexpr uint ADC_SCK_PIN  = 10;
constexpr uint ADC_MOSI_PIN = 11;
constexpr uint ADC_MISO_PIN = 12;
constexpr uint ADC_CS_PIN   = 13;
#define ADC_SPI_PORT spi1

// Kanal 0 = Frequenz-Poti (wie im vorigen Schritt), Kanal 1 = Mic-Amp
// (MAX9814 oder MAX4466), Ausgang direkt an den ADC-Kanal - beide
// Module liefern schon einen ADC-tauglichen Bias, kein externes
// Bias-Netzwerk nötig.
constexpr uint8_t POT_ADC_CHANNEL = 0;
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
        panic("PCM5100A I2S setup fehlgeschlagen - Pins/Format pruefen");
    }

    audio_i2s_connect(pool);
    audio_i2s_set_enabled(true);
    return pool;
}

void setup_pot_adc() {
    spi_init(ADC_SPI_PORT, 2 * 1000 * 1000); // 2 MHz, ADC128S102 erlaubt bis ~3.2 MHz
    gpio_set_function(ADC_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(ADC_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(ADC_MISO_PIN, GPIO_FUNC_SPI);

    gpio_init(ADC_CS_PIN);
    gpio_set_dir(ADC_CS_PIN, GPIO_OUT);
    gpio_put(ADC_CS_PIN, 1);
}

// ADC128S102-Protokoll: 16 SCLK-Zyklen. Sendewort: 3-bit Kanaladresse
// (MSB-first) in den oberen Bits. Empfangswort: 4 führende Nullen,
// dann 12-bit Ergebnis. Pipeline-Delay von 1 Zyklus (Ergebnis gehört
// zur Kanaladresse aus dem VORHERIGEN Transfer) - für kontinuierliches
// Polling eines einzelnen Kanals unkritisch.
float read_adc_channel(uint8_t channel) {
    uint16_t txWord = (uint16_t)(channel & 0x07) << 12;
    uint8_t txBuf[2] = { (uint8_t)(txWord >> 8), (uint8_t)(txWord & 0xFF) };
    uint8_t rxBuf[2] = {0};

    gpio_put(ADC_CS_PIN, 0);
    spi_write_read_blocking(ADC_SPI_PORT, txBuf, rxBuf, 2);
    gpio_put(ADC_CS_PIN, 1);

    uint16_t raw = (((uint16_t)rxBuf[0] << 8) | rxBuf[1]) & 0x0FFF;
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

    // DAC unmuten (XSMT=high) und Deemphase aus (DEMP=low), bevor der
    // I2S-Stream startet - sonst bleibt der Ausgang stumm.
    gpio_init(XSMT_PIN);
    gpio_set_dir(XSMT_PIN, GPIO_OUT);
    gpio_put(XSMT_PIN, 1);

    gpio_init(DEMP_PIN);
    gpio_set_dir(DEMP_PIN, GPIO_OUT);
    gpio_put(DEMP_PIN, 0);

    audio_buffer_pool_t *pool = setup_audio();
    setup_pot_adc();

    EnvelopeFollower envFollower;
    envFollower.init(kEnvelopeBandHz, kEnvelopeQ, /*attackMs=*/5.0f, /*releaseMs=*/120.0f, (float)kSampleRateHz);

    // Phasenakkumulator in Wavetable-Schritten (Fixed-Point, 16.16)
    uint32_t phase = 0;
    float smoothedToneHz = 440.0f; // Startwert, bis der erste Poti-Read kommt

    while (true) {
        // Poti (Frequenz) einmal pro Puffer lesen reicht - ändert sich
        // langsam. Die Frequenz-Phaseninkrement wird pro Puffer neu
        // berechnet, gilt dann für alle Samples darin.
        float potNorm = read_adc_channel(POT_ADC_CHANNEL);
        float targetHz = kMinToneHz + potNorm * (kMaxToneHz - kMinToneHz);
        smoothedToneHz += (targetHz - smoothedToneHz) * 0.2f;
        uint32_t phaseInc = (uint32_t)((smoothedToneHz * kTableSize / (float)kSampleRateHz) * 65536.0f);

        audio_buffer_t *buf = take_audio_buffer(pool, true);
        int16_t *samples = (int16_t *)buf->buffer->bytes;

        for (uint32_t i = 0; i < buf->max_sample_count; ++i) {
            // WICHTIG: Mic-Sample + Bandpass + Hüllkurve müssen PRO
            // SAMPLE laufen, nicht einmal pro Puffer - sonst filtert
            // der Bandpass nur 1 von 256 Samples und die "Hüllkurve"
            // wird bedeutungslos (massives Unter-Abtasten).
            float micNorm = read_adc_channel(MIC_ADC_CHANNEL); // 0..1, Bias inklusive
            float micBipolar = (micNorm - 0.5f) * 2.0f;        // Bandpass entfernt den Rest-Bias
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
