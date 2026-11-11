# Proiect APD - Mesagerie offline

Proiectul va permite schimbul de mesaje intre doua telefoane fara internet. Fiecare telefon se va conecta prin Wi-Fi SoftAP la placa sa ESP32, iar placile vor comunica prin ESP-NOW. Firmware-ul de pornire afiseaza numele nodului si citeste un fisier din LittleFS.

## Tehnologii

- VS Code, PlatformIO si Arduino-ESP32 pentru firmware
- LittleFS pentru fisierele web
- PowerShell pentru flash, Unity pentru teste si GitHub Actions pentru CI

## Instalare

1. Instaleaza Git si VS Code.
2. Instaleaza extensia PlatformIO IDE in VS Code.
3. Cloneaza proiectul cu `git clone https://github.com/Stefan-Serafim/Proiect-APD.git`.
4. Deschide folderul `Proiect-APD` in VS Code si porneste un terminal PlatformIO.
5. Compileaza ambele noduri cu `pio run` sau ruleaza separat comenzile de mai jos.

```powershell
pio run -e node_a
pio run -e node_b
```

Profilurile folosesc placa `esp32dev`. Dupa ce confirmam modelul, schimba `board` in sectiunea `[esp32]` din `platformio.ini`. Daca placile sunt diferite, seteaza `board` in fiecare profil.

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

Dupa flash, deschide monitorul serial si reporneste placa. Ar trebui sa vezi `Node A` sau `Node B`, urmat de `Proiect APD - LittleFS OK`.

## Structura

- `src/`, `include/`, `lib/` pentru firmware, headere si biblioteci locale
- `data/www/` pentru fisierele web, disponibile pe placa in `/www/`
- `test/` pentru teste Unity si `docs/` pentru documentatie
- `scripts/` pentru flash si `.github/workflows/` pentru CI

Creeaza directoarele goale cand adaugi primele fisiere. Pune fiecare suita Unity care ruleaza pe PC intr-un folder `test/test_native_<modul>/`.

## GitHub

Porneste din `main` actualizat si creeaza un branch pentru task-ul tau.

```powershell
git switch main
git pull --ff-only
git switch -c feature/modul-radio
git add cale/catre/fisier
git commit -m "Add radio module"
git push -u origin feature/modul-radio
```

Deschide un PR catre `main`, noteaza cum ai verificat schimbarile si cere review. CI compileaza ambele noduri la PR si la push pe `main`. Ruleaza si suitele native cand sunt adaugate.

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
