# Proiect APD - Mesagerie offline

Proiectul va permite schimbul de mesaje intre doua telefoane fara internet. Fiecare telefon se va conecta prin Wi-Fi SoftAP la placa sa ESP32, iar placile vor comunica prin ESP-NOW.

## Tehnologii

- VS Code, PlatformIO si ESP-IDF pentru firmware C/C++
- LittleFS pentru fisierele web
- PowerShell pentru flash, Unity pentru teste si GitHub Actions pentru CI

## Instalare

1. Instaleaza Git si VS Code.
2. Instaleaza extensia PlatformIO IDE in VS Code.
3. Cloneaza proiectul cu `git clone https://github.com/Stefan-Serafim/Proiect-APD.git`.
4. Deschide folderul `Proiect-APD` in VS Code si porneste un terminal PlatformIO.
5. Dupa ce echipa adauga `embedded/main.c` sau `embedded/main.cpp`, ruleaza comanda de mai jos ca sa compilezi ambele noduri. Poti face acest test fara placi.

```powershell
pio run
```

PlatformIO descarca ESP-IDF si componenta LittleFS la prima compilare. Profilurile folosesc `esp32dev` cu 4 MB de flash. Pentru alta placa, schimba `board` in `platformio.ini`. Daca difera memoria flash, ajusteaza si `partitions.csv` si `sdkconfig.defaults`.

## Flash si LittleFS

Identifica porturile cu `pio device list` si inlocuieste `COM3` si `COM4` cu porturile tale. Scriptul compileaza si incarca firmware-ul, apoi incarca imaginea LittleFS.

```powershell
.\scripts\flash_nodes.ps1 -Node A -Port COM3
.\scripts\flash_nodes.ps1 -Node B -Port COM4
```

Pentru incarcare manuala, foloseste `upload` pentru firmware si `uploadfs` cand se schimba fisierele din `data/www/`.

```powershell
pio run -e node_a -t upload --upload-port COM3
pio run -e node_a -t uploadfs --upload-port COM3
pio run -e node_b -t upload --upload-port COM4
pio run -e node_b -t uploadfs --upload-port COM4
```

Dupa ce incarci firmware-ul, deschide monitorul serial la 115200 si reporneste placa ca sa vezi mesajele de la pornire.

Echipa Web poate verifica aspectul paginii in browser cand exista `data/www/index.html`. Mesageria se testeaza cu ambele placi dupa integrarea modulelor.

## Structura

- `embedded/`, `include/`, `lib/` pentru firmware, headere si biblioteci locale
- `data/www/` pentru fisierele web, disponibile in firmware la `/littlefs/www/`
- `test/` pentru teste Unity si `docs/` pentru documentatie
- `scripts/` pentru flash si `.github/workflows/` pentru CI

Pune codul ESP32 in `embedded/`. Porneste cu `main.c` sau `main.cpp` si functia `app_main()`. Daca adaugi cod intr-un subfolder, include-l in `SRC_DIRS` din `embedded/CMakeLists.txt`.

Pune fiecare suita Unity care ruleaza pe PC intr-un folder `test/test_native_<modul>/`.

## Cum lucram pe GitHub

Accepta invitatia la repository si foloseste branch-ul din tabel pentru rolul tau. Embedded si Web isi impart fisierele in echipa. Fiecare coleg pune 1-2 commituri cu schimbarile sale pe branch-ul echipei.

Exemplu pentru Embedded. Pentru Web, Protocol sau QA, inlocuieste `feature/embedded` cu branch-ul tau.

```powershell
git fetch origin
git switch feature/embedded
git pull --ff-only
```

Dupa ce ai facut modificarile, salveaza-le intr-un commit si adu schimbarile colegilor inainte de push.

```powershell
git add cale/catre/fisier
git commit -m "Add my changes"
git pull --rebase
git push
```

Primul push al unui coleg pe un branch `feature/...` deschide automat un PR catre `main`. Urmatoarele push-uri se aduna in acelasi PR.

GitHub cere automat review lui Darius (`dcirstoc34-ai`) pentru PR-urile create la push-urile celorlalti colegi. Pentru un PR deschis manual, adauga-l pe Darius la `Reviewers`. Darius cere review unui alt coleg pentru PR-urile sale.

CI compileaza ambele noduri si construieste imaginile LittleFS dupa ce apare fisierul de pornire. Ruleaza si suitele native disponibile. CI si review-ul sunt consultative. DevOps integreaza PR-urile echipei manual si poate face push direct pe `main` pentru modificarile proprii.

| Rol | Branch | Unde lucreaza |
| --- | --- | --- |
| PM | `feature/<task>` | `docs/management/`, `docs/testing_protocol.md`, `scripts/plot_metrics.py` |
| Technical Lead | `feature/<task>` | `docs/architecture.md`, `embedded/main.cpp`, `embedded/app_core.h` |
| Embedded | `feature/embedded` | `embedded/radio/`, `embedded/messaging/`, `embedded/server/` |
| Web | `feature/web` | `data/www/` |
| Protocol | `feature/protocol` | `embedded/protocol/`, `test/test_native_protocol/`, `docs/protocol_spec.md` |
| QA | `feature/qa` | `docs/test_cases.md`, `docs/performance_analysis.md`, `tests/results/` |
| DevOps | `main` sau `dev-ops` | `platformio.ini`, `partitions.csv`, `sdkconfig.defaults`, `.github/`, `scripts/flash_nodes.ps1`, `README.md` |

Pentru un task de PM sau Technical Lead, creeaza branch-ul din `main` actualizat.

## Probleme frecvente

- **Port COM gresit**. Verifica lista dupa conectarea fiecarei placi si inchide monitorul serial inainte de flash.
- **Placa lipseste din lista**. Incearca un cablu USB de date si verifica driverul USB-serial.
- **Eroare la compilare**. Verifica profilul si modelul placii, apoi citeste prima eroare afisata de PlatformIO.

## Comenzi utile

| Comanda | Ce face |
| --- | --- |
| `pio device monitor -e node_a --port COM3` | Deschide monitorul serial la 115200 |
| `pio run -e node_a -e node_b -t buildfs` | Genereaza imaginile LittleFS fara placa |
| `pio test -e native` | Ruleaza suitele Unity existente pe PC, cu GCC in PATH |
