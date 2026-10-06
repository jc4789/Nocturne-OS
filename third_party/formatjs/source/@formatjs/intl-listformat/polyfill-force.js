import { LookupSupportedLocales, ResolveLocale } from "@formatjs/intl-localematcher";
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.supported-locales@0.0.0/node_modules/@formatjs_generated/cldr.supported-locales/default-content.js
const defaultContent = {
	"aa": ["aa-ET"],
	"ab": ["ab-GE"],
	"af": ["af-ZA"],
	"agq": ["agq-CM"],
	"ak": ["ak-GH"],
	"am": ["am-ET"],
	"an": ["an-ES"],
	"ann": ["ann-NG"],
	"apc": ["apc-SY"],
	"ar": ["ar-001"],
	"arn": ["arn-CL"],
	"as": ["as-IN"],
	"asa": ["asa-TZ"],
	"ast": ["ast-ES"],
	"az-Arab": ["az-Arab-IR"],
	"az-Cyrl": ["az-Cyrl-AZ"],
	"az-Latn": ["az-Latn-AZ"],
	"ba": ["ba-RU"],
	"bal-Arab": ["bal-Arab-PK"],
	"bal-Latn": ["bal-Latn-PK"],
	"bas": ["bas-CM"],
	"be": ["be-BY"],
	"bem": ["bem-ZM"],
	"bew": ["bew-ID"],
	"bez": ["bez-TZ"],
	"bg": ["bg-BG"],
	"bgc": ["bgc-IN"],
	"bgn": ["bgn-PK"],
	"bho": ["bho-IN"],
	"blo": ["blo-BJ"],
	"blt": ["blt-VN"],
	"bm": ["bm-ML"],
	"bm-Nkoo": ["bm-Nkoo-ML"],
	"bn": ["bn-BD"],
	"bo": ["bo-CN"],
	"bqi": ["bqi-IR"],
	"br": ["br-FR"],
	"brx": ["brx-IN"],
	"bs-Cyrl": ["bs-Cyrl-BA"],
	"bs-Latn": ["bs-Latn-BA"],
	"bss": ["bss-CM"],
	"bua": ["bua-RU"],
	"byn": ["byn-ER"],
	"ca": ["ca-ES"],
	"cad": ["cad-US"],
	"cch": ["cch-NG"],
	"ccp": ["ccp-BD"],
	"ce": ["ce-RU"],
	"ceb": ["ceb-PH"],
	"cgg": ["cgg-UG"],
	"cho": ["cho-US"],
	"chr": ["chr-US"],
	"cic": ["cic-US"],
	"ckb": ["ckb-IQ"],
	"co": ["co-FR"],
	"cop": ["cop-EG"],
	"cs": ["cs-CZ"],
	"csw": ["csw-CA"],
	"cu": ["cu-RU"],
	"cv": ["cv-RU"],
	"cy": ["cy-GB"],
	"da": ["da-DK"],
	"dav": ["dav-KE"],
	"de": ["de-DE"],
	"dje": ["dje-NE"],
	"doi": ["doi-IN"],
	"dsb": ["dsb-DE"],
	"dua": ["dua-CM"],
	"dv": ["dv-MV"],
	"dyo": ["dyo-SN"],
	"dz": ["dz-BT"],
	"ebu": ["ebu-KE"],
	"ee": ["ee-GH"],
	"el": ["el-GR"],
	"en-Dsrt": ["en-Dsrt-US"],
	"en-Shaw": ["en-Shaw-GB"],
	"en": ["en-US"],
	"eo": ["eo-001"],
	"es": ["es-ES"],
	"et": ["et-EE"],
	"eu": ["eu-ES"],
	"ewo": ["ewo-CM"],
	"fa": ["fa-IR"],
	"ff-Adlm": ["ff-Adlm-GN"],
	"ff-Latn": ["ff-Latn-SN"],
	"fi": ["fi-FI"],
	"fil": ["fil-PH"],
	"fo": ["fo-FO"],
	"fr": ["fr-FR"],
	"frr": ["frr-DE"],
	"fur": ["fur-IT"],
	"fy": ["fy-NL"],
	"ga": ["ga-IE"],
	"gaa": ["gaa-GH"],
	"gd": ["gd-GB"],
	"gez": ["gez-ET"],
	"gl": ["gl-ES"],
	"gn": ["gn-PY"],
	"gsw": ["gsw-CH"],
	"gu": ["gu-IN"],
	"guz": ["guz-KE"],
	"gv": ["gv-IM"],
	"ha-Arab": ["ha-Arab-NG"],
	"ha": ["ha-NG"],
	"haw": ["haw-US"],
	"he": ["he-IL"],
	"hi": ["hi-IN"],
	"hi-Latn": ["hi-Latn-IN"],
	"hnj-Hmnp": ["hnj-Hmnp-US"],
	"hr": ["hr-HR"],
	"hsb": ["hsb-DE"],
	"ht": ["ht-HT"],
	"hu": ["hu-HU"],
	"hy": ["hy-AM"],
	"ia": ["ia-001"],
	"id": ["id-ID"],
	"ie": ["ie-EE"],
	"ig": ["ig-NG"],
	"ii": ["ii-CN"],
	"io": ["io-001"],
	"is": ["is-IS"],
	"it": ["it-IT"],
	"iu": ["iu-CA"],
	"iu-Latn": ["iu-Latn-CA"],
	"ja": ["ja-JP"],
	"jbo": ["jbo-001"],
	"jgo": ["jgo-CM"],
	"jmc": ["jmc-TZ"],
	"jv": ["jv-ID"],
	"ka": ["ka-GE"],
	"kaa-Cyrl": ["kaa-Cyrl-UZ"],
	"kaa-Latn": ["kaa-Latn-UZ"],
	"kab": ["kab-DZ"],
	"kaj": ["kaj-NG"],
	"kam": ["kam-KE"],
	"kcg": ["kcg-NG"],
	"kde": ["kde-TZ"],
	"kea": ["kea-CV"],
	"kek": ["kek-GT"],
	"ken": ["ken-CM"],
	"kgp": ["kgp-BR"],
	"khq": ["khq-ML"],
	"ki": ["ki-KE"],
	"kk-Arab": ["kk-Arab-CN"],
	"kk-Cyrl": ["kk-Cyrl-KZ"],
	"kkj": ["kkj-CM"],
	"kl": ["kl-GL"],
	"kln": ["kln-KE"],
	"km": ["km-KH"],
	"kn": ["kn-IN"],
	"ko": ["ko-KR"],
	"kok-Deva": ["kok-Deva-IN"],
	"kok-Latn": ["kok-Latn-IN"],
	"kpe": ["kpe-LR"],
	"ks-Arab": ["ks-Arab-IN"],
	"ks-Deva": ["ks-Deva-IN"],
	"ksb": ["ksb-TZ"],
	"ksf": ["ksf-CM"],
	"ksh": ["ksh-DE"],
	"ku-Arab": ["ku-Arab-IQ"],
	"ku-Latn": ["ku-Latn-TR"],
	"kw": ["kw-GB"],
	"kxv-Deva": ["kxv-Deva-IN"],
	"kxv-Latn": ["kxv-Latn-IN"],
	"kxv-Orya": ["kxv-Orya-IN"],
	"kxv-Telu": ["kxv-Telu-IN"],
	"ky": ["ky-KG"],
	"la": ["la-VA"],
	"lag": ["lag-TZ"],
	"lb": ["lb-LU"],
	"lg": ["lg-UG"],
	"lij": ["lij-IT"],
	"lkt": ["lkt-US"],
	"lld": ["lld-IT"],
	"lmo": ["lmo-IT"],
	"ln": ["ln-CD"],
	"lo": ["lo-LA"],
	"lrc": ["lrc-IR"],
	"lt": ["lt-LT"],
	"ltg": ["ltg-LV"],
	"lu": ["lu-CD"],
	"luo": ["luo-KE"],
	"luy": ["luy-KE"],
	"lv": ["lv-LV"],
	"lzz": ["lzz-TR"],
	"mai": ["mai-IN"],
	"mas": ["mas-KE"],
	"mdf": ["mdf-RU"],
	"mer": ["mer-KE"],
	"mfe": ["mfe-MU"],
	"mg": ["mg-MG"],
	"mgh": ["mgh-MZ"],
	"mgo": ["mgo-CM"],
	"mhn": ["mhn-IT"],
	"mi": ["mi-NZ"],
	"mic": ["mic-CA"],
	"mk": ["mk-MK"],
	"ml": ["ml-IN"],
	"mn": ["mn-MN"],
	"mn-Mong": ["mn-Mong-CN"],
	"mni-Beng": ["mni-Beng-IN"],
	"mni-Mtei": ["mni-Mtei-IN"],
	"moh": ["moh-CA"],
	"mr": ["mr-IN"],
	"ms-Arab": ["ms-Arab-MY"],
	"ms": ["ms-MY"],
	"mt": ["mt-MT"],
	"mua": ["mua-CM"],
	"mus": ["mus-US"],
	"mww-Hmnp": ["mww-Hmnp-US"],
	"my": ["my-MM"],
	"myv": ["myv-RU"],
	"mzn": ["mzn-IR"],
	"naq": ["naq-NA"],
	"nb": ["nb-NO"],
	"nd": ["nd-ZW"],
	"nds": ["nds-DE"],
	"ne": ["ne-NP"],
	"nl": ["nl-NL"],
	"nmg": ["nmg-CM"],
	"nn": ["nn-NO"],
	"nnh": ["nnh-CM"],
	"nqo": ["nqo-GN"],
	"nr": ["nr-ZA"],
	"nso": ["nso-ZA"],
	"nus": ["nus-SS"],
	"nv": ["nv-US"],
	"ny": ["ny-MW"],
	"nyn": ["nyn-UG"],
	"oc": ["oc-FR"],
	"oka": ["oka-CA"],
	"om": ["om-ET"],
	"or": ["or-IN"],
	"os": ["os-GE"],
	"osa": ["osa-US"],
	"pa-Arab": ["pa-Arab-PK"],
	"pa-Guru": ["pa-Guru-IN"],
	"pap": ["pap-CW"],
	"pcm": ["pcm-NG"],
	"pi-Latn": ["pi-Latn-GB"],
	"pis": ["pis-SB"],
	"pl": ["pl-PL"],
	"pms": ["pms-IT"],
	"prg": ["prg-PL"],
	"ps": ["ps-AF"],
	"pt": ["pt-BR"],
	"qu": ["qu-PE"],
	"quc": ["quc-GT"],
	"raj": ["raj-IN"],
	"rhg-Rohg": ["rhg-Rohg-MM"],
	"rif": ["rif-MA"],
	"rm": ["rm-CH"],
	"rn": ["rn-BI"],
	"ro": ["ro-RO"],
	"rof": ["rof-TZ"],
	"ru": ["ru-RU"],
	"rw": ["rw-RW"],
	"rwk": ["rwk-TZ"],
	"sa": ["sa-IN"],
	"sah": ["sah-RU"],
	"saq": ["saq-KE"],
	"sat-Deva": ["sat-Deva-IN"],
	"sat-Olck": ["sat-Olck-IN"],
	"sbp": ["sbp-TZ"],
	"sc": ["sc-IT"],
	"scn": ["scn-IT"],
	"sd-Arab": ["sd-Arab-PK"],
	"sd-Deva": ["sd-Deva-IN"],
	"sdh": ["sdh-IR"],
	"se": ["se-NO"],
	"seh": ["seh-MZ"],
	"ses": ["ses-ML"],
	"sg": ["sg-CF"],
	"sgs": ["sgs-LT"],
	"shi-Latn": ["shi-Latn-MA"],
	"shi-Tfng": ["shi-Tfng-MA"],
	"shn": ["shn-MM"],
	"si": ["si-LK"],
	"sid": ["sid-ET"],
	"sk": ["sk-SK"],
	"skr": ["skr-PK"],
	"sl": ["sl-SI"],
	"sma": ["sma-SE"],
	"smj": ["smj-SE"],
	"smn": ["smn-FI"],
	"sms": ["sms-FI"],
	"sn": ["sn-ZW"],
	"so": ["so-SO"],
	"sq": ["sq-AL"],
	"sr-Cyrl": ["sr-Cyrl-RS"],
	"sr-Latn": ["sr-Latn-RS"],
	"ss": ["ss-ZA"],
	"ssy": ["ssy-ER"],
	"st": ["st-ZA"],
	"su-Latn": ["su-Latn-ID"],
	"suz-Deva": ["suz-Deva-NP"],
	"suz-Sunu": ["suz-Sunu-NP"],
	"sv": ["sv-SE"],
	"sw": ["sw-TZ"],
	"syr": ["syr-IQ"],
	"szl": ["szl-PL"],
	"ta": ["ta-IN"],
	"te": ["te-IN"],
	"teo": ["teo-UG"],
	"tg": ["tg-TJ"],
	"th": ["th-TH"],
	"ti": ["ti-ET"],
	"tig": ["tig-ER"],
	"tk": ["tk-TM"],
	"tn": ["tn-ZA"],
	"to": ["to-TO"],
	"tok": ["tok-001"],
	"tpi": ["tpi-PG"],
	"tr": ["tr-TR"],
	"trv": ["trv-TW"],
	"trw": ["trw-PK"],
	"ts": ["ts-ZA"],
	"tt": ["tt-RU"],
	"twq": ["twq-NE"],
	"tyv": ["tyv-RU"],
	"tzm": ["tzm-MA"],
	"ug": ["ug-CN"],
	"uk": ["uk-UA"],
	"ur": ["ur-PK"],
	"uz-Arab": ["uz-Arab-AF"],
	"uz-Cyrl": ["uz-Cyrl-UZ"],
	"uz-Latn": ["uz-Latn-UZ"],
	"vai-Latn": ["vai-Latn-LR"],
	"vai-Vaii": ["vai-Vaii-LR"],
	"ve": ["ve-ZA"],
	"vec": ["vec-IT"],
	"vi": ["vi-VN"],
	"vmw": ["vmw-MZ"],
	"vo": ["vo-001"],
	"vun": ["vun-TZ"],
	"wa": ["wa-BE"],
	"wae": ["wae-CH"],
	"wal": ["wal-ET"],
	"wbp": ["wbp-AU"],
	"wo": ["wo-SN"],
	"xh": ["xh-ZA"],
	"xnr": ["xnr-IN"],
	"xog": ["xog-UG"],
	"yav": ["yav-CM"],
	"yi": ["yi-UA"],
	"yo": ["yo-NG"],
	"yrl": ["yrl-BR"],
	"yue-Hans": ["yue-Hans-CN"],
	"yue-Hant": ["yue-Hant-HK"],
	"za": ["za-CN"],
	"zgh": ["zgh-MA"],
	"zh-Hans": ["zh-Hans-CN"],
	"zh-Hant": ["zh-Hant-TW"],
	"zh-Latn": ["zh-Latn-CN"],
	"zu": ["zu-ZA"]
};
//#endregion
//#region packages/ecma402-abstract/registerLocaleData.js
const explicitLocales = /* @__PURE__ */ new WeakMap();
function registerLocaleData(locale, data, localeData, availableLocales) {
	let loaded = explicitLocales.get(localeData);
	if (!loaded) {
		loaded = /* @__PURE__ */ new Set();
		explicitLocales.set(localeData, loaded);
	}
	loaded.add(locale);
	const locales = [locale, ...defaultContent[locale] || []];
	for (const tag of locales) {
		if (tag === locale || !loaded.has(tag) || localeData[tag] === void 0) localeData[tag] = data;
		availableLocales.add(tag);
	}
	for (const tag of locales) {
		const parts = tag.split("-");
		const fallbacks = [];
		while (parts.length > 1) {
			if (parts.length > 2 && parts[1].length === 4) fallbacks.push([parts[0], ...parts.slice(2)].join("-"));
			parts.pop();
			fallbacks.push(parts.join("-"));
		}
		for (const fallback of fallbacks) {
			if (localeData[fallback] === void 0) localeData[fallback] = localeData[tag];
			availableLocales.add(fallback);
		}
	}
}
//#endregion
//#region packages/ecma402-abstract/CanonicalizeLocaleList.js
/**
* http://ecma-international.org/ecma-402/7.0/index.html#sec-canonicalizelocalelist
* @param locales
*/
function CanonicalizeLocaleList(locales) {
	return Intl.getCanonicalLocales(locales);
}
//#endregion
//#region packages/ecma262-abstract/ToString.js
/**
* https://tc39.es/ecma262/#sec-tostring
*/
function ToString(o) {
	if (typeof o === "symbol") throw TypeError("Cannot convert a Symbol value to a string");
	return String(o);
}
//#endregion
//#region packages/ecma402-abstract/GetOption.js
/**
* https://tc39.es/ecma402/#sec-getoption
* @param opts
* @param prop
* @param type
* @param values
* @param fallback
*/
function GetOption(opts, prop, type, values, fallback) {
	if (opts === null || typeof opts !== "object" && typeof opts !== "function") throw new TypeError("Options must be an object");
	let value = opts[prop];
	if (value !== void 0) {
		if (type !== "boolean" && type !== "string") throw new TypeError("invalid type");
		if (type === "boolean") value = Boolean(value);
		if (type === "string") value = ToString(value);
		if (values !== void 0 && !values.filter((val) => val == value).length) throw new RangeError(`${value} is not within ${values.join(", ")}`);
		return value;
	}
	return fallback;
}
//#endregion
//#region packages/ecma402-abstract/GetOptionsObject.js
/**
* ECMA-262 §7.3.36 GetOptionsObject, steps 2–3.
* https://tc39.es/ecma262/#sec-getoptionsobject
* https://github.com/tc39/ecma262/blob/dcf59856a8184792a9e42f0ffb7dc064094a5dcc/spec.html#L7008-L7010
* @param options
* @returns
*/
function GetOptionsObject(options) {
	if (typeof options === "undefined") return Object.create(null);
	if (options !== null && (typeof options === "object" || typeof options === "function")) return options;
	throw new TypeError("Options must be an object");
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs+fast-memoize@0.0.0/node_modules/@formatjs/fast-memoize/index.js
function memoize(fn, options) {
	const cache = options && options.cache ? options.cache : cacheDefault;
	const serializer = options && options.serializer ? options.serializer : serializerDefault;
	return (options && options.strategy ? options.strategy : strategyDefault)(fn, {
		cache,
		serializer
	});
}
function isPrimitive(value) {
	return value == null || typeof value === "number" || typeof value === "boolean";
}
function monadic(fn, cache, serializer, arg) {
	const cacheKey = isPrimitive(arg) ? arg : serializer(arg);
	let computedValue = cache.get(cacheKey);
	if (typeof computedValue === "undefined") {
		computedValue = fn.call(this, arg);
		cache.set(cacheKey, computedValue);
	}
	return computedValue;
}
function variadic(fn, cache, serializer) {
	const args = Array.prototype.slice.call(arguments, 3);
	const cacheKey = serializer(args);
	let computedValue = cache.get(cacheKey);
	if (typeof computedValue === "undefined") {
		computedValue = fn.apply(this, args);
		cache.set(cacheKey, computedValue);
	}
	return computedValue;
}
function assemble(fn, context, strategy, cache, serialize) {
	return strategy.bind(context, fn, cache, serialize);
}
function strategyDefault(fn, options) {
	const strategy = fn.length === 1 ? monadic : variadic;
	return assemble(fn, this, strategy, options.cache.create(), options.serializer);
}
function strategyVariadic(fn, options) {
	return assemble(fn, this, variadic, options.cache.create(), options.serializer);
}
function strategyMonadic(fn, options) {
	return assemble(fn, this, monadic, options.cache.create(), options.serializer);
}
const serializerDefault = function() {
	return JSON.stringify(arguments);
};
var ObjectWithoutPrototypeCache = class {
	constructor() {
		this.cache = Object.create(null);
	}
	get(key) {
		return this.cache[key];
	}
	set(key, value) {
		this.cache[key] = value;
	}
};
const cacheDefault = { create: function create() {
	return new ObjectWithoutPrototypeCache();
} };
const strategies = {
	variadic: strategyVariadic,
	monadic: strategyMonadic
};
//#endregion
//#region packages/ecma402-abstract/utils.js
function setInternalSlot(map, pl, field, value) {
	if (!map.get(pl)) map.set(pl, Object.create(null));
	const slots = map.get(pl);
	slots[field] = value;
}
function getInternalSlot(map, pl, field) {
	return getMultiInternalSlots(map, pl, field)[field];
}
function getMultiInternalSlots(map, pl, ...fields) {
	const slots = map.get(pl);
	if (!slots) throw new TypeError(`${pl} InternalSlot has not been initialized`);
	return fields.reduce((all, f) => {
		all[f] = slots[f];
		return all;
	}, Object.create(null));
}
function isLiteralPart(patternPart) {
	return patternPart.type === "literal";
}
function defineProperty(target, name, { value }) {
	Object.defineProperty(target, name, {
		configurable: true,
		enumerable: false,
		writable: true,
		value
	});
}
function ensureIntl() {
	if (typeof Intl === "undefined") Object.defineProperty(globalThis, "Intl", {
		configurable: true,
		enumerable: false,
		writable: true,
		value: {}
	});
	return Intl;
}
function invariant(condition, message, Err = Error) {
	if (!condition) throw new Err(message);
}
memoize((...args) => new Intl.NumberFormat(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.PluralRules(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.ListFormat(...args), { strategy: strategies.variadic });
//#endregion
//#region packages/ecma402-abstract/PartitionPattern.js
/**
* Partition a pattern into a list of literals and placeholders
* https://tc39.es/ecma402/#sec-partitionpattern
* @param pattern
*/
function PartitionPattern(pattern) {
	const result = [];
	let beginIndex = pattern.indexOf("{");
	let endIndex = 0;
	let nextIndex = 0;
	const length = pattern.length;
	while (beginIndex < pattern.length && beginIndex > -1) {
		endIndex = pattern.indexOf("}", beginIndex);
		invariant(endIndex > beginIndex, `Invalid pattern ${pattern}`);
		if (beginIndex > nextIndex) result.push({
			type: "literal",
			value: pattern.substring(nextIndex, beginIndex)
		});
		result.push({
			type: pattern.substring(beginIndex + 1, endIndex),
			value: void 0
		});
		nextIndex = endIndex + 1;
		beginIndex = pattern.indexOf("{", nextIndex);
	}
	if (nextIndex < length) result.push({
		type: "literal",
		value: pattern.substring(nextIndex, length)
	});
	return result;
}
//#endregion
//#region packages/ecma262-abstract/ToObject.js
/**
* https://tc39.es/ecma262/#sec-toobject
*/
function ToObject(arg) {
	if (arg == null) throw new TypeError("undefined/null cannot be converted to object");
	return Object(arg);
}
//#endregion
//#region packages/ecma402-abstract/SupportedLocales.js
/**
* https://tc39.es/ecma402/#sec-supportedlocales
* @param availableLocales
* @param requestedLocales
* @param options
*/
function SupportedLocales(availableLocales, requestedLocales, options) {
	let matcher = "best fit";
	if (options !== void 0) {
		options = ToObject(options);
		matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
	}
	if (matcher === "best fit") return LookupSupportedLocales(Array.from(availableLocales), requestedLocales);
	return LookupSupportedLocales(Array.from(availableLocales), requestedLocales);
}
//#endregion
//#region packages/intl-listformat/index.ts
function validateInstance(instance, method) {
	if (!(instance instanceof ListFormat)) throw new TypeError(`Method Intl.ListFormat.prototype.${method} called on incompatible receiver ${String(instance)}`);
}
/**
* https://tc39.es/proposal-intl-list-format/#sec-createstringlistfromiterable
* @param iterable list
*/
function stringListFromIterable(iterable) {
	if (iterable === void 0) return [];
	const elements = [];
	for (const value of iterable) {
		if (typeof value !== "string") throw new TypeError("Iterable yielded a non-string value");
		elements.push(value);
	}
	return elements;
}
function createPartsFromList(internalSlotMap, lf, list) {
	const size = list.length;
	if (size === 0) return [];
	if (size === 2) return deconstructPattern(getInternalSlot(internalSlotMap, lf, "templatePair"), {
		"0": {
			type: "element",
			value: list[0]
		},
		"1": {
			type: "element",
			value: list[1]
		}
	});
	let parts = {
		type: "element",
		value: list[size - 1]
	};
	let i = size - 2;
	while (i >= 0) {
		let pattern;
		if (i === 0) pattern = getInternalSlot(internalSlotMap, lf, "templateStart");
		else if (i < size - 2) pattern = getInternalSlot(internalSlotMap, lf, "templateMiddle");
		else pattern = getInternalSlot(internalSlotMap, lf, "templateEnd");
		const head = {
			type: "element",
			value: list[i]
		};
		parts = deconstructPattern(pattern, {
			"0": head,
			"1": parts
		});
		i--;
	}
	return parts;
}
function deconstructPattern(pattern, placeables) {
	const patternParts = PartitionPattern(pattern);
	const result = [];
	for (const patternPart of patternParts) {
		const { type: part } = patternPart;
		if (isLiteralPart(patternPart)) result.push({
			type: "literal",
			value: patternPart.value
		});
		else {
			invariant(part in placeables, `${part} is missing from placables`);
			const subst = placeables[part];
			if (Array.isArray(subst)) result.push(...subst);
			else result.push(subst);
		}
	}
	return result;
}
var ListFormat = class ListFormat {
	constructor(locales, options) {
		if (!(this && this instanceof ListFormat ? this.constructor : void 0)) throw new TypeError("Intl.ListFormat must be called with 'new'");
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "initializedListFormat", true);
		const requestedLocales = CanonicalizeLocaleList(locales);
		const opt = Object.create(null);
		const opts = GetOptionsObject(options);
		opt.localeMatcher = GetOption(opts, "localeMatcher", "string", ["best fit", "lookup"], "best fit");
		const { localeData } = ListFormat;
		const r = ResolveLocale(ListFormat.availableLocales, requestedLocales, opt, ListFormat.relevantExtensionKeys, localeData, ListFormat.getDefaultLocale);
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "locale", r.locale);
		const type = GetOption(opts, "type", "string", [
			"conjunction",
			"disjunction",
			"unit"
		], "conjunction");
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "type", type);
		const style = GetOption(opts, "style", "string", [
			"long",
			"short",
			"narrow"
		], "long");
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "style", style);
		const { dataLocale } = r;
		const dataLocaleData = localeData[dataLocale];
		invariant(!!dataLocaleData, `Missing locale data for ${dataLocale}`);
		const templates = dataLocaleData[type][style];
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "templatePair", templates.pair);
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "templateStart", templates.start);
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "templateMiddle", templates.middle);
		setInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "templateEnd", templates.end);
	}
	format(elements) {
		validateInstance(this, "format");
		let result = "";
		const parts = createPartsFromList(ListFormat.__INTERNAL_SLOT_MAP__, this, stringListFromIterable(elements));
		if (!Array.isArray(parts)) return parts.value;
		for (const p of parts) result += p.value;
		return result;
	}
	formatToParts(elements) {
		validateInstance(this, "format");
		const parts = createPartsFromList(ListFormat.__INTERNAL_SLOT_MAP__, this, stringListFromIterable(elements));
		if (!Array.isArray(parts)) return [parts];
		const result = [];
		for (const part of parts) result.push({ ...part });
		return result;
	}
	resolvedOptions() {
		validateInstance(this, "resolvedOptions");
		return {
			locale: getInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "locale"),
			type: getInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "type"),
			style: getInternalSlot(ListFormat.__INTERNAL_SLOT_MAP__, this, "style")
		};
	}
	static supportedLocalesOf(locales, options) {
		return SupportedLocales(ListFormat.availableLocales, CanonicalizeLocaleList(locales), options);
	}
	static __addLocaleData(...data) {
		for (const { data: d, locale } of data) {
			registerLocaleData(locale, d, ListFormat.localeData, ListFormat.availableLocales);
			if (!ListFormat.__defaultLocale) ListFormat.__defaultLocale = locale;
		}
	}
	static {
		this.localeData = {};
	}
	static {
		this.availableLocales = /* @__PURE__ */ new Set();
	}
	static {
		this.__defaultLocale = "";
	}
	static getDefaultLocale() {
		return ListFormat.__defaultLocale;
	}
	static {
		this.relevantExtensionKeys = [];
	}
	static {
		this.polyfilled = true;
	}
	static {
		this.__INTERNAL_SLOT_MAP__ = /* @__PURE__ */ new WeakMap();
	}
};
try {
	if (typeof Symbol !== "undefined") Object.defineProperty(ListFormat.prototype, Symbol.toStringTag, {
		value: "Intl.ListFormat",
		writable: false,
		enumerable: false,
		configurable: true
	});
	Object.defineProperty(ListFormat.prototype.constructor, "length", {
		value: 0,
		writable: false,
		enumerable: false,
		configurable: true
	});
	Object.defineProperty(ListFormat.supportedLocalesOf, "length", {
		value: 1,
		writable: false,
		enumerable: false,
		configurable: true
	});
} catch {}
defineProperty(ensureIntl(), "ListFormat", { value: ListFormat });
const buf = globalThis.__FORMATJS_LISTFORMAT_DATA__;
if (buf) {
	for (const d of buf) ListFormat.__addLocaleData(d);
	delete globalThis.__FORMATJS_LISTFORMAT_DATA__;
}
//#endregion

//# sourceMappingURL=polyfill-force.js.map