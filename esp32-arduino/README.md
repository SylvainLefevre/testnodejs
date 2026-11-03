# Version Arduino IDE : fond sonore + insultes aléatoires

Sketch : `esp32_usb_jukebox/esp32_usb_jukebox.ino`.

**Une seule bibliothèque à installer : [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost)** (Gestionnaire de bibliothèques → chercher « EspUsbHost », auteur TANAKA Masayuki). Elle gère la clé USB (Mass Storage) et la monte comme un système de fichiers Arduino. Le décodeur MP3 ([minimp3](https://github.com/lieff/minimp3), domaine public) est inclus dans le `.ino` : il permet de jouer le fond et une insulte en même temps.

## Utilisation
1. Gestionnaire de cartes → *esp32 by Espressif Systems* **≥ 3.2.0** (exigé par EspUsbHost).
2. Gestionnaire de bibliothèques → installer **EspUsbHost**.
3. Carte : `ESP32S3 Dev Module`, **USB CDC On Boot : Disabled**, CPU 240 MHz.
4. Ouvrir le `.ino`, téléverser par le port **UART**, moniteur série 115200 bauds.

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
FAT32 conseillé (les formats gérés dépendent d'EspUsbHost / FatFs).
```
fond/1.mp3        -> boucle à 45 %
Insultes/*.mp3    -> 1 fichier au hasard toutes les 1 à 5 min, mixé par-dessus le fond
```
Les MP3 illisibles ou parasites (ex. `._xxx` créés par macOS) sont ignorés. Toute fréquence / mono / stéréo est acceptée (rééchantillonnage grossier vers 44,1 kHz).

## Console série (115200 bauds)
À chaque branchement de la clé, le contenu de la racine, de `/fond` et de `/Insultes` est affiché (noms longs, taille en Ko, fichiers ignorés signalés), suivi des messages de lecture :
```
===== Contenu de la clé USB =====
 /fond :
   1.MP3  (189 Ko)
 /Insultes :
   Autre.mp3  (49 Ko)
=================================
Fond : /fond/1.mp3
Prochaine insulte dans 142 s
Insulte : Autre.mp3
```

## Réglages
En tête du `.ino` : broches, `BG_VOL_PCT` (45), `FG_VOL_PCT`, `insultMinMs` / `insultMaxMs`.

## État des tests
- **Testé sur PC** (`host-test/`, avec un faux système de fichiers) : décodage MP3 (44,1 kHz stéréo et 22,05 kHz mono), mixage à 45 %, boucle du fond, tirage aléatoire sans répétition, fichier parasite ignoré, affichage du contenu.
  `g++ -std=gnu++17 -O2 -Ihost-test/shim host-test/host_test.cpp -o host_test && ./host_test dossier_cle sortie.pcm 30`
- **Non testé sur matériel** : l'appel à EspUsbHost (montage de la clé), l'I2S et la charge CPU réelle. En cas de souci, envoyez la sortie complète du moniteur série.
- Alimentation : beaucoup de DevKit ESP32-S3 ne fournissent pas le 5 V sur le port OTG ; une clé qui n'est pas alimentée ne sera jamais détectée.
