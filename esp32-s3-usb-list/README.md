# Lister les fichiers d'une clé USB — ESP32-S3 (USB OTG, Arduino)

Au branchement d'une clé USB sur le port OTG, l'ESP32-S3 affiche toute son arborescence (fichiers, tailles, dossiers) dans le moniteur série. La liste est réaffichée à chaque nouveau branchement. La clé est lue en lecture seule.

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
         PC : téléversement  Adaptateur OTG USB-C → USB-A
         + moniteur série       │
                           Clé USB (FAT32)
```

| Port      | Branché sur              | Rôle                                |
|-----------|--------------------------|-------------------------------------|
| **UART**  | PC                       | Alimentation, téléversement, moniteur série |
| **USB**   | Adaptateur OTG → clé USB | Hôte USB (l'ESP32-S3 lit la clé)    |

Le MAX98357A du projet `esp32-s3-bip` (GPIO4/5/6) peut rester branché : il n'utilise pas les mêmes broches.

### Alimentation de la clé (important)

Par défaut, le port **USB** de la carte ne fournit pas de 5 V vers l'extérieur : la clé ne s'allume pas. Deux solutions :

- **Ponter les pastilles `USB-OTG` (IN-OUT)** au dos de la carte, près du port USB. Le 5 V venant du port UART est alors envoyé sur le port USB. Vérifie la sérigraphie de ta carte et la doc Espressif avant de souder.
- **Ou** utiliser un câble OTG en Y avec entrée d'alimentation, qui fournit lui-même le 5 V à la clé (pas un hub : les hubs USB ne sont pas gérés).
- **Ou**, sans souder sur la carte, brancher la clé sur un **connecteur USB-A femelle (breakout)** relié directement aux broches, et ne rien brancher sur le port USB de la carte :

| Breakout USB-A femelle | ESP32-S3     |
|------------------------|--------------|
| VBUS (5V)              | 5V           |
| GND                    | GND          |
| D−                     | GPIO19       |
| D+                     | GPIO20       |

  Le 5V de la broche vient du câble branché sur le port UART. Fils de données courts (< 10 cm). Un condensateur de 100 µF entre 5V et GND évite que la carte redémarre quand on branche la clé.

### La clé

- Formatée en **FAT32** (ou FAT16). Les clés de plus de 32 Go sont souvent livrées en exFAT, qui peut ne pas être lu.
- Une seule partition, une seule clé à la fois (pas de hub USB).

## Arduino IDE

1. Core **esp32 by Espressif** en version **3.x** (aucune bibliothèque supplémentaire).
2. Menu **Outils** :
   - Carte : **ESP32S3 Dev Module**
   - USB Mode : **Hardware CDC and JTAG**
   - USB CDC On Boot : **Disabled** (le moniteur série passe par le port UART)
3. Brancher le PC sur le port **UART**, téléverser, ouvrir le moniteur série à **115200 bauds**.
4. Brancher la clé sur le port **USB** via l'adaptateur OTG.

Exemple de sortie :

```
En attente d'une clé USB sur le port OTG...
Clé détectée : SanDisk  Cruzer Blade
Capacité : 15267 Mo (31266816 secteurs de 512 octets)

===== Contenu de la clé USB =====
musique/
  bip.wav  (88244 octets)
photos/
  vacances/
    plage.jpg  (2048311 octets)
lisezmoi.txt  (512 octets)
===== 3 fichier(s), 3 dossier(s), 2137067 octets =====

Clé retirée. En attente d'une clé USB...
```

## Fonctionnement

Le core Arduino-ESP32 n'a pas de bibliothèque « clé USB » toute faite. Le sketch utilise donc directement :

- la pile **hôte USB** d'ESP-IDF incluse dans le core (`usb/usb_host.h`) pour détecter la clé et échanger avec elle ;
- son propre petit pilote **Mass Storage Bulk-Only** (commandes SCSI INQUIRY, TEST UNIT READY, READ CAPACITY, READ(10)) ;
- **FatFs** (inclus dans le core) branché sur ce pilote pour lire le système de fichiers.
