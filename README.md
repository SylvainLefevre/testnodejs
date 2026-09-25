# Xtream Filter API (Node-RED)

Filtre une source Xtream Codes (chaînes live, films VOD, séries) selon des
critères configurables, et republie un **catalogue filtré** via une API
locale compatible Xtream. Ne relaie **jamais** le flux vidéo lui-même : la
lecture se fait toujours en direct depuis le serveur source (voir
[docs/API.md](docs/API.md#lecture-vidéo)).

Conçu pour tourner sur un Raspberry Pi 3 via [Node-RED](https://nodered.org/).

## Contenu du dépôt

```
node-red/
  flows/xtream-filter.flows.json   -> flow Node-RED à importer
  functions/                       -> logique métier (filtrage, client Xtream, config)
  config/                          -> fichiers de config d'exemple (à copier et adapter)
  settings-snippet.js              -> extrait à fusionner dans settings.js de Node-RED
docs/
  INSTALL_RPI3.md                  -> installation et déploiement sur Raspberry Pi 3
  API.md                           -> référence de l'API locale republiée
  FILTERS.md                       -> référence des règles de filtrage (filters.json)
```

## Démarrage rapide

1. Suivre [docs/INSTALL_RPI3.md](docs/INSTALL_RPI3.md) pour installer Node-RED
   sur le Raspberry Pi et déployer ce dépôt.
2. Copier `node-red/config/config.example.json` -> `config.json` et
   `node-red/config/filters.example.json` -> `filters.json`, puis les adapter
   (voir [docs/FILTERS.md](docs/FILTERS.md)).
3. Importer `node-red/flows/xtream-filter.flows.json` dans Node-RED et
   déployer.
4. Pointer votre lecteur IPTV (TiviMate, etc.) sur
   `http://<ip-du-pi>:1880/player_api.php` avec les identifiants définis dans
   `config.json` (`localAuth`).

## Architecture en un coup d'œil

- **Cache local** : toutes les 6h (par défaut) ou sur demande
  (`POST /admin/refresh`), le flow interroge l'API Xtream source
  (`get_live_categories`/`streams`, `get_vod_*`, `get_series*`), applique les
  règles de `filters.json`, et stocke le résultat filtré en contexte global
  Node-RED. Les requêtes clientes sont donc servies depuis ce cache, pas
  depuis la source à chaque appel.
- **API locale** : `player_api.php` et `get.php` reproduisent le contrat
  Xtream standard pour que n'importe quel lecteur IPTV compatible Xtream
  fonctionne sans modification, à ceci près qu'il ne voit que le catalogue
  filtré.
- **Pas de proxy vidéo** : le `server_info` renvoyé au login pointe vers le
  serveur source réel ; le lecteur y va donc directement pour lire un flux.
  Le Raspberry Pi ne fait transiter que les métadonnées (JSON), jamais la
  vidéo — indispensable vu les capacités limitées d'un Pi 3.
