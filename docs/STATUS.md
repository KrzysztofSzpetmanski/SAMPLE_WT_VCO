# Wave File WT VCO - Status

## Data
- Date: 2026-03-28
- Branch: `main`
- Version: `0.1.0` (draft)

## Zrobione
- Skopiowany i uruchomiony szkielet modułu z poprzedniego projektu.
- Podmiana źródła WT:
  - z generatora noise
  - na dane z pliku WAV (max 5s).
- Dodany parser WAV (PCM 16/24/32 + float32, mix do mono).
- Dodane menu modułu:
  - `Load WAV...`
  - `Clear WAV`
- Dodana serializacja ścieżki WAV w patchu Rack (`dataToJson/dataFromJson`).
- Zachowany tor audio i obróbka WT: morph, mipmap, hermite, unison, env, reverb.

## Draft UX
- `SCAN` i `SPAN` są aktualnie mapowane na istniejące gałki (dawne DENS/SMOTH).
- `JUMP` wykonuje losowy skok pozycji dla nowego okna.
- Status źródła (`WAV xx.xx s` / error / random) jest pokazywany na wyświetlaczu modułu.

## Następne kroki
- Dodać 2-ekranowy podgląd:
  - overview całego 5s pliku,
  - zoom wybranego okna.
- Rozdzielić kontrolę nawigacji od parametrów kształtu WT (docelowe dedykowane gałki).
- Dodać opcjonalne snap/grid dla skanowania po oknach.
- Strojenie mapowania `SPAN` i długości okna dla bardziej muzycznych rezultatów.

## Ryzyka
- Loader WAV jest customowy (nie zewnętrzna biblioteka), więc warto dołożyć testy na nietypowe pliki.
- Długie ścieżki/plik usunięty po zapisie patcha: moduł wraca do fallback random.
