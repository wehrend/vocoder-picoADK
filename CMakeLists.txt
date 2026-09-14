cmake_minimum_required(VERSION 3.13)

# Vor dem Bauen setzen:
#   export PICO_SDK_PATH=/pfad/zu/pico-sdk
#   export PICO_EXTRAS_PATH=/pfad/zu/pico-extras
#
# Zusätzlich einmalig die Import-Datei von pico-extras hierher kopieren
# (analog zu pico_sdk_import.cmake):
#   cp $PICO_EXTRAS_PATH/external/pico_extras_import.cmake .
#
# Siehe README.md für Details.

set(PICO_BOARD pico CACHE STRING "Board type")

# Beide Import-Dateien MÜSSEN vor project() eingebunden werden.
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(vocoder_envelope_step C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(vocoder_envelope_step
    src/main.cpp
)

# Generischer Pico + externes PCM5100A/PCM5102A-Breakout: I2S-Pins sind
# frei wählbar - an deine tatsächliche Verkabelung anpassen.
# Empfehlung (wie im i2s-sine-hello-pico-Projekt): GPIO18=DIN, GPIO16=BCK,
# GPIO17=LRCK (BCK/LRCK müssen aufeinanderfolgende GPIO-Nummern sein).
target_compile_definitions(vocoder_envelope_step PRIVATE
    PICO_AUDIO_I2S_DATA_PIN=18
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=16
)

target_link_libraries(vocoder_envelope_step PRIVATE
    pico_stdlib
    pico_audio_i2s
    hardware_adc
)

pico_enable_stdio_usb(vocoder_envelope_step 1)
pico_enable_stdio_uart(vocoder_envelope_step 0)

pico_add_extra_outputs(vocoder_envelope_step)