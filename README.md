# KSZ Sample VCO

VCV Rack 2 plugin: VCO oparty o wavetable, gdzie źródłem danych jest zwykły plik WAV (maks. pierwsze 5 sekund).

## Aktualny stan (v2.0.0)

- Jedna tabela WT (bez morph A/B).
- `SCAN` wybiera pozycję okna w pliku WAV.
- `WT SIZE` ustala długość okna (256..2048 próbek).
- `DENS` upraszcza okno:
  - `100` = pełna gęstość (kształt najbardziej zbliżony do oryginału),
  - `50` = około co druga próbka jest punktem „prawdziwym”,
  - `0` = minimum 64 punktów „prawdziwych”.
- `SMOTH` zmienia interpolację między punktami od liniowej do sinusoidalnej.
- Dostępne dwa ekrany:
  - górny: podgląd całego źródła WAV,
  - dolny: aktualna tabela WT używana przez oscylator.

## WAV loader

- Menu kontekstowe modułu: `Right click -> Load WAV...` / `Clear WAV`.
- Obsługiwane WAV: PCM 16/24/32-bit oraz float32.
- Sygnał stereo jest miksowany do mono.
- Jeśli plik jest dłuższy niż 5s, używane jest tylko pierwsze 5s.

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
