# Build And Run

## Build
```bash
cd /Users/lazuli/Documents/PROGRAMMING/VCV_PROGRAMMING/SAMPLE_VCO
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
```

## Package
```bash
make dist RACK_DIR=/absolute/path/to/Rack-SDK
```

Oczekiwane artefakty:
- `plugin.dylib` (lub odpowiednik platformowy)
- `dist/SampleWtVCO/`
- `dist/SampleWtVCO-<version>-<platform>.vcvplugin`

## Install lokalnie do Rack2
```bash
make install RACK_DIR=/absolute/path/to/Rack-SDK
```

## Smoke test
1. Otwórz VCV Rack 2 i dodaj moduł `SAMPLE WT VCO`.
2. Right-click modułu -> `Load WAV...`.
3. Sprawdź, że moduł czyta tylko pierwsze 5s pliku.
4. Kręć `SCAN`, `SPAN`, `MORPH`, `WT SIZE` i odsłuchaj `L/R OUT`.
5. Sprawdź `JUMP` i wejście `JUMP TRIG`.

## Clean
```bash
make clean RACK_DIR=/absolute/path/to/Rack-SDK
```
