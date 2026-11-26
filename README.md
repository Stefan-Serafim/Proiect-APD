# Proiect APD - Mesagerie offline

Proiectul va permite schimbul de mesaje intre doua telefoane fara internet. Fiecare telefon se va conecta prin Wi-Fi SoftAP la placa sa ESP32, iar placile vor comunica prin ESP-NOW. Firmware-ul de pornire afiseaza numele nodului si citeste un fisier din LittleFS.

## Tehnologii

- VS Code, PlatformIO si ESP-IDF pentru firmware C/C++
- LittleFS pentru fisierele web
- PowerShell pentru flash, Unity pentru teste si GitHub Actions pentru CI

## Instalare

1. Instaleaza Git si VS Code.
2. Instaleaza extensia PlatformIO IDE in VS Code.
3. Cloneaza proiectul cu `git clone https://github.com/Stefan-Serafim/Proiect-APD.git`.
4. Deschide folderul `Proiect-APD` in VS Code si porneste un terminal PlatformIO.
5. Ruleaza comanda de mai jos. Daca trece, ai compilat ambele noduri. Poti face acest test fara placi.

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

La prima incercare, ruleaza scriptul pentru fiecare placa si deschide monitorul serial la 115200. Dupa ce repornesti placa, ar trebui sa vezi `Node A` sau `Node B`, urmat de `Proiect APD - LittleFS OK`.

Echipa Web poate verifica aspectul paginii in browser cand exista `data/www/index.html`. Mesageria se testeaza cu ambele placi dupa integrarea modulelor.

## Structura

- `src/`, `include/`, `lib/` pentru firmware, headere si biblioteci locale
- `data/www/` pentru fisierele web, disponibile in firmware la `/littlefs/www/`
- `test/` pentru teste Unity si `docs/` pentru documentatie
- `scripts/` pentru flash si `.github/workflows/` pentru CI

Punctul de intrare al firmware-ului este `app_main()` in `src/main.cpp`. Cand adaugi fisiere C/C++ in `src/`, trece-le in lista `SRCS` din `src/CMakeLists.txt` si declara componentele ESP-IDF folosite in `PRIV_REQUIRES`. Pentru SoftAP si ESP-NOW, foloseste API-urile `esp_wifi` si `esp_now`.

Pune fiecare suita Unity care ruleaza pe PC intr-un folder `test/test_native_<modul>/`.

## Cum lucram pe GitHub

Accepta invitatia la repository. Embedded foloseste branch-ul comun `feature/embedded`, iar Web foloseste `feature/web`. Stabiliti in echipa cine lucreaza la fiecare fisier. Fiecare pune 1-2 commituri cu schimbarile sale pe branch-ul comun. Pentru celelalte task-uri, porneste din `main` actualizat si creeaza un branch `feature/<task>`.

Exemplu pentru Embedded. Colegii de la Web folosesc aceiasi pasi cu `feature/web`.

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

Primul push cu modificari pe branch-ul echipei deschide automat un PR catre `main`. Push-urile colegilor se aduna in acelasi PR. Scrieti acolo ce ati schimbat si ce ati incercat. GitHub cere review Technical Lead-ului pentru modificarile Embedded.

CI compileaza ambele noduri, construieste imaginile LittleFS si ruleaza suitele native disponibile. Rezultatul CI si review-ul sunt consultative. DevOps integreaza PR-urile echipei manual si poate face push direct pe `main` pentru modificarile proprii.

| Rol | Unde lucreaza |
| --- | --- |
| PM | `docs/management/`, `docs/testing_protocol.md`, `scripts/plot_metrics.py` |
| Technical Lead | `docs/architecture.md`, `src/main.cpp`, `src/app_core.h` |
| Embedded | `src/radio/`, `src/messaging/`, `src/server/` |
| Web | `data/www/` |
| Protocol | `src/protocol/`, `test/test_native_protocol/`, `docs/protocol_spec.md` |
| QA | `docs/test_cases.md`, `docs/performance_analysis.md`, `tests/results/` |
| DevOps | `platformio.ini`, `partitions.csv`, `sdkconfig.defaults`, `.github/`, `scripts/flash_nodes.ps1`, `README.md` |

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
