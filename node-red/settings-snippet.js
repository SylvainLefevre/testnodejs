// À fusionner dans le settings.js de Node-RED (~/.node-red/settings.js).
// Ajoute simplement la clé `functionGlobalContext` ci-dessous à l'objet
// `module.exports` existant (ne remplacez pas tout le fichier, Node-RED en a
// besoin pour son propre fonctionnement).
//
// Adaptez NODE_RED_XTREAM_DIR si vous avez copié le dossier `node-red/` de ce
// dépôt ailleurs que dans ~/.node-red/xtream-filter.

const path = require('path');
const XTREAM_DIR = process.env.NODE_RED_XTREAM_DIR || path.join(process.env.HOME || '/home/pi', '.node-red', 'xtream-filter');

module.exports = {
    // ... reste de votre settings.js existant ...

    functionGlobalContext: {
        filterEngine: require(path.join(XTREAM_DIR, 'functions', 'filter-engine.js')),
        xtreamClient: require(path.join(XTREAM_DIR, 'functions', 'xtream-client.js')),
        configLoader: require(path.join(XTREAM_DIR, 'functions', 'config-loader.js')),
    },
};
