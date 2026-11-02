/* ECMA-402 Intl, Intl.Segmenter, Intl.Collator and the locale-sensitive methods
 * they replace, with their CLDR and ICU tables: a program of its own, compiled
 * apart from the bindings. js_bootstrap.js calls this function with its host
 * once, when the realm first reads Intl or calls one of those methods. */
(function (host) {
    'use strict';
    /* @include js_intl.js */
    /* @include js_segmenter.js */
    /* @include js_collator.js */
    /* ECMA-402 DefaultLocale: the user's language (settings' N_LANGUAGE_UI). */
    for (const service of [Intl.NumberFormat, Intl.DateTimeFormat, Intl.PluralRules, Intl.RelativeTimeFormat,
                           Intl.ListFormat, Intl.DisplayNames, Intl.Segmenter, Intl.Collator])
        if (service && Object.hasOwn(service, '__defaultLocale')) service.__defaultLocale = host.intlLocale;
})
