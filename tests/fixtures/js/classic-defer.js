globalThis.deferRan = true;
fixtureOrder.push('defer');
record('external defer sees completed body', document.getElementById('future') !== null);
