'use strict';

const { checkAdminToken } = require('../lib/auth');

function sendJson(res, statusCode, payload) {
    res.writeHead(statusCode, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(payload));
}

function handleStatus(req, res, ctx) {
    const { config, cache } = ctx;
    if (!checkAdminToken(req.headers, config)) return sendJson(res, 403, { error: 'Forbidden' });

    const payload = {};
    ['live', 'vod', 'series'].forEach((type) => {
        const c = cache.get(type);
        payload[type] = { count: c.items.length, lastRefresh: c.lastRefresh };
    });
    return sendJson(res, 200, payload);
}

function handleReloadConfig(req, res, ctx) {
    const { config, configLoader, configDir, setState } = ctx;
    if (!checkAdminToken(req.headers, config)) return sendJson(res, 403, { error: 'Forbidden' });

    try {
        const { config: newConfig, filters: newFilters } = configLoader.loadAll(configDir);
        setState(newConfig, newFilters);
        return sendJson(res, 200, { status: 'reloaded' });
    } catch (err) {
        return sendJson(res, 500, { error: err.message });
    }
}

function handleRefresh(req, res, ctx) {
    const { config, triggerRefresh } = ctx;
    if (!checkAdminToken(req.headers, config)) return sendJson(res, 403, { error: 'Forbidden' });

    sendJson(res, 202, { status: 'refresh triggered' });
    triggerRefresh().catch((err) => console.error('[admin/refresh] erreur:', err.message));
}

module.exports = { handleStatus, handleReloadConfig, handleRefresh };
