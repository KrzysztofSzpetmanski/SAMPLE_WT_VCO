# Noise_VCO M4L (milestone v0)

Pierwszy milestone portu VCV -> Max for Live Instrument.

## Co działa teraz

- Reakcja na MIDI (`notein` -> `mtof` -> `phasor~` -> `wave~ interp`).
- Bramka amplitudy z MIDI velocity (`line~`, attack/release).
- Audio wyjściowe do Abletona przez `plugout~`.
- Zachowany core patcha interpolacji WT (`wave1`, `wave2`, `interp`, `MORPH` slider).

Plik patcha:
- `m4l/Noise_VCO_M4L.maxpat`

## Czego celowo NIE ma w v0

- Reverb (zgodnie z założeniem).
- Wejść CV (zgodnie z założeniem).
- Pełnej architektury poly/unison znanej z wersji VCV.
- Generatora noise WT 1:1 jak w C++ (to będzie następny etap).

## Jak użyć w Ableton Live

1. Otwórz patch `Noise_VCO_M4L.maxpat` w Max.
2. `File -> Save As...` i zapisz jako `.amxd` (Max for Live Instrument).
3. W Live załaduj urządzenie na tor MIDI.

## Next ver (plan)

1. Przenieść algorytm `DENS/SMOTH/WT SIZE + GEN` do M4L.
2. Dodać stabilną polyfonię przez `poly~`.
3. Dodać `DETUNE/UNISON/OCTAVE` pod MIDI.
4. Zastąpić szkicowy tor audio wersją bardziej zbliżoną do DSP z VCV.
