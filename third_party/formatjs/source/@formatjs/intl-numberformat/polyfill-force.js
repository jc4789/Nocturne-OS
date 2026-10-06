import { Decimal } from "@formatjs/bigdecimal";
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
//#region packages/ecma262-abstract/OrdinaryHasInstance.js
function IsCallable$1(fn) {
	return typeof fn === "function";
}
/**
* https://tc39.es/ecma262/#sec-ordinaryhasinstance
*/
function OrdinaryHasInstance(C, O, internalSlots) {
	if (!IsCallable$1(C)) return false;
	if (internalSlots?.boundTargetFunction) return O instanceof internalSlots?.boundTargetFunction;
	if (typeof O !== "object") return false;
	let P = C.prototype;
	if (typeof P !== "object") throw new TypeError("OrdinaryHasInstance called on an object with an invalid prototype property.");
	return Object.prototype.isPrototypeOf.call(P, O);
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
function repeat(s, times) {
	if (typeof s.repeat === "function") return s.repeat(times);
	const arr = Array.from({ length: times });
	for (let i = 0; i < arr.length; i++) arr[i] = s;
	return arr.join("");
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
function invariant$1(condition, message, Err = Error) {
	if (!condition) throw new Err(message);
}
memoize((...args) => new Intl.NumberFormat(...args), { strategy: strategies.variadic });
const createMemoizedPluralRules = memoize((...args) => new Intl.PluralRules(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.ListFormat(...args), { strategy: strategies.variadic });
//#endregion
//#region packages/ecma402-abstract/NumberFormat/decimal-cache.js
/**
* Cached function to compute powers of 10 for Decimal.js operations.
* This cache significantly reduces overhead in ComputeExponent and ToRawFixed
* by memoizing expensive Decimal.pow(10, n) calculations.
*
* Common exponents (e.g., -20 to 20) are used repeatedly in number formatting,
* so caching provides substantial performance benefits.
*
* @param exponent - Can be a number or Decimal. If Decimal, it will be converted to string for cache key.
*/
const getPowerOf10 = memoize((exponent) => {
	return Decimal.pow(10, exponent);
});
//#endregion
//#region packages/ecma402-abstract/NumberFormat/ComputeExponentForMagnitude.js
/**
* The abstract operation ComputeExponentForMagnitude computes an exponent by which to scale a
* number of the given magnitude (power of ten of the most significant digit) according to the
* locale and the desired notation (scientific, engineering, or compact).
*/
function ComputeExponentForMagnitude(internalSlots, magnitude) {
	const { notation, dataLocaleData, numberingSystem } = internalSlots;
	switch (notation) {
		case "standard": return 0;
		case "scientific": return magnitude.toNumber();
		case "engineering": return magnitude.div(3).floor().times(3).toNumber();
		default: {
			invariant$1(notation === "compact", "Invalid notation");
			const { compactDisplay, style, currencyDisplay } = internalSlots;
			let thresholdMap;
			if (style === "currency" && currencyDisplay !== "name") thresholdMap = (dataLocaleData.numbers.currency[numberingSystem] || dataLocaleData.numbers.currency[dataLocaleData.numbers.nu[0]]).short;
			else {
				const decimal = dataLocaleData.numbers.decimal[numberingSystem] || dataLocaleData.numbers.decimal[dataLocaleData.numbers.nu[0]];
				thresholdMap = compactDisplay === "long" ? decimal.long : decimal.short;
			}
			if (!thresholdMap) return 0;
			const num = getPowerOf10(magnitude).toString();
			const thresholds = Object.keys(thresholdMap);
			if (num < thresholds[0]) return 0;
			if (num > thresholds[thresholds.length - 1]) {
				const magnitudeKey = thresholds[thresholds.length - 1];
				if (thresholdMap[magnitudeKey].other === "0") return 0;
				return magnitudeKey.length - thresholdMap[magnitudeKey].other.match(/0+/)[0].length;
			}
			const i = thresholds.indexOf(num);
			if (i === -1) return 0;
			const magnitudeKey = thresholds[i];
			if (thresholdMap[magnitudeKey].other === "0") return 0;
			return magnitudeKey.length - thresholdMap[magnitudeKey].other.match(/0+/)[0].length;
		}
	}
}
//#endregion
//#region packages/ecma402-abstract/constants.js
const ZERO = new Decimal(0);
const NEGATIVE_ZERO = new Decimal(-0);
//#endregion
//#region packages/ecma402-abstract/NumberFormat/GetUnsignedRoundingMode.js
const negativeMapping = {
	ceil: "zero",
	floor: "infinity",
	expand: "infinity",
	trunc: "zero",
	halfCeil: "half-zero",
	halfFloor: "half-infinity",
	halfExpand: "half-infinity",
	halfTrunc: "half-zero",
	halfEven: "half-even"
};
const positiveMapping = {
	ceil: "infinity",
	floor: "zero",
	expand: "infinity",
	trunc: "zero",
	halfCeil: "half-infinity",
	halfFloor: "half-zero",
	halfExpand: "half-infinity",
	halfTrunc: "half-zero",
	halfEven: "half-even"
};
function GetUnsignedRoundingMode(roundingMode, isNegative) {
	if (isNegative) return negativeMapping[roundingMode];
	return positiveMapping[roundingMode];
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/ApplyUnsignedRoundingMode.js
function ApplyUnsignedRoundingMode(x, r1, r2, unsignedRoundingMode) {
	if (x.eq(r1) || r1.eq(r2)) return r1;
	if (x.eq(r2)) return r2;
	invariant$1(r1.lessThan(x) && x.lessThan(r2), `x should be between r1 and r2 but x=${x}, r1=${r1}, r2=${r2}`);
	if (unsignedRoundingMode === "zero") return r1;
	if (unsignedRoundingMode === "infinity") return r2;
	const d1 = x.minus(r1);
	const d2 = r2.minus(x);
	if (d1.lessThan(d2)) return r1;
	if (d2.lessThan(d1)) return r2;
	invariant$1(d1.eq(d2), "d1 should be equal to d2");
	if (unsignedRoundingMode === "half-zero") return r1;
	if (unsignedRoundingMode === "half-infinity") return r2;
	invariant$1(unsignedRoundingMode === "half-even", "unsignedRoundingMode should be half-even");
	if (r1.div(r2.minus(r1)).mod(2).isZero()) return r1;
	return r2;
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/ToRawFixed.js
function ToRawFixedFn(n, f) {
	return n.times(getPowerOf10(-f));
}
function findN1R1(x, f, roundingIncrement) {
	const n1 = x.times(getPowerOf10(f)).floor().div(roundingIncrement).floor().times(roundingIncrement);
	return {
		n1,
		r1: ToRawFixedFn(n1, f)
	};
}
function findN2R2(x, f, roundingIncrement) {
	const n2 = x.times(getPowerOf10(f)).ceil().div(roundingIncrement).ceil().times(roundingIncrement);
	return {
		n2,
		r2: ToRawFixedFn(n2, f)
	};
}
/**
* https://tc39.es/ecma402/#sec-torawfixed
* @param x a finite non-negative Number or BigInt
* @param minFraction an integer between 0 and 20
* @param maxFraction an integer between 0 and 20
*/
function ToRawFixed(x, minFraction, maxFraction, roundingIncrement, unsignedRoundingMode) {
	const f = maxFraction;
	const { n1, r1 } = findN1R1(x, f, roundingIncrement);
	const { n2, r2 } = findN2R2(x, f, roundingIncrement);
	const r = ApplyUnsignedRoundingMode(x, r1, r2, unsignedRoundingMode);
	let n, xFinal;
	let m;
	if (r.eq(r1)) {
		n = n1;
		xFinal = r1;
	} else {
		n = n2;
		xFinal = r2;
	}
	if (n.isZero()) m = "0";
	else m = n.toString();
	let int;
	if (f !== 0) {
		let k = m.length;
		if (k <= f) {
			m = repeat("0", f - k + 1) + m;
			k = f + 1;
		}
		const a = m.slice(0, k - f);
		const b = m.slice(m.length - f);
		m = a + "." + b;
		int = a.length;
	} else int = m.length;
	let cut = maxFraction - minFraction;
	while (cut > 0 && m[m.length - 1] === "0") {
		m = m.slice(0, m.length - 1);
		cut--;
	}
	if (m[m.length - 1] === ".") m = m.slice(0, m.length - 1);
	return {
		formattedString: m,
		roundedNumber: xFinal,
		integerDigitsCount: int,
		roundingMagnitude: -f
	};
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/ToRawPrecision.js
function findN1E1R1(x, p) {
	const maxN1 = getPowerOf10(p);
	const minN1 = getPowerOf10(p - 1);
	let e1 = x.log(10).floor();
	const divisor = getPowerOf10(e1.minus(p).plus(1));
	let n1 = x.div(divisor).floor();
	let r1 = n1.times(divisor);
	if (n1.greaterThanOrEqualTo(maxN1)) {
		e1 = e1.plus(1);
		const newDivisor = getPowerOf10(e1.minus(p).plus(1));
		n1 = x.div(newDivisor).floor();
		r1 = n1.times(newDivisor);
	} else if (n1.lessThan(minN1)) {
		e1 = e1.minus(1);
		const newDivisor = getPowerOf10(e1.minus(p).plus(1));
		n1 = x.div(newDivisor).floor();
		r1 = n1.times(newDivisor);
	}
	if (r1.lessThanOrEqualTo(x) && n1.lessThan(maxN1) && n1.greaterThanOrEqualTo(minN1)) return {
		n1,
		e1,
		r1
	};
	let currentE1 = x.div(minN1).log(10).plus(p).minus(1).ceil();
	while (true) {
		const currentDivisor = getPowerOf10(currentE1.minus(p).plus(1));
		let currentN1 = x.div(currentDivisor).floor();
		if (currentN1.lessThan(maxN1) && currentN1.greaterThanOrEqualTo(minN1)) {
			const currentR1 = currentN1.times(currentDivisor);
			if (currentR1.lessThanOrEqualTo(x)) return {
				n1: currentN1,
				e1: currentE1,
				r1: currentR1
			};
		}
		currentE1 = currentE1.minus(1);
	}
}
function findN2E2R2(x, p) {
	const maxN2 = getPowerOf10(p);
	const minN2 = getPowerOf10(p - 1);
	let e2 = x.log(10).floor();
	const divisor = getPowerOf10(e2.minus(p).plus(1));
	let n2 = x.div(divisor).ceil();
	let r2 = n2.times(divisor);
	if (n2.greaterThanOrEqualTo(maxN2)) {
		e2 = e2.plus(1);
		const newDivisor = getPowerOf10(e2.minus(p).plus(1));
		n2 = x.div(newDivisor).ceil();
		r2 = n2.times(newDivisor);
	} else if (n2.lessThan(minN2)) {
		e2 = e2.minus(1);
		const newDivisor = getPowerOf10(e2.minus(p).plus(1));
		n2 = x.div(newDivisor).ceil();
		r2 = n2.times(newDivisor);
	}
	if (r2.greaterThanOrEqualTo(x) && n2.lessThan(maxN2) && n2.greaterThanOrEqualTo(minN2)) return {
		n2,
		e2,
		r2
	};
	let currentE2 = x.div(maxN2).log(10).plus(p).minus(1).floor();
	while (true) {
		const currentDivisor = getPowerOf10(currentE2.minus(p).plus(1));
		let currentN2 = x.div(currentDivisor).ceil();
		if (currentN2.lessThan(maxN2) && currentN2.greaterThanOrEqualTo(minN2)) {
			const currentR2 = currentN2.times(currentDivisor);
			if (currentR2.greaterThanOrEqualTo(x)) return {
				n2: currentN2,
				e2: currentE2,
				r2: currentR2
			};
		}
		currentE2 = currentE2.plus(1);
	}
}
/**
* https://tc39.es/ecma402/#sec-torawprecision
* @param x a finite non-negative Number or BigInt
* @param minPrecision an integer between 1 and 21
* @param maxPrecision an integer between 1 and 21
*/
function ToRawPrecision(x, minPrecision, maxPrecision, unsignedRoundingMode) {
	const p = maxPrecision;
	let m;
	let e;
	let xFinal;
	if (x.isZero()) {
		m = repeat("0", p);
		e = 0;
		xFinal = ZERO;
	} else {
		const { n1, e1, r1 } = findN1E1R1(x, p);
		const { n2, e2, r2 } = findN2E2R2(x, p);
		let r = ApplyUnsignedRoundingMode(x, r1, r2, unsignedRoundingMode);
		let n;
		if (r.eq(r1)) {
			n = n1;
			e = e1.toNumber();
			xFinal = r1;
		} else {
			n = n2;
			e = e2.toNumber();
			xFinal = r2;
		}
		m = n.toString();
	}
	let int;
	if (e >= p - 1) {
		m = m + repeat("0", e - p + 1);
		int = e + 1;
	} else if (e >= 0) {
		m = m.slice(0, e + 1) + "." + m.slice(m.length - (p - (e + 1)));
		int = e + 1;
	} else {
		invariant$1(e < 0, "e should be less than 0");
		m = "0." + repeat("0", -e - 1) + m;
		int = 1;
	}
	if (m.includes(".") && maxPrecision > minPrecision) {
		let cut = maxPrecision - minPrecision;
		while (cut > 0 && m[m.length - 1] === "0") {
			m = m.slice(0, m.length - 1);
			cut--;
		}
		if (m[m.length - 1] === ".") m = m.slice(0, m.length - 1);
	}
	return {
		formattedString: m,
		roundedNumber: xFinal,
		integerDigitsCount: int,
		roundingMagnitude: e - p + 1
	};
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/FormatNumericToString.js
/**
* https://tc39.es/ecma402/#sec-formatnumberstring
*/
function FormatNumericToString(intlObject, _x) {
	let x = _x;
	let sign;
	if (x.isZero() && x.isNegative()) {
		sign = "negative";
		x = ZERO;
	} else {
		invariant$1(x.isFinite(), "NumberFormatDigitInternalSlots value is not finite");
		if (x.lessThan(0)) sign = "negative";
		else sign = "positive";
		if (sign === "negative") x = x.negated();
	}
	let result;
	const roundingType = intlObject.roundingType;
	const unsignedRoundingMode = GetUnsignedRoundingMode(intlObject.roundingMode, sign === "negative");
	switch (roundingType) {
		case "significantDigits":
			result = ToRawPrecision(x, intlObject.minimumSignificantDigits, intlObject.maximumSignificantDigits, unsignedRoundingMode);
			break;
		case "fractionDigits":
			result = ToRawFixed(x, intlObject.minimumFractionDigits, intlObject.maximumFractionDigits, intlObject.roundingIncrement, unsignedRoundingMode);
			break;
		default:
			let sResult = ToRawPrecision(x, intlObject.minimumSignificantDigits, intlObject.maximumSignificantDigits, unsignedRoundingMode);
			let fResult = ToRawFixed(x, intlObject.minimumFractionDigits, intlObject.maximumFractionDigits, intlObject.roundingIncrement, unsignedRoundingMode);
			if (intlObject.roundingType === "morePrecision") {
				if (sResult.roundingMagnitude <= fResult.roundingMagnitude) result = sResult;
				else result = fResult;
			} else {
				invariant$1(intlObject.roundingType === "lessPrecision", "Invalid roundingType");
				if (sResult.roundingMagnitude <= fResult.roundingMagnitude) result = fResult;
				else result = sResult;
			}
	}
	x = result.roundedNumber;
	let string = result.formattedString;
	if (intlObject.trailingZeroDisplay === "stripIfInteger" && x.isInteger()) {
		let i = string.indexOf(".");
		if (i > -1) string = string.slice(0, i);
	}
	const int = result.integerDigitsCount;
	const minInteger = intlObject.minimumIntegerDigits;
	if (int < minInteger) string = repeat("0", minInteger - int) + string;
	if (sign === "negative") {
		if (x.isZero()) x = NEGATIVE_ZERO;
		else x = x.negated();
	}
	return {
		roundedNumber: x,
		formattedString: string
	};
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/ComputeExponent.js
/**
* The abstract operation ComputeExponent computes an exponent (power of ten) by which to scale x
* according to the number formatting settings. It handles cases such as 999 rounding up to 1000,
* requiring a different exponent.
*
* NOT IN SPEC: it returns [exponent, magnitude].
*/
function ComputeExponent(internalSlots, x) {
	if (x.isZero()) return [0, 0];
	if (x.isNegative()) x = x.negated();
	const xNum = x.toNumber();
	let magnitude;
	if (Number.isFinite(xNum) && Number.isSafeInteger(xNum) && xNum > 0 && xNum <= 999999) magnitude = new Decimal(Math.floor(Math.log10(xNum)));
	else magnitude = x.log(10).floor();
	const exponent = ComputeExponentForMagnitude(internalSlots, magnitude);
	x = x.times(getPowerOf10(-exponent));
	const formatNumberResult = FormatNumericToString(internalSlots, x);
	if (formatNumberResult.roundedNumber.isZero()) return [exponent, magnitude.toNumber()];
	const roundedNum = formatNumberResult.roundedNumber.toNumber();
	let newMagnitude;
	if (Number.isFinite(roundedNum) && Number.isSafeInteger(roundedNum) && roundedNum > 0 && roundedNum <= 999999) newMagnitude = new Decimal(Math.floor(Math.log10(roundedNum)));
	else newMagnitude = formatNumberResult.roundedNumber.log(10).floor();
	if (newMagnitude.eq(magnitude.minus(exponent))) return [exponent, magnitude.toNumber()];
	return [ComputeExponentForMagnitude(internalSlots, magnitude.plus(1)), magnitude.plus(1).toNumber()];
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+unicode@0.0.0/node_modules/@formatjs_generated/unicode/regex.js
const S_UNICODE_REGEX = /[\$\+<->\^`\|~\xA2-\xA6\xA8\xA9\xAC\xAE-\xB1\xB4\xB8\xD7\xF7\u02C2-\u02C5\u02D2-\u02DF\u02E5-\u02EB\u02ED\u02EF-\u02FF\u0375\u0384\u0385\u03F6\u0482\u058D-\u058F\u0606-\u0608\u060B\u060E\u060F\u06DE\u06E9\u06FD\u06FE\u07F6\u07FE\u07FF\u0888\u09F2\u09F3\u09FA\u09FB\u0AF1\u0B70\u0BF3-\u0BFA\u0C7F\u0D4F\u0D79\u0E3F\u0F01-\u0F03\u0F13\u0F15-\u0F17\u0F1A-\u0F1F\u0F34\u0F36\u0F38\u0FBE-\u0FC5\u0FC7-\u0FCC\u0FCE\u0FCF\u0FD5-\u0FD8\u109E\u109F\u1390-\u1399\u166D\u17DB\u1940\u19DE-\u19FF\u1B61-\u1B6A\u1B74-\u1B7C\u1FBD\u1FBF-\u1FC1\u1FCD-\u1FCF\u1FDD-\u1FDF\u1FED-\u1FEF\u1FFD\u1FFE\u2044\u2052\u207A-\u207C\u208A-\u208C\u20A0-\u20C1\u2100\u2101\u2103-\u2106\u2108\u2109\u2114\u2116-\u2118\u211E-\u2123\u2125\u2127\u2129\u212E\u213A\u213B\u2140-\u2144\u214A-\u214D\u214F\u218A\u218B\u2190-\u2307\u230C-\u2328\u232B-\u2429\u2440-\u244A\u249C-\u24E9\u2500-\u2767\u2794-\u27C4\u27C7-\u27E5\u27F0-\u2982\u2999-\u29D7\u29DC-\u29FB\u29FE-\u2B73\u2B76-\u2BFF\u2CE5-\u2CEA\u2E50\u2E51\u2E80-\u2E99\u2E9B-\u2EF3\u2F00-\u2FD5\u2FF0-\u2FFF\u3004\u3012\u3013\u3020\u3036\u3037\u303E\u303F\u309B\u309C\u3190\u3191\u3196-\u319F\u31C0-\u31E5\u31EF\u3200-\u321E\u322A-\u3247\u3250\u3260-\u327F\u328A-\u32B0\u32C0-\u33FF\u4DC0-\u4DFF\uA490-\uA4C6\uA700-\uA716\uA720\uA721\uA789\uA78A\uA828-\uA82B\uA836-\uA839\uAA77-\uAA79\uAB5B\uAB6A\uAB6B\uFB29\uFBB2-\uFBD2\uFD40-\uFD4F\uFD90\uFD91\uFDC8-\uFDCF\uFDFC-\uFDFF\uFE62\uFE64-\uFE66\uFE69\uFF04\uFF0B\uFF1C-\uFF1E\uFF3E\uFF40\uFF5C\uFF5E\uFFE0-\uFFE6\uFFE8-\uFFEE\uFFFC\uFFFD]|\uD800[\uDD37-\uDD3F\uDD79-\uDD89\uDD8C-\uDD8E\uDD90-\uDD9C\uDDA0\uDDD0-\uDDFC]|\uD802[\uDC77\uDC78\uDEC8]|\uD803[\uDD8E\uDD8F\uDED1-\uDED8]|\uD805\uDF3F|\uD807[\uDFD5-\uDFF1]|\uD81A[\uDF3C-\uDF3F\uDF45]|\uD82F\uDC9C|\uD833[\uDC00-\uDCEF\uDCFA-\uDCFC\uDD00-\uDEB3\uDEBA-\uDED0\uDEE0-\uDEF0\uDF50-\uDFC3]|\uD834[\uDC00-\uDCF5\uDD00-\uDD26\uDD29-\uDD64\uDD6A-\uDD6C\uDD83\uDD84\uDD8C-\uDDA9\uDDAE-\uDDEA\uDE00-\uDE41\uDE45\uDF00-\uDF56]|\uD835[\uDEC1\uDEDB\uDEFB\uDF15\uDF35\uDF4F\uDF6F\uDF89\uDFA9\uDFC3]|\uD836[\uDC00-\uDDFF\uDE37-\uDE3A\uDE6D-\uDE74\uDE76-\uDE83\uDE85\uDE86]|\uD838[\uDD4F\uDEFF]|\uD83B[\uDCAC\uDCB0\uDD2E\uDEF0\uDEF1]|\uD83C[\uDC00-\uDC2B\uDC30-\uDC93\uDCA0-\uDCAE\uDCB1-\uDCBF\uDCC1-\uDCCF\uDCD1-\uDCF5\uDD0D-\uDDAD\uDDE6-\uDE02\uDE10-\uDE3B\uDE40-\uDE48\uDE50\uDE51\uDE60-\uDE65\uDF00-\uDFFF]|\uD83D[\uDC00-\uDED8\uDEDC-\uDEEC\uDEF0-\uDEFC\uDF00-\uDFD9\uDFE0-\uDFEB\uDFF0]|\uD83E[\uDC00-\uDC0B\uDC10-\uDC47\uDC50-\uDC59\uDC60-\uDC87\uDC90-\uDCAD\uDCB0-\uDCBB\uDCC0\uDCC1\uDCD0-\uDCD8\uDD00-\uDE57\uDE60-\uDE6D\uDE70-\uDE7C\uDE80-\uDE8A\uDE8E-\uDEC6\uDEC8\uDECD-\uDEDC\uDEDF-\uDEEA\uDEEF-\uDEF8\uDF00-\uDF92\uDF94-\uDFEF\uDFFA]/;
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+unicode@0.0.0/node_modules/@formatjs_generated/unicode/digit-mapping.js
const digitMapping = {
	"adlm": [
		"𞥐",
		"𞥑",
		"𞥒",
		"𞥓",
		"𞥔",
		"𞥕",
		"𞥖",
		"𞥗",
		"𞥘",
		"𞥙"
	],
	"ahom": [
		"𑜰",
		"𑜱",
		"𑜲",
		"𑜳",
		"𑜴",
		"𑜵",
		"𑜶",
		"𑜷",
		"𑜸",
		"𑜹"
	],
	"arab": [
		"٠",
		"١",
		"٢",
		"٣",
		"٤",
		"٥",
		"٦",
		"٧",
		"٨",
		"٩"
	],
	"arabext": [
		"۰",
		"۱",
		"۲",
		"۳",
		"۴",
		"۵",
		"۶",
		"۷",
		"۸",
		"۹"
	],
	"bali": [
		"᭐",
		"᭑",
		"᭒",
		"᭓",
		"᭔",
		"᭕",
		"᭖",
		"᭗",
		"᭘",
		"᭙"
	],
	"beng": [
		"০",
		"১",
		"২",
		"৩",
		"৪",
		"৫",
		"৬",
		"৭",
		"৮",
		"৯"
	],
	"bhks": [
		"𑱐",
		"𑱑",
		"𑱒",
		"𑱓",
		"𑱔",
		"𑱕",
		"𑱖",
		"𑱗",
		"𑱘",
		"𑱙"
	],
	"brah": [
		"𑁦",
		"𑁧",
		"𑁨",
		"𑁩",
		"𑁪",
		"𑁫",
		"𑁬",
		"𑁭",
		"𑁮",
		"𑁯"
	],
	"cakm": [
		"𑄶",
		"𑄷",
		"𑄸",
		"𑄹",
		"𑄺",
		"𑄻",
		"𑄼",
		"𑄽",
		"𑄾",
		"𑄿"
	],
	"cham": [
		"꩐",
		"꩑",
		"꩒",
		"꩓",
		"꩔",
		"꩕",
		"꩖",
		"꩗",
		"꩘",
		"꩙"
	],
	"deva": [
		"०",
		"१",
		"२",
		"३",
		"४",
		"५",
		"६",
		"७",
		"८",
		"९"
	],
	"diak": [
		"𑥐",
		"𑥑",
		"𑥒",
		"𑥓",
		"𑥔",
		"𑥕",
		"𑥖",
		"𑥗",
		"𑥘",
		"𑥙"
	],
	"fullwide": [
		"０",
		"１",
		"２",
		"３",
		"４",
		"５",
		"６",
		"７",
		"８",
		"９"
	],
	"gara": [
		"𐵀",
		"𐵁",
		"𐵂",
		"𐵃",
		"𐵄",
		"𐵅",
		"𐵆",
		"𐵇",
		"𐵈",
		"𐵉"
	],
	"gong": [
		"𑶠",
		"𑶡",
		"𑶢",
		"𑶣",
		"𑶤",
		"𑶥",
		"𑶦",
		"𑶧",
		"𑶨",
		"𑶩"
	],
	"gonm": [
		"𑵐",
		"𑵑",
		"𑵒",
		"𑵓",
		"𑵔",
		"𑵕",
		"𑵖",
		"𑵗",
		"𑵘",
		"𑵙"
	],
	"gujr": [
		"૦",
		"૧",
		"૨",
		"૩",
		"૪",
		"૫",
		"૬",
		"૭",
		"૮",
		"૯"
	],
	"gukh": [
		"𖄰",
		"𖄱",
		"𖄲",
		"𖄳",
		"𖄴",
		"𖄵",
		"𖄶",
		"𖄷",
		"𖄸",
		"𖄹"
	],
	"guru": [
		"੦",
		"੧",
		"੨",
		"੩",
		"੪",
		"੫",
		"੬",
		"੭",
		"੮",
		"੯"
	],
	"hanidec": [
		"〇",
		"一",
		"二",
		"三",
		"四",
		"五",
		"六",
		"七",
		"八",
		"九"
	],
	"hmng": [
		"𖭐",
		"𖭑",
		"𖭒",
		"𖭓",
		"𖭔",
		"𖭕",
		"𖭖",
		"𖭗",
		"𖭘",
		"𖭙"
	],
	"hmnp": [
		"𞅀",
		"𞅁",
		"𞅂",
		"𞅃",
		"𞅄",
		"𞅅",
		"𞅆",
		"𞅇",
		"𞅈",
		"𞅉"
	],
	"java": [
		"꧐",
		"꧑",
		"꧒",
		"꧓",
		"꧔",
		"꧕",
		"꧖",
		"꧗",
		"꧘",
		"꧙"
	],
	"kali": [
		"꤀",
		"꤁",
		"꤂",
		"꤃",
		"꤄",
		"꤅",
		"꤆",
		"꤇",
		"꤈",
		"꤉"
	],
	"kawi": [
		"𑽐",
		"𑽑",
		"𑽒",
		"𑽓",
		"𑽔",
		"𑽕",
		"𑽖",
		"𑽗",
		"𑽘",
		"𑽙"
	],
	"khmr": [
		"០",
		"១",
		"២",
		"៣",
		"៤",
		"៥",
		"៦",
		"៧",
		"៨",
		"៩"
	],
	"knda": [
		"೦",
		"೧",
		"೨",
		"೩",
		"೪",
		"೫",
		"೬",
		"೭",
		"೮",
		"೯"
	],
	"krai": [
		"𖵰",
		"𖵱",
		"𖵲",
		"𖵳",
		"𖵴",
		"𖵵",
		"𖵶",
		"𖵷",
		"𖵸",
		"𖵹"
	],
	"lana": [
		"᪀",
		"᪁",
		"᪂",
		"᪃",
		"᪄",
		"᪅",
		"᪆",
		"᪇",
		"᪈",
		"᪉"
	],
	"lanatham": [
		"᪐",
		"᪑",
		"᪒",
		"᪓",
		"᪔",
		"᪕",
		"᪖",
		"᪗",
		"᪘",
		"᪙"
	],
	"laoo": [
		"໐",
		"໑",
		"໒",
		"໓",
		"໔",
		"໕",
		"໖",
		"໗",
		"໘",
		"໙"
	],
	"lepc": [
		"᱀",
		"᱁",
		"᱂",
		"᱃",
		"᱄",
		"᱅",
		"᱆",
		"᱇",
		"᱈",
		"᱉"
	],
	"limb": [
		"᥆",
		"᥇",
		"᥈",
		"᥉",
		"᥊",
		"᥋",
		"᥌",
		"᥍",
		"᥎",
		"᥏"
	],
	"mathbold": [
		"𝟎",
		"𝟏",
		"𝟐",
		"𝟑",
		"𝟒",
		"𝟓",
		"𝟔",
		"𝟕",
		"𝟖",
		"𝟗"
	],
	"mathdbl": [
		"𝟘",
		"𝟙",
		"𝟚",
		"𝟛",
		"𝟜",
		"𝟝",
		"𝟞",
		"𝟟",
		"𝟠",
		"𝟡"
	],
	"mathmono": [
		"𝟶",
		"𝟷",
		"𝟸",
		"𝟹",
		"𝟺",
		"𝟻",
		"𝟼",
		"𝟽",
		"𝟾",
		"𝟿"
	],
	"mathsanb": [
		"𝟬",
		"𝟭",
		"𝟮",
		"𝟯",
		"𝟰",
		"𝟱",
		"𝟲",
		"𝟳",
		"𝟴",
		"𝟵"
	],
	"mathsans": [
		"𝟢",
		"𝟣",
		"𝟤",
		"𝟥",
		"𝟦",
		"𝟧",
		"𝟨",
		"𝟩",
		"𝟪",
		"𝟫"
	],
	"mlym": [
		"൦",
		"൧",
		"൨",
		"൩",
		"൪",
		"൫",
		"൬",
		"൭",
		"൮",
		"൯"
	],
	"modi": [
		"𑙐",
		"𑙑",
		"𑙒",
		"𑙓",
		"𑙔",
		"𑙕",
		"𑙖",
		"𑙗",
		"𑙘",
		"𑙙"
	],
	"mong": [
		"᠐",
		"᠑",
		"᠒",
		"᠓",
		"᠔",
		"᠕",
		"᠖",
		"᠗",
		"᠘",
		"᠙"
	],
	"mroo": [
		"𖩠",
		"𖩡",
		"𖩢",
		"𖩣",
		"𖩤",
		"𖩥",
		"𖩦",
		"𖩧",
		"𖩨",
		"𖩩"
	],
	"mtei": [
		"꯰",
		"꯱",
		"꯲",
		"꯳",
		"꯴",
		"꯵",
		"꯶",
		"꯷",
		"꯸",
		"꯹"
	],
	"mymr": [
		"၀",
		"၁",
		"၂",
		"၃",
		"၄",
		"၅",
		"၆",
		"၇",
		"၈",
		"၉"
	],
	"mymrepka": [
		"𑛚",
		"𑛛",
		"𑛜",
		"𑛝",
		"𑛞",
		"𑛟",
		"𑛠",
		"𑛡",
		"𑛢",
		"𑛣"
	],
	"mymrpao": [
		"𑛐",
		"𑛑",
		"𑛒",
		"𑛓",
		"𑛔",
		"𑛕",
		"𑛖",
		"𑛗",
		"𑛘",
		"𑛙"
	],
	"mymrshan": [
		"႐",
		"႑",
		"႒",
		"႓",
		"႔",
		"႕",
		"႖",
		"႗",
		"႘",
		"႙"
	],
	"mymrtlng": [
		"꧰",
		"꧱",
		"꧲",
		"꧳",
		"꧴",
		"꧵",
		"꧶",
		"꧷",
		"꧸",
		"꧹"
	],
	"nagm": [
		"𞓰",
		"𞓱",
		"𞓲",
		"𞓳",
		"𞓴",
		"𞓵",
		"𞓶",
		"𞓷",
		"𞓸",
		"𞓹"
	],
	"newa": [
		"𑑐",
		"𑑑",
		"𑑒",
		"𑑓",
		"𑑔",
		"𑑕",
		"𑑖",
		"𑑗",
		"𑑘",
		"𑑙"
	],
	"nkoo": [
		"߀",
		"߁",
		"߂",
		"߃",
		"߄",
		"߅",
		"߆",
		"߇",
		"߈",
		"߉"
	],
	"olck": [
		"᱐",
		"᱑",
		"᱒",
		"᱓",
		"᱔",
		"᱕",
		"᱖",
		"᱗",
		"᱘",
		"᱙"
	],
	"onao": [
		"𞗱",
		"𞗲",
		"𞗳",
		"𞗴",
		"𞗵",
		"𞗶",
		"𞗷",
		"𞗸",
		"𞗹",
		"𞗺"
	],
	"orya": [
		"୦",
		"୧",
		"୨",
		"୩",
		"୪",
		"୫",
		"୬",
		"୭",
		"୮",
		"୯"
	],
	"osma": [
		"𐒠",
		"𐒡",
		"𐒢",
		"𐒣",
		"𐒤",
		"𐒥",
		"𐒦",
		"𐒧",
		"𐒨",
		"𐒩"
	],
	"outlined": [
		"𜳰",
		"𜳱",
		"𜳲",
		"𜳳",
		"𜳴",
		"𜳵",
		"𜳶",
		"𜳷",
		"𜳸",
		"𜳹"
	],
	"rohg": [
		"𐴰",
		"𐴱",
		"𐴲",
		"𐴳",
		"𐴴",
		"𐴵",
		"𐴶",
		"𐴷",
		"𐴸",
		"𐴹"
	],
	"saur": [
		"꣐",
		"꣑",
		"꣒",
		"꣓",
		"꣔",
		"꣕",
		"꣖",
		"꣗",
		"꣘",
		"꣙"
	],
	"segment": [
		"🯰",
		"🯱",
		"🯲",
		"🯳",
		"🯴",
		"🯵",
		"🯶",
		"🯷",
		"🯸",
		"🯹"
	],
	"shrd": [
		"𑇐",
		"𑇑",
		"𑇒",
		"𑇓",
		"𑇔",
		"𑇕",
		"𑇖",
		"𑇗",
		"𑇘",
		"𑇙"
	],
	"sind": [
		"𑋰",
		"𑋱",
		"𑋲",
		"𑋳",
		"𑋴",
		"𑋵",
		"𑋶",
		"𑋷",
		"𑋸",
		"𑋹"
	],
	"sinh": [
		"෦",
		"෧",
		"෨",
		"෩",
		"෪",
		"෫",
		"෬",
		"෭",
		"෮",
		"෯"
	],
	"sora": [
		"𑃰",
		"𑃱",
		"𑃲",
		"𑃳",
		"𑃴",
		"𑃵",
		"𑃶",
		"𑃷",
		"𑃸",
		"𑃹"
	],
	"sund": [
		"᮰",
		"᮱",
		"᮲",
		"᮳",
		"᮴",
		"᮵",
		"᮶",
		"᮷",
		"᮸",
		"᮹"
	],
	"sunu": [
		"𑯰",
		"𑯱",
		"𑯲",
		"𑯳",
		"𑯴",
		"𑯵",
		"𑯶",
		"𑯷",
		"𑯸",
		"𑯹"
	],
	"takr": [
		"𑛀",
		"𑛁",
		"𑛂",
		"𑛃",
		"𑛄",
		"𑛅",
		"𑛆",
		"𑛇",
		"𑛈",
		"𑛉"
	],
	"talu": [
		"᧐",
		"᧑",
		"᧒",
		"᧓",
		"᧔",
		"᧕",
		"᧖",
		"᧗",
		"᧘",
		"᧙"
	],
	"tamldec": [
		"௦",
		"௧",
		"௨",
		"௩",
		"௪",
		"௫",
		"௬",
		"௭",
		"௮",
		"௯"
	],
	"telu": [
		"౦",
		"౧",
		"౨",
		"౩",
		"౪",
		"౫",
		"౬",
		"౭",
		"౮",
		"౯"
	],
	"thai": [
		"๐",
		"๑",
		"๒",
		"๓",
		"๔",
		"๕",
		"๖",
		"๗",
		"๘",
		"๙"
	],
	"tibt": [
		"༠",
		"༡",
		"༢",
		"༣",
		"༤",
		"༥",
		"༦",
		"༧",
		"༨",
		"༩"
	],
	"tirh": [
		"𑓐",
		"𑓑",
		"𑓒",
		"𑓓",
		"𑓔",
		"𑓕",
		"𑓖",
		"𑓗",
		"𑓘",
		"𑓙"
	],
	"tnsa": [
		"𖫀",
		"𖫁",
		"𖫂",
		"𖫃",
		"𖫄",
		"𖫅",
		"𖫆",
		"𖫇",
		"𖫈",
		"𖫉"
	],
	"tols": [
		"𑷠",
		"𑷡",
		"𑷢",
		"𑷣",
		"𑷤",
		"𑷥",
		"𑷦",
		"𑷧",
		"𑷨",
		"𑷩"
	],
	"vaii": [
		"꘠",
		"꘡",
		"꘢",
		"꘣",
		"꘤",
		"꘥",
		"꘦",
		"꘧",
		"꘨",
		"꘩"
	],
	"wara": [
		"𑣠",
		"𑣡",
		"𑣢",
		"𑣣",
		"𑣤",
		"𑣥",
		"𑣦",
		"𑣧",
		"𑣨",
		"𑣩"
	],
	"wcho": [
		"𞋰",
		"𞋱",
		"𞋲",
		"𞋳",
		"𞋴",
		"𞋵",
		"𞋶",
		"𞋷",
		"𞋸",
		"𞋹"
	]
};
//#endregion
//#region packages/ecma402-abstract/NumberFormat/format_to_parts.js
const CARET_S_UNICODE_REGEX = new RegExp(`^${S_UNICODE_REGEX.source}`);
const S_DOLLAR_UNICODE_REGEX = new RegExp(`${S_UNICODE_REGEX.source}$`);
const CLDR_NUMBER_PATTERN = /[#0](?:[.,][#0]+)*/g;
function formatToParts$1(numberResult, data, pl, options, approximately = false) {
	const { sign, exponent, magnitude } = numberResult;
	const { notation, style, numberingSystem } = options;
	const defaultNumberingSystem = data.numbers.nu[0];
	const symbols = data.numbers.symbols[numberingSystem] || data.numbers.symbols[defaultNumberingSystem];
	approximately = approximately && !!symbols.approximatelySign;
	let compactNumberPattern = null;
	if (notation === "compact" && magnitude) compactNumberPattern = getCompactDisplayPattern(numberResult, pl, data, style, options.compactDisplay, options.currencyDisplay, numberingSystem);
	let nonNameCurrencyPart;
	if (style === "currency" && options.currencyDisplay !== "name") {
		const byCurrencyDisplay = data.currencies[options.currency];
		if (byCurrencyDisplay) switch (options.currencyDisplay) {
			case "code":
				nonNameCurrencyPart = options.currency;
				break;
			case "symbol":
				nonNameCurrencyPart = byCurrencyDisplay.symbol;
				break;
			default: nonNameCurrencyPart = byCurrencyDisplay.narrow;
		}
		else nonNameCurrencyPart = options.currency;
	}
	let numberPattern;
	if (!compactNumberPattern) {
		if (style === "decimal" || style === "unit" || style === "currency" && options.currencyDisplay === "name") numberPattern = getPatternForSign((data.numbers.decimal[numberingSystem] || data.numbers.decimal[defaultNumberingSystem]).standard, sign, approximately);
		else if (style === "currency") numberPattern = getPatternForSign((data.numbers.currency[numberingSystem] || data.numbers.currency[defaultNumberingSystem])[options.currencySign], sign, approximately);
		else numberPattern = getPatternForSign(data.numbers.percent[numberingSystem] || data.numbers.percent[defaultNumberingSystem], sign, approximately);
	} else numberPattern = approximately ? insertApproximatelySign(compactNumberPattern, sign) : compactNumberPattern;
	const decimalNumberPattern = CLDR_NUMBER_PATTERN.exec(numberPattern)[0];
	numberPattern = numberPattern.replace(CLDR_NUMBER_PATTERN, "{0}").replace(/'(.)'/g, "$1");
	if (style === "currency" && options.currencyDisplay !== "name") {
		const currencyData = data.numbers.currency[numberingSystem] || data.numbers.currency[defaultNumberingSystem];
		const afterCurrency = currencyData.currencySpacing.afterInsertBetween;
		if (afterCurrency && !S_DOLLAR_UNICODE_REGEX.test(nonNameCurrencyPart)) numberPattern = numberPattern.replace("¤{0}", `¤${afterCurrency}{0}`);
		const beforeCurrency = currencyData.currencySpacing.beforeInsertBetween;
		if (beforeCurrency && !CARET_S_UNICODE_REGEX.test(nonNameCurrencyPart)) numberPattern = numberPattern.replace("{0}¤", `{0}${beforeCurrency}¤`);
	}
	const numberPatternParts = numberPattern.split(/({c:[^}]+}|\{approximatelySign\}|\{0\}|[¤%\-+])/g);
	const numberParts = [];
	for (const part of numberPatternParts) {
		if (!part) continue;
		switch (part) {
			case "{0}":
				numberParts.push(...partitionNumberIntoParts(symbols, numberResult, notation, exponent, numberingSystem, !compactNumberPattern && (options.useGrouping ?? true), decimalNumberPattern, data.numbers.minimumGroupingDigits ?? 1, style, options.roundingIncrement, GetUnsignedRoundingMode(options.roundingMode, sign === -1)));
				break;
			case "{approximatelySign}":
				numberParts.push({
					type: "approximatelySign",
					value: symbols.approximatelySign
				});
				break;
			case "-":
				numberParts.push({
					type: "minusSign",
					value: symbols.minusSign
				});
				break;
			case "+":
				numberParts.push({
					type: "plusSign",
					value: symbols.plusSign
				});
				break;
			case "%":
				numberParts.push({
					type: "percentSign",
					value: symbols.percentSign
				});
				break;
			case "¤":
				numberParts.push({
					type: "currency",
					value: nonNameCurrencyPart
				});
				break;
			default: if (part.startsWith("{c:")) numberParts.push({
				type: "compact",
				value: part.substring(3, part.length - 1)
			});
			else numberParts.push({
				type: "literal",
				value: part
			});
		}
	}
	switch (style) {
		case "currency": if (options.currencyDisplay === "name") {
			const unitPattern = (data.numbers.currency[numberingSystem] || data.numbers.currency[defaultNumberingSystem]).unitPattern;
			let unitName;
			const currencyNameData = data.currencies[options.currency];
			if (currencyNameData) unitName = selectPlural(pl, numberResult.roundedNumber.times(getPowerOf10(exponent)).toNumber(), currencyNameData.displayName);
			else unitName = options.currency;
			const unitPatternParts = unitPattern.split(/(\{[01]\})/g);
			const result = [];
			for (const part of unitPatternParts) switch (part) {
				case "{0}":
					result.push(...numberParts);
					break;
				case "{1}":
					result.push({
						type: "currency",
						value: unitName
					});
					break;
				default: if (part) result.push({
					type: "literal",
					value: part
				});
			}
			return result;
		} else return numberParts;
		case "unit": {
			const { unit, unitDisplay } = options;
			let unitData = data.units.simple[unit];
			let unitPattern;
			if (unitData) unitPattern = selectPlural(pl, numberResult.roundedNumber.times(getPowerOf10(exponent)).toNumber(), data.units.simple[unit][unitDisplay]);
			else {
				const [numeratorUnit, denominatorUnit] = unit.split("-per-");
				unitData = data.units.simple[numeratorUnit];
				const numeratorUnitPattern = selectPlural(pl, numberResult.roundedNumber.times(getPowerOf10(exponent)).toNumber(), data.units.simple[numeratorUnit][unitDisplay]);
				const perUnitPattern = data.units.simple[denominatorUnit].perUnit[unitDisplay];
				if (perUnitPattern) unitPattern = perUnitPattern.replace("{0}", numeratorUnitPattern);
				else {
					const perPattern = data.units.compound.per[unitDisplay];
					const denominatorPattern = selectPlural(pl, 1, data.units.simple[denominatorUnit][unitDisplay]);
					unitPattern = unitPattern = perPattern.replace("{0}", numeratorUnitPattern).replace("{1}", denominatorPattern.replace("{0}", ""));
				}
			}
			const result = [];
			for (const part of unitPattern.split(/(\s*\{0\}\s*)/)) {
				const interpolateMatch = /^(\s*)\{0\}(\s*)$/.exec(part);
				if (interpolateMatch) {
					if (interpolateMatch[1]) result.push({
						type: "literal",
						value: interpolateMatch[1]
					});
					result.push(...numberParts);
					if (interpolateMatch[2]) result.push({
						type: "literal",
						value: interpolateMatch[2]
					});
				} else if (part) result.push({
					type: "unit",
					value: part
				});
			}
			return result;
		}
		default: return numberParts;
	}
}
function partitionNumberIntoParts(symbols, numberResult, notation, exponent, numberingSystem, useGrouping, decimalNumberPattern, minimumGroupingDigits, style, roundingIncrement, unsignedRoundingMode) {
	const result = [];
	let { formattedString: n, roundedNumber: x } = numberResult;
	if (x.isNaN()) return [{
		type: "nan",
		value: n
	}];
	else if (!x.isFinite()) return [{
		type: "infinity",
		value: n
	}];
	const asciiDecimalSepIndex = n.indexOf(".");
	const integerDigitCount = asciiDecimalSepIndex < 0 ? n.length : asciiDecimalSepIndex;
	const digitReplacementTable = digitMapping[numberingSystem];
	if (digitReplacementTable) n = n.replace(/\d/g, (digit) => digitReplacementTable[+digit] || digit);
	const decimalSepIndex = n.indexOf(".");
	let integer;
	let fraction;
	if (decimalSepIndex > 0) {
		integer = n.slice(0, decimalSepIndex);
		fraction = n.slice(decimalSepIndex + 1);
	} else integer = n;
	const patternGroups = decimalNumberPattern.split(".")[0].split(",");
	const primaryGroupingSize = patternGroups.length > 1 ? patternGroups[patternGroups.length - 1].length : 3;
	const secondaryGroupingSize = patternGroups.length > 2 ? patternGroups[patternGroups.length - 2].length : primaryGroupingSize;
	if (Boolean(useGrouping) && (useGrouping === "always" || integerDigitCount >= primaryGroupingSize + (useGrouping === "min2" ? 2 : minimumGroupingDigits))) {
		const groupSepSymbol = style === "currency" && symbols.currencyGroup != null ? symbols.currencyGroup : symbols.group;
		const groups = [];
		const codePoints = integer.length === integerDigitCount ? void 0 : Array.from(integer);
		let end = integerDigitCount;
		let groupSize = primaryGroupingSize;
		while (end > 0) {
			const start = Math.max(0, end - groupSize);
			groups.push(codePoints ? codePoints.slice(start, end).join("") : integer.slice(start, end));
			end = start;
			groupSize = secondaryGroupingSize;
		}
		while (groups.length > 0) {
			const integerGroup = groups.pop();
			result.push({
				type: "integer",
				value: integerGroup
			});
			if (groups.length > 0) result.push({
				type: "group",
				value: groupSepSymbol
			});
		}
	} else result.push({
		type: "integer",
		value: integer
	});
	if (fraction !== void 0) {
		const decimalSepSymbol = style === "currency" && symbols.currencyDecimal != null ? symbols.currencyDecimal : symbols.decimal;
		result.push({
			type: "decimal",
			value: decimalSepSymbol
		}, {
			type: "fraction",
			value: fraction
		});
	}
	if ((notation === "scientific" || notation === "engineering") && x.isFinite()) {
		result.push({
			type: "exponentSeparator",
			value: symbols.exponential
		});
		if (exponent < 0) {
			result.push({
				type: "exponentMinusSign",
				value: symbols.minusSign
			});
			exponent = -exponent;
		}
		const exponentResult = ToRawFixed(new Decimal(exponent), 0, 0, roundingIncrement, unsignedRoundingMode);
		result.push({
			type: "exponentInteger",
			value: exponentResult.formattedString
		});
	}
	return result;
}
function getPatternForSign(pattern, sign, approximately = false) {
	if (pattern.indexOf(";") < 0) pattern = `${pattern};-${pattern}`;
	const [zeroPattern, negativePattern] = pattern.split(";");
	if (sign === 0 && approximately && negativePattern.includes("-")) return negativePattern.replace("-", "{approximatelySign}");
	let signedPattern = zeroPattern;
	if (sign === -1) signedPattern = negativePattern;
	else if (sign === 1) signedPattern = negativePattern.includes("-") ? negativePattern.replace(/-/g, "+") : `+${zeroPattern}`;
	return approximately ? insertApproximatelySign(signedPattern, sign) : signedPattern;
}
function insertApproximatelySign(pattern, sign) {
	const symbol = sign === 1 ? "+" : "-";
	const index = sign === 0 ? -1 : pattern.indexOf(symbol);
	return index < 0 ? `{approximatelySign}${pattern}` : `${pattern.slice(0, index)}{approximatelySign}${pattern.slice(index)}`;
}
function getCompactDisplayPattern(numberResult, pl, data, style, compactDisplay, currencyDisplay, numberingSystem) {
	const { roundedNumber, sign, magnitude } = numberResult;
	let magnitudeKey = String(10 ** magnitude);
	const defaultNumberingSystem = data.numbers.nu[0];
	let pattern;
	if (style === "currency" && currencyDisplay !== "name") {
		const byNumberingSystem = data.numbers.currency;
		const currencyData = byNumberingSystem[numberingSystem] || byNumberingSystem[defaultNumberingSystem];
		let compactPluralRules = currencyData.short?.[magnitudeKey];
		if (!compactPluralRules) {
			const thresholds = Object.keys(currencyData.short || {});
			if (thresholds.length > 0 && magnitudeKey > thresholds[thresholds.length - 1]) {
				magnitudeKey = thresholds[thresholds.length - 1];
				compactPluralRules = currencyData.short?.[magnitudeKey];
			}
		}
		if (!compactPluralRules) return null;
		pattern = selectPlural(pl, roundedNumber.toNumber(), compactPluralRules);
	} else {
		const byNumberingSystem = data.numbers.decimal;
		const byCompactDisplay = byNumberingSystem[numberingSystem] || byNumberingSystem[defaultNumberingSystem];
		let compactPlaralRule = byCompactDisplay[compactDisplay][magnitudeKey];
		if (!compactPlaralRule) {
			const thresholds = Object.keys(byCompactDisplay[compactDisplay]);
			if (thresholds.length > 0 && magnitudeKey > thresholds[thresholds.length - 1]) {
				magnitudeKey = thresholds[thresholds.length - 1];
				compactPlaralRule = byCompactDisplay[compactDisplay][magnitudeKey];
			}
		}
		if (!compactPlaralRule) return null;
		pattern = selectPlural(pl, roundedNumber.toNumber(), compactPlaralRule);
	}
	if (pattern === "0") return null;
	pattern = getPatternForSign(pattern, sign).replace(/([^\s;\-+\d¤]+)/g, "{c:$1}").replace(/0+/, "0");
	return pattern;
}
function selectPlural(pl, x, rules) {
	return rules[pl.select(x)] || rules.other;
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/PartitionNumberPattern.js
/**
* https://tc39.es/ecma402/#sec-partitionnumberpattern
*/
function PartitionNumberPattern(internalSlots, _x, approximately = false) {
	let x = _x;
	let magnitude = 0;
	const { pl, dataLocaleData, numberingSystem } = internalSlots;
	const symbols = dataLocaleData.numbers.symbols[numberingSystem] || dataLocaleData.numbers.symbols[dataLocaleData.numbers.nu[0]];
	let exponent = 0;
	let n;
	if (x.isNaN()) n = symbols.nan;
	else if (!x.isFinite()) n = symbols.infinity;
	else {
		if (!x.isZero()) {
			invariant$1(x.isFinite(), "Input must be a mathematical value");
			if (internalSlots.style == "percent") x = x.times(100);
			[exponent, magnitude] = ComputeExponent(internalSlots, x);
			x = x.times(getPowerOf10(-exponent));
		}
		const formatNumberResult = FormatNumericToString(internalSlots, x);
		n = formatNumberResult.formattedString;
		x = formatNumberResult.roundedNumber;
	}
	let sign;
	const signDisplay = internalSlots.signDisplay;
	switch (signDisplay) {
		case "never":
			sign = 0;
			break;
		case "auto":
			if (x.isPositive() || x.isNaN()) sign = 0;
			else sign = -1;
			break;
		case "always":
			if (x.isPositive() || x.isNaN()) sign = 1;
			else sign = -1;
			break;
		case "exceptZero":
			if (x.isZero() || x.isNaN()) sign = 0;
			else if (x.isNegative()) sign = -1;
			else sign = 1;
			break;
		default:
			invariant$1(signDisplay === "negative", "signDisplay must be \"negative\"");
			if (x.isNegative() && !x.isZero()) sign = -1;
			else sign = 0;
	}
	return formatToParts$1({
		roundedNumber: x,
		formattedString: n,
		exponent,
		magnitude,
		sign
	}, internalSlots.dataLocaleData, pl, internalSlots, approximately);
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/FormatNumeric.js
function FormatNumeric(internalSlots, x) {
	return PartitionNumberPattern(internalSlots, x).map((p) => p.value).join("");
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/CollapseNumberRange.js
const AFFIX_TYPES = /* @__PURE__ */ new Set([
	"unit",
	"minusSign",
	"plusSign",
	"percentSign",
	"currency",
	"literal"
]);
const DIGIT_TYPES = /* @__PURE__ */ new Set([
	"integer",
	"fraction",
	"exponentInteger"
]);
function affixLength(parts, fromEnd) {
	let length = 0;
	while (length < parts.length) {
		const part = parts[fromEnd ? parts.length - length - 1 : length];
		if (!AFFIX_TYPES.has(part.type)) break;
		length++;
	}
	return length;
}
function canCollapse(a, b, sharedSuffix = false) {
	return a.length === b.length && a.some((part) => part.type !== "literal") && a.every((part, i) => part.type === b[i].type && part.value === b[i].value) && (Array.from(a.map((part) => part.value).join("")).length > 1 || sharedSuffix && a.length === 1 && (a[0].type === "plusSign" || a[0].type === "minusSign"));
}
function CollapseNumberRange(_numberFormat, result, _options) {
	const separatorIndex = result.findIndex((part) => part.source === "shared");
	if (separatorIndex < 0) return result;
	const start = result.slice(0, separatorIndex);
	const end = result.slice(separatorIndex + 1);
	const separator = { ...result[separatorIndex] };
	const prefix = [];
	const suffix = [];
	const startPrefix = start.slice(0, affixLength(start, false));
	const endPrefix = end.slice(0, affixLength(end, false));
	const startSuffixLength = affixLength(start, true);
	const endSuffixLength = affixLength(end, true);
	const startSuffix = start.slice(start.length - startSuffixLength);
	const collapseSuffix = canCollapse(startSuffix, end.slice(end.length - endSuffixLength));
	if (canCollapse(startPrefix, endPrefix, collapseSuffix && startSuffix.some((part) => part.type === "currency" || part.type === "unit" || part.type === "percentSign"))) {
		prefix.push(...start.splice(0, startPrefix.length));
		end.splice(0, endPrefix.length);
	}
	if (collapseSuffix) {
		start.splice(start.length - startSuffixLength);
		suffix.push(...end.splice(end.length - endSuffixLength));
	}
	for (const part of [...prefix, ...suffix]) part.source = "shared";
	if (start.length && end.length && (!DIGIT_TYPES.has(start[start.length - 1].type) || !DIGIT_TYPES.has(end[0].type))) {
		if (!separator.value || separator.value.charAt(0).trim()) separator.value = ` ${separator.value}`;
		if (separator.value.charAt(separator.value.length - 1).trim()) separator.value += " ";
	}
	return [
		...prefix,
		...start,
		separator,
		...end,
		...suffix
	];
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/FormatApproximately.js
function FormatApproximately(internalSlots, x) {
	return PartitionNumberPattern(internalSlots, x, true);
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/PartitionNumberRangePattern.js
/**
* https://tc39.es/ecma402/#sec-partitionnumberrangepattern
*/
function PartitionNumberRangePattern(numberFormat, x, y, { getInternalSlots }) {
	invariant$1(!x.isNaN() && !y.isNaN(), "Input must be a number", RangeError);
	const internalSlots = getInternalSlots(numberFormat);
	const xResult = PartitionNumberPattern(internalSlots, x);
	const yResult = PartitionNumberPattern(internalSlots, y);
	if (xResult.map((part) => part.value).join("") === yResult.map((part) => part.value).join("")) {
		const appxResult = FormatApproximately(internalSlots, x);
		appxResult.forEach((el) => {
			el.source = "shared";
		});
		return appxResult;
	}
	let result = [];
	xResult.forEach((el) => {
		el.source = "startRange";
		result.push(el);
	});
	const rangeSeparator = internalSlots.dataLocaleData.numbers.symbols[internalSlots.numberingSystem].rangeSign;
	result.push({
		type: "literal",
		value: rangeSeparator,
		source: "shared"
	});
	yResult.forEach((el) => {
		el.source = "endRange";
		result.push(el);
	});
	return CollapseNumberRange(numberFormat, result, { getInternalSlots });
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/FormatNumericRange.js
/**
* https://tc39.es/ecma402/#sec-formatnumericrange
*/
function FormatNumericRange(numberFormat, x, y, { getInternalSlots }) {
	return PartitionNumberRangePattern(numberFormat, x, y, { getInternalSlots }).map((part) => part.value).join("");
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/FormatNumericRangeToParts.js
/**
* https://tc39.es/ecma402/#sec-formatnumericrangetoparts
*/
function FormatNumericRangeToParts(numberFormat, x, y, { getInternalSlots }) {
	return PartitionNumberRangePattern(numberFormat, x, y, { getInternalSlots }).map((part) => ({
		type: part.type,
		value: part.value,
		source: part.source
	}));
}
//#endregion
//#region packages/ecma262-abstract/ArrayCreate.js
/**
* https://www.ecma-international.org/ecma-262/11.0/index.html#sec-arraycreate
*/
function ArrayCreate(len) {
	return Array.from({ length: len });
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/FormatNumericToParts.js
function FormatNumericToParts(nf, x, implDetails) {
	const parts = PartitionNumberPattern(implDetails.getInternalSlots(nf), x);
	const result = ArrayCreate(0);
	for (const part of parts) result.push({
		type: part.type,
		value: part.value
	});
	return result;
}
//#endregion
//#region packages/ecma402-abstract/IsUnicodeLocaleIdentifierType.js
/** Tests the Unicode locale identifier `type` grammar, not locale support. */
function IsUnicodeLocaleIdentifierType(value) {
	return /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*(?![\s\S])/i.test(value);
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
//#region packages/ecma402-abstract/CoerceOptionsToObject.js
/**
* https://tc39.es/ecma402/#sec-coerceoptionstoobject
* @param options
* @returns
*/
function CoerceOptionsToObject(options) {
	if (typeof options === "undefined") return Object.create(null);
	return ToObject(options);
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
//#region packages/ecma402-abstract/GetStringOrBooleanOption.js
/**
* https://tc39.es/ecma402/#sec-getstringorbooleanoption
* @param opts
* @param prop
* @param values
* @param trueValue
* @param falsyValue
* @param fallback
*/
function GetStringOrBooleanOption(opts, prop, values, trueValue, falsyValue, fallback) {
	let value = opts[prop];
	if (value === void 0) return fallback;
	if (value === true) return trueValue;
	if (Boolean(value) === false) return falsyValue;
	value = ToString(value);
	if (value === "true" || value === "false") return fallback;
	if ((values || []).indexOf(value) === -1) throw new RangeError(`Invalid value ${value}`);
	return value;
}
//#endregion
//#region packages/ecma262-abstract/HasOwnProperty.js
/**
* https://www.ecma-international.org/ecma-262/11.0/index.html#sec-hasownproperty
*/
function HasOwnProperty(o, prop) {
	return Object.prototype.hasOwnProperty.call(o, prop);
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/CurrencyDigits.js
/**
* https://tc39.es/ecma402/#sec-currencydigits
*/
function CurrencyDigits(c, { currencyDigitsData }) {
	return HasOwnProperty(currencyDigitsData, c) ? currencyDigitsData[c] : 2;
}
//#endregion
//#region packages/ecma402-abstract/DefaultNumberOption.js
/**
* ECMA-402 §9.2.13 DefaultNumberOption, step 2.
* https://tc39.es/ecma402/#sec-defaultnumberoption
* https://github.com/tc39/ecma402/blob/b1c961988b9a07894b1dc3dc2b5626ea48387d61/spec/negotiation.html#L437
* @param val
* @param min
* @param max
* @param fallback
*/
function DefaultNumberOption(inputVal, min, max, fallback) {
	if (inputVal === void 0) return fallback;
	const val = +inputVal;
	if (isNaN(val) || val < min || val > max) throw new RangeError(`${val} is outside of range [${min}, ${max}]`);
	return Math.floor(val);
}
//#endregion
//#region packages/ecma402-abstract/GetNumberOption.js
/**
* https://tc39.es/ecma402/#sec-getnumberoption
* @param options
* @param property
* @param min
* @param max
* @param fallback
*/
function GetNumberOption(options, property, minimum, maximum, fallback) {
	const val = options[property];
	return DefaultNumberOption(val, minimum, maximum, fallback);
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/SetNumberFormatDigitOptions.js
const VALID_ROUNDING_INCREMENTS = /* @__PURE__ */ new Set([
	1,
	2,
	5,
	10,
	20,
	25,
	50,
	100,
	200,
	250,
	500,
	1e3,
	2e3,
	2500,
	5e3
]);
/**
* https://tc39.es/ecma402/#sec-setnfdigitoptions
*/
function SetNumberFormatDigitOptions(internalSlots, opts, mnfdDefault, mxfdDefault, notation) {
	const mnid = GetNumberOption(opts, "minimumIntegerDigits", 1, 21, 1);
	let mnfd = opts.minimumFractionDigits;
	let mxfd = opts.maximumFractionDigits;
	let mnsd = opts.minimumSignificantDigits;
	let mxsd = opts.maximumSignificantDigits;
	internalSlots.minimumIntegerDigits = mnid;
	const roundingIncrement = GetNumberOption(opts, "roundingIncrement", 1, 5e3, 1);
	invariant$1(VALID_ROUNDING_INCREMENTS.has(roundingIncrement), `Invalid rounding increment value: ${roundingIncrement}.
Valid values are ${Array.from(VALID_ROUNDING_INCREMENTS).join(", ")}.`, RangeError);
	const roundingMode = GetOption(opts, "roundingMode", "string", [
		"ceil",
		"floor",
		"expand",
		"trunc",
		"halfCeil",
		"halfFloor",
		"halfExpand",
		"halfTrunc",
		"halfEven"
	], "halfExpand");
	const roundingPriority = GetOption(opts, "roundingPriority", "string", [
		"auto",
		"morePrecision",
		"lessPrecision"
	], "auto");
	const trailingZeroDisplay = GetOption(opts, "trailingZeroDisplay", "string", ["auto", "stripIfInteger"], "auto");
	if (roundingIncrement !== 1) mxfdDefault = mnfdDefault;
	internalSlots.roundingIncrement = roundingIncrement;
	internalSlots.roundingMode = roundingMode;
	internalSlots.trailingZeroDisplay = trailingZeroDisplay;
	const hasSd = mnsd !== void 0 || mxsd !== void 0;
	const hasFd = mnfd !== void 0 || mxfd !== void 0;
	let needSd = true;
	let needFd = true;
	if (roundingPriority === "auto") {
		needSd = hasSd;
		if (hasSd || !hasFd && notation === "compact") needFd = false;
	}
	if (needSd) {
		if (hasSd) {
			internalSlots.minimumSignificantDigits = DefaultNumberOption(mnsd, 1, 21, 1);
			internalSlots.maximumSignificantDigits = DefaultNumberOption(mxsd, internalSlots.minimumSignificantDigits, 21, 21);
		} else {
			internalSlots.minimumSignificantDigits = 1;
			internalSlots.maximumSignificantDigits = 21;
		}
	}
	if (needFd) {
		if (hasFd) {
			mnfd = DefaultNumberOption(mnfd, 0, 100, void 0);
			mxfd = DefaultNumberOption(mxfd, 0, 100, void 0);
			if (mnfd === void 0) {
				invariant$1(mxfd !== void 0, "maximumFractionDigits must be defined");
				mnfd = Math.min(mnfdDefault, mxfd);
			} else if (mxfd === void 0) mxfd = Math.max(mxfdDefault, mnfd);
			else if (mnfd > mxfd) throw new RangeError(`Invalid range, ${mnfd} > ${mxfd}`);
			internalSlots.minimumFractionDigits = mnfd;
			internalSlots.maximumFractionDigits = mxfd;
		} else {
			internalSlots.minimumFractionDigits = mnfdDefault;
			internalSlots.maximumFractionDigits = mxfdDefault;
		}
	}
	if (!needSd && !needFd) {
		internalSlots.minimumFractionDigits = 0;
		internalSlots.maximumFractionDigits = 0;
		internalSlots.minimumSignificantDigits = 1;
		internalSlots.maximumSignificantDigits = 2;
		internalSlots.roundingType = "morePrecision";
		internalSlots.roundingPriority = "morePrecision";
	} else if (roundingPriority === "morePrecision") {
		internalSlots.roundingType = "morePrecision";
		internalSlots.roundingPriority = "morePrecision";
	} else if (roundingPriority === "lessPrecision") {
		internalSlots.roundingType = "lessPrecision";
		internalSlots.roundingPriority = "lessPrecision";
	} else if (hasSd) {
		internalSlots.roundingType = "significantDigits";
		internalSlots.roundingPriority = "auto";
	} else {
		internalSlots.roundingType = "fractionDigits";
		internalSlots.roundingPriority = "auto";
	}
	if (roundingIncrement !== 1) {
		invariant$1(internalSlots.roundingType === "fractionDigits", "Invalid roundingType", TypeError);
		invariant$1(internalSlots.maximumFractionDigits === internalSlots.minimumFractionDigits, "With roundingIncrement > 1, maximumFractionDigits and minimumFractionDigits must be equal.", RangeError);
	}
}
//#endregion
//#region packages/ecma402-abstract/IsWellFormedCurrencyCode.js
/**
* This follows https://tc39.es/ecma402/#sec-case-sensitivity-and-case-mapping
* @param str string to convert
*/
function toUpperCase(str) {
	return str.replace(/([a-z])/g, (_, c) => c.toUpperCase());
}
const NOT_A_Z_REGEX = /[^A-Z]/;
/**
* https://tc39.es/ecma402/#sec-iswellformedcurrencycode
*/
function IsWellFormedCurrencyCode(currency) {
	currency = toUpperCase(currency);
	if (currency.length !== 3) return false;
	if (NOT_A_Z_REGEX.test(currency)) return false;
	return true;
}
//#endregion
//#region packages/ecma402-abstract/IsSanctionedSimpleUnitIdentifier.js
/**
* https://tc39.es/ecma402/#table-sanctioned-simple-unit-identifiers
*/
const SANCTIONED_UNITS = [
	"angle-degree",
	"area-acre",
	"area-hectare",
	"concentr-percent",
	"digital-bit",
	"digital-byte",
	"digital-gigabit",
	"digital-gigabyte",
	"digital-kilobit",
	"digital-kilobyte",
	"digital-megabit",
	"digital-megabyte",
	"digital-petabyte",
	"digital-terabit",
	"digital-terabyte",
	"duration-day",
	"duration-hour",
	"duration-microsecond",
	"duration-millisecond",
	"duration-minute",
	"duration-month",
	"duration-nanosecond",
	"duration-second",
	"duration-week",
	"duration-year",
	"length-centimeter",
	"length-foot",
	"length-inch",
	"length-kilometer",
	"length-meter",
	"length-mile-scandinavian",
	"length-mile",
	"length-millimeter",
	"length-yard",
	"mass-gram",
	"mass-kilogram",
	"mass-ounce",
	"mass-pound",
	"mass-stone",
	"temperature-celsius",
	"temperature-fahrenheit",
	"volume-fluid-ounce",
	"volume-gallon",
	"volume-liter",
	"volume-milliliter"
];
function removeUnitNamespace(unit) {
	return unit.slice(unit.indexOf("-") + 1);
}
/**
* https://tc39.es/ecma402/#table-sanctioned-simple-unit-identifiers
*/
const SIMPLE_UNITS = SANCTIONED_UNITS.map(removeUnitNamespace);
/**
* https://tc39.es/ecma402/#sec-issanctionedsimpleunitidentifier
*/
function IsSanctionedSimpleUnitIdentifier(unitIdentifier) {
	return SIMPLE_UNITS.indexOf(unitIdentifier) > -1;
}
//#endregion
//#region packages/ecma402-abstract/IsWellFormedUnitIdentifier.js
/**
* https://tc39.es/ecma402/#sec-iswellformedunitidentifier
* @param unit
*/
function IsWellFormedUnitIdentifier(unit) {
	if (IsSanctionedSimpleUnitIdentifier(unit)) return true;
	const units = unit.split("-per-");
	if (units.length !== 2) return false;
	const [numerator, denominator] = units;
	if (!IsSanctionedSimpleUnitIdentifier(numerator) || !IsSanctionedSimpleUnitIdentifier(denominator)) return false;
	return true;
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/SetNumberFormatUnitOptions.js
/**
* https://tc39.es/ecma402/#sec-setnumberformatunitoptions
*/
function SetNumberFormatUnitOptions(internalSlots, options = Object.create(null)) {
	const style = GetOption(options, "style", "string", [
		"decimal",
		"percent",
		"currency",
		"unit"
	], "decimal");
	internalSlots.style = style;
	const currency = GetOption(options, "currency", "string", void 0, void 0);
	invariant$1(currency === void 0 || IsWellFormedCurrencyCode(currency), "Malformed currency code", RangeError);
	invariant$1(style !== "currency" || currency !== void 0, "currency cannot be undefined", TypeError);
	const currencyDisplay = GetOption(options, "currencyDisplay", "string", [
		"code",
		"symbol",
		"narrowSymbol",
		"name"
	], "symbol");
	const currencySign = GetOption(options, "currencySign", "string", ["standard", "accounting"], "standard");
	const unit = GetOption(options, "unit", "string", void 0, void 0);
	invariant$1(unit === void 0 || IsWellFormedUnitIdentifier(unit), "Invalid unit argument for Intl.NumberFormat()", RangeError);
	invariant$1(style !== "unit" || unit !== void 0, "unit cannot be undefined", TypeError);
	const unitDisplay = GetOption(options, "unitDisplay", "string", [
		"short",
		"narrow",
		"long"
	], "short");
	if (style === "currency") {
		internalSlots.currency = currency.toUpperCase();
		internalSlots.currencyDisplay = currencyDisplay;
		internalSlots.currencySign = currencySign;
	}
	if (style === "unit") {
		internalSlots.unit = unit;
		internalSlots.unitDisplay = unitDisplay;
	}
}
//#endregion
//#region packages/ecma402-abstract/NumberFormat/InitializeNumberFormat.js
/**
* https://tc39.es/ecma402/#sec-initializenumberformat
*/
function InitializeNumberFormat(nf, locales, opts, { getInternalSlots, localeData, availableLocales, getDefaultLocale, currencyDigitsData }) {
	const requestedLocales = CanonicalizeLocaleList(locales);
	const options = CoerceOptionsToObject(opts);
	const opt = Object.create(null);
	opt.localeMatcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
	const numberingSystem = GetOption(options, "numberingSystem", "string", void 0, void 0);
	if (numberingSystem !== void 0 && !IsUnicodeLocaleIdentifierType(numberingSystem)) throw RangeError(`Invalid numberingSystems: ${numberingSystem}`);
	opt.nu = numberingSystem;
	const r = ResolveLocale(Array.from(availableLocales), requestedLocales, opt, ["nu"], localeData, getDefaultLocale);
	const dataLocaleData = localeData[r.dataLocale];
	invariant$1(!!dataLocaleData, `Missing locale data for ${r.dataLocale}`);
	const internalSlots = getInternalSlots(nf);
	internalSlots.locale = r.locale;
	internalSlots.dataLocale = r.dataLocale;
	internalSlots.numberingSystem = r.nu;
	internalSlots.dataLocaleData = dataLocaleData;
	SetNumberFormatUnitOptions(internalSlots, options);
	const style = internalSlots.style;
	const notation = GetOption(options, "notation", "string", [
		"standard",
		"scientific",
		"engineering",
		"compact"
	], "standard");
	internalSlots.notation = notation;
	let mnfdDefault;
	let mxfdDefault;
	if (style === "currency" && notation === "standard") {
		const currency = internalSlots.currency;
		const cDigits = CurrencyDigits(currency, { currencyDigitsData });
		mnfdDefault = cDigits;
		mxfdDefault = cDigits;
	} else {
		mnfdDefault = 0;
		mxfdDefault = style === "percent" ? 0 : 3;
	}
	SetNumberFormatDigitOptions(internalSlots, options, mnfdDefault, mxfdDefault, notation);
	const compactDisplay = GetOption(options, "compactDisplay", "string", ["short", "long"], "short");
	let defaultUseGrouping = "auto";
	if (notation === "compact") {
		internalSlots.compactDisplay = compactDisplay;
		defaultUseGrouping = "min2";
	}
	internalSlots.useGrouping = GetStringOrBooleanOption(options, "useGrouping", [
		"min2",
		"auto",
		"always"
	], "always", false, defaultUseGrouping);
	internalSlots.signDisplay = GetOption(options, "signDisplay", "string", [
		"auto",
		"never",
		"always",
		"exceptZero",
		"negative"
	], "auto");
	return nf;
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
//#region packages/ecma262-abstract/ToPrimitive.js
function invariant(condition, message, Err = Error) {
	if (!condition) throw new Err(message);
}
function IsCallable(fn) {
	return typeof fn === "function";
}
function OrdinaryToPrimitive(O, hint) {
	let methodNames;
	if (hint === "string") methodNames = ["toString", "valueOf"];
	else methodNames = ["valueOf", "toString"];
	for (const name of methodNames) {
		const method = O[name];
		if (IsCallable(method)) {
			let result = method.call(O);
			if (typeof result !== "object") return result;
		}
	}
	throw new TypeError("Cannot convert object to primitive value");
}
/**
* https://tc39.es/ecma262/#sec-toprimitive
*/
function ToPrimitive(input, preferredType) {
	if (typeof input === "object" && input != null) {
		const exoticToPrim = Symbol.toPrimitive in input ? input[Symbol.toPrimitive] : void 0;
		let hint;
		if (exoticToPrim !== void 0) {
			if (preferredType === void 0) hint = "default";
			else if (preferredType === "string") hint = "string";
			else {
				invariant(preferredType === "number", "preferredType must be \"string\" or \"number\"");
				hint = "number";
			}
			let result = exoticToPrim.call(input, hint);
			if (typeof result !== "object") return result;
			throw new TypeError("Cannot convert exotic object to primitive.");
		}
		if (preferredType === void 0) preferredType = "number";
		return OrdinaryToPrimitive(input, preferredType);
	}
	return input;
}
//#endregion
//#region packages/ecma402-abstract/ToIntlMathematicalValue.js
/**
* https://tc39.es/ecma402/#sec-tointlmathematicalvalue
* Converts input to a mathematical value, supporting BigInt
*/
function ToIntlMathematicalValue(input) {
	if (typeof input === "bigint") return new Decimal(input.toString());
	let primValue = ToPrimitive(input, "number");
	if (typeof primValue === "symbol") throw new TypeError("Cannot convert a Symbol value to a number");
	if (primValue === void 0) return new Decimal(NaN);
	if (primValue === true) return new Decimal(1);
	if (primValue === false) return new Decimal(0);
	if (primValue === null) return new Decimal(0);
	try {
		return new Decimal(primValue);
	} catch {
		return new Decimal(NaN);
	}
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.number@0.0.0/node_modules/@formatjs_generated/cldr.number/currency-digits.js
const currencyDigitsData = {
	"ADP": 0,
	"AFN": 0,
	"ALL": 0,
	"AMD": 2,
	"BHD": 3,
	"BIF": 0,
	"BYN": 2,
	"BYR": 0,
	"CAD": 2,
	"CHF": 2,
	"CLF": 4,
	"CLP": 0,
	"COP": 0,
	"CRC": 2,
	"CZK": 2,
	"DEFAULT": 2,
	"DJF": 0,
	"DKK": 2,
	"ESP": 0,
	"GNF": 0,
	"GYD": 2,
	"HUF": 0,
	"IDR": 0,
	"IQD": 0,
	"IRR": 0,
	"ISK": 0,
	"ITL": 0,
	"JOD": 3,
	"JPY": 0,
	"KMF": 0,
	"KPW": 0,
	"KRW": 0,
	"KWD": 3,
	"LAK": 0,
	"LBP": 0,
	"LUF": 0,
	"LYD": 3,
	"MGA": 0,
	"MGF": 0,
	"MMK": 0,
	"MNT": 2,
	"MRO": 0,
	"MUR": 2,
	"NOK": 2,
	"OMR": 3,
	"PKR": 0,
	"PYG": 0,
	"RSD": 2,
	"RWF": 0,
	"SEK": 2,
	"SLE": 2,
	"SLL": 0,
	"SOS": 0,
	"STD": 0,
	"SYP": 0,
	"TMM": 0,
	"TND": 3,
	"TRL": 0,
	"TWD": 2,
	"TZS": 2,
	"UGX": 0,
	"UYI": 0,
	"UYW": 4,
	"UZS": 2,
	"VEF": 2,
	"VND": 0,
	"VUV": 0,
	"XAD": 2,
	"XAF": 0,
	"XAU": 2,
	"XOF": 0,
	"XPF": 0,
	"YER": 0,
	"ZMK": 0,
	"ZWD": 0
};
//#endregion
//#region packages/intl-numberformat/get_internal_slots.ts
const internalSlotMap = /* @__PURE__ */ new WeakMap();
function getInternalSlots(x, initialize = false) {
	let internalSlots = internalSlotMap.get(x);
	if (!internalSlots && initialize) {
		internalSlots = Object.create(null);
		internalSlotMap.set(x, internalSlots);
	}
	if (!internalSlots) throw new TypeError("Receiver is not an initialized Intl.NumberFormat");
	return internalSlots;
}
//#endregion
//#region packages/intl-numberformat/core.ts
const RESOLVED_OPTIONS_KEYS = [
	"locale",
	"numberingSystem",
	"style",
	"currency",
	"currencyDisplay",
	"currencySign",
	"unit",
	"unitDisplay",
	"minimumIntegerDigits",
	"minimumFractionDigits",
	"maximumFractionDigits",
	"minimumSignificantDigits",
	"maximumSignificantDigits",
	"useGrouping",
	"notation",
	"compactDisplay",
	"signDisplay",
	"roundingIncrement",
	"roundingMode"
];
/**
* https://tc39.es/ecma402/#sec-intl-numberformat-constructor
*/
const NumberFormat = function(locales, options) {
	if (!this || !OrdinaryHasInstance(NumberFormat, this)) return new NumberFormat(locales, options);
	InitializeNumberFormat(this, locales, options, {
		getInternalSlots: (nf) => getInternalSlots(nf, true),
		localeData: NumberFormat.localeData,
		availableLocales: NumberFormat.availableLocales,
		getDefaultLocale: NumberFormat.getDefaultLocale,
		currencyDigitsData
	});
	const internalSlots = getInternalSlots(this);
	const dataLocale = internalSlots.dataLocale;
	const dataLocaleData = NumberFormat.localeData[dataLocale];
	invariant$1(dataLocaleData !== void 0, `Cannot load locale-dependent data for ${dataLocale}.`);
	internalSlots.pl = createMemoizedPluralRules(dataLocale, Object.assign(Object.create(null), {
		minimumFractionDigits: internalSlots.minimumFractionDigits,
		maximumFractionDigits: internalSlots.maximumFractionDigits,
		minimumIntegerDigits: internalSlots.minimumIntegerDigits,
		minimumSignificantDigits: internalSlots.minimumSignificantDigits,
		maximumSignificantDigits: internalSlots.maximumSignificantDigits
	}));
	return this;
};
const { formatToParts, formatRange, formatRangeToParts } = {
	formatToParts(x) {
		getInternalSlots(this);
		return FormatNumericToParts(this, ToIntlMathematicalValue(x), { getInternalSlots });
	},
	formatRange(start, end) {
		getInternalSlots(this);
		if (start === void 0 || end === void 0) throw new TypeError("Range endpoints must not be undefined");
		return FormatNumericRange(this, ToIntlMathematicalValue(start), ToIntlMathematicalValue(end), { getInternalSlots });
	},
	formatRangeToParts(start, end) {
		getInternalSlots(this);
		if (start === void 0 || end === void 0) throw new TypeError("Range endpoints must not be undefined");
		return FormatNumericRangeToParts(this, ToIntlMathematicalValue(start), ToIntlMathematicalValue(end), { getInternalSlots });
	}
};
defineProperty(NumberFormat.prototype, "formatToParts", { value: formatToParts });
defineProperty(NumberFormat.prototype, "formatRange", { value: formatRange });
defineProperty(NumberFormat.prototype, "formatRangeToParts", { value: formatRangeToParts });
const { resolvedOptions } = { resolvedOptions() {
	const internalSlots = getInternalSlots(this);
	const ro = {};
	for (const key of RESOLVED_OPTIONS_KEYS) {
		const value = internalSlots[key];
		if (value !== void 0) ro[key] = value;
	}
	if (internalSlots.roundingType === "morePrecision") ro.roundingPriority = "morePrecision";
	else if (internalSlots.roundingType === "lessPrecision") ro.roundingPriority = "lessPrecision";
	else ro.roundingPriority = "auto";
	ro.trailingZeroDisplay = internalSlots.trailingZeroDisplay;
	return ro;
} };
defineProperty(NumberFormat.prototype, "resolvedOptions", { value: resolvedOptions });
const formatDescriptor = {
	enumerable: false,
	configurable: true,
	get() {
		const internalSlots = getInternalSlots(this);
		let boundFormat = internalSlots.boundFormat;
		if (boundFormat === void 0) {
			boundFormat = (value) => FormatNumeric(internalSlots, ToIntlMathematicalValue(value));
			try {
				Object.defineProperty(boundFormat, "name", {
					configurable: true,
					enumerable: false,
					writable: false,
					value: ""
				});
			} catch {}
			internalSlots.boundFormat = boundFormat;
		}
		return boundFormat;
	}
};
try {
	Object.defineProperty(formatDescriptor.get, "name", {
		configurable: true,
		enumerable: false,
		writable: false,
		value: "get format"
	});
} catch {}
Object.defineProperty(NumberFormat.prototype, "format", formatDescriptor);
const { supportedLocalesOf } = { supportedLocalesOf(locales, options) {
	return SupportedLocales(NumberFormat.availableLocales, CanonicalizeLocaleList(locales), options);
} };
defineProperty(NumberFormat, "supportedLocalesOf", { value: supportedLocalesOf });
NumberFormat.__addLocaleData = function __addLocaleData(...data) {
	for (const { data: source, locale } of data) {
		const d = source.numbers.aliases ? {
			...source,
			numbers: {
				...source.numbers,
				symbols: { ...source.numbers.symbols },
				decimal: { ...source.numbers.decimal },
				percent: { ...source.numbers.percent },
				currency: { ...source.numbers.currency }
			}
		} : source;
		for (const system of Object.keys(source.numbers.aliases || {})) {
			const target = source.numbers.aliases[system];
			d.numbers.symbols[system] = d.numbers.symbols[target];
			d.numbers.decimal[system] = d.numbers.decimal[target];
			d.numbers.percent[system] = d.numbers.percent[target];
			d.numbers.currency[system] = d.numbers.currency[target];
		}
		registerLocaleData(locale, d, NumberFormat.localeData, NumberFormat.availableLocales);
		if (!NumberFormat.__defaultLocale) NumberFormat.__defaultLocale = locale;
	}
};
NumberFormat.__addUnitData = function __addUnitData(locale, unitsData) {
	const { [locale]: existingData } = NumberFormat.localeData;
	if (!existingData) throw new Error(`Locale data for "${locale}" has not been loaded in NumberFormat. 
Please __addLocaleData before adding additional unit data`);
	for (const unit in unitsData.simple) existingData.units.simple[unit] = unitsData.simple[unit];
	for (const unit in unitsData.compound) existingData.units.compound[unit] = unitsData.compound[unit];
};
NumberFormat.__defaultLocale = "";
NumberFormat.localeData = {};
NumberFormat.availableLocales = /* @__PURE__ */ new Set();
NumberFormat.getDefaultLocale = () => {
	return NumberFormat.__defaultLocale;
};
NumberFormat.polyfilled = true;
try {
	if (typeof Symbol !== "undefined") Object.defineProperty(NumberFormat.prototype, Symbol.toStringTag, {
		configurable: true,
		enumerable: false,
		writable: false,
		value: "Intl.NumberFormat"
	});
	Object.defineProperty(NumberFormat.prototype.constructor, "length", {
		configurable: true,
		enumerable: false,
		writable: false,
		value: 0
	});
	Object.defineProperty(NumberFormat.supportedLocalesOf, "length", {
		configurable: true,
		enumerable: false,
		writable: false,
		value: 1
	});
	Object.defineProperty(NumberFormat, "prototype", {
		configurable: false,
		enumerable: false,
		writable: false,
		value: NumberFormat.prototype
	});
} catch {}
//#endregion
//#region packages/intl-numberformat/to_locale_string.ts
/**
* Number.prototype.toLocaleString and BigInt.prototype.toLocaleString ponyfill
* https://tc39.es/ecma402/#sup-number.prototype.tolocalestring
*/
function toLocaleString(x, locales, options) {
	return new NumberFormat(locales, options).format(x);
}
defineProperty(ensureIntl(), "NumberFormat", { value: NumberFormat });
defineProperty(Number.prototype, "toLocaleString", { value: function toLocaleString$1(locales, options) {
	return toLocaleString(this, locales, options);
} });
if (typeof BigInt !== "undefined") defineProperty(BigInt.prototype, "toLocaleString", { value: function toLocaleString$2(locales, options) {
	return toLocaleString(this, locales, options);
} });
const buf = globalThis.__FORMATJS_NUMBERFORMAT_DATA__;
if (buf) {
	for (const d of buf) NumberFormat.__addLocaleData(d);
	delete globalThis.__FORMATJS_NUMBERFORMAT_DATA__;
}
//#endregion

//# sourceMappingURL=polyfill-force.js.map