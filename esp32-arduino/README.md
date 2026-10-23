# Version Arduino IDE : fond sonore + insultes aléatoires

Sketch : `esp32_usb_jukebox/esp32_usb_jukebox.ino` (⚠️ **non compilé ni testé sur matériel** : écrit sans carte ni toolchain Arduino sous la main).

## Câblage
| MAX98357A | ESP32-S3 |
|-----------|----------|
| LRC       | GPIO 48  |
| BLK (BCLK)| GPIO 45  |
| DIN       | GPIO 47  |
| GAIN      | GND      |
| VIN / GND | 5 V / GND |
| SD        | non connecté (canal gauche+droit mélangés) |

- Alimentez la carte par le port **UART** ; la clé USB va sur le port **USB natif (OTG)** avec un adaptateur OTG. Sur la DevKitC-1, il faut souder le jumper « USB-OTG » (ou alimenter la clé en 5 V) pour que la clé soit alimentée.
- Sur la v1.1, la LED RGB est sur le GPIO 38, donc le GPIO 48 est libre. Le GPIO 45 est une broche de strapping : si le démarrage pose problème, débranchez BCLK pendant le flash/reset.

## Clé USB (FAT32)
```
fond/1.mp3        -> boucle à 45 %
Insultes/*.mp3    -> 1 fichier au hasard toutes les 1 à 5 min, mixé par-dessus le fond
```
Idéalement des MP3 en 44,1 kHz (les autres fréquences sont rééchantillonnées grossièrement).

## Dépendances (Arduino IDE)
1. **Carte** : Gestionnaire de cartes → « esp32 by Espressif Systems » **≥ 3.0** ; choisir `ESP32S3 Dev Module`, option **USB CDC On Boot : Disabled**, USB Mode : *USB-OTG (TinyUSB)* ou *Hardware CDC and JTAG* (pas de Serial sur le port natif).
2. **Décodeur MP3** : bibliothèque [arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix) (Croquis → Inclure une bibliothèque → Ajouter la bibliothèque .ZIP).
3. **Pilote clé USB (MSC host)** : n'est *pas* fourni avec le core Arduino. Copiez dans le dossier du sketch (à côté du `.ino`) les fichiers du composant Espressif
   [`usb/usb_host_msc`](https://github.com/espressif/idf-extra-components/tree/master/usb/usb_host_msc) :
   `src/msc_host.c`, `src/msc_scsi_bot.c`, `src/msc_host_vfs.c`, `include/msc_host.h`, `include/msc_host_vfs.h`, `private_include/msc_common.h`, `private_include/msc_scsi_bot.h`.
   Si la compilation échoue sur des en-têtes ESP-IDF internes, utilisez plutôt la version ESP-IDF du dépôt (`../esp32-player`), qui télécharge ce composant automatiquement.

## Réglages
Constantes en tête du `.ino` : broches, `BG_VOL_PCT` (45), `FG_VOL_PCT`, délais min/max des insultes.
