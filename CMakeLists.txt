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

project(i2s_sine_hello C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(i2s_sine_hello
    src/main.cpp
)

# Fest verdrahtete PCM5100A-I2S-Pins laut DatanoiseTV/PicoADK-Hardware
# README ("Internal Signals"): GPIO16=DAT, GPIO17=BCLK, GPIO18=LRCLK.
# Diese Pins sind NICHT am Header herausgeführt, sondern intern fest verdrahtet.
target_compile_definitions(i2s_sine_hello PRIVATE
    PICO_AUDIO_I2S_DATA_PIN=16
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=17
)

target_link_libraries(i2s_sine_hello PRIVATE
    pico_stdlib
    pico_audio_i2s
    hardware_spi
)

pico_enable_stdio_usb(i2s_sine_hello 1)
pico_enable_stdio_uart(i2s_sine_hello 0)

pico_add_extra_outputs(i2s_sine_hello)
