'use strict';

// Cache en mémoire du catalogue filtré. Un seul process Node.js, donc un
// simple objet module-level suffit.

const filterEngine = require('./filter-engine');
const xtreamClient = require('./xtream-client');
const { fetchJson } = require('./http-client');

const CONTENT_TYPES = {
    live: { categoriesAction: 'get_live_categories', itemsAction: 'get_live_streams' },
    vod: { categoriesAction: 'get_vod_categories', itemsAction: 'get_vod_streams' },
    series: { categoriesAction: 'get_series_categories', itemsAction: 'get_series' },
};

const store = {
    live: { categories: [], items: [], lastRefresh: null },
    vod: { categories: [], items: [], lastRefresh: null },
    series: { categories: [], items: [], lastRefresh: null },
};

function get(type) {
    return store[type];
}

async function refreshContentType(type, config, filterRules) {
    const { categoriesAction, itemsAction } = CONTENT_TYPES[type];
    const categoriesUrl = xtreamClient.playerApiUrl(config.source, categoriesAction);
    const itemsUrl = xtreamClient.playerApiUrl(config.source, itemsAction);

    const [categoriesRaw, itemsRaw] = await Promise.all([
        fetchJson(categoriesUrl, config.httpTimeoutMs),
        fetchJson(itemsUrl, config.httpTimeoutMs),
    ]);

    const result = filterEngine.filterContent(
        Array.isArray(categoriesRaw) ? categoriesRaw : [],
        Array.isArray(itemsRaw) ? itemsRaw : [],
        filterRules || {}
    );

    store[type] = { categories: result.categories, items: result.items, lastRefresh: Date.now() };
    return store[type];
}

async function refreshAll(config, filters) {
    filters = filters || {};
    const types = Object.keys(CONTENT_TYPES);
    const results = await Promise.allSettled(types.map((type) => refreshContentType(type, config, filters[type])));

    results.forEach((r, i) => {
        const type = types[i];
        if (r.status === 'rejected') {
            console.error(`[cache] échec du rafraîchissement "${type}":`, r.reason && r.reason.message);
        } else {
            console.log(`[cache] ${type}: ${r.value.items.length} éléments / ${r.value.categories.length} catégories`);
        }
    });
}

module.exports = { get, refreshAll, refreshContentType };
