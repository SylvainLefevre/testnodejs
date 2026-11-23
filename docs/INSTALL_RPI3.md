# Installation sur Raspberry Pi 3

Le Pi 3 (4 cœurs Cortex-A53, 1 Go de RAM) est largement suffisant : le
serveur ne manipule que du JSON (catalogue filtré), jamais de flux vidéo, et
n'a aucune dépendance npm à installer.

## 1. Système

```bash
sudo apt update && sudo apt full-upgrade -y
```

## 2. Node.js ≥ 18

Le projet utilise le `fetch` natif de Node.js (disponible depuis Node 18),
donc aucune dépendance npm n'est nécessaire. Installer une version récente
via le dépôt NodeSource (architecture ARM du Pi gérée automatiquement) :

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
node -v   # doit afficher v20.x ou plus
```

## 3. Déployer les fichiers du projet

```bash
git clone https://github.com/SylvainLefevre/testnodejs.git ~/xtream-filter-api
cd ~/xtream-filter-api
git checkout claude/xtream-filtering-node-red-dlucug   # tant que la PR n'est pas mergée sur master
```

> Pour retrouver cette URL vous-même (ou si vous travaillez depuis un fork) :
> ouvrez le repo sur GitHub, cliquez sur le bouton vert **Code**, et copiez
> l'URL HTTPS (ou SSH si vous avez une clé configurée sur le Pi). Une fois la
> PR mergée sur `master`, l'étape `git checkout` ci-dessus devient inutile.
>
> Si vous n'avez pas `git` sur le Pi, vous pouvez aussi copier le dossier
> depuis votre machine de dev avec `scp -r . <user-pi>@<ip-du-pi>:~/xtream-filter-api`.
>
> - `<user-pi>` est le nom de l'utilisateur **sur le Raspberry Pi** (celui
>   créé lors du flashage de la carte SD avec Raspberry Pi Imager, pas
>   forcément `pi` sur les installations récentes), indépendant de
>   l'utilisateur de votre machine locale. Vérifiez-le avec
>   `ssh <user-pi>@<ip-du-pi> whoami` si besoin.
> - `<ip-du-pi>` : utilisez l'**IP locale** du Pi (ex. `192.168.1.x`, visible
>   via `hostname -I` sur le Pi), pas l'IP publique — cette API n'est pas
>   destinée à être exposée sur Internet (voir
>   [Sécurité et exposition réseau](#sécurité-et-exposition-réseau)).

Créer les fichiers de config réels à partir des exemples :

```bash
cd ~/xtream-filter-api
cp config/config.example.json config/config.json
cp config/filters.example.json config/filters.json
nano config/config.json    # renseigner l'hôte/identifiants de la source Xtream
nano config/filters.json   # définir les règles de filtrage (voir docs/FILTERS.md)
```

> `config/config.json` et `config/filters.json` contiennent les identifiants
> de votre source Xtream : ne les commitez pas dans un dépôt public (ils sont
> exclus par défaut, seuls les `*.example.json` sont versionnés).

## 4. Tester en direct (avant de passer en service)

```bash
npm start
```

Dans un autre terminal, ou depuis votre machine :

```bash
curl "http://<ip-du-pi>:3000/player_api.php?username=<user>&password=<pass>&action=get_live_categories"
```

Arrêter avec `Ctrl+C` une fois que ça répond correctement.

## 5. Lancer en service systemd (démarrage automatique)

Le fichier `deploy/xtream-filter-api.service` contient un modèle d'unité
systemd. L'installer :

```bash
sed -e "s|__USER__|$(whoami)|g" -e "s|__APP_DIR__|$HOME/xtream-filter-api|g" \
    deploy/xtream-filter-api.service | sudo tee /etc/systemd/system/xtream-filter-api.service

sudo systemctl daemon-reload
sudo systemctl enable xtream-filter-api
sudo systemctl start xtream-filter-api
sudo systemctl status xtream-filter-api
```

Logs en direct :

```bash
journalctl -u xtream-filter-api -f
```

## 6. Tester le service

```bash
curl "http://<ip-du-pi>:3000/player_api.php?username=<user>&password=<pass>&action=get_live_categories"
curl "http://<ip-du-pi>:3000/admin/status" -H "x-admin-token: <votre-token-admin>"
```

Puis configurer votre lecteur IPTV (TiviMate, IPTV Smarters, etc.) en mode
"Xtream Codes" avec :
- Serveur : `http://<ip-du-pi>:3000`
- Utilisateur / mot de passe : ceux définis dans `config/config.json` ->
  `localAuth`

Voir [API.md](API.md) pour la référence complète des endpoints et
[FILTERS.md](FILTERS.md) pour écrire vos règles de filtrage.

## Sécurité et exposition réseau

- Le service n'écoute que sur le port configuré (3000 par défaut, via la
  variable d'environnement `PORT`) : ne l'exposez pas directement sur
  Internet sans reverse proxy + authentification supplémentaire (les
  identifiants Xtream transitent en clair sur HTTP par défaut, comme le
  protocole Xtream Codes standard).
- `adminToken` (dans `config.json`) protège les endpoints `/admin/*` :
  changez la valeur par défaut avant tout déploiement.

## Maintenance

- **Changer les règles de filtrage** : éditer `config/filters.json` puis
  `curl -X POST http://<ip-du-pi>:3000/admin/reload-config -H "x-admin-token: ..."`
  suivi de `curl -X POST http://<ip-du-pi>:3000/admin/refresh -H "x-admin-token: ..."`
  (pas besoin de redémarrer le service).
- **Changer l'intervalle de rafraîchissement automatique** : éditer
  `refreshIntervalMinutes` dans `config.json` puis recharger/redémarrer.
- **Changer le port** : variable d'environnement `PORT` dans
  `/etc/systemd/system/xtream-filter-api.service`, puis
  `sudo systemctl daemon-reload && sudo systemctl restart xtream-filter-api`.
- **Mettre à jour le code** : `git pull` dans `~/xtream-filter-api` puis
  `sudo systemctl restart xtream-filter-api`.
- **Logs** : `journalctl -u xtream-filter-api -f`.
