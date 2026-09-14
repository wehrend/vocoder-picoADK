# Vocoder Envelope-Step (generischer Pico + PCM5102A + MAX9814)

Schrittweiser Testaufbau Richtung VC16-artiger Vocoder. Portiert vom
ursprünglichen PicoADK-Aufbau auf einen generischen Raspberry Pi Pico
mit externem PCM5100A/PCM5102A-Breakout, weil weitere PicoADK-Boards
(DatanoiseTV) nicht verfügbar waren. Kein FreeRTOS bisher - jeder
Schritt baut auf dem vorigen auf und verifiziert einen weiteren
Baustein, bevor mehr Komplexität dazukommt.

**Aktueller Stand: Schritt 3 (Envelope-Follower) läuft, portiert auf
generische Hardware.**

## Was das Programm aktuell macht

1. Erzeugt einen Sinuston per Wavetable (512 Punkte, einmalig `sinf()`
   beim Start, danach nur Tabellen-Lookup - RP2040 hat keine FPU)
2. Die **Frequenz** des Tons wird von einem Poti gesteuert (nativer
   RP2040-ADC, Kanal 0), linear auf 80–1500 Hz gemappt, leicht
   geglättet gegen Zipper-Noise
3. Die **Lautstärke** des Tons wird von einem Mic-Signal gesteuert
   (nativer RP2040-ADC, Kanal 1, MAX9814-Ausgang): Bandpass (~1kHz) →
   Gleichrichter → Attack/Release-Hüllkurve. Das ist im Kern schon
   1/16 eines vollständigen Filterbank-Vocoders.
4. Sample-Rate 44.1 kHz, Puffergröße 256 Samples, Ausgabe über I2S an
   den PCM5102A

## Unterschiede zur PicoADK-Version

- I2S-Pins sind frei wählbar (unten), da kein fest verdrahteter
  interner DAC mehr vorliegt - selbst verkabelt statt onboard.
- Kein ADC128S102/SPI mehr für Poti+Mic - der native RP2040-ADC
  (GPIO26-28) übernimmt beide Kanäle.
- Kein XSMT/DEMP-GPIO-Handling mehr im Code - das übernehmen feste
  Jumper/Lötbrücken auf dem PCM5102A-Breakout selbst (siehe unten).
- Kein Debug-Logging/Heartbeat-LED mehr aktiv (war während der
  I2S-Fehlersuche drin, aktuell wieder entfernt - bei Bedarf leicht
  wieder ergänzbar, `stdio_init_all()` läuft bereits).

## Pins

### I2S zum PCM5102A - frei verkabelt, an CMake-Defines anpassen

```
GPIO16 = BCK   (PICO_AUDIO_I2S_CLOCK_PIN_BASE)
GPIO17 = LRCK  (= CLOCK_PIN_BASE + 1, automatisch)
GPIO18 = DIN   (PICO_AUDIO_I2S_DATA_PIN)
```

BCK/LRCK müssen aufeinanderfolgende GPIO-Nummern sein, DIN ist
unabhängig wählbar. Werte stehen in `CMakeLists.txt` als
`target_compile_definitions`.

### PCM5102A-Breakout: Jumper/Lötbrücken (nicht software-gesteuert!)

```
XSMT -> HIGH / "un-mute"        (sonst dauerhaft stumm, unabhängig vom Code)
FMT  -> LOW  / "I2S"            (nicht Left-Justified, sonst Rauschen statt Ton)
DEMP -> LOW  / "off"
FLT  -> LOW  / "normal latency"
SCK  -> LOW  / "kein externer Master-Clock"
```

Alle fünf sind reine Hardware-Konfiguration auf dem Modul, GPIO-seitig
passiert dafür nichts. Falls dein Modul diese Signale stattdessen als
GPIO-Pins herausführt statt sie zu jumpern, müssten sie wie bei der
PicoADK-Version per `gpio_init`/`gpio_put` gesetzt werden.

### Nativer RP2040-ADC für Poti und Mic

```
GPIO26 = ADC0 = Frequenz-Poti  (Schleifer -> GPIO26, Außenbeine -> 3V3/GND)
GPIO27 = ADC1 = Mic-Amp-Ausgang (MAX9814)
```

Kanalzuordnung in `main.cpp` als `POT_ADC_CHANNEL`/`MIC_ADC_CHANNEL`
änderbar. Wichtig im Code: `adc_select_input()` wird nur beim
tatsächlichen Kanalwechsel aufgerufen (einmal pro Puffer kurz auf
Poti, dann fest auf Mic für die gesamte Sample-Schleife) - nicht vor
jedem einzelnen Read.

### MAX9814-Verdrahtung

```
MAX9814 VDD  -> 3V3   (nicht 5V/VBUS - RP2040-ADC verträgt nur bis 3.3V)
MAX9814 GND  -> GND
MAX9814 OUT  -> GPIO27
MAX9814 GAIN -> offen lassen = 60dB (Standard) | VDD = 40dB | GND = 50dB
MAX9814 A/R  -> offen lassen = 1:4000 (Standard) | VDD = 1:2000 | GND = 1:500
```

Kein zusätzliches Bias-Netzwerk nötig - das Modul liefert bereits ein
ADC-taugliches, vorgespanntes Signal (Bias laut Datenblatt fest bei
ca. 1,25V, unabhängig von VDD). Der Bandpass im Code entfernt den
verbleibenden Rest-Bias automatisch (0 Gain bei DC), exakte
Zentrierung ist daher nicht kritisch.

### Analog-Ausgang

Line-Pegel, **kein** Kopfhörerverstärker auf dem PCM5102A-Breakout.
Kopfhörer direkt anschließen funktioniert (verbreitete DIY-Praxis),
belastet den Ausgang aber unterhalb der spezifizierten Lastimpedanz
(Datenblatt empfiehlt ≥10kΩ, Kopfhörer liegen typischerweise bei
16–300Ω). Für Dauerbetrieb empfiehlt sich ein Serienwiderstand
(100–470Ω) je Kanal vor der Klinkenbuchse - schützt den Ausgang und
senkt gleichzeitig den ohnehin oft zu hohen Line-Pegel für Kopfhörer.

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
nicht per `add_subdirectory()` danach.

## Bauen

```bash
mkdir build && cd build
cmake -DPICO_BOARD=pico ..
make -j4
```

Erzeugt `vocoder_envelope_step.uf2`. Pico im BOOTSEL-Modus (Boot-Taste
beim Einstecken gedrückt halten) anschließen, `.uf2` draufkopieren.

## Erfolgskriterien

- Sauberer Ton ohne Knacken/Aussetzer, kein Rauschen
- Frequenz reagiert glatt auf Poti-Drehung
- Lautstärke reagiert hörbar auf Sprechen/Klatschen vor dem Mic

## Wenn es nicht funktioniert

- **Stille:** XSMT-Jumper auf HIGH prüfen; Verkabelung
  (Durchgangsprüfung stromlos) und Data-/Clock-Pins gegen die Tabelle
  oben verifizieren. Power-LED des Breakouts zeigt nur "Strom
  kommt an", nichts über die DAC-Funktion selbst.
- **Rauschen statt Ton:** FMT-Jumper prüfen (muss auf I2S/LOW stehen,
  nicht Left-Justified) - ein falsch gesetztes oder offenes FMT bringt
  genau dieses Symptom, kein Software-Fix möglich (Protokoll ist im
  PIO-Programm der Library fest verdrahtet).
- **Kein Takt/DAC bleibt tot trotz XSMT=HIGH:** SCK-Jumper prüfen,
  muss auf LOW stehen, damit das Modul seinen internen PLL aus BCK
  ableitet.
- **Falsche Tonhöhe/verzerrt:** `kSampleRateHz` muss zur tatsächlichen
  I2S-Konfiguration passen.
- **Knacken in regelmäßigen Abständen:** `kBufferSamples` erhöhen oder
  Pufferanzahl in `audio_new_producer_pool()` (aktuell 3) erhöhen.
- **Hüllkurve reagiert kaum/übersteuert sofort:** Skalierungsfaktor
  `6.0f` vor dem Clipping in `main()` ist ein grober Platzhalter -
  hängt von Mic-Gain (MAX9814 GAIN-Pin: 40/50/60dB) und Sprechabstand
  ab. Bei Bedarf `printf` für den rohen `envelope`-Wert temporär wieder
  einbauen und Faktor so einstellen, dass er bei normaler
  Sprechlautstärke nahe 1.0 bleibt, ohne dauerhaft zu clippen.
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