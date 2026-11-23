'use strict';

// Planifie une tâche périodique. Un seul minuteur actif à la fois : un
// nouvel appel à start() remplace le précédent.

let timer = null;

function start(intervalMs, task) {
    stop();
    timer = setInterval(() => {
        task().catch((err) => console.error('[scheduler] erreur pendant le rafraîchissement périodique:', err.message));
    }, intervalMs);
}

function stop() {
    if (timer) clearInterval(timer);
    timer = null;
}

module.exports = { start, stop };
