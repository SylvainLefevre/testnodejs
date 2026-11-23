'use strict';

// Petit wrapper autour du fetch natif de Node.js (>=18) avec timeout.
async function fetchJson(url, timeoutMs) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs || 15000);
    try {
        const res = await fetch(url, { signal: controller.signal });
        if (!res.ok) {
            throw new Error(`HTTP ${res.status} en appelant ${url}`);
        }
        return await res.json();
    } finally {
        clearTimeout(timer);
    }
}

module.exports = { fetchJson };
