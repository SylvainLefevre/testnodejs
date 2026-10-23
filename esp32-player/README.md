# ESP32-S3 : fond sonore + insultes aléatoires (MAX98357A + clé USB)

## Câblage
| MAX98357A | ESP32-S3 |
|-----------|----------|
| LRC       | GPIO 48  |
| BCLK (BLK)| GPIO 45  |
| DIN       | GPIO 47  |
| GAIN      | GND (gain 12 dB) |
| VIN / GND | 5 V / GND |

Clé USB sur le port **USB (OTG natif)** de la carte, flashage/alimentation par le port **UART**.
Sur la DevKitC-1, soudez le jumper « USB-OTG » (ou alimentez en 5 V) pour que la clé reçoive du 5 V.

## Clé USB (FAT32)
```
fond/1.mp3          -> boucle, volume 45 %
Insultes/*.mp3      -> un fichier au hasard toutes les 1 à 5 min
```

## Build (ESP-IDF >= 5.1)
```
cd esp32-player
idf.py set-target esp32s3
idf.py build flash monitor
```
Les dépendances (`usb_host_msc`, `esp-libhelix-mp3`) sont téléchargées automatiquement via `main/idf_component.yml`.
Réglages (volumes, délais, broches) : constantes en tête de `main/main.c`.
