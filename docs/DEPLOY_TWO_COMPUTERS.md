# Deploy On Two Computers

Poniżej prosty workflow dla dwóch maszyn (np. `Mac-mini` i `Laptop`) z jednym repo GitHub.

## Założenia
- Repo: osobne dla tego projektu.
- Obie maszyny mają:
  - `git`
  - `make`
  - VCV Rack SDK
  - VCV Rack 2

## 1) Pierwsza konfiguracja (na obu komputerach)

```bash
git clone <YOUR_REPO_URL> /path/to/WaveFileVCO
cd /path/to/WaveFileVCO
```

## 2) Build + instalacja lokalna

```bash
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
make install RACK_DIR=/absolute/path/to/Rack-SDK
```

## 3) Codzienny workflow

### Komputer A (zmiany)

```bash
git checkout -b codex/<nazwa-zmiany>
# edycje
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
git add .
git commit -m "<opis zmiany>"
git push -u origin codex/<nazwa-zmiany>
```

Po merge do `main`:

```bash
git checkout main
git pull
```

### Komputer B (aktualizacja)

```bash
git checkout main
git pull
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
make install RACK_DIR=/absolute/path/to/Rack-SDK
```

## 4) Szybki deploy pluginu bez paczki

Jeśli chcesz tylko szybko odpalić moduł lokalnie:

```bash
make install RACK_DIR=/absolute/path/to/Rack-SDK
```

To kopiuje plugin do folderu Rack2 plugins.

## 5) Deploy przez paczkę `.vcvplugin`

```bash
make dist RACK_DIR=/absolute/path/to/Rack-SDK
```

Artefakt znajdziesz w `dist/`.

## 6) Dobre praktyki
- Na obu komputerach trzymaj ten sam `RACK_DIR` lub alias w shellu.
- Nie commituj `build/`, `dist/`, `plugin.dylib`.
- Przed pull/push zawsze sprawdź:

```bash
git status
```
