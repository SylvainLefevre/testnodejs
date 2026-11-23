'use strict';

const http = require('http');
const path = require('path');

const configLoader = require('./lib/config-loader');
const cache = require('./lib/cache');
const scheduler = require('./lib/scheduler');
const { fetchJson } = require('./lib/http-client');
const { handlePlayerApi } = require('./routes/player-api');
const { handleGetPhp } = require('./routes/get-php');
const { handleStatus, handleReloadConfig, handleRefresh } = require('./routes/admin');

const CONFIG_DIR = process.env.XTREAM_CONFIG_DIR || path.join(__dirname, '..', 'config');
const PORT = parseInt(process.env.PORT, 10) || 3000;
const DEFAULT_REFRESH_INTERVAL_MINUTES = 360;

let state = { config: null, filters: null };

function setState(config, filters) {
    state = { config, filters };
}

function loadState() {
    const { config, filters } = configLoader.loadAll(CONFIG_DIR);
    setState(config, filters);
}

async function refresh() {
    await cache.refreshAll(state.config, state.filters);
}

// Exécute un handler (sync ou async) et répond 500 proprement s'il rejette -
// évite qu'une erreur réseau vers la source Xtream fasse planter le process.
function run(res, work) {
    Promise.resolve()
        .then(work)
        .catch((err) => {
            console.error('[server] erreur non gérée:', err);
            if (!res.headersSent) {
                res.writeHead(500, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: 'Internal server error' }));
            }
        });
}

function requestListener(req, res) {
    const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    const ctx = {
        config: state.config,
        filters: state.filters,
        cache,
        fetchJson,
        configLoader,
        configDir: CONFIG_DIR,
        setState,
        triggerRefresh: refresh,
    };

    if (req.method === 'GET' && url.pathname === '/player_api.php') {
        return run(res, () => handlePlayerApi(req, res, url.searchParams, ctx));
    }
    if (req.method === 'GET' && url.pathname === '/get.php') {
        return run(res, () => handleGetPhp(req, res, url.searchParams, ctx));
    }
    if (req.method === 'GET' && url.pathname === '/admin/status') {
        return run(res, () => handleStatus(req, res, ctx));
    }
    if (req.method === 'POST' && url.pathname === '/admin/reload-config') {
        return run(res, () => handleReloadConfig(req, res, ctx));
    }
    if (req.method === 'POST' && url.pathname === '/admin/refresh') {
        return run(res, () => handleRefresh(req, res, ctx));
    }

    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found' }));
}

async function main() {
    loadState();
    console.log('[server] config chargée depuis', CONFIG_DIR);

    await refresh();

    const intervalMinutes = state.config.refreshIntervalMinutes || DEFAULT_REFRESH_INTERVAL_MINUTES;
    scheduler.start(intervalMinutes * 60 * 1000, refresh);
    console.log(`[server] rafraîchissement automatique toutes les ${intervalMinutes} minutes`);

    const server = http.createServer(requestListener);
    server.listen(PORT, () => {
        console.log(`[server] Xtream Filter API à l'écoute sur http://0.0.0.0:${PORT}`);
    });
}

main().catch((err) => {
    console.error('[server] erreur fatale au démarrage:', err);
    process.exit(1);
});
