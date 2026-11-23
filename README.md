# Xtream Filter API

Filtre une source Xtream Codes (chaînes live, films VOD, séries) selon des
critères configurables, et republie un **catalogue filtré** via une API
locale compatible Xtream. Ne relaie **jamais** le flux vidéo lui-même : la
lecture se fait toujours en direct depuis le serveur source (voir
[docs/API.md](docs/API.md#lecture-vidéo)).

Application Node.js autonome, **sans dépendance npm externe** (juste le
`fetch` natif de Node ≥ 18), pensée pour tourner sur un Raspberry Pi 3.

## Contenu du dépôt

```
src/
  server.js      -> point d'entrée : serveur HTTP + planification du rafraîchissement
  lib/            -> logique métier (filtrage, client Xtream, cache, config)
  routes/         -> handlers des endpoints HTTP
config/
  *.example.json -> modèles de configuration à copier et adapter
deploy/
  xtream-filter-api.service -> unité systemd pour le déploiement
docs/
  INSTALL_RPI3.md -> installation et déploiement sur Raspberry Pi 3
  API.md          -> référence de l'API locale republiée
  FILTERS.md      -> référence des règles de filtrage (filters.json)
```

## Démarrage rapide

```bash
cp config/config.example.json config/config.json
cp config/filters.example.json config/filters.json
nano config/config.json    # hôte + identifiants de la source Xtream
nano config/filters.json   # règles de filtrage (voir docs/FILTERS.md)

npm start
```

Puis pointer votre lecteur IPTV (TiviMate, etc.) sur
`http://<ip-locale>:3000/player_api.php` avec les identifiants définis dans
`config.json` (`localAuth`).

Pour le déploiement réel sur un Raspberry Pi 3 (service systemd, démarrage
automatique), voir [docs/INSTALL_RPI3.md](docs/INSTALL_RPI3.md).

## Architecture en un coup d'œil

- **Cache en mémoire** : au démarrage, puis toutes les `refreshIntervalMinutes`
  minutes (6h par défaut) ou sur demande (`POST /admin/refresh`), le serveur
  interroge l'API Xtream source (`get_live_categories`/`streams`,
  `get_vod_*`, `get_series*`), applique les règles de `filters.json`, et
  garde le résultat filtré en mémoire. Les requêtes clientes sont donc
  servies depuis ce cache, pas depuis la source à chaque appel.
- **API locale** : `player_api.php` et `get.php` reproduisent le contrat
  Xtream standard pour que n'importe quel lecteur IPTV compatible Xtream
  fonctionne sans modification, à ceci près qu'il ne voit que le catalogue
  filtré.
- **Pas de proxy vidéo** : le `server_info` renvoyé au login pointe vers le
  serveur source réel ; le lecteur y va donc directement pour lire un flux.
  Le serveur ne fait transiter que les métadonnées (JSON), jamais la vidéo —
  indispensable vu les capacités limitées d'un Pi 3.
- **Zéro dépendance** : `node:http`, `fetch` et `fs` natifs suffisent ; pas
  de `npm install` à faire tourner sur le Pi.
