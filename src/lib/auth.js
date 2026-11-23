'use strict';

function checkLocalAuth(query, config) {
    const local = (config && config.localAuth) || {};
    return Boolean(local.username) && Boolean(local.password) && query.get('username') === local.username && query.get('password') === local.password;
}

function checkAdminToken(headers, config) {
    const token = headers['x-admin-token'];
    return Boolean(config && config.adminToken) && token === config.adminToken;
}

module.exports = { checkLocalAuth, checkAdminToken };
