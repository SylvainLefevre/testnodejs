# Version Arduino IDE : fond sonore + insultes aléatoires (un seul fichier, sans bibliothèque)

Sketch : `esp32_usb_jukebox/esp32_usb_jukebox.ino` — **seul le core « esp32 by Espressif » ≥ 3.0 est nécessaire**.
Le décodeur MP3 ([minimp3](https://github.com/lieff/minimp3), domaine public CC0), le pilote USB Mass Storage et le lecteur FAT32 sont inclus dans le fichier.

## Utilisation
1. Gestionnaire de cartes → installer *esp32 by Espressif Systems* (≥ 3.0).
2. Carte : `ESP32S3 Dev Module`, **USB CDC On Boot : Disabled**, CPU 240 MHz.
3. Ouvrir le `.ino`, téléverser par le port **UART**, moniteur série 115200 bauds.

## Câblage
| MAX98357A | ESP32-S3 |
|-----------|----------|
| LRC       | GPIO 48  |
| BLK (BCLK)| GPIO 45  |
| DIN       | GPIO 47  |
| GAIN      | GND      |
| VIN / GND | 5 V / GND |

Clé USB sur le port **USB natif (OTG)** via un adaptateur OTG ; sur la DevKitC-1, souder le jumper « USB-OTG » (ou fournir 5 V à la clé) pour qu'elle soit alimentée. Pas de hub USB.

## Clé USB
FAT32 (partition MBR ou clé sans partition), secteurs de 512 octets (exFAT / NTFS / GPT non supportés).
```
fond/1.mp3        -> boucle à 45 %
Insultes/*.mp3    -> 1 fichier au hasard toutes les 1 à 5 min, mixé par-dessus le fond
```
Les noms de dossiers `fond` et `Insultes` sont résolus en noms courts 8.3 (insensible à la casse) : ils tiennent dans 8 caractères, donc OK. Les fichiers d'insultes peuvent avoir des noms longs. Les MP3 illisibles (ex. `._xxx` créés par macOS) sont ignorés. Toute fréquence/mono/stéréo est acceptée (rééchantillonnage grossier vers 44,1 kHz).

## Réglages
En tête du `.ino` : broches, `BG_VOL_PCT` (45), `FG_VOL_PCT`, `insultMinMs` / `insultMaxMs`.

## État des tests
- **Testé sur PC** (`host-test/`) : lecture FAT32 (image sans partition et image MBR, noms longs), décodage MP3 (44,1 kHz stéréo et 22,05 kHz mono), mixage à 45 %, boucle du fond sans coupure, tirage aléatoire sans répétition, fichier parasite ignoré.
  `g++ -std=gnu++17 -O2 -Ihost-test/shim host-test/host_test.cpp -o host_test && ./host_test image.img sortie.pcm 40`
- **Non testé sur matériel** : la partie USB host (API `usb_host` du core), l'I2S et la charge CPU réelle (deux décodages simultanés) n'ont pas pu être exécutés ici. Si la compilation ou l'énumération de la clé échoue, envoyez-moi les messages du moniteur série.
