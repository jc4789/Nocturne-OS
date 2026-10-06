import { LookupSupportedLocales, ResolveLocale } from "@formatjs/intl-localematcher";
import Decimal$1, { Decimal } from "@formatjs/bigdecimal";
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
//#region packages/ecma262-abstract/ToObject.js
/**
* https://tc39.es/ecma262/#sec-toobject
*/
function ToObject(arg) {
	if (arg == null) throw new TypeError("undefined/null cannot be converted to object");
	return Object(arg);
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
function invariant$1(condition, message, Err = Error) {
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
				invariant$1(preferredType === "number", "preferredType must be \"string\" or \"number\"");
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
function invariant(condition, message, Err = Error) {
	if (!condition) throw new Err(message);
}
memoize((...args) => new Intl.NumberFormat(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.PluralRules(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.ListFormat(...args), { strategy: strategies.variadic });
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
	invariant(VALID_ROUNDING_INCREMENTS.has(roundingIncrement), `Invalid rounding increment value: ${roundingIncrement}.
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
				invariant(mxfd !== void 0, "maximumFractionDigits must be defined");
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
		invariant(internalSlots.roundingType === "fractionDigits", "Invalid roundingType", TypeError);
		invariant(internalSlots.maximumFractionDigits === internalSlots.minimumFractionDigits, "With roundingIncrement > 1, maximumFractionDigits and minimumFractionDigits must be equal.", RangeError);
	}
}
//#endregion
//#region packages/ecma402-abstract/PluralRules/InitializePluralRules.js
function InitializePluralRules(pl, locales, options, { availableLocales, relevantExtensionKeys, localeData, getDefaultLocale, getInternalSlots }) {
	const requestedLocales = CanonicalizeLocaleList(locales);
	const opt = Object.create(null);
	const opts = CoerceOptionsToObject(options);
	const internalSlots = getInternalSlots(pl);
	internalSlots.initializedPluralRules = true;
	opt.localeMatcher = GetOption(opts, "localeMatcher", "string", ["best fit", "lookup"], "best fit");
	const r = ResolveLocale(availableLocales, requestedLocales, opt, relevantExtensionKeys, localeData, getDefaultLocale);
	internalSlots.locale = r.locale;
	internalSlots.type = GetOption(opts, "type", "string", ["cardinal", "ordinal"], "cardinal");
	const notation = GetOption(opts, "notation", "string", [
		"standard",
		"scientific",
		"engineering",
		"compact"
	], "standard");
	internalSlots.notation = notation;
	const compactDisplay = GetOption(opts, "compactDisplay", "string", ["short", "long"], "short");
	if (notation === "compact") {
		internalSlots.compactDisplay = compactDisplay;
		internalSlots.compactExponents = localeData[r.dataLocale]?.compactExponents;
	}
	SetNumberFormatDigitOptions(internalSlots, opts, 0, 3, notation);
	return pl;
}
//#endregion
//#region packages/ecma262-abstract/Type.js
/**
* https://www.ecma-international.org/ecma-262/11.0/index.html#sec-type
*/
function Type(x) {
	if (x === null) return "Null";
	if (typeof x === "undefined") return "Undefined";
	if (typeof x === "function" || typeof x === "object") return "Object";
	if (typeof x === "number") return "Number";
	if (typeof x === "boolean") return "Boolean";
	if (typeof x === "string") return "String";
	if (typeof x === "symbol") return "Symbol";
	if (typeof x === "bigint") return "BigInt";
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
	invariant(r1.lessThan(x) && x.lessThan(r2), `x should be between r1 and r2 but x=${x}, r1=${r1}, r2=${r2}`);
	if (unsignedRoundingMode === "zero") return r1;
	if (unsignedRoundingMode === "infinity") return r2;
	const d1 = x.minus(r1);
	const d2 = r2.minus(x);
	if (d1.lessThan(d2)) return r1;
	if (d2.lessThan(d1)) return r2;
	invariant(d1.eq(d2), "d1 should be equal to d2");
	if (unsignedRoundingMode === "half-zero") return r1;
	if (unsignedRoundingMode === "half-infinity") return r2;
	invariant(unsignedRoundingMode === "half-even", "unsignedRoundingMode should be half-even");
	if (r1.div(r2.minus(r1)).mod(2).isZero()) return r1;
	return r2;
}
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
		invariant(e < 0, "e should be less than 0");
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
		invariant(x.isFinite(), "NumberFormatDigitInternalSlots value is not finite");
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
				invariant(intlObject.roundingType === "lessPrecision", "Invalid roundingType");
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
//#region packages/ecma402-abstract/PluralRules/GetOperands.js
/**
* CLDR operands for the implementation-defined PluralRuleSelect
* ECMA-402 §17.5.1 PluralRuleSelect, decimalString parameter and return definition.
* https://tc39.es/ecma402/#sec-pluralruleselect
* https://github.com/tc39/ecma402/blob/b1c961988b9a07894b1dc3dc2b5626ea48387d61/spec/pluralrules.html#L285-L290
*
* Implementation: Extended to support compact exponent (c/e operands)
*
* @param s Formatted number string
* @param exponent Compact decimal exponent (c/e operand), defaults to 0
*/
function GetOperands(s, exponent = 0) {
	invariant(typeof s === "string", `GetOperands should have been called with a string`);
	const n = new Decimal$1(s);
	invariant(n.isFinite(), "n should be finite");
	let dp = s.indexOf(".");
	let iv;
	let f;
	let v;
	let fv = "";
	if (dp === -1) {
		iv = s;
		f = ZERO;
		v = 0;
	} else {
		iv = s.slice(0, dp);
		fv = s.slice(dp, s.length);
		f = new Decimal$1(fv);
		v = fv.length;
	}
	const i = new Decimal$1(iv).abs();
	let w;
	let t;
	if (!f.isZero()) {
		const ft = fv.replace(/0+$/, "");
		w = ft.length;
		t = new Decimal$1(ft);
	} else {
		w = 0;
		t = ZERO;
	}
	return {
		Number: n,
		IntegerDigits: i.lessThanOrEqualTo(Number.MAX_SAFE_INTEGER) && i.greaterThanOrEqualTo(-Number.MAX_SAFE_INTEGER) ? i.toNumber() : i.toString(),
		NumberOfFractionDigits: v,
		NumberOfFractionDigitsWithoutTrailing: w,
		FractionDigits: f.toNumber(),
		FractionDigitsWithoutTrailing: t.toNumber(),
		CompactExponent: exponent
	};
}
//#endregion
//#region packages/ecma402-abstract/PluralRules/ResolvePlural.js
/**
* ResolvePluralInternal ( pluralRules, n )
*
* Internal version of ResolvePlural that returns both the formatted string and plural category.
* This is needed for selectRange, which must compare formatted strings to determine if the
* start and end values are identical.
*
* The formatted string is obtained by applying the number formatting options (digit options)
* from the PluralRules object to the input number. This ensures that formatting-sensitive
* plural rules work correctly (e.g., rules that depend on visible fraction digits).
*
* @param pl - An initialized PluralRules object
* @param n - Mathematical value to resolve
* @returns Record containing the formatted string and plural category
*/
function ResolvePluralInternal(pl, n, { getInternalSlots, PluralRuleSelect }) {
	const internalSlots = getInternalSlots(pl);
	invariant(Type(internalSlots) === "Object", "pl has to be an object");
	invariant("initializedPluralRules" in internalSlots, "pluralrules must be initialized");
	if (!n.isFinite()) return {
		formattedString: String(n),
		pluralCategory: "other"
	};
	const { locale, type, notation } = internalSlots;
	const s = FormatNumericToString(internalSlots, n).formattedString;
	let exponent = 0;
	if (notation === "compact") {
		const patterns = internalSlots.compactExponents?.[internalSlots.compactDisplay || "short"];
		const integer = (s[0] === "-" ? s.slice(1) : s).split(".")[0];
		let first = 0;
		while (first < integer.length && integer[first] === "0") first++;
		const magnitude = integer.length - first - 1;
		let selectedMagnitude = -1;
		if (patterns) for (const key in patterns) {
			const threshold = Number(key);
			if (threshold <= magnitude && threshold > selectedMagnitude) {
				selectedMagnitude = threshold;
				exponent = patterns[threshold];
			}
		}
	}
	return {
		formattedString: s,
		pluralCategory: PluralRuleSelect(locale, type, n, GetOperands(s, exponent))
	};
}
/**
* http://ecma-international.org/ecma-402/7.0/index.html#sec-resolveplural
* @param pl
* @param n
* @param PluralRuleSelect Has to pass in bc it's implementation-specific
*/
function ResolvePlural(pl, n, { getInternalSlots, PluralRuleSelect }) {
	return ResolvePluralInternal(pl, n, {
		getInternalSlots,
		PluralRuleSelect
	}).pluralCategory;
}
//#endregion
//#region packages/ecma402-abstract/PluralRules/ResolvePluralRange.js
/**
* ResolvePluralRange ( pluralRules, x, y )
*
* The ResolvePluralRange abstract operation is called with arguments pluralRules (which must be
* an object initialized as a PluralRules), x (a mathematical value), and y (a mathematical value).
* It resolves the appropriate plural form for a range by determining the plural forms of both the
* start and end values, then consulting locale-specific range data.
*
* ECMA-402 §17.5.4 ResolvePluralRange, steps 1–3.
* Specification: https://tc39.es/ecma402/#sec-resolvepluralrange
* https://github.com/tc39/ecma402/blob/b1c961988b9a07894b1dc3dc2b5626ea48387d61/spec/pluralrules.html#L356-L358
*
* @param pluralRules - An initialized PluralRules object
* @param x - Mathematical value for the range start
* @param y - Mathematical value for the range end
* @returns The plural category for the range (zero, one, two, few, many, or other)
*/
function ResolvePluralRange(pluralRules, x, y, { getInternalSlots, PluralRuleSelect, PluralRuleSelectRange }) {
	if (x.isNaN() || y.isNaN()) throw new RangeError("selectRange requires start and end values not to be NaN");
	const internalSlots = getInternalSlots(pluralRules);
	invariant(Type(internalSlots) === "Object", "pluralRules has to be an object");
	invariant("initializedPluralRules" in internalSlots, "pluralrules must be initialized");
	const xp = ResolvePluralInternal(pluralRules, x, {
		getInternalSlots,
		PluralRuleSelect
	});
	const yp = ResolvePluralInternal(pluralRules, y, {
		getInternalSlots,
		PluralRuleSelect
	});
	if (xp.formattedString === yp.formattedString) return xp.pluralCategory;
	const { locale, type } = internalSlots;
	return PluralRuleSelectRange(locale, type, xp.pluralCategory, yp.pluralCategory);
}
//#endregion
//#region packages/intl-pluralrules/get_internal_slots.ts
const internalSlotMap = /* @__PURE__ */ new WeakMap();
function getInternalSlots(x) {
	let internalSlots = internalSlotMap.get(x);
	if (!internalSlots) {
		internalSlots = Object.create(null);
		internalSlotMap.set(x, internalSlots);
	}
	return internalSlots;
}
//#endregion
//#region packages/intl-pluralrules/index.ts
function validateInstance(instance, method) {
	if (!(instance instanceof PluralRules)) throw new TypeError(`Method Intl.PluralRules.prototype.${method} called on incompatible receiver ${String(instance)}`);
}
/**
* http://ecma-international.org/ecma-402/7.0/index.html#sec-pluralruleselect
* @param locale
* @param type
* @param _n
* @param param3
*/
function PluralRuleSelect(locale, type, _n, { IntegerDigits, NumberOfFractionDigits, FractionDigits, CompactExponent }) {
	return PluralRules.localeData[locale].fn(NumberOfFractionDigits ? `${IntegerDigits}.${FractionDigits}` : String(IntegerDigits), type === "ordinal", CompactExponent);
}
/**
* PluralRuleSelectRange ( locale, type, notation, compactDisplay, start, end )
*
* Implementation-defined abstract operation that determines the plural category for a range
* by consulting CLDR plural range data. Each locale defines how different combinations of
* start and end plural categories map to a range plural category.
*
* Examples from CLDR:
* - English: "one" + "other" → "other" (e.g., "1-2 items")
* - French: "one" + "one" → "one" (e.g., "0-1 vue")
* - Arabic: "few" + "many" → "many" (e.g., complex range rules)
*
* The spec allows this to be implementation-defined, and we use CLDR supplemental data
* from pluralRanges.json which provides explicit mappings for each locale.
*
* @param locale - BCP 47 locale identifier
* @param type - "cardinal" or "ordinal"
* @param xp - Start plural category
* @param yp - End plural category
* @returns The plural category for the range
*/
function PluralRuleSelectRange(locale, type, xp, yp) {
	const localeData = PluralRules.localeData[locale];
	if (!localeData || !localeData.pluralRanges) return yp;
	const key = `${xp}_${yp}`;
	return (type === "ordinal" ? localeData.pluralRanges.ordinal : localeData.pluralRanges.cardinal)?.[key] ?? yp;
}
var PluralRules = class PluralRules {
	constructor(locales, options) {
		if (!(this && this instanceof PluralRules ? this.constructor : void 0)) throw new TypeError("Intl.PluralRules must be called with 'new'");
		return InitializePluralRules(this, locales, options, {
			availableLocales: PluralRules.availableLocales,
			relevantExtensionKeys: PluralRules.relevantExtensionKeys,
			localeData: PluralRules.localeData,
			getDefaultLocale: PluralRules.getDefaultLocale,
			getInternalSlots
		});
	}
	resolvedOptions() {
		validateInstance(this, "resolvedOptions");
		const opts = {};
		const internalSlots = getInternalSlots(this);
		opts.locale = internalSlots.locale;
		opts.type = internalSlots.type;
		[
			"notation",
			"compactDisplay",
			"minimumIntegerDigits",
			"minimumFractionDigits",
			"maximumFractionDigits",
			"minimumSignificantDigits",
			"maximumSignificantDigits"
		].forEach((field) => {
			const val = internalSlots[field];
			if (val !== void 0) opts[field] = val;
		});
		const categories = PluralRules.localeData[internalSlots.locale].categories[internalSlots.type];
		opts.pluralCategories = [
			"zero",
			"one",
			"two",
			"few",
			"many",
			"other"
		].filter((category) => categories.includes(category));
		for (const field of [
			"roundingIncrement",
			"roundingMode",
			"roundingPriority",
			"trailingZeroDisplay"
		]) opts[field] = internalSlots[field];
		return opts;
	}
	select(val) {
		validateInstance(this, "select");
		const n = ToIntlMathematicalValue(val);
		return ResolvePlural(this, n, {
			getInternalSlots,
			PluralRuleSelect
		});
	}
	/**
	* Intl.PluralRules.prototype.selectRange ( start, end )
	*
	* Returns a string indicating which plural rule applies to a range of numbers.
	* This is useful for formatting ranges like "1-2 items" vs "2-3 items" where
	* different languages have different plural rules for ranges.
	*
	* Specification: https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.selectrange
	*
	* @param start - The start value of the range (number or bigint)
	* @param end - The end value of the range (number or bigint)
	* @returns The plural category for the range (zero, one, two, few, many, or other)
	*
	* @example
	* const pr = new Intl.PluralRules('en');
	* pr.selectRange(1, 2); // "other" (English: "1-2 items")
	* pr.selectRange(1, 1); // "one" (same value: "1 item")
	*
	* @example
	* const prFr = new Intl.PluralRules('fr');
	* prFr.selectRange(0, 1); // "one" (French: "0-1 vue")
	* prFr.selectRange(1, 2); // "other" (French: "1-2 vues")
	*
	* @example
	* // BigInt support (spec-compliant, but Chrome has a bug as of early 2025)
	* pr.selectRange(BigInt(1), BigInt(2)); // "other"
	*
	* @throws {TypeError} If start or end is undefined
	* @throws {RangeError} If start or end converts to NaN
	*
	* @note Chrome's native implementation (as of early 2025) has a bug where it throws
	* "Cannot convert a BigInt value to a number" when using BigInt arguments. This is
	* a browser bug - the spec requires BigInt support. This polyfill handles BigInt correctly.
	*/
	selectRange(start, end) {
		validateInstance(this, "selectRange");
		if (start === void 0 || end === void 0) throw new TypeError("selectRange requires both start and end arguments");
		const x = ToIntlMathematicalValue(start);
		const y = ToIntlMathematicalValue(end);
		return ResolvePluralRange(this, x, y, {
			getInternalSlots,
			PluralRuleSelect,
			PluralRuleSelectRange
		});
	}
	toString() {
		return "[object Intl.PluralRules]";
	}
	static supportedLocalesOf(locales, options) {
		return SupportedLocales(PluralRules.availableLocales, CanonicalizeLocaleList(locales), options);
	}
	static __addLocaleData(...data) {
		for (const { data: d, locale } of data) {
			registerLocaleData(locale, d, PluralRules.localeData, PluralRules.availableLocales);
			if (!PluralRules.__defaultLocale) PluralRules.__defaultLocale = locale;
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
		return PluralRules.__defaultLocale;
	}
	static {
		this.relevantExtensionKeys = [];
	}
	static {
		this.polyfilled = true;
	}
};
try {
	if (typeof Symbol !== "undefined") Object.defineProperty(PluralRules.prototype, Symbol.toStringTag, {
		value: "Intl.PluralRules",
		writable: false,
		enumerable: false,
		configurable: true
	});
	try {
		Object.defineProperty(PluralRules, "length", {
			value: 0,
			writable: false,
			enumerable: false,
			configurable: true
		});
	} catch {}
	Object.defineProperty(PluralRules.prototype.constructor, "length", {
		value: 0,
		writable: false,
		enumerable: false,
		configurable: true
	});
	Object.defineProperty(PluralRules.supportedLocalesOf, "length", {
		value: 1,
		writable: false,
		enumerable: false,
		configurable: true
	});
	Object.defineProperty(PluralRules, "name", {
		value: "PluralRules",
		writable: false,
		enumerable: false,
		configurable: true
	});
} catch {}
defineProperty(ensureIntl(), "PluralRules", { value: PluralRules });
const buf = globalThis.__FORMATJS_PLURALRULES_DATA__;
if (buf) {
	for (const d of buf) PluralRules.__addLocaleData(d);
	delete globalThis.__FORMATJS_PLURALRULES_DATA__;
}
//#endregion

//# sourceMappingURL=polyfill-force.js.map