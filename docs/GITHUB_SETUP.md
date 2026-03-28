# GitHub Setup

## 1. Inicjalizacja repo lokalnie
```bash
cd /Users/lazuli/Documents/PROGRAMMING/VCV_PROGRAMMING/NOISE_WT_VCO
git init
git add .
git commit -m "Initial Noise_VCO scaffold"
```

## 2. Utworzenie repo na GitHub i podpięcie remote
```bash
git remote add origin <YOUR_GITHUB_REPO_URL>
git branch -M main
git push -u origin main
```

## 3. Dalsza praca
- pracuj na branchach `codex/...` lub feature branchach,
- trzymaj aktualne `README.md`, `BUILD_AND_RUN.md`, `SETUP.md`,
- po zmianach DSP/UI aktualizuj sekcję statusu i checklistę testów.
