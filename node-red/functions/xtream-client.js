'use strict';

// Construction des URLs vers l'API Xtream source. Chargé dans le contexte
// global de Node-RED via functionGlobalContext (voir settings-snippet.js).

function baseUrl(source) {
    const proto = source.https ? 'https' : 'http';
    const port = source.port ? ':' + source.port : '';
    return `${proto}://${source.host}${port}`;
}

function playerApiUrl(source, action, extraParams) {
    const params = new URLSearchParams({
        username: source.username,
        password: source.password,
    });
    if (action) params.set('action', action);
    if (extraParams) {
        Object.entries(extraParams).forEach(([k, v]) => {
            if (v !== undefined && v !== null) params.set(k, v);
        });
    }
    return `${baseUrl(source)}/player_api.php?${params.toString()}`;
}

function liveStreamUrl(source, streamId, ext) {
    return `${baseUrl(source)}/live/${source.username}/${source.password}/${streamId}.${ext || 'ts'}`;
}

module.exports = {
    baseUrl,
    playerApiUrl,
    liveStreamUrl,
};
