# Lister les fichiers d'une clé USB — ESP32-S3 (USB OTG)

Au branchement d'une clé USB sur le port OTG, l'ESP32-S3 affiche toute son arborescence (fichiers, tailles, dossiers) dans le moniteur série. La liste est réaffichée à chaque nouveau branchement.

## Branchement

L'ESP32-S3-DevKitC-1 v1.1 a deux ports USB-C :

```
        ┌──────────────────────────────┐
        │     ESP32-S3-DevKitC-1 v1.1  │
        │                              │
        │   [UART]            [USB]    │
        └─────┬─────────────────┬──────┘
              │                 │  (GPIO19 = D-, GPIO20 = D+)
              │                 │
         PC : flash +      Adaptateur OTG USB-C → USB-A
         moniteur série         │
                           Clé USB (FAT32)
```

| Port      | Branché sur                      | Rôle                                      |
|-----------|----------------------------------|-------------------------------------------|
| **UART**  | PC                               | Alimentation, flash, moniteur série       |
| **USB**   | Adaptateur OTG → clé USB         | Hôte USB (l'ESP32-S3 lit la clé)          |

Le MAX98357A du projet `esp32-s3-bip` (GPIO4/5/6) peut rester branché : il n'utilise pas les mêmes broches.

### Alimentation de la clé (important)

Par défaut, le port **USB** de la carte ne fournit pas de 5 V vers l'extérieur : la clé ne s'allume pas. Deux solutions :

- **Ponter les pastilles `USB-OTG` (IN-OUT)** au dos de la carte, près du port USB. Le 5 V venant du port UART est alors envoyé sur le port USB. Vérifie la sérigraphie de ta carte et la doc Espressif avant de souder.
- **Ou** utiliser un câble/hub OTG alimenté qui fournit lui-même le 5 V à la clé.

### La clé

- Formatée en **FAT32** (ou FAT16). L'exFAT et le NTFS ne sont pas lus : les clés de plus de 32 Go sont souvent livrées en exFAT.
- Le programme ne formate jamais la clé (`format_if_mount_failed = false`).

## Code

Projet **ESP-IDF** (v5.1 ou plus) : l'Arduino IDE ne fournit pas de pilote hôte USB pour les clés. Le pilote `espressif/usb_host_msc` est téléchargé automatiquement depuis le registre de composants Espressif au premier build.

```bash
cd esp32-s3-usb-list
idf.py set-target esp32s3
idf.py -p /dev/ttyUSB0 flash monitor
```

(Remplacer `/dev/ttyUSB0` par le port du connecteur **UART**, par ex. `COM5` sous Windows.)

Exemple de sortie :

```
En attente d'une clé USB sur le port OTG...
I (2345) usb_list: Clé détectée (adresse 1)

===== Contenu de la clé USB =====
musique/
  bip.wav  (88244 octets)
photos/
  vacances/
    plage.jpg  (2048311 octets)
lisezmoi.txt  (512 octets)
===== 3 fichier(s), 3 dossier(s), 2137067 octets =====
```
