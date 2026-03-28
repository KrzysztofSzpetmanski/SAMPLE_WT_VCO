# KSZ Wave File WT VCO

VCV Rack 2 plugin: VCO wavetable, w którym źródłem WT jest zwykły plik WAV (nie plik wavetable).

## Draft v0.1 - co działa

- Ładowanie WAV z menu kontekstowego modułu (`Right click -> Load WAV...`).
- Wczytywane jest maksymalnie pierwsze `5.0s` pliku.
- Obsługiwane formaty WAV:
  - PCM: 16/24/32-bit
  - Float: 32-bit
- Stereo jest miksowane do mono.
- Generator WT i tor audio pozostają zgodne z poprzednim modułem:
  - dual table A/B
  - morph A<->B
  - mipmapy WT
  - odczyt 4-point Hermite
  - unison / detune / voct
  - env + reverb

## Nawigacja po pliku (draft)

W tym pierwszym drafcie nawigacja jest mapowana na istniejące gałki:

- `SCAN` (gałka 2. rzędu, pozycja dawnego DENS): pozycja globalna w próbce.
- `SPAN` (gałka 3. rzędu, pozycja dawnego SMOTH): dystans między oknem A i B.
- `MORPH`: blend A<->B.
- `WT SIZE`: długość okna używanego do budowy WT.
- `JUMP` (przycisk + trig): losowy skok pozycji skanowania.

## Build

```bash
cd /Users/lazuli/Documents/PROGRAMMING/VCV_PROGRAMMING/SAMPLE_VCO
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
```

## Install lokalnie

```bash
make install RACK_DIR=/absolute/path/to/Rack-SDK
```

## Dokumentacja

- Status implementacji: `docs/STATUS.md`
- Deploy na dwa komputery: `docs/DEPLOY_TWO_COMPUTERS.md`
- Build smoke test: `BUILD_AND_RUN.md`

## Ograniczenia draftu

- Brak dwóch osobnych ekranów (overview + zoom) - na razie jest pojedynczy podgląd WT.
- Pozycja skanu korzysta z istniejących kontrolek (bez dedykowanego enkodera endless).
- Dalsze strojenie mapowania `SCAN/SPAN` będzie potrzebne po odsłuchach.
