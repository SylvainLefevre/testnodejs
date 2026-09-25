'use strict';

// Moteur de filtrage générique pour les catalogues Xtream (live / vod / series).
// Chargé dans le contexte global de Node-RED via functionGlobalContext (voir settings-snippet.js)
// et appelé depuis les nœuds Function du flow "Xtream Filter API".

function normalize(str) {
    return (str || '').toString().toLowerCase();
}

function matchesIncludeKeywords(name, keywords) {
    if (!keywords || !keywords.length) return true;
    const n = normalize(name);
    return keywords.some((k) => n.includes(normalize(k)));
}

function matchesExcludeKeywords(name, keywords) {
    if (!keywords || !keywords.length) return false;
    const n = normalize(name);
    return keywords.some((k) => n.includes(normalize(k)));
}

function safeRegExp(pattern) {
    try {
        return new RegExp(pattern, 'i');
    } catch (err) {
        return null;
    }
}

function matchesIncludeRegex(name, pattern) {
    if (!pattern) return true;
    const re = safeRegExp(pattern);
    return re ? re.test(name || '') : true;
}

function matchesExcludeRegex(name, pattern) {
    if (!pattern) return false;
    const re = safeRegExp(pattern);
    return re ? re.test(name || '') : false;
}

function matchesLanguagePrefixes(name, prefixes) {
    if (!prefixes || !prefixes.length) return true;
    const n = normalize(name);
    return prefixes.some((p) => {
        const np = normalize(p);
        return n.startsWith(np) || n.includes('|' + np) || n.includes(np + ':') || n.includes('[' + np + ']');
    });
}

function extractYear(item) {
    const dateStr = item.releaseDate || item.release_date || item.added;
    if (!dateStr) return null;
    const m = String(dateStr).match(/(\d{4})/);
    return m ? parseInt(m[1], 10) : null;
}

function categoryAllowed(category, rule) {
    if (!rule || rule.mode === 'all') return true;
    const ids = rule.ids || [];
    const patterns = rule.namePatterns || [];
    const idMatch = ids.map(String).includes(String(category.category_id));
    const nameMatch = patterns.some((p) => {
        const re = safeRegExp(p);
        return re ? re.test(category.category_name || '') : false;
    });
    const matched = idMatch || nameMatch;
    if (rule.mode === 'whitelist') return matched;
    if (rule.mode === 'blacklist') return !matched;
    return true;
}

function filterCategories(categories, rule) {
    return (categories || []).filter((c) => categoryAllowed(c, rule));
}

function filterItems(items, allowedCategoryIds, rules) {
    rules = rules || {};
    return (items || []).filter((item) => {
        if (allowedCategoryIds && !allowedCategoryIds.has(String(item.category_id))) {
            return false;
        }
        const name = item.name || '';
        if (!matchesIncludeKeywords(name, rules.includeKeywords)) return false;
        if (matchesExcludeKeywords(name, rules.excludeKeywords)) return false;
        if (!matchesIncludeRegex(name, rules.includeRegex)) return false;
        if (matchesExcludeRegex(name, rules.excludeRegex)) return false;
        if (!matchesLanguagePrefixes(name, rules.includeLanguagePrefixes)) return false;

        if (rules.minRating != null) {
            const r = parseFloat(item.rating || item.rating_5based || 0);
            if (isNaN(r) || r < rules.minRating) return false;
        }

        if (rules.minYear != null || rules.maxYear != null) {
            const y = extractYear(item);
            if (rules.minYear != null && (!y || y < rules.minYear)) return false;
            if (rules.maxYear != null && (!y || y > rules.maxYear)) return false;
        }

        return true;
    });
}

// Filtre un couple (catégories, éléments) selon les règles d'un type de contenu
// (rules = filters.json.live | .vod | .series). Recalcule ensuite la liste de
// catégories pour ne garder que celles qui ont encore au moins un élément.
function filterContent(categories, items, rules) {
    rules = rules || {};
    const categoryRule = rules.categories || { mode: 'all' };

    const categoriesAfterRule = filterCategories(categories, categoryRule);
    const allowedIds = categoryRule.mode === 'all' ? null : new Set(categoriesAfterRule.map((c) => String(c.category_id)));

    const filteredItems = filterItems(items, allowedIds, rules);

    const remainingCategoryIds = new Set(filteredItems.map((i) => String(i.category_id)));
    const finalCategories = categoriesAfterRule.filter((c) => remainingCategoryIds.has(String(c.category_id)));

    return { categories: finalCategories, items: filteredItems };
}

module.exports = {
    filterContent,
    filterCategories,
    filterItems,
    categoryAllowed,
    extractYear,
};
