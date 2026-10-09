import {Segmenter} from './source/@formatjs/intl-segmenter/index.js';

// Preserve a future native implementation. This is a browser-wide ECMA-402
// implementation, not a site-specific compatibility hook.
if (typeof Intl.Segmenter !== 'function') {
    Object.defineProperty(Intl, 'Segmenter', {
        value: Segmenter, writable: true, configurable: true,
    });
}
