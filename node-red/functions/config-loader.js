'use strict';

const fs = require('fs');
const path = require('path');

function loadJson(filePath) {
    const raw = fs.readFileSync(filePath, 'utf8');
    return JSON.parse(raw);
}

// Lit config.json et filters.json depuis le dossier configDir.
// Lève une erreur explicite si un fichier est absent ou invalide -
// c'est au nœud appelant (Function "Load Config") de la reporter via node.error().
function loadAll(configDir) {
    const config = loadJson(path.join(configDir, 'config.json'));
    const filters = loadJson(path.join(configDir, 'filters.json'));
    return { config, filters };
}

module.exports = { loadJson, loadAll };
