# Bip toutes les secondes — ESP32-S3 + MAX98357A

Émet un bip de 1 kHz pendant 100 ms, une fois par seconde, via l'ampli I2S MAX98357A.

## Schéma de branchement

```
   ESP32-S3-DevKitC-1 v1.1                MAX98357A
  ┌───────────────────────┐          ┌─────────────────┐
  │                   5V  ├──────────┤ VIN             │
  │                  GND  ├──────────┤ GND             │
  │                GPIO4  ├──────────┤ LRC             │
  │                GPIO5  ├──────────┤ BCLK         +  ├─────┐
  │                GPIO6  ├──────────┤ DIN             │   ┌─┴─┐ Haut-parleur
  │                       │          │ GAIN  (libre)   │   │ HP│ 4 à 8 Ω
  │                       │          │ SD    (libre)   │   └─┬─┘
  └───────────────────────┘          │              -  ├─────┘
                                     └─────────────────┘
```

| MAX98357A | ESP32-S3    | Rôle                                   |
|-----------|-------------|----------------------------------------|
| VIN       | 5V          | Alimentation (3V3 possible, moins fort) |
| GND       | GND         | Masse                                  |
| LRC       | GPIO4       | Horloge gauche/droite (WS)             |
| BCLK      | GPIO5       | Horloge bit                            |
| DIN       | GPIO6       | Données audio                          |
| GAIN      | non branché | Gain de 9 dB                           |
| SD        | non branché | Ampli actif, mixage (G+D)/2            |

- Le haut-parleur se branche uniquement sur les bornes `+` et `-` du module. Ne jamais relier `-` à la masse : la sortie est en pont.
- Si aucun son ne sort, relier `SD` au 3V3 : sur certains clones la résistance de tirage manque et l'ampli reste en veille.

## Code

Le sketch est dans [`esp32-s3-bip.ino`](esp32-s3-bip.ino).

1. Arduino IDE avec le core **esp32 by Espressif** en version 3.x.
2. Carte : **ESP32S3 Dev Module**.
3. Ouvrir le sketch, téléverser.

Les constantes en tête du fichier règlent la fréquence (`BEEP_FREQ`), la durée (`BEEP_MS`), la période (`PERIOD_MS`) et le volume (`AMPLITUDE`).
