# Proiect APD - Mesagerie offline

Proiectul va permite schimbul de mesaje intre doua telefoane fara internet. Fiecare telefon se va conecta prin Wi-Fi SoftAP la placa sa ESP32, iar placile vor comunica prin ESP-NOW.

## Tehnologii

- VS Code si ESP-IDF 5.5.5 pentru firmware C/C++
- SPIFFS pentru fisierele web si istoricul mesajelor
- PowerShell pentru flash, PlatformIO si Unity pentru testele pe PC, GitHub Actions pentru CI

## Instalare

1. Instaleaza Git si VS Code.
2. Instaleaza extensia Espressif IDF in VS Code si configureaza ESP-IDF 5.5.5.
3. Cloneaza proiectul cu `git clone https://github.com/Stefan-Serafim/Proiect-APD.git`.
4. Deschide folderul `Proiect-APD` in VS Code si porneste un terminal ESP-IDF.
5. Ruleaza comenzile de mai jos ca sa compilezi ambele noduri. Poti face acest test fara placi.

```powershell
idf.py -B build/node_a -D APD_NODE=a build
idf.py -B build/node_b -D APD_NODE=b build
```

Profilurile din `embedded/profiles/` aleg nodul si numele retelei Wi-Fi. Configuratia este pentru ESP32 cu 4 MB de flash.

## Flash si SPIFFS

Identifica porturile in Device Manager si inlocuieste `COM3` si `COM4` cu porturile tale. Din terminalul ESP-IDF, ruleaza scriptul pentru placa pe care vrei sa o incarci.

```powershell
.\scripts\flash_nodes.ps1 -Node A -Port COM3
.\scripts\flash_nodes.ps1 -Node B -Port COM4
```

Scriptul incarca firmware-ul si imaginea SPIFFS din `data/www/` in partitia `www`. Partitia `storage` este folosita de firmware pentru istoricul mesajelor.

Dupa ce incarci firmware-ul, deschide monitorul serial la 115200 si reporneste placa ca sa vezi mesajele de la pornire.

Echipa Web poate verifica aspectul paginii in browser cand exista `data/www/index.html`. Mesageria se testeaza cu ambele placi dupa integrarea modulelor.

## Structura

- `embedded/src/` pentru firmware si modulele ESP-IDF
- `data/www/` pentru fisierele web, disponibile in firmware la `/www/`
- `test/` pentru teste Unity si `docs/` pentru documentatie
- `scripts/` pentru flash si `.github/workflows/` pentru CI

Firmware-ul porneste din `app_main()` in `embedded/src/main/main.cpp`. Fiecare modul isi declara fisierele in propriul `CMakeLists.txt`.

Pune fiecare suita Unity care ruleaza pe PC intr-un folder `test/test_native_<modul>/`.

## Cum lucram pe GitHub

Accepta invitatia la repository si foloseste branch-ul din tabel pentru rolul tau. Embedded lucreaza pe un branch comun, iar colegii Web lucreaza pe branch-urile lor. Fiecare coleg pune 1-2 commituri cu schimbarile sale pe branch-ul din tabel.

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

Primul push al unui coleg pe un branch `feature/...` sau pe unul dintre cele doua branch-uri Web deschide automat un PR catre `main`. Urmatoarele push-uri se aduna in acelasi PR.

GitHub cere automat review lui Darius (`dcirstoc34-ai`) pentru PR-urile create la push-urile celorlalti colegi. Pentru un PR deschis manual, adauga-l pe Darius la `Reviewers`. Darius cere review unui alt coleg pentru PR-urile sale.

CI compileaza ambele noduri, genereaza imaginile SPIFFS si ruleaza suitele native disponibile. CI si review-ul sunt consultative. DevOps integreaza PR-urile echipei manual si poate face push direct pe `main` pentru modificarile proprii.

| Rol | Branch | Unde lucreaza |
| --- | --- | --- |
| PM | `feature/<task>` | `docs/management/`, `docs/testing_protocol.md`, `scripts/plot_metrics.py` |
| Technical Lead | `feature/<task>` | `docs/architecture.md`, `embedded/src/main/` |
| Embedded | `feature/embedded` | `embedded/src/radio/`, `embedded/src/messaging/`, `embedded/src/server/` |
| Web 1 | `web1-chat-ui` | Interfata de chat in `data/www/` |
| Web 2 | `feat/web2-client-diagnostics` | `data/www/client.js`, `data/www/diagnostics.js` |
| Protocol | `feature/protocol` | `embedded/src/protocol/`, `test/test_native_protocol/`, `docs/protocol_spec.md` |
| QA | `feature/qa` | `docs/test_cases.md`, `docs/performance_analysis.md`, `tests/results/` |
| DevOps | `main` sau `dev-ops` | `CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `.github/`, `scripts/flash_nodes.ps1`, `README.md` |

Pentru un task de PM sau Technical Lead, creeaza branch-ul din `main` actualizat.

## Probleme frecvente

- **Port COM gresit**. Verifica lista dupa conectarea fiecarei placi si inchide monitorul serial inainte de flash.
- **Placa lipseste din lista**. Incearca un cablu USB de date si verifica driverul USB-serial.
- **Eroare la compilare**. Verifica profilul si modelul placii, apoi citeste prima eroare afisata de PlatformIO.

## Comenzi utile

| Comanda | Ce face |
| --- | --- |
| `idf.py -B build/node_a -p COM3 monitor` | Deschide monitorul serial pentru nodul A |
| `pio test -e native` | Ruleaza suitele Unity existente pe PC, cu GCC in PATH |
