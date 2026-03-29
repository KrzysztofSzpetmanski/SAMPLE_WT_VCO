# Sample VCO - Status

## Data
- Date: 2026-03-29
- Branch: `main`
- Version: `2.0.0`

## Zrobione
- Źródło WT: plik WAV (zwykły audio), limit do pierwszych 5 sekund.
- Loader WAV: PCM 16/24/32 + float32, miks do mono.
- Generator okna WT oparty o `SCAN` + `WT SIZE`.
- `DENS` działa jako simplify (kontrola liczby punktów "prawdziwych").
- `SMOTH` steruje charakterem interpolacji (liniowa -> sinusoidalna).
- Dwa ekrany:
  - overview całego WAV,
  - podgląd finalnej WT.

## Poprawki spójności i ryzyk (2026-03-29)
- Naprawiony błąd off-by-one w mapowaniu `SCAN` do startu okna.
- Marker na overview pokazuje realny start i szerokość aktualnego okna WT.
- Usunięte kosztowne kopiowanie całego bufora 5s przy każdej regeneracji WT.
- Usunięte race-condition na flagach między UI i audio (`pendingGenRequest`, `tableBlend`).
- Usunięte martwe elementy po starym układzie (`JUMP/GEN`, stare helpery DENS).
- Uporządkowane nazewnictwo kodu do `SampleVCO` i panelu `res/SampleVCO.svg`.

## Otwarte tematy
- Dalsze strojenie mapowania `DENS/SMOTH` pod odsłuch i zakresy muzyczne.
- Możliwy dodatkowy debug overlay (wartości `truePoints`, start okna) do szybkiego strojenia.
- Do sprawdzenia eksperymentalny tryb `native wtSize` (bez stałego 2048), jako opcja obok aktualnego trybu HQ.
