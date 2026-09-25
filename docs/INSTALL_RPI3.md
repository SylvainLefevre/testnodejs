# Installation sur Raspberry Pi 3

Le Pi 3 (4 cœurs Cortex-A53, 1 Go de RAM) est largement suffisant : le flow ne
manipule que du JSON (catalogue filtré), jamais de flux vidéo.

## 1. Système

```bash
sudo apt update && sudo apt full-upgrade -y
```

Node-RED recommande une carte SD de 16 Go minimum et, idéalement, de faire
tourner le système depuis une clé USB/SSD plutôt qu'une carte SD (plus fiable
dans la durée), mais une carte SD convient pour ce projet vu le faible volume
d'écritures.

## 2. Node.js + Node-RED

Utiliser le script d'installation officiel, qui installe une version de
Node.js adaptée à l'architecture ARM du Pi :

```bash
bash <(curl -sL https://raw.githubusercontent.com/node-red/linux-installers/master/deb/update-nodejs-and-nodered)
```

Vérifier :

```bash
node -v
node-red --version
```

## 3. Déployer les fichiers du projet

Le dossier `node-red/` de ce dépôt doit être copié sur le Pi, par exemple
dans `~/.node-red/xtream-filter/` :

```bash
mkdir -p ~/.node-red/xtream-filter
# depuis votre machine de dev, ou via git clone directement sur le Pi :
scp -r node-red/functions node-red/config pi@<ip-du-pi>:~/.node-red/xtream-filter/
```

Puis, sur le Pi, créer les fichiers de config réels à partir des exemples :

```bash
cd ~/.node-red/xtream-filter/config
cp config.example.json config.json
cp filters.example.json filters.json
nano config.json    # renseigner l'hôte/identifiants de la source Xtream
nano filters.json   # définir les règles de filtrage (voir docs/FILTERS.md)
```

> `config.json` et `filters.json` contiennent les identifiants de votre
> source Xtream : ne les commitez pas dans un dépôt public (ils sont exclus
> par défaut, seuls les `*.example.json` sont versionnés).

## 4. Brancher `functionGlobalContext` dans `settings.js`

Éditer `~/.node-red/settings.js` et ajouter la clé `functionGlobalContext`
(voir `node-red/settings-snippet.js` pour le détail) :

```js
const path = require('path');
const XTREAM_DIR = path.join(process.env.HOME, '.node-red', 'xtream-filter');

module.exports = {
    // ... reste de votre settings.js existant, ne pas supprimer ...

    functionGlobalContext: {
        filterEngine: require(path.join(XTREAM_DIR, 'functions', 'filter-engine.js')),
        xtreamClient: require(path.join(XTREAM_DIR, 'functions', 'xtream-client.js')),
        configLoader: require(path.join(XTREAM_DIR, 'functions', 'config-loader.js')),
    },
};
```

## 5. Variable d'environnement `XTREAM_CONFIG_DIR`

Le flow lit `config.json`/`filters.json` depuis le dossier pointé par la
variable d'environnement `XTREAM_CONFIG_DIR` (par défaut
`/home/pi/.node-red/xtream-filter/config` si non définie). Si vous avez suivi
les chemins ci-dessus avec l'utilisateur `pi`, la valeur par défaut convient
déjà. Sinon, définissez-la explicitement dans le service systemd (étape
suivante).

## 6. Lancer Node-RED en service systemd

Le script d'installation crée normalement déjà un service `nodered.service`.
Pour fixer la variable d'environnement et s'assurer du démarrage automatique :

```bash
sudo systemctl edit nodered.service
```

Ajouter :

```ini
[Service]
Environment=XTREAM_CONFIG_DIR=/home/pi/.node-red/xtream-filter/config
```

Puis :

```bash
sudo systemctl daemon-reload
sudo systemctl enable nodered
sudo systemctl restart nodered
sudo systemctl status nodered
```

## 7. Importer le flow

1. Ouvrir l'éditeur Node-RED : `http://<ip-du-pi>:1880`
2. Menu ☰ -> Import -> sélectionner `node-red/flows/xtream-filter.flows.json`
3. Déployer (bouton "Deploy")

Au déploiement, le nœud "Au déploiement" charge `config.json`/`filters.json`
puis déclenche un premier rafraîchissement du catalogue (fetch source +
filtrage). Vérifier dans l'onglet debug/statuts des nœuds que le statut passe
au vert avec un nombre de chaînes/films/séries.

## 8. Tester

```bash
curl "http://<ip-du-pi>:1880/player_api.php?username=<user>&password=<pass>&action=get_live_categories"
curl "http://<ip-du-pi>:1880/admin/status" -H "x-admin-token: <votre-token-admin>"
```

Puis configurer votre lecteur IPTV (TiviMate, IPTV Smarters, etc.) en mode
"Xtream Codes" avec :
- Serveur : `http://<ip-du-pi>:1880`
- Utilisateur / mot de passe : ceux définis dans `config.json` -> `localAuth`

Voir [API.md](API.md) pour la référence complète des endpoints et
[FILTERS.md](FILTERS.md) pour écrire vos règles de filtrage.

## Sécurité et exposition réseau

- Le service n'écoute que ce que Node-RED expose (port 1880 par défaut) : ne
  l'exposez pas directement sur Internet sans reverse proxy + authentification
  supplémentaire (les identifiants Xtream transitent en clair sur HTTP par
  défaut, comme le protocole Xtream Codes standard).
- Protégez l'éditeur Node-RED lui-même (`http://<ip>:1880`, différent de
  l'API republiée) avec `adminAuth` dans `settings.js` si le Pi est accessible
  au-delà de votre réseau local.
- `adminToken` (dans `config.json`) protège les endpoints `/admin/*` : changez
  la valeur par défaut avant tout déploiement.

## Maintenance

- **Changer les règles de filtrage** : éditer `filters.json` puis
  `curl -X POST http://<ip-du-pi>:1880/admin/reload-config -H "x-admin-token: ..."`
  suivi de `curl -X POST http://<ip-du-pi>:1880/admin/refresh -H "x-admin-token: ..."`
  (pas besoin de redéployer le flow).
- **Changer l'intervalle de rafraîchissement automatique** : éditer la
  propriété "repeat" du nœud inject "Rafraîchissement périodique" dans
  l'éditeur Node-RED (valeur par défaut : 21600 s = 6h).
- **Logs** : `journalctl -u nodered -f`.
