# I2S Sine Hello World (PicoADK / PCM5100A)

Schrittweiser Testaufbau Richtung VC16-artiger Vocoder auf dem
PicoADK. Kein FreeRTOS bisher - jeder Schritt baut auf dem vorigen
auf und verifiziert einen weiteren Baustein, bevor mehr Komplexität
dazukommt.

**Aktueller Stand: Schritt 3 (Envelope-Follower) läuft.**

## Was das Programm aktuell macht

1. Erzeugt einen Sinuston per Wavetable (512 Punkte, einmalig `sinf()`
   beim Start, danach nur Tabellen-Lookup - RP2040 hat keine FPU)
2. Die **Frequenz** des Tons wird von einem Poti gesteuert (ADC128S102,
   Kanal 0), linear auf 80–1500 Hz gemappt, leicht geglättet gegen
   Zipper-Noise
3. Die **Lautstärke** des Tons wird von einem Mic-Signal gesteuert
   (ADC128S102, Kanal 1): Bandpass (~1kHz) → Gleichrichter →
   Attack/Release-Hüllkurve. Das ist im Kern schon 1/16 eines
   vollständigen Filterbank-Vocoders.
4. Sample-Rate 44.1 kHz, Puffergröße 256 Samples, Ausgabe über I2S an
   den PCM5100A

## Pins (verifiziert gegen DatanoiseTV/PicoADK-Hardware README)

### I2S zum PCM5100A - intern fest verdrahtet, NICHT am Header

```
GPIO16 = PCM5100A I2S DAT
GPIO17 = PCM5100A I2S BCLK
GPIO18 = PCM5100A I2S LRCLK
GPIO25 = PCM5100A XSMT (Mute/Unmute) - muss auf HIGH gesetzt werden
GPIO23 = PCM5100A DEMP (Deemphase 44.1kHz) - LOW, wenn nicht gebraucht
```

XSMT/DEMP werden in `main.cpp` vor dem I2S-Setup gesetzt - ohne HIGH
auf XSMT bleibt der DAC-Ausgang stumm, selbst bei korrektem I2S.
Quelle: https://github.com/DatanoiseTV/PicoADK-Hardware (Abschnitt
"Internal Signals").

### SPI-ADC (ADC128S102) für Poti und Mic - auch am Header verfügbar

```
GPIO10 = SCK
GPIO11 = MOSI
GPIO12 = MISO
GPIO13 = CSn
```

Kanalzuordnung (in `main.cpp` als `POT_ADC_CHANNEL` /
`MIC_ADC_CHANNEL` änderbar):

```
Kanal 0 = Frequenz-Poti  (Schleifer -> Kanal 0, Außenbeine -> 3V3/GND)
Kanal 1 = Mic-Amp-Ausgang (MAX9814 oder MAX4466)
```

### Mic-Amp-Verdrahtung (MAX9814 und MAX4466 identisch anschließbar)

```
Mic-Modul VCC -> 3V3
Mic-Modul GND -> GND
Mic-Modul OUT -> ADC128S102 Kanal 1
```

Kein zusätzliches Bias-Netzwerk nötig - beide Module liefern bereits
ein ADC-taugliches, vorgespanntes Signal (MAX9814: 1,25V Bias,
MAX4466: ~VCC/2 Bias). Der Bandpass im Code entfernt den verbleibenden
Rest-Bias automatisch (0 Gain bei DC).

### Analog-Ausgang

Die mit **L**/**R** markierten Pins (unten rechts, USB nach oben)
sind Line-Pegel, **kein** Kopfhörerverstärker. Für Kopfhörer direkt
ist ein externer Amp nötig (z. B. PAM8908).

## Abhängigkeiten

- [pico-sdk](https://github.com/raspberrypi/pico-sdk)
- [pico-extras](https://github.com/raspberrypi/pico-extras) (liefert `pico_audio_i2s`)

```bash
export PICO_SDK_PATH=/pfad/zu/pico-sdk
export PICO_EXTRAS_PATH=/pfad/zu/pico-extras
cp $PICO_EXTRAS_PATH/external/pico_extras_import.cmake .
```

Der letzte Schritt (Import-Datei kopieren) ist zwingend nötig -
`pico-extras` muss per `include()` vor `project()` eingebunden werden,
nicht per `add_subdirectory()` danach (siehe DEVLOG.md für die
Fehlersuche dazu).

## Bauen

```bash
mkdir build && cd build
cmake -DPICO_BOARD=pico ..
make -j4
```

Erzeugt `i2s_sine_hello.uf2`. PicoADK im BOOTSEL-Modus (Boot-Taste beim
Einstecken gedrückt halten) anschließen, `.uf2` draufkopieren.

## Erfolgskriterien

- Sauberer Ton ohne Knacken/Aussetzer
- Frequenz reagiert glatt auf Poti-Drehung
- Lautstärke reagiert hörbar auf Sprechen/Klatschen vor dem Mic

## Wenn es nicht funktioniert

- **Stille:** XSMT (GPIO25) auf HIGH prüfen; Data-/Clock-Pins gegen
  die Tabelle oben verifizieren.
- **Falsche Tonhöhe/verzerrt:** `kSampleRateHz` muss zur tatsächlichen
  I2S-Konfiguration passen.
- **Knacken in regelmäßigen Abständen:** `kBufferSamples` erhöhen oder
  Pufferanzahl in `audio_new_producer_pool()` (aktuell 3) erhöhen.
- **Hüllkurve reagiert kaum/übersteuert sofort:** Skalierungsfaktor
  `6.0f` vor dem Clipping in `main()` ist ein grober Platzhalter -
  hängt von Mic-Gain (MAX9814-Jumper: 40/50/60dB) und Sprechabstand
  ab. Rohen `envelope`-Wert per USB-Seriell ausgeben und Faktor so
  einstellen, dass er bei normaler Sprechlautstärke nahe 1.0 bleibt,
  ohne dauerhaft zu clippen.
- **Bandpass scheint nichts zu tun:** Mic-Read + Filter müssen **pro
  Sample** laufen, nicht einmal pro Puffer (das Poti darf das, Audio
  nicht) - in `main.cpp` prüfen, dass das innerhalb der
  Sample-Schleife passiert.

## Nächster Schritt

FreeRTOS-Grundgerüst: Audio-I/O und Envelope-Follower in einen
eigenen hochprioren Task auslagern, Pot-Handling in einen separaten
niedrigprioren Task - Vorbereitung für die volle 16-Band-Filterbank,
bei der die aktuelle single-threaded `while`-Schleife an ihre Grenzen
kommt.
