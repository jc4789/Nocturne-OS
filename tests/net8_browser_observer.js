/* Read-only real-site observer. No DOM/style/function replacement or input. */
(() => {
    let previous = '', hover = '';
    const identity = node => node ? node.tagName + '#' + (node.id || '') +
        (node.tagName === 'A' ? ' ' + node.href : '') : 'none';
    document.addEventListener('mousemove', event => {
        if (!event.isTrusted) return;
        const value = identity(event.target);
        if (value !== hover) {
            hover = value;
            console.log('NET8_HOVER', event.clientX, event.clientY, value);
        }
    });
    document.addEventListener('click', event => {
        if (event.isTrusted) console.log('NET8_CLICK', event.clientX, event.clientY, identity(event.target));
    });
    document.addEventListener('input', event => {
        if (event.isTrusted) console.log('NET8_INPUT', identity(event.target), event.target.value);
    });
    setInterval(() => {
        const field = document.querySelector('input[name="q"]');
        const focus = identity(document.activeElement);
        const state = location.href + ' scroll=' + scrollX + ',' + scrollY + ' focus=' + focus;
        if (state === previous) return;
        previous = state;
        console.log('NET8_STATE', state);
        if (field) {
            const r = field.getBoundingClientRect();
            console.log('NET8_SEARCH_RECT', Math.round(r.x + r.width / 2),
                        Math.round(r.y + r.height / 2), Math.round(r.width), Math.round(r.height));
        }
    }, 500);
    console.log('NET8_OBSERVER_READY', location.href);
})();
