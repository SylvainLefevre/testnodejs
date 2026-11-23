'use strict';

const { checkLocalAuth } = require('../lib/auth');

// Playlist M3U des chaînes live filtrées uniquement (pas de VOD/séries en
// M3U). Chaque entrée pointe directement vers le serveur source : voir
// docs/API.md#lecture-vidéo.
function handleGetPhp(req, res, query, ctx) {
    const { config, cache } = ctx;

    if (!checkLocalAuth(query, config)) {
        res.writeHead(401, { 'Content-Type': 'text/plain' });
        return res.end('Unauthorized');
    }

    const src = config.source || {};
    const proto = src.https ? 'https' : 'http';
    const port = src.port ? ':' + src.port : '';
    const { items, categories } = cache.get('live');

    const catMap = {};
    categories.forEach((c) => {
        catMap[String(c.category_id)] = c.category_name;
    });

    const lines = ['#EXTM3U'];
    items.forEach((it) => {
        const group = catMap[String(it.category_id)] || '';
        const logo = it.stream_icon || '';
        const streamUrl = `${proto}://${src.host}${port}/live/${src.username}/${src.password}/${it.stream_id}.ts`;
        lines.push(`#EXTINF:-1 tvg-id="${it.epg_channel_id || ''}" tvg-logo="${logo}" group-title="${group}",${it.name}`);
        lines.push(streamUrl);
    });

    res.writeHead(200, { 'Content-Type': 'application/x-mpegurl' });
    res.end(lines.join('\n'));
}

module.exports = { handleGetPhp };
