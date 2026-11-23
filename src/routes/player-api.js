'use strict';

const xtreamClient = require('../lib/xtream-client');
const { checkLocalAuth } = require('../lib/auth');

function sendJson(res, statusCode, payload) {
    res.writeHead(statusCode, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(payload));
}

function filterByCategory(items, query) {
    const catId = query.get('category_id');
    if (!catId) return items;
    return items.filter((i) => String(i.category_id) === String(catId));
}

function buildHandshake(query, config) {
    const src = config.source || {};
    const now = Math.floor(Date.now() / 1000);
    return {
        user_info: {
            username: query.get('username'),
            password: query.get('password'),
            message: '',
            auth: 1,
            status: 'Active',
            exp_date: null,
            is_trial: '0',
            active_cons: '0',
            created_at: null,
            max_connections: '1',
            allowed_output_formats: ['m3u8', 'ts'],
        },
        server_info: {
            url: src.host,
            port: String(src.port || 80),
            https_port: String(src.httpsPort || src.port || 443),
            server_protocol: src.https ? 'https' : 'http',
            rtmp_port: '0',
            timezone: config.timezone || 'Europe/Paris',
            timestamp_now: now,
            time_now: new Date().toISOString(),
        },
    };
}

// get_vod_info / get_series_info : vérifie que l'id appartient au catalogue
// déjà filtré, puis relaie l'appel en direct vers la source (pas de filtrage
// au niveau des épisodes/détails, seulement au niveau de l'appartenance).
async function handleInfoProxy(res, query, config, items, idField, queryParam, action, fetchJson) {
    const id = query.get(queryParam);
    const allowed = items.some((i) => String(i[idField]) === String(id));
    if (!allowed) {
        return sendJson(res, 404, {});
    }
    const url = xtreamClient.playerApiUrl(config.source, action, { [queryParam]: id });
    const data = await fetchJson(url, config.httpTimeoutMs);
    return sendJson(res, 200, data);
}

async function handlePlayerApi(req, res, query, ctx) {
    const { config, cache, fetchJson } = ctx;

    if (!checkLocalAuth(query, config)) {
        return sendJson(res, 401, {
            user_info: { auth: 0, status: 'Disabled', message: 'Invalid username or password' },
            server_info: {},
        });
    }

    switch (query.get('action')) {
        case 'get_live_categories':
            return sendJson(res, 200, cache.get('live').categories);
        case 'get_live_streams':
            return sendJson(res, 200, filterByCategory(cache.get('live').items, query));
        case 'get_vod_categories':
            return sendJson(res, 200, cache.get('vod').categories);
        case 'get_vod_streams':
            return sendJson(res, 200, filterByCategory(cache.get('vod').items, query));
        case 'get_vod_info':
            return handleInfoProxy(res, query, config, cache.get('vod').items, 'stream_id', 'vod_id', 'get_vod_info', fetchJson);
        case 'get_series_categories':
            return sendJson(res, 200, cache.get('series').categories);
        case 'get_series':
            return sendJson(res, 200, filterByCategory(cache.get('series').items, query));
        case 'get_series_info':
            return handleInfoProxy(res, query, config, cache.get('series').items, 'series_id', 'series_id', 'get_series_info', fetchJson);
        default:
            return sendJson(res, 200, buildHandshake(query, config));
    }
}

module.exports = { handlePlayerApi };
