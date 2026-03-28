# Setup

## Cel
Przygotowanie lokalnego środowiska dla projektu `WaveFileVCO` (VCV Rack 2 plugin).

## Wymagania
- macOS/Linux shell
- `git`
- `make`
- kompilator C++ zgodny z Rack SDK
- zewnętrzny VCV Rack SDK dostępny przez `RACK_DIR`

## Katalog projektu
- `/Users/lazuli/Documents/PROGRAMMING/VCV_PROGRAMMING/SAMPLE_VCO`

## Rack SDK
Projekt używa zewnętrznego SDK:
```bash
make RACK_DIR=/absolute/path/to/Rack-SDK
```

Typowa ścieżka używana w Twoim środowisku:
- `/Users/lazuli/Documents/PROGRAMMING/TEENSY/KSZ_TEENSY_PLATFORMIO/Teensy_Chord_Gen/Rack-SDK`

## Szybki check
```bash
cd /Users/lazuli/Documents/PROGRAMMING/VCV_PROGRAMMING/SAMPLE_VCO
ls -la
cat plugin.json
```
