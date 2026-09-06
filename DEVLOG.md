# DEVLOG: I2S Sine Hello World auf dem PicoADK

Ziel: Kleinstmöglicher Testaufbau, um den Audio-Signalweg (I2S →
PCM5100A) auf dem PicoADK zu verifizieren, als Vorstufe zu einem
VC16-artigen Vocoder-Projekt. Kein FreeRTOS, kein ADC - nur ein
Sinuston, um Pins, Toolchain und Sample-Rate zu bestätigen.

**Ergebnis: Erfolgreich.** Sauberer 440 Hz Ton über den Onboard-DAC.

## Hardware-Findings

### PCM5100A ist bereits verbaut, aber nicht am Header

Der PicoADK hat den I2S-DAC PCM5100A fest verbaut. Die drei I2S-
Signale sind aber **intern fest verdrahtet**, nicht am Pin-Header
herausgeführt - deswegen sind sie im normalen (Header-)Pinout-Tool
nicht zu finden. Quelle: `DatanoiseTV/PicoADK-Hardware`-Repo, Abschnitt
"Internal Signals".

| GPIO | Funktion |
|------|----------|
| 16 | PCM5100A I2S **DAT** |
| 17 | PCM5100A I2S **BCLK** |
| 18 | PCM5100A I2S **LRCLK** |
| 23 | PCM5100A **DEMP** (Deemphase 44.1kHz) |
| 25 | PCM5100A **XSMT** (Mute/Unmute) |

### Stolperfalle 1: XSMT ist standardmäßig stumm geschaltet

`XSMT` (GPIO25) muss von der Software explizit auf **HIGH** gesetzt
werden. Der Default-Zustand ist "gemutet" - ohne das bleibt der
Ausgang stumm, selbst wenn I2S-Takt und Daten technisch korrekt
ankommen. Das ist leicht mit einem generellen I2S-Konfigurationsfehler
zu verwechseln, weil beide Fälle als "Stille" erscheinen.

```cpp
gpio_init(XSMT_PIN);
gpio_set_dir(XSMT_PIN, GPIO_OUT);
gpio_put(XSMT_PIN, 1); // unmute
```

### Analog-Ausgang: Line-Pegel, kein Kopfhörerverstärker

Die mit **L**/**R** beschrifteten Pins (unten rechts am Board, USB
nach oben) sind reine Line-Ausgänge des PCM5100A. Es gibt **keinen**
dedizierten Kopfhörerverstärker-Chip auf dem Board. Für brauchbare
Kopfhörerlautstärke ist ein externer Amp nötig.

### Modulator-Eingang für den späteren Vocoder (noch nicht umgesetzt)

Der Onboard-ADC (**ADC128S102**, 8-Kanal, 12-Bit, bis 1 MS/s, an
**SPI1**: GPIO10=SCK, GPIO11=MOSI, GPIO12=MISO, GPIO13=CSn) ist für
Pots/CV ausgelegt - unipolar, kein AC-Coupling, kein Mic-Gain. Für
Stimme als Modulatorsignal wird zusätzlich ein kleiner externer
Preamp benötigt (Mic-Kapsel + 1 OpAmp-Stufe, AC-gekoppelt, auf VREF/2
vorgespannt). Das ist der nächste Hardware-Schritt.

## Toolchain-Stolperfallen

### Stolperfalle 2: ARM-Cross-Compiler fehlte

`arm-none-eabi-gcc` war initial nicht installiert. Fix unter
Debian/Ubuntu/Kali:

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi
```

### Stolperfalle 3: pico-extras falsch eingebunden

Erster Ansatz war `add_subdirectory($ENV{PICO_EXTRAS_PATH} ...)` nach
`pico_sdk_init()` - das lässt CMake fehlerfrei durchlaufen, aber ohne
dass die `pico_audio_i2s`-Bibliothek als Target entsteht (sichtbar
über `make help | grep audio`, das leer blieb). Symptom war ein
verwirrender `fatal error: pico/audio_i2s.h: No such file or
directory`, obwohl die Datei nachweislich im `pico-extras`-Checkout
vorhanden war und `PICO_EXTRAS_PATH` korrekt gesetzt war.

**Ursache:** `pico-extras` hat eine eigene Import-Datei
(`external/pico_extras_import.cmake`), analog zu
`pico_sdk_import.cmake`, die **vor** `project()` per `include()`
eingebunden werden muss - nicht per `add_subdirectory()` danach.

**Fix:**

```bash
cp $PICO_EXTRAS_PATH/external/pico_extras_import.cmake .
```

```cmake
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)   # vor project()!

project(i2s_sine_hello C CXX ASM)
...
pico_sdk_init()
# KEIN add_subdirectory für pico-extras nötig
```

**Nebenbei gelernt:** Ein zwischenzeitliches `make clean` reicht nach
einer CMake-Konfigurationsänderung nicht aus - `CMakeCache.txt` bleibt
bestehen und friert alte (falsche) Pfade ein. Bei Konfigurationsfehlern
immer `rm -rf build` und komplett neu konfigurieren.

## Build-Kommandos (funktionierend)

```bash
export PICO_SDK_PATH=/pfad/zu/pico-sdk
export PICO_EXTRAS_PATH=/pfad/zu/pico-extras

cd i2s-sine-hello
rm -rf build
mkdir build && cd build
cmake -DPICO_BOARD=pico ..
make -j4
```

Flashen: BOOT-Taste gedrückt halten, USB einstecken, `.uf2` auf das
erscheinende `RP2_BOOT`-Laufwerk kopieren.

## Nächster Schritt

Ein Carrier-Oszillator (Sägezahn/Puls) mit Frequenzsteuerung über ein
Poti am ADC128S102 (SPI1) - isoliert den ADC-Signalpfad, bevor der
Mic-Preamp für den eigentlichen Vocoder-Modulator dazukommt.
