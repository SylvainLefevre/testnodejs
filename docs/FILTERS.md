# Référence des règles de filtrage (`filters.json`)

`filters.json` contient trois blocs indépendants : `live`, `vod`, `series`.
Chacun a la même structure, appliquée par `node-red/functions/filter-engine.js`.

```json
{
  "live":   { "categories": { ... }, "includeKeywords": [], ... },
  "vod":    { ... },
  "series": { ... }
}
```

Après modification, appliquer via l'API admin (voir
[API.md](API.md#post-adminreload-config)) plutôt que de redéployer le flow.

## `categories`

```json
"categories": {
  "mode": "blacklist",
  "ids": ["17", "42"],
  "namePatterns": ["adult", "xxx"]
}
```

- `mode` :
  - `"all"` — aucune restriction par catégorie (toutes conservées, sous
    réserve des autres règles ci-dessous).
  - `"whitelist"` — seules les catégories dont l'`id` est dans `ids` OU dont
    le nom matche un des `namePatterns` sont conservées.
  - `"blacklist"` — toutes les catégories sont conservées **sauf** celles qui
    matchent `ids`/`namePatterns`.
- `ids` : liste d'`category_id` Xtream (chaînes de caractères).
- `namePatterns` : liste d'expressions régulières (JS), testées sur
  `category_name`, **insensibles à la casse par défaut** (inutile d'ajouter
  un flag d'insensibilité dans le pattern).

Les catégories qui, après filtrage des éléments, ne contiennent plus aucune
chaîne/film/série sont automatiquement retirées de la liste des catégories
renvoyée (pas besoin de les lister explicitement).

## Filtres au niveau de l'élément (chaîne / film / série)

Appliqués en plus du filtre de catégorie, sur le champ `name` de chaque
élément :

| Clé | Type | Effet |
|---|---|---|
| `includeKeywords` | `string[]` | Si non vide, l'élément doit contenir au moins un de ces mots (sous-chaîne, insensible à la casse) dans son nom. |
| `excludeKeywords` | `string[]` | Si l'élément contient un de ces mots, il est rejeté. |
| `includeRegex` | `string \| null` | Regex que le nom doit matcher (insensible à la casse). |
| `excludeRegex` | `string \| null` | Regex qui, si elle matche, rejette l'élément. |
| `includeLanguagePrefixes` | `string[]` | Si non vide, le nom doit commencer par un de ces préfixes, ou le contenir sous la forme `\|FR`, `FR:` ou `[FR]` — pratique pour les listes IPTV qui préfixent les chaînes par langue/pays (`"FR | TF1"`, `"FR: TF1"`...). |

## Filtres numériques (VOD et séries uniquement)

| Clé | Effet |
|---|---|
| `minRating` | Note minimale (`rating` ou `rating_5based` du flux source). |
| `minYear` / `maxYear` | Année extraite de `releaseDate`/`added`. `null` = pas de borne. |

Ignorés silencieusement pour `live` (les chaînes live n'ont pas ces champs).

## Exemples

### Ne garder que les chaînes françaises, sans les catégories adultes

```json
"live": {
  "categories": { "mode": "blacklist", "ids": [], "namePatterns": ["adult", "xxx", "for.*adult"] },
  "excludeKeywords": ["adult", "xxx"],
  "includeLanguagePrefixes": ["FR"]
}
```

### Ne garder que les films d'une catégorie précise, sortis après 2015, notés 6+

```json
"vod": {
  "categories": { "mode": "whitelist", "ids": ["12"], "namePatterns": [] },
  "minYear": 2015,
  "minRating": 6
}
```

### Exclure des séries par mot-clé précis via regex

```json
"series": {
  "categories": { "mode": "all", "ids": [], "namePatterns": [] },
  "excludeRegex": "^(TEST|DEMO) "
}
```

## Débogage

- `GET /admin/status` (voir [API.md](API.md)) donne le nombre d'éléments
  retenus par type après le dernier rafraîchissement — utile pour vérifier
  qu'une règle ne filtre pas tout par erreur.
- Les statuts affichés sous chaque nœud "Filtrer + stocker cache ..." dans
  l'éditeur Node-RED affichent aussi ces compteurs en temps réel après un
  déploiement ou un rafraîchissement manuel.
- Une regex invalide dans `namePatterns`/`includeRegex`/`excludeRegex` est
  ignorée silencieusement (traitée comme non filtrante) plutôt que de faire
  planter le flow — vérifiez sa syntaxe si le résultat ne correspond pas à
  vos attentes.
