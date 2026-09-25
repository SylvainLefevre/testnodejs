# API locale republiée

Base URL : `http://<ip-du-pi>:1880` (port Node-RED par défaut).

Tous les endpoints (sauf `/admin/*`) reproduisent le contrat de l'API Xtream
Codes standard, avec authentification par `username`/`password` en query
string (comme le protocole d'origine), vérifiés contre `config.json` ->
`localAuth`.

## `GET /player_api.php`

### Sans paramètre `action` — handshake / login

```
GET /player_api.php?username=U&password=P
```

Retourne `user_info` (identifiants échoués tels que reçus) et `server_info`
**pointant vers le serveur source réel** — voir [Lecture vidéo](#lecture-vidéo)
ci-dessous pour pourquoi c'est volontaire.

### Catalogue live

| Action | Paramètres | Retourne |
|---|---|---|
| `get_live_categories` | — | Catégories live après filtrage |
| `get_live_streams` | `category_id` (optionnel) | Chaînes live filtrées, éventuellement restreintes à une catégorie |

### Catalogue VOD (films)

| Action | Paramètres | Retourne |
|---|---|---|
| `get_vod_categories` | — | Catégories VOD après filtrage |
| `get_vod_streams` | `category_id` (optionnel) | Films filtrés |
| `get_vod_info` | `vod_id` (obligatoire) | Détail d'un film — proxy en direct vers la source, uniquement si `vod_id` fait partie du catalogue filtré (sinon 404) |

### Catalogue séries

| Action | Paramètres | Retourne |
|---|---|---|
| `get_series_categories` | — | Catégories séries après filtrage |
| `get_series` | `category_id` (optionnel) | Séries filtrées (sans les épisodes) |
| `get_series_info` | `series_id` (obligatoire) | Détail + épisodes d'une série — proxy en direct vers la source, uniquement si `series_id` fait partie du catalogue filtré (sinon 404) |

`get_vod_info`/`get_series_info` ne sont pas filtrés en profondeur (les
épisodes d'une série passée ne sont pas eux-mêmes filtrés individuellement) :
seule l'appartenance de l'identifiant au catalogue déjà filtré est vérifiée.

## `GET /get.php` — playlist M3U

```
GET /get.php?username=U&password=P
```

Retourne une playlist M3U des **chaînes live filtrées uniquement** (pas de
VOD/séries en M3U dans cette version). Chaque entrée pointe directement vers
l'URL de streaming du serveur source (`http(s)://source/live/user/pass/id.ts`)
— voir [Lecture vidéo](#lecture-vidéo).

## Lecture vidéo

**Le Raspberry Pi ne sert jamais de flux vidéo.** Le catalogue (JSON ou M3U)
renvoyé par l'API locale contient des URLs de lecture qui pointent
**directement vers le serveur Xtream source**, avec les identifiants source.
Concrètement :

- Le `server_info.url`/`port` renvoyé au login est celui de la source, pas du
  Pi.
- Pour que la lecture fonctionne, **les identifiants locaux
  (`config.json` -> `localAuth`) doivent être valides sur le serveur
  source** — le plus simple est de les faire identiques aux identifiants
  source (valeur par défaut dans `config.example.json`), car les lecteurs
  Xtream construisent l'URL de lecture avec les identifiants utilisés au
  login combinés au `server_info` reçu.
- Si vous avez besoin d'identifiants locaux différents des identifiants
  source (par ex. pour donner un accès restreint à un tiers), la lecture
  vidéo ne fonctionnera pas dans cette version : il faudrait ajouter un vrai
  proxy de flux, volontairement hors périmètre ici (charge trop lourde pour
  un Pi 3 avec plusieurs flux HD simultanés).

## Endpoints d'administration

Protégés par l'en-tête `x-admin-token` (valeur définie dans `config.json`).

### `GET /admin/status`

```bash
curl http://<ip-du-pi>:1880/admin/status -H "x-admin-token: <token>"
```

```json
{
  "live":   { "count": 42, "lastRefresh": 1732450000000 },
  "vod":    { "count": 130, "lastRefresh": 1732450000000 },
  "series": { "count": 18, "lastRefresh": 1732450000000 }
}
```

### `POST /admin/reload-config`

Recharge `config.json` et `filters.json` depuis le disque sans redémarrer
Node-RED ni redéployer le flow. À appeler après avoir modifié les règles de
filtrage.

### `POST /admin/refresh`

Déclenche immédiatement un nouveau fetch + filtrage de la source (en plus du
rafraîchissement automatique périodique). Répond immédiatement, le
rafraîchissement se poursuit en tâche de fond.

## Codes d'erreur

| Cas | Code | Corps |
|---|---|---|
| Identifiants locaux invalides (`player_api.php`, `get.php`) | 401 | JSON `{user_info:{auth:0,...}}` ou texte |
| `vod_id`/`series_id` hors catalogue filtré | 404 | `{}` |
| Token admin invalide/absent | 403 | `{"error":"Forbidden"}` |
