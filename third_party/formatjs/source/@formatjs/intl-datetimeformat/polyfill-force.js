import Decimal$1, { Decimal } from "@formatjs/bigdecimal";
import { LookupSupportedLocales, ResolveLocale } from "@formatjs/intl-localematcher";
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
/**
* 7.3.5 CreateDataProperty
* @param target
* @param name
* @param value
*/
function createDataProperty(target, name, value) {
	Object.defineProperty(target, name, {
		configurable: true,
		enumerable: true,
		writable: true,
		value
	});
}
function invariant(condition, message, Err = Error) {
	if (!condition) throw new Err(message);
}
const createMemoizedNumberFormat = memoize((...args) => new Intl.NumberFormat(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.PluralRules(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.ListFormat(...args), { strategy: strategies.variadic });
//#endregion
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
function IsCallable(fn) {
	return typeof fn === "function";
}
/**
* https://tc39.es/ecma262/#sec-ordinaryhasinstance
*/
function OrdinaryHasInstance(C, O, internalSlots) {
	if (!IsCallable(C)) return false;
	if (internalSlots?.boundTargetFunction) return O instanceof internalSlots?.boundTargetFunction;
	if (typeof O !== "object") return false;
	let P = C.prototype;
	if (typeof P !== "object") throw new TypeError("OrdinaryHasInstance called on an object with an invalid prototype property.");
	return Object.prototype.isPrototypeOf.call(P, O);
}
//#endregion
//#region packages/ecma262-abstract/ToNumber.js
/**
* ECMA-262 §7.1.4 ToNumber, steps 2, 8–10.
* https://tc39.es/ecma262/#sec-tonumber
* https://github.com/tc39/ecma262/blob/dcf59856a8184792a9e42f0ffb7dc064094a5dcc/spec.html#L5183-L5191
*/
function ToNumber(arg) {
	return new Decimal(+arg);
}
//#endregion
//#region packages/ecma262-abstract/DateOperations.js
const MS_PER_DAY = 864e5;
function mod(x, y) {
	return x - Math.floor(x / y) * y;
}
function Day(t) {
	return Math.floor(t / MS_PER_DAY);
}
function WeekDay(t) {
	return mod(Day(t) + 4, 7);
}
function DayFromYear(y) {
	return 365 * (y - 1970) + Math.floor((y - 1969) / 4) - Math.floor((y - 1901) / 100) + Math.floor((y - 1601) / 400);
}
function YearFromTime(t) {
	if (!Number.isFinite(t)) return NaN;
	const day = Day(t);
	let year = 1970 + Math.floor(day / 365.2425);
	while (DayFromYear(year) > day) year--;
	while (DayFromYear(year + 1) <= day) year++;
	return year;
}
function DaysInYear(y) {
	if (y % 4 !== 0) return 365;
	if (y % 100 !== 0) return 366;
	if (y % 400 !== 0) return 365;
	return 366;
}
function DayWithinYear(t) {
	return Day(t) - DayFromYear(YearFromTime(t));
}
function InLeapYear(t) {
	return DaysInYear(YearFromTime(t)) === 365 ? 0 : 1;
}
function MonthFromTime(t) {
	const dwy = DayWithinYear(t);
	const leap = InLeapYear(t);
	if (dwy >= 0 && dwy < 31) return 0;
	if (dwy < 59 + leap) return 1;
	if (dwy < 90 + leap) return 2;
	if (dwy < 120 + leap) return 3;
	if (dwy < 151 + leap) return 4;
	if (dwy < 181 + leap) return 5;
	if (dwy < 212 + leap) return 6;
	if (dwy < 243 + leap) return 7;
	if (dwy < 273 + leap) return 8;
	if (dwy < 304 + leap) return 9;
	if (dwy < 334 + leap) return 10;
	if (dwy < 365 + leap) return 11;
	throw new Error("Invalid time");
}
function DateFromTime(t) {
	const dwy = DayWithinYear(t);
	const mft = MonthFromTime(t);
	const leap = InLeapYear(t);
	if (mft === 0) return dwy + 1;
	if (mft === 1) return dwy - 30;
	if (mft === 2) return dwy - 58 - leap;
	if (mft === 3) return dwy - 89 - leap;
	if (mft === 4) return dwy - 119 - leap;
	if (mft === 5) return dwy - 150 - leap;
	if (mft === 6) return dwy - 180 - leap;
	if (mft === 7) return dwy - 211 - leap;
	if (mft === 8) return dwy - 242 - leap;
	if (mft === 9) return dwy - 272 - leap;
	if (mft === 10) return dwy - 303 - leap;
	if (mft === 11) return dwy - 333 - leap;
	throw new Error("Invalid time");
}
const HOURS_PER_DAY = 24;
const MINUTES_PER_HOUR = 60;
const SECONDS_PER_MINUTE = 60;
const MS_PER_SECOND = 1e3;
const MS_PER_MINUTE = MS_PER_SECOND * SECONDS_PER_MINUTE;
const MS_PER_HOUR = MS_PER_MINUTE * MINUTES_PER_HOUR;
function HourFromTime(t) {
	return mod(Math.floor(t / MS_PER_HOUR), HOURS_PER_DAY);
}
function MinFromTime(t) {
	return mod(Math.floor(t / MS_PER_MINUTE), MINUTES_PER_HOUR);
}
function SecFromTime(t) {
	return mod(Math.floor(t / MS_PER_SECOND), SECONDS_PER_MINUTE);
}
function msFromTime(t) {
	return mod(t, MS_PER_SECOND);
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/TemporalDateTime.js
const temporal = globalThis.Temporal;
const readers = temporal ? [
	"PlainDate",
	"PlainYearMonth",
	"PlainMonthDay",
	"PlainTime",
	"PlainDateTime",
	"Instant",
	"ZonedDateTime"
].map((kind) => ({
	kind,
	read: kind === "Instant" || kind === "ZonedDateTime" ? Object.getOwnPropertyDescriptor(temporal[kind].prototype, "epochNanoseconds").get : Object.getOwnPropertyDescriptor(temporal[kind].prototype, "toString").value
})) : [];
const stringOptions = Object.assign(Object.create(null), { calendarName: "always" });
const CALENDAR_ANNOTATION = /\[u-ca=([^\]]+)\]/;
const ISO_DATE = /^([+-]\d{6}|\d{4})-(\d{2})-(\d{2})/;
const ISO_TIME = /(?:^|T)(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?/;
const dateGetTime = Date.prototype.getTime;
const monthStarts = [
	0,
	31,
	59,
	90,
	120,
	151,
	181,
	212,
	243,
	273,
	304,
	334
];
function ToDateTimeFormattable(value) {
	if (value !== null && typeof value === "object" && readers.length) {
		let isDate = false;
		try {
			dateGetTime.call(value);
			isDate = true;
		} catch {}
		if (isDate) return ToNumber(value);
		for (const { kind, read } of readers) {
			let result;
			try {
				result = read.call(value, stringOptions);
			} catch {
				continue;
			}
			if (kind === "Instant" || kind === "ZonedDateTime") return {
				kind,
				epochNanoseconds: BigInt(result)
			};
			const text = String(result);
			const calendar = CALENDAR_ANNOTATION.exec(text)?.[1];
			const date = ISO_DATE.exec(text);
			const time = ISO_TIME.exec(text);
			let epochNanoseconds = BigInt(0);
			if (date) {
				const year = Number(date[1]);
				const month = Number(date[2]);
				const days = DayFromYear(year) + monthStarts[month - 1] + Number(date[3]) - 1 + (month > 2 && DaysInYear(year) === 366 ? 1 : 0);
				epochNanoseconds = BigInt(days) * BigInt(864e11);
			}
			if (time) epochNanoseconds += BigInt(Number(time[1]) * 3600 + Number(time[2]) * 60 + Number(time[3])) * BigInt(1e9) + BigInt(((time[4] || "") + "000000000").slice(0, 9));
			else epochNanoseconds += BigInt(432e11);
			return {
				kind,
				calendar,
				epochNanoseconds
			};
		}
	}
	return ToNumber(value);
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
//#region packages/ecma402-abstract/CanonicalizeTimeZoneName.js
const OFFSET_TIMEZONE_PREFIX_REGEX$2 = /^[+-]/;
const OFFSET_TIMEZONE_FORMAT_REGEX$2 = /^([+-])(\d{2})(?::?(\d{2}))?(?::?(\d{2}))?(?:\.(\d{1,9}))?$/;
const TRAILING_ZEROS_REGEX = /0+$/;
/**
* IsTimeZoneOffsetString ( offsetString )
* https://tc39.es/ecma262/#sec-istimezoneoffsetstring
*
* Simplified check to determine if a string is a UTC offset identifier.
*
* @param offsetString - The string to check
* @returns true if offsetString starts with '+' or '-'
*/
function IsTimeZoneOffsetString$1(offsetString) {
	return OFFSET_TIMEZONE_PREFIX_REGEX$2.test(offsetString);
}
/**
* ParseTimeZoneOffsetString ( offsetString )
* https://tc39.es/ecma262/#sec-parsetimezoneoffsetstring
*
* Parses a UTC offset string and returns its canonical representation.
* Normalizes various formats (±HH, ±HHMM, ±HH:MM, etc.) to ±HH:MM format.
*
* @param offsetString - The UTC offset string to parse
* @returns The canonical offset string in ±HH:MM format (with :SS.sss if non-zero)
*/
function ParseTimeZoneOffsetString$1(offsetString) {
	const match = OFFSET_TIMEZONE_FORMAT_REGEX$2.exec(offsetString);
	if (!match) return offsetString;
	const hours = match[2];
	const minutes = match[3] ? match[3] : "00";
	const seconds = match[4];
	const fractional = match[5];
	let canonical = `${Number(hours) || Number(minutes) || Number(seconds) || Number(fractional) ? match[1] : "+"}${hours}:${minutes}`;
	if (seconds && (parseInt(seconds, 10) !== 0 || fractional)) {
		canonical += `:${seconds}`;
		if (fractional) {
			const trimmedFractional = fractional.replace(TRAILING_ZEROS_REGEX, "");
			if (trimmedFractional) canonical += `.${trimmedFractional}`;
		}
	}
	return canonical;
}
/**
* CanonicalizeTimeZoneName ( timeZone )
* https://tc39.es/ecma402/#sec-canonicalizetimezonename
*
* Extended to support UTC offset time zones per ECMA-402 PR #788 (ES2026).
* Returns the canonical and case-regularized form of a timezone identifier.
*
* @param tz - The timezone identifier to canonicalize
* @param implDetails - Implementation details containing timezone data
* @returns The canonical timezone identifier
*/
function CanonicalizeTimeZoneName(tz, { zoneNames, uppercaseLinks }) {
	if (IsTimeZoneOffsetString$1(tz)) return ParseTimeZoneOffsetString$1(tz);
	const uppercasedTz = tz.toUpperCase();
	const uppercasedZones = zoneNames.reduce((all, z) => {
		all[z.toUpperCase()] = z;
		return all;
	}, {});
	const ianaTimeZone = uppercaseLinks[uppercasedTz] || uppercasedZones[uppercasedTz];
	if (ianaTimeZone === "Etc/UTC" || ianaTimeZone === "Etc/GMT") return "UTC";
	return ianaTimeZone;
}
//#endregion
//#region packages/ecma402-abstract/IsValidTimeZoneName.js
const OFFSET_TIMEZONE_PREFIX_REGEX$1 = /^[+-]/;
const OFFSET_TIMEZONE_FORMAT_REGEX$1 = /^([+-])(\d{2})(?::?(\d{2}))?$/;
/**
* IsValidDateTimeFormatOffset ( offsetString )
* ECMA-402 §11.1.2 CreateDateTimeFormat, step 19.c.
* https://tc39.es/ecma402/#sec-createdatetimeformat
* https://github.com/tc39/ecma402/blob/b1c961988b9a07894b1dc3dc2b5626ea48387d61/spec/datetimeformat.html#L94
*
* Validates whether a string represents a valid UTC offset timezone.
* Supports DateTimeFormat offsets: ±HH, ±HHMM, ±HH:MM
*
* @param offsetString - The string to validate as a timezone offset
* @returns true if offsetString is a valid UTC offset format
*/
function IsValidDateTimeFormatOffset(offsetString) {
	if (!OFFSET_TIMEZONE_PREFIX_REGEX$1.test(offsetString)) return false;
	const match = OFFSET_TIMEZONE_FORMAT_REGEX$1.exec(offsetString);
	if (!match) return false;
	const hours = parseInt(match[2], 10);
	const minutes = match[3] ? parseInt(match[3], 10) : 0;
	if (hours > 23 || minutes > 59) return false;
	return true;
}
/**
* IsValidTimeZoneName ( timeZone )
* https://tc39.es/ecma402/#sec-isvalidtimezonename
*
* Extended to support UTC offset time zones per ECMA-402 PR #788 (ES2026).
* The abstract operation validates both:
* 1. UTC offset identifiers (e.g., "+01:00", "-05:30")
* 2. Available named time zone identifiers from IANA Time Zone Database
*
* @param tz - The timezone identifier to validate
* @param implDetails - Implementation details containing timezone data
* @returns true if timeZone is a valid identifier
*/
function IsValidTimeZoneName(tz, { zoneNamesFromData, uppercaseLinks }) {
	if (IsValidDateTimeFormatOffset(tz)) return true;
	for (let i = 0; i < tz.length; i++) if (tz.charCodeAt(i) > 127) return false;
	const uppercasedTz = tz.toUpperCase();
	const zoneNames = /* @__PURE__ */ new Set();
	const linkNames = /* @__PURE__ */ new Set();
	zoneNamesFromData.map((z) => z.toUpperCase()).forEach((z) => zoneNames.add(z));
	Object.keys(uppercaseLinks).forEach((linkName) => {
		linkNames.add(linkName.toUpperCase());
		zoneNames.add(uppercaseLinks[linkName].toUpperCase());
	});
	if (zoneNames.has(uppercasedTz) || linkNames.has(uppercasedTz)) return true;
	return false;
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
//#region packages/ecma402-abstract/types/date-time.js
let RangePatternType = /* @__PURE__ */ function(RangePatternType) {
	RangePatternType["startRange"] = "startRange";
	RangePatternType["shared"] = "shared";
	RangePatternType["endRange"] = "endRange";
	return RangePatternType;
}({});
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
//#region packages/ecma262-abstract/TimeClip.js
const ZERO = new Decimal(0);
function ToInteger(n) {
	const number = ToNumber(n);
	if (number.isNaN() || number.isZero()) return ZERO;
	if (!number.isFinite()) return number;
	let integer = number.abs().floor();
	if (number.isNegative()) integer = integer.negated();
	return integer.isZero() ? ZERO : integer;
}
/**
* https://tc39.es/ecma262/#sec-timeclip
*/
function TimeClip(time) {
	if (!time.isFinite()) return new Decimal(NaN);
	if (time.abs().greaterThan(864e13)) return new Decimal(NaN);
	return ToInteger(time);
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/HandleDateTimeValue.js
function isTemporalDateTimeValue(value) {
	return Object.prototype.hasOwnProperty.call(value, "kind");
}
/** https://tc39.es/proposal-temporal/#sec-temporal-handledatetimevalue */
function HandleDateTimeValue(slots, value) {
	if (!isTemporalDateTimeValue(value)) {
		const clipped = TimeClip(value);
		if (clipped.isNaN()) throw new RangeError("Invalid time");
		return {
			format: slots,
			epochNanoseconds: BigInt(clipped.toNumber()) * BigInt(1e6),
			isPlain: false
		};
	}
	if (value.kind === "ZonedDateTime") throw new TypeError("Temporal.ZonedDateTime is not supported by DateTimeFormat");
	if (value.calendar !== void 0 && value.calendar !== slots.calendar && (value.calendar !== "iso8601" || value.kind === "PlainYearMonth" || value.kind === "PlainMonthDay")) throw new RangeError("Temporal calendar does not match DateTimeFormat");
	return {
		format: slots.getTemporalFormat(value.kind),
		epochNanoseconds: value.epochNanoseconds,
		isPlain: value.kind !== "Instant"
	};
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/CalendarDateFromTime.js
/**
* FormatJS helper, not a named ECMA-402 abstract operation. Converts an already
* timezone-adjusted millisecond value into calendar fields for ToLocalTime.
* ECMA-402 leaves non-Gregorian calculations to the best available calendar data:
* https://tc39.es/ecma402/#sec-tolocaltime
*
* Optional calendar modules supply arithmetic and ICU4X tables. Keeping them
* outside this module lets Gregorian-only applications omit those payloads.
*/
function CalendarDateFromTime(t, calendar, calendarData = {}) {
	if (calendar === "gregory" || calendar === "iso8601") {
		const year = YearFromTime(t);
		return {
			era: year > 0 ? "AD" : "BC",
			year: year > 0 ? year : 1 - year,
			month: MonthFromTime(t),
			day: DateFromTime(t)
		};
	}
	if (!Object.prototype.hasOwnProperty.call(calendarData, calendar)) throw new RangeError(`Calendar data not loaded: ${calendar}`);
	return calendarData[calendar](t);
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/ToLocalTime.js
const OFFSET_TIMEZONE_PREFIX_REGEX = /^[+-]/;
const OFFSET_TIMEZONE_FORMAT_REGEX = /^([+-])(\d{2})(?::?(\d{2}))?(?::?(\d{2}))?(?:\.(\d{1,9}))?$/;
/**
* IsTimeZoneOffsetString ( offsetString )
* https://tc39.es/ecma262/#sec-istimezoneoffsetstring
*
* Determines if a string is a UTC offset identifier.
*
* @param offsetString - The string to check
* @returns true if offsetString is a UTC offset format
*/
function IsTimeZoneOffsetString(offsetString) {
	return OFFSET_TIMEZONE_PREFIX_REGEX.test(offsetString);
}
/**
* ParseTimeZoneOffsetString ( offsetString )
* https://tc39.es/ecma262/#sec-parsetimezoneoffsetstring
*
* Parses a UTC offset string and returns the offset in milliseconds.
* This is used to calculate the timezone offset for ToLocalTime.
*
* Supports formats: ±HH, ±HHMM, ±HH:MM, ±HH:MM:SS, ±HH:MM:SS.sss
*
* @param offsetString - The UTC offset string to parse (e.g., "+01:00")
* @returns The offset in milliseconds
*/
function ParseTimeZoneOffsetString(offsetString) {
	const match = OFFSET_TIMEZONE_FORMAT_REGEX.exec(offsetString);
	if (!match) return 0;
	const sign = match[1] === "+" ? 1 : -1;
	const hours = parseInt(match[2], 10);
	const minutes = match[3] ? parseInt(match[3], 10) : 0;
	const seconds = match[4] ? parseInt(match[4], 10) : 0;
	const paddedFractional = ((match[5] || "0") + "000000000").slice(0, 9);
	const fractional = parseInt(paddedFractional, 10) / 1e6;
	return sign * (hours * 36e5 + minutes * 6e4 + seconds * 1e3 + fractional);
}
/**
* GetNamedTimeZoneOffsetNanoseconds ( timeZone, t )
* Similar to abstract operation in ECMA-262, adapted for IANA timezone data.
* Extended to support UTC offset time zones per ECMA-402 PR #788.
*
* Returns the timezone offset in milliseconds (not nanoseconds for this impl)
* and DST flag for the given timezone at time t.
*
* @param t - Time value in milliseconds since epoch
* @param timeZone - The timezone identifier
* @param tzData - IANA timezone database
* @returns Tuple of [offset in milliseconds, inDST boolean]
*/
function getApplicableZoneData(t, timeZone, tzData) {
	if (IsTimeZoneOffsetString(timeZone)) return [ParseTimeZoneOffsetString(timeZone), false];
	const zoneData = tzData[timeZone];
	if (!zoneData) return [0, false];
	let i = 0;
	let offset = 0;
	let dst = false;
	for (; i <= zoneData.length; i++) if (i === zoneData.length || zoneData[i][0] * 1e3 > t) {
		[, , offset, dst] = zoneData[i - 1];
		break;
	}
	return [offset * 1e3, dst];
}
/**
* https://tc39.es/ecma402/#sec-tolocaltime
* @param t
* @param calendar
* @param timeZone
*/
function ToLocalTime(epochNanoseconds, calendar, timeZone, { tzData, calendarData }) {
	const milliseconds = new Decimal$1(String(epochNanoseconds)).div(1e6);
	const [timeZoneOffset, inDST] = getApplicableZoneData(milliseconds.floor().toNumber(), timeZone, tzData);
	const tz = milliseconds.plus(timeZoneOffset).floor().toNumber();
	return {
		...CalendarDateFromTime(tz, calendar, calendarData),
		weekday: WeekDay(tz),
		hour: HourFromTime(tz),
		minute: MinFromTime(tz),
		second: SecFromTime(tz),
		millisecond: msFromTime(tz),
		inDST,
		timeZoneOffset
	};
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/utils.js
const DATE_TIME_PROPS = [
	"weekday",
	"era",
	"year",
	"month",
	"day",
	"dayPeriod",
	"hour",
	"minute",
	"second",
	"fractionalSecondDigits",
	"timeZoneName"
];
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/FormatDateTimePattern.js
function getDayPeriodName(data, time, width = "short") {
	const value = ((time.hour * 60 + time.minute) * 60 + time.second) * 1e3 + time.millisecond;
	const rules = data.dayPeriodRules || [];
	const rule = rules.find((rule) => rule.at === value) || rules.find((rule) => rule.from !== void 0 && rule.before !== void 0 && (rule.from < rule.before ? value >= rule.from && value < rule.before : value >= rule.from || value < rule.before));
	return rule && data.dayPeriods?.[width][rule.name] || (time.hour < 12 ? data.am : data.pm);
}
function pad(n) {
	if (n < 10) return `0${n}`;
	return String(n);
}
function offsetToGmtString(gmtFormat, hourFormat, offsetInMs, style) {
	const offsetInMinutes = Math.floor(offsetInMs / 6e4);
	const mins = Math.abs(offsetInMinutes) % 60;
	const hours = Math.floor(Math.abs(offsetInMinutes) / 60);
	const [positivePattern, negativePattern] = hourFormat.split(";");
	let offsetStr = "";
	let pattern = offsetInMs < 0 ? negativePattern : positivePattern;
	if (style === "long") offsetStr = pattern.replace("HH", pad(hours)).replace("H", String(hours)).replace("mm", pad(mins)).replace("m", String(mins));
	else if (mins || hours) {
		if (!mins) pattern = pattern.replace(/:?m+/, "");
		offsetStr = pattern.replace(/H+/, String(hours)).replace(/m+/, String(mins));
	}
	return gmtFormat.replace("{0}", offsetStr);
}
function createNumberFormatters(internalSlots) {
	const locale = internalSlots.locale;
	const nfOptions = Object.create(null);
	nfOptions.numberingSystem = internalSlots.numberingSystem;
	nfOptions.useGrouping = false;
	const nf = createMemoizedNumberFormat(locale, nfOptions);
	const nf2Options = Object.create(null);
	nf2Options.minimumIntegerDigits = 2;
	nf2Options.numberingSystem = internalSlots.numberingSystem;
	nf2Options.useGrouping = false;
	const nf2 = createMemoizedNumberFormat(locale, nf2Options);
	const fractionalSecondDigits = internalSlots.fractionalSecondDigits;
	let nf3;
	if (fractionalSecondDigits !== void 0) {
		const nf3Options = Object.create(null);
		nf3Options.minimumIntegerDigits = fractionalSecondDigits;
		nf3Options.numberingSystem = internalSlots.numberingSystem;
		nf3Options.useGrouping = false;
		nf3 = createMemoizedNumberFormat(locale, nf3Options);
	}
	return {
		nf,
		nf2,
		nf3
	};
}
/**
* https://tc39.es/ecma402/#sec-partitiondatetimepattern
* @param dtf
* @param x
*/
function FormatDateTimePattern(dtf, patternParts, epochNanoseconds, isPlain, { getInternalSlots, localeData, getDefaultTimeZone, tzData, calendarData, rangeFormatOptions }) {
	/** IMPL START */
	const internalSlots = getInternalSlots(dtf);
	const rootLocaleData = localeData[internalSlots.dataLocale];
	const dataLocaleData = rootLocaleData.calendarData?.[internalSlots.calendar] ?? rootLocaleData;
	/** IMPL END */
	const numberFormatters = rangeFormatOptions?.numberFormatters || createNumberFormatters(internalSlots);
	if (rangeFormatOptions) rangeFormatOptions.numberFormatters = numberFormatters;
	const { nf, nf2, nf3 } = numberFormatters;
	const fractionalSecondDigits = internalSlots.fractionalSecondDigits;
	const tm = rangeFormatOptions?.localTime ?? ToLocalTime(epochNanoseconds, internalSlots.calendar, isPlain ? "+00:00" : internalSlots.timeZone, {
		tzData,
		calendarData
	});
	const result = [];
	const contextParts = rangeFormatOptions?.patternParts || patternParts;
	const hasMonth = contextParts.some((part) => part.type === "month");
	const hasOtherDateFields = contextParts.some((part) => part.type === "day" || part.type === "year" || part.type === "relatedYear" || part.type === "yearName" || part.type === "weekday" || part.type === "era");
	const isMonthStandalone = hasMonth && !hasOtherDateFields;
	for (const patternPart of patternParts) {
		const p = patternPart.type;
		if (p === "literal") result.push({
			type: "literal",
			value: patternPart.value
		});
		else if (p === "fractionalSecondDigits") {
			const v = new Decimal$1(tm.millisecond).times(Decimal$1.pow(10, (fractionalSecondDigits || 0) - 3)).floor().toNumber();
			result.push({
				type: "fractionalSecond",
				value: nf3.format(v)
			});
		} else if (p === "dayPeriod") result.push({
			type: p,
			value: getDayPeriodName(dataLocaleData, tm, internalSlots.dayPeriod)
		});
		else if (p === "timeZoneName") {
			const f = internalSlots.timeZoneName;
			let fv;
			const { timeZoneName, gmtFormat, hourFormat } = dataLocaleData;
			const timeZoneData = timeZoneName[internalSlots.timeZone || getDefaultTimeZone()];
			if (timeZoneData && timeZoneData[f]) {
				const names = timeZoneData[f];
				if (tm.inDST && names.length >= 2 && names[0] === names[1]) fv = offsetToGmtString(gmtFormat, hourFormat, tm.timeZoneOffset, f);
				else fv = names[+tm.inDST];
			} else fv = offsetToGmtString(gmtFormat, hourFormat, tm.timeZoneOffset, f);
			result.push({
				type: p,
				value: fv
			});
		} else if (DATE_TIME_PROPS.indexOf(p) > -1) {
			let fv = "";
			let f = internalSlots[p];
			let v = tm[p];
			if (p === "year" && v <= 0 && (internalSlots.calendar === "gregory" || internalSlots.calendar === "iso8601")) v = 1 - v;
			if (p === "month") {
				v = (tm.monthNumber ?? v) + 1;
				if (internalSlots.calendar === "hebrew" && hasOtherDateFields && (f === "numeric" || f === "2-digit")) f = "short";
			}
			const hourCycle = internalSlots.hourCycle;
			if (p === "hour" && (hourCycle === "h11" || hourCycle === "h12")) {
				v = v % 12;
				if (v === 0 && hourCycle === "h12") v = 12;
			}
			if (p === "hour" && hourCycle === "h24" && v === 0) v = 24;
			if (f === "numeric") fv = nf.format(v);
			else if (f === "2-digit") {
				fv = nf2.format(v);
				if (fv.length > 2) fv = Array.from(fv).slice(-2).join("");
			} else if (f === "narrow" || f === "short" || f === "long") {
				if (p === "era") fv = dataLocaleData[p][f][v];
				else if (p === "month") fv = (isMonthStandalone && dataLocaleData.monthStandalone ? dataLocaleData.monthStandalone : dataLocaleData.month)[f][tm.monthNameIndex ?? v - 1];
				else fv = dataLocaleData[p][f][v];
			}
			if (p === "month" && tm.leapMonth && dataLocaleData.leapMonthPatterns) {
				const patterns = dataLocaleData.leapMonthPatterns;
				fv = (f === "numeric" || f === "2-digit" ? patterns.numeric : (isMonthStandalone ? patterns.standalone : patterns.format)[f]).replace("{0}", fv);
			}
			result.push({
				type: p,
				value: fv
			});
		} else if (p === "ampm") {
			const v = tm.hour;
			let fv;
			if (v > 11) fv = dataLocaleData.pm;
			else fv = dataLocaleData.am;
			result.push({
				type: "dayPeriod",
				value: fv
			});
		} else if (p === "relatedYear") {
			const v = tm.relatedYear;
			const fv = nf.format(v);
			result.push({
				type: "relatedYear",
				value: fv
			});
		} else if (p === "yearName") {
			const v = tm.yearName;
			const fv = dataLocaleData.yearNames[v];
			result.push({
				type: "yearName",
				value: fv
			});
		}
	}
	return result;
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/PartitionDateTimePattern.js
/**
* https://tc39.es/ecma402/#sec-partitiondatetimepattern
* @param dtf
* @param x
*/
function PartitionDateTimePattern(dtf, x, implDetails) {
	const record = HandleDateTimeValue(implDetails.getInternalSlots(dtf), x);
	const internalSlots = record.format;
	const { pattern } = internalSlots;
	return FormatDateTimePattern(dtf, PartitionPattern(pattern), record.epochNanoseconds, record.isPlain, {
		...implDetails,
		getInternalSlots: () => internalSlots
	});
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/FormatDateTime.js
/**
* https://tc39.es/ecma402/#sec-formatdatetime
* @param dtf DateTimeFormat
* @param x
*/
function FormatDateTime(dtf, x, implDetails) {
	const parts = PartitionDateTimePattern(dtf, x, implDetails);
	let result = "";
	for (const part of parts) result += part.value;
	return result;
}
//#endregion
//#region packages/ecma262-abstract/SameValue.js
/**
* https://www.ecma-international.org/ecma-262/11.0/index.html#sec-samevalue
*/
function SameValue(x, y) {
	if (Object.is) return Object.is(x, y);
	if (x === y) return x !== 0 || 1 / x === 1 / y;
	return x !== x && y !== y;
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/PartitionDateTimeRangePattern.js
const TABLE_2_FIELDS = [
	"era",
	"year",
	"month",
	"day",
	"ampm",
	"dayPeriod",
	"hour",
	"minute",
	"second",
	"fractionalSecondDigits"
];
function PartitionDateTimeRangePattern(dtf, x, y, implDetails) {
	const firstTemporal = isTemporalDateTimeValue(x);
	const secondTemporal = isTemporalDateTimeValue(y);
	if (firstTemporal !== secondTemporal || firstTemporal && secondTemporal && x.kind !== y.kind) throw new TypeError("Range endpoints must have the same Temporal type");
	const slots = implDetails.getInternalSlots(dtf);
	const first = HandleDateTimeValue(slots, x);
	const second = HandleDateTimeValue(slots, y);
	const internalSlots = first.format;
	implDetails = {
		...implDetails,
		getInternalSlots: () => internalSlots
	};
	const { tzData, calendarData, localeData } = implDetails;
	const rootLocaleData = localeData[internalSlots.dataLocale];
	const dataLocaleData = rootLocaleData.calendarData?.[internalSlots.calendar] ?? rootLocaleData;
	/** IMPL END */
	const tm1 = ToLocalTime(first.epochNanoseconds, internalSlots.calendar, first.isPlain ? "+00:00" : internalSlots.timeZone, {
		tzData,
		calendarData
	});
	const tm2 = ToLocalTime(second.epochNanoseconds, internalSlots.calendar, first.isPlain ? "+00:00" : internalSlots.timeZone, {
		tzData,
		calendarData
	});
	const { pattern, rangePatterns } = internalSlots;
	const parts = PartitionPattern(pattern);
	let lastField = -1;
	for (const part of parts) {
		const field = part.type === "weekday" ? "day" : part.type === "relatedYear" || part.type === "yearName" ? "year" : part.type;
		lastField = Math.max(lastField, TABLE_2_FIELDS.indexOf(field));
	}
	const fallback = rangePatterns.default || { patternParts: PartitionPattern(dataLocaleData.intervalFormatFallback).map((part) => ({
		source: part.type === "literal" ? RangePatternType.shared : part.type === "0" ? RangePatternType.startRange : RangePatternType.endRange,
		pattern: part.type === "literal" ? part.value : pattern
	})) };
	let rangePattern;
	for (let index = 0; index <= lastField; index++) {
		const field = TABLE_2_FIELDS[index];
		let equal;
		if (field === "ampm") equal = tm1.hour < 12 === tm2.hour < 12;
		else if (field === "dayPeriod") equal = getDayPeriodName(dataLocaleData, tm1, internalSlots.dayPeriod) === getDayPeriodName(dataLocaleData, tm2, internalSlots.dayPeriod);
		else if (field === "fractionalSecondDigits") {
			const digits = internalSlots.fractionalSecondDigits ?? 3;
			equal = Math.floor(tm1.millisecond * 10 ** (digits - 3)) === Math.floor(tm2.millisecond * 10 ** (digits - 3));
		} else equal = SameValue(tm1[field], tm2[field]);
		if (!equal) {
			rangePattern = rangePatterns[field] || (field === "ampm" ? rangePatterns.hour : void 0) || fallback;
			break;
		}
	}
	if (rangePattern === void 0) {
		const result = FormatDateTimePattern(dtf, parts, first.epochNanoseconds, first.isPlain, {
			...implDetails,
			rangeFormatOptions: { localTime: tm1 }
		});
		for (const part of result) part.source = RangePatternType.shared;
		return result;
	}
	const result = [];
	const rangeFormatOptions = { patternParts: PartitionPattern(rangePattern.patternParts.map((part) => part.pattern === "{0}" || part.pattern === "{1}" ? pattern : part.pattern).join("")) };
	const rangeImplDetails = {
		...implDetails,
		rangeFormatOptions
	};
	for (const part of rangePattern.patternParts) {
		const { source } = part;
		const partPattern = part.pattern === "{0}" || part.pattern === "{1}" ? pattern : part.pattern;
		if (!partPattern.includes("{")) {
			result.push({
				type: "literal",
				value: partPattern,
				source
			});
			continue;
		}
		const value = source === RangePatternType.endRange ? second.epochNanoseconds : first.epochNanoseconds;
		rangeFormatOptions.localTime = source === RangePatternType.endRange ? tm2 : tm1;
		const formatted = FormatDateTimePattern(dtf, PartitionPattern(partPattern), value, first.isPlain, rangeImplDetails);
		for (const item of formatted) {
			item.source = source;
			result.push(item);
		}
	}
	return result;
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/FormatDateTimeRange.js
function FormatDateTimeRange(dtf, x, y, implDetails) {
	const parts = PartitionDateTimeRangePattern(dtf, x, y, implDetails);
	let result = "";
	for (const part of parts) result += part.value;
	return result;
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/FormatDateTimeRangeToParts.js
function FormatDateTimeRangeToParts(dtf, x, y, implDetails) {
	const parts = PartitionDateTimeRangePattern(dtf, x, y, implDetails);
	const result = [];
	for (const part of parts) result.push({
		type: part.type,
		value: part.value,
		source: part.source
	});
	return result;
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
//#region packages/ecma402-abstract/DateTimeFormat/FormatDateTimeToParts.js
/**
* https://tc39.es/ecma402/#sec-formatdatetimetoparts
*
* @param dtf
* @param x
* @param implDetails
*/
function FormatDateTimeToParts(dtf, x, implDetails) {
	const parts = PartitionDateTimePattern(dtf, x, implDetails);
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
//#region packages/ecma402-abstract/DateTimeFormat/BasicFormatMatcher.js
/**
* https://tc39.es/ecma402/#sec-basicformatmatcher
* @param options
* @param formats
*/
function BasicFormatMatcher(options, formats) {
	let bestScore = -Infinity;
	let bestFormat = formats[0];
	invariant(Array.isArray(formats), "formats should be a list of things");
	for (const format of formats) {
		let score = 0;
		for (const prop of DATE_TIME_PROPS) {
			const optionsProp = options[prop];
			const formatProp = format[prop];
			if (optionsProp === void 0 && formatProp !== void 0) score -= 20;
			else if (optionsProp !== void 0 && formatProp === void 0) score -= 120;
			else if (prop === "timeZoneName") {
				if (optionsProp === "short" || optionsProp === "shortGeneric") {
					if (formatProp === "shortOffset") score -= 1;
					else if (formatProp === "longOffset") score -= 4;
					else if (optionsProp === "short" && formatProp === "long") score -= 3;
					else if (optionsProp === "shortGeneric" && formatProp === "longGeneric") score -= 3;
					else if (optionsProp !== formatProp) score -= 120;
				} else if (optionsProp === "shortOffset" && formatProp === "longOffset") score -= 3;
				else if (optionsProp === "long" || optionsProp === "longGeneric") {
					if (formatProp === "longOffset") score -= 1;
					else if (formatProp === "shortOffset") score -= 9;
					else if (optionsProp === "long" && formatProp === "short") score -= 8;
					else if (optionsProp === "longGeneric" && formatProp === "shortGeneric") score -= 8;
					else if (optionsProp !== formatProp) score -= 120;
				} else if (optionsProp === "longOffset" && formatProp === "shortOffset") score -= 8;
				else if (optionsProp !== formatProp) score -= 120;
			} else if (optionsProp !== formatProp) {
				let values;
				if (prop === "fractionalSecondDigits") values = [
					1,
					2,
					3
				];
				else values = [
					"2-digit",
					"numeric",
					"narrow",
					"short",
					"long"
				];
				const optionsPropIndex = values.indexOf(optionsProp);
				const formatPropIndex = values.indexOf(formatProp);
				const delta = Math.max(-2, Math.min(formatPropIndex - optionsPropIndex, 2));
				if (delta === 2) score -= 6;
				else if (delta === 1) score -= 3;
				else if (delta === -1) score -= 6;
				else if (delta === -2) score -= 8;
			}
		}
		if (score > bestScore) {
			bestScore = score;
			bestFormat = format;
		}
	}
	return { ...bestFormat };
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/skeleton.js
const patternFields = /* @__PURE__ */ new WeakMap();
function getDateTimePatternFields(format) {
	const cached = patternFields.get(format);
	if (cached) return cached;
	const fields = Object.create(null);
	processDateTimePattern(format.rawPattern, fields);
	patternFields.set(format, fields);
	return fields;
}
/**
* https://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
* Credit: https://github.com/caridy/intl-datetimeformat-pattern/blob/master/index.js
* with some tweaks
*/
function matchSkeletonPattern(match, result) {
	const len = match.length;
	switch (match[0]) {
		case "G":
			result.era = len === 4 ? "long" : len === 5 ? "narrow" : "short";
			return "{era}";
		case "y":
		case "Y":
		case "u":
			result.year = len === 2 ? "2-digit" : "numeric";
			return "{year}";
		case "U":
			result.year = "numeric";
			return "{yearName}";
		case "r":
			result.year = "numeric";
			return "{relatedYear}";
		case "q":
		case "Q": throw new RangeError("`w/Q` (quarter) patterns are not supported");
		case "M":
		case "L":
			result.month = [
				"numeric",
				"2-digit",
				"short",
				"long",
				"narrow"
			][len - 1];
			return "{month}";
		case "w":
		case "W": throw new RangeError("`w/W` (week of year) patterns are not supported");
		case "d":
			result.day = ["numeric", "2-digit"][len - 1];
			return "{day}";
		case "D":
		case "F":
		case "g":
			result.day = "numeric";
			return "{day}";
		case "E":
			result.weekday = len === 4 ? "long" : len === 5 ? "narrow" : "short";
			return "{weekday}";
		case "e":
			result.weekday = [
				void 0,
				void 0,
				"short",
				"long",
				"narrow",
				"short"
			][len - 1];
			return "{weekday}";
		case "c":
			result.weekday = [
				void 0,
				void 0,
				"short",
				"long",
				"narrow",
				"short"
			][len - 1];
			return "{weekday}";
		case "a":
		case "b":
			result.hour12 = true;
			return "{ampm}";
		case "B":
			result.dayPeriod = len === 4 ? "long" : len === 5 ? "narrow" : "short";
			return "{dayPeriod}";
		case "h":
			result.hour = ["numeric", "2-digit"][len - 1];
			result.hour12 = true;
			return "{hour}";
		case "H":
			result.hour = ["numeric", "2-digit"][len - 1];
			return "{hour}";
		case "K":
			result.hour = ["numeric", "2-digit"][len - 1];
			result.hour12 = true;
			return "{hour}";
		case "k":
			result.hour = ["numeric", "2-digit"][len - 1];
			return "{hour}";
		case "j":
		case "J":
		case "C": throw new RangeError("`j/J/C` (hour) patterns are not supported, use `h/H/K/k` instead");
		case "m":
			result.minute = ["numeric", "2-digit"][len - 1];
			return "{minute}";
		case "s":
			result.second = ["numeric", "2-digit"][len - 1];
			return "{second}";
		case "S":
		case "A":
			result.second = "numeric";
			return "{second}";
		case "z":
		case "Z":
		case "O":
		case "v":
		case "V":
		case "X":
		case "x":
			result.timeZoneName = len < 4 ? "short" : "long";
			return "{timeZoneName}";
	}
	return "";
}
function skeletonTokenToTable2(c) {
	switch (c) {
		case "G": return "era";
		case "y":
		case "Y":
		case "u":
		case "U":
		case "r": return "year";
		case "M":
		case "L": return "month";
		case "d":
		case "D":
		case "F":
		case "g": return "day";
		case "a":
		case "b": return "ampm";
		case "B": return "dayPeriod";
		case "h":
		case "H":
		case "K":
		case "k": return "hour";
		case "m": return "minute";
		case "s":
		case "S":
		case "A": return "second";
		default: throw new RangeError("Invalid range pattern token");
	}
}
function replaceDateTimeTokens(pattern, result) {
	let output = "";
	let quoted = false;
	for (let i = 0; i < pattern.length;) {
		const c = pattern[i];
		if (c === "'") {
			if (pattern[i + 1] === "'") {
				output += "'";
				i += 2;
			} else {
				quoted = !quoted;
				i++;
			}
			continue;
		}
		const maximum = "Eec".includes(c) ? 6 : "GQqUMLabB".includes(c) ? 5 : "zZOvVxX".includes(c) ? 4 : c === "D" ? 3 : "dhkHKwms".includes(c) ? 2 : "FW".includes(c) ? 1 : "yYur".includes(c) ? Infinity : 0;
		if (quoted || !maximum) {
			output += c;
			i++;
			continue;
		}
		let end = i + 1;
		while (end < pattern.length && end - i < maximum && pattern[end] === c) end++;
		output += matchSkeletonPattern(pattern.slice(i, end), result);
		i = end;
	}
	return output;
}
function processDateTimePattern(pattern, result = Object.create(null)) {
	const pattern12 = replaceDateTimeTokens(pattern, result);
	const period = pattern12.indexOf("{ampm}");
	let withoutPeriod = pattern12;
	if (period >= 0) {
		const after = period + 6;
		const surroundingSpace = period > 0 && after < pattern12.length && !pattern12[period - 1].trim() && !pattern12[after].trim();
		withoutPeriod = pattern12.slice(0, period) + pattern12.slice(after + (surroundingSpace ? 1 : 0));
	}
	return [withoutPeriod.trim(), pattern12];
}
function parseDateTimeSkeleton(skeleton, rawPattern = skeleton, rangePatterns, intervalFormatFallback) {
	const result = Object.assign(Object.create(null), {
		pattern: "",
		pattern12: "",
		skeleton,
		rawPattern,
		rangePatterns: Object.create(null),
		rangePatterns12: Object.create(null)
	});
	if (rangePatterns) for (const k in rangePatterns) {
		const key = skeletonTokenToTable2(k);
		const rawPattern = rangePatterns[k];
		const intervalResult = Object.assign(Object.create(null), { patternParts: [] });
		const [pattern, pattern12] = processDateTimePattern(rawPattern, intervalResult);
		result.rangePatterns[key] = {
			...intervalResult,
			patternParts: splitRangePattern(pattern)
		};
		result.rangePatterns12[key] = {
			...intervalResult,
			patternParts: splitRangePattern(pattern12)
		};
	}
	if (intervalFormatFallback) {
		const patternParts = splitFallbackRangePattern(intervalFormatFallback);
		result.rangePatterns.default = { patternParts };
		result.rangePatterns12.default = { patternParts };
	}
	replaceDateTimeTokens(skeleton, result);
	const fields = Object.create(null);
	const [pattern, pattern12] = processDateTimePattern(rawPattern, fields);
	patternFields.set(result, fields);
	result.pattern = pattern;
	result.pattern12 = pattern12;
	const yearFields = ["{relatedYear}", "{yearName}"].filter((field) => pattern.includes(field));
	if (yearFields.length) for (const patterns of [result.rangePatterns, result.rangePatterns12]) for (const field of Object.keys(patterns)) {
		if (field === "default") continue;
		const range = patterns[field].patternParts.map((part) => part.pattern).join("");
		if (yearFields.some((yearField) => !range.includes(yearField))) delete patterns[field];
	}
	return result;
}
function splitFallbackRangePattern(pattern) {
	let parts = [];
	let start = 0;
	for (let i = 0; i < pattern.length; i++) {
		const token = pattern.slice(i, i + 3);
		if (token !== "{0}" && token !== "{1}") continue;
		if (i > start) parts = [...parts, {
			source: RangePatternType.shared,
			pattern: pattern.slice(start, i)
		}];
		parts = [...parts, {
			source: token === "{0}" ? RangePatternType.startRange : RangePatternType.endRange,
			pattern: token
		}];
		start = i + 3;
		i += 2;
	}
	if (start < pattern.length) parts = [...parts, {
		source: RangePatternType.shared,
		pattern: pattern.slice(start)
	}];
	return parts;
}
function splitRangePattern(pattern) {
	const fields = /* @__PURE__ */ new Map();
	let startBegin = pattern.length;
	let startEnd = 0;
	let endBegin = pattern.length;
	let endEnd = 0;
	for (let index = pattern.indexOf("{"); index >= 0;) {
		const close = pattern.indexOf("}", index + 1);
		if (close < 0) break;
		const key = pattern.slice(index + 1, close);
		const first = fields.get(key);
		const end = close + 1;
		if (first) {
			startBegin = Math.min(startBegin, first.start);
			startEnd = Math.max(startEnd, first.end);
			endBegin = Math.min(endBegin, index);
			endEnd = Math.max(endEnd, end);
		} else fields.set(key, {
			start: index,
			end
		});
		index = pattern.indexOf("{", end);
	}
	if (!startEnd) return [{
		source: RangePatternType.shared,
		pattern
	}];
	let result = [];
	function append(begin, end, source) {
		if (begin < end) result = [...result, {
			source,
			pattern: pattern.slice(begin, end)
		}];
	}
	append(0, startBegin, RangePatternType.shared);
	append(startBegin, startEnd, RangePatternType.startRange);
	append(startEnd, endBegin, RangePatternType.shared);
	append(endBegin, endEnd, RangePatternType.endRange);
	append(endEnd, pattern.length, RangePatternType.shared);
	return result;
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/BestFitFormatMatcher.js
function isNumericType(t) {
	return t === "numeric" || t === "2-digit";
}
/**
* Credit: https://github.com/andyearnshaw/Intl.js/blob/0958dc1ad8153f1056653ea22b8208f0df289a4e/src/12.datetimeformat.js#L611
* with some modifications
* @param options
* @param format
*/
function bestFitFormatMatcherScore(options, format) {
	let score = 0;
	if (options.hour !== void 0) {
		if (options.hour12 && !format.hour12) score -= 120;
		else if (!options.hour12 && format.hour12) score -= 20;
	}
	for (const prop of DATE_TIME_PROPS) {
		const optionsProp = options[prop];
		const formatProp = format[prop];
		if (optionsProp === void 0 && formatProp !== void 0) score -= 20;
		else if (optionsProp !== void 0 && formatProp === void 0) score -= 120;
		else if (optionsProp !== formatProp) {
			if (isNumericType(optionsProp) !== isNumericType(formatProp)) score -= 15;
			else {
				const values = [
					"2-digit",
					"numeric",
					"narrow",
					"short",
					"long"
				];
				const optionsPropIndex = values.indexOf(optionsProp);
				const formatPropIndex = values.indexOf(formatProp);
				const delta = Math.max(-2, Math.min(formatPropIndex - optionsPropIndex, 2));
				if (delta === 2) score -= 6;
				else if (delta === 1) score -= 3;
				else if (delta === -1) score -= 6;
				else if (delta === -2) score -= 8;
			}
		}
	}
	return score;
}
/**
* https://tc39.es/ecma402/#sec-bestfitformatmatcher
* Just alias to basic for now
* @param options
* @param formats
* @param implDetails Implementation details
*/
function BestFitFormatMatcher(options, formats) {
	let bestScore = -Infinity;
	let bestFormat = formats[0];
	invariant(Array.isArray(formats), "formats should be a list of things");
	for (const format of formats) {
		const score = bestFitFormatMatcherScore(options, format);
		if (score > bestScore) {
			bestScore = score;
			bestFormat = format;
		}
	}
	const skeletonFormat = { ...bestFormat };
	const patternFormat = Object.create(null);
	patternFormat.rawPattern = bestFormat.rawPattern;
	Object.assign(patternFormat, getDateTimePatternFields(bestFormat));
	for (const prop in skeletonFormat) {
		const skeletonValue = skeletonFormat[prop];
		const patternValue = patternFormat[prop];
		const requestedValue = options[prop];
		if (prop === "minute" || prop === "second") continue;
		if (!requestedValue) continue;
		if (prop === "month" && isNumericType(skeletonValue) && isNumericType(requestedValue) && !isNumericType(patternValue)) continue;
		if (isNumericType(patternValue) && !isNumericType(requestedValue)) continue;
		if (skeletonValue === requestedValue) continue;
		patternFormat[prop] = requestedValue;
	}
	patternFormat.pattern = skeletonFormat.pattern;
	patternFormat.pattern12 = skeletonFormat.pattern12;
	patternFormat.skeleton = skeletonFormat.skeleton;
	patternFormat.rangePatterns = skeletonFormat.rangePatterns;
	patternFormat.rangePatterns12 = skeletonFormat.rangePatterns12;
	return patternFormat;
}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/DateTimeStyleFormat.js
function getTimeStyleFormat(timeStyle, dataLocaleData, formats, hour12) {
	if (timeStyle === void 0) return;
	invariant(timeStyle === "full" || timeStyle === "long" || timeStyle === "medium" || timeStyle === "short", "invalid timeStyle");
	const timeFormat = dataLocaleData.timeFormat[timeStyle];
	const formatHour12 = timeFormat.hour12 === true;
	if (hour12 === void 0 || hour12 === formatHour12) return timeFormat;
	const matcherOptions = { hour12 };
	for (const prop of DATE_TIME_PROPS) {
		const value = timeFormat[prop];
		if (value !== void 0) matcherOptions[prop] = value;
	}
	return BestFitFormatMatcher(matcherOptions, formats);
}
function DateTimeStyleFormat(dateStyle, timeStyle, dataLocaleData, formats, hour12) {
	let dateFormat, timeFormat;
	timeFormat = getTimeStyleFormat(timeStyle, dataLocaleData, formats, hour12);
	if (dateStyle !== void 0) {
		invariant(dateStyle === "full" || dateStyle === "long" || dateStyle === "medium" || dateStyle === "short", "invalid dateStyle");
		dateFormat = dataLocaleData.dateFormat[dateStyle];
	}
	if (dateStyle !== void 0 && timeStyle !== void 0) {
		const format = {};
		for (const field in dateFormat) if (field !== "pattern" && field !== "rangePatterns" && field !== "rangePatterns12") format[field] = dateFormat[field];
		for (const field in timeFormat) if (field !== "pattern" && field !== "pattern12" && field !== "rangePatterns" && field !== "rangePatterns12") format[field] = timeFormat[field];
		const connector = dataLocaleData.dateTimeFormat[dateStyle];
		format.pattern = connector.replace("{0}", timeFormat.pattern).replace("{1}", dateFormat.pattern);
		if ("pattern12" in timeFormat) format.pattern12 = connector.replace("{0}", timeFormat.pattern12).replace("{1}", dateFormat.pattern);
		if (timeFormat.rangePatterns) format.rangePatterns = timeFormat.rangePatterns;
		if (timeFormat.rangePatterns12) format.rangePatterns12 = timeFormat.rangePatterns12;
		return format;
	}
	if (timeStyle !== void 0) return timeFormat;
	invariant(dateStyle !== void 0, "dateStyle should not be undefined");
	return dateFormat;
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
//#region packages/ecma402-abstract/DateTimeFormat/InitializeDateTimeFormat.js
function isTimeRelated(opt) {
	for (const prop of [
		"hour",
		"minute",
		"second"
	]) if (opt[prop] !== void 0) return true;
	return false;
}
function resolveHourCycle(hc, data, hour12) {
	if (hour12 === true) return data.hourCycle12 || data.hc.find((cycle) => cycle === "h11" || cycle === "h12") || "h12";
	if (hour12 === false) return data.hourCycle24 || data.hc.find((cycle) => cycle === "h23" || cycle === "h24") || "h23";
	return hc == null ? data.hourCycle : hc;
}
function applyExplicitTimePatternOptions(pattern, opt, locale, numberingSystem) {
	if (opt.fractionalSecondDigits !== void 0 && pattern.includes("{second}") && !pattern.includes("{fractionalSecondDigits}")) {
		const numberOptions = Object.create(null);
		numberOptions.numberingSystem = numberingSystem;
		const decimal = createMemoizedNumberFormat(locale, numberOptions).formatToParts(1.1).find((part) => part.type === "decimal")?.value;
		invariant(decimal !== void 0, "Missing decimal separator for fractional seconds");
		pattern = pattern.replace("{second}", `{second}${decimal}{fractionalSecondDigits}`);
	}
	if (opt.dayPeriod !== void 0) pattern = pattern.replace("{ampm}", "{dayPeriod}");
	return pattern;
}
/**
* https://tc39.es/ecma402/#sec-createdatetimeformat
* @param dtf DateTimeFormat
* @param locales locales
* @param opts options
*/
function InitializeDateTimeFormat(dtf, locales, opts, { getInternalSlots, availableLocales, localeData, getDefaultLocale, getDefaultTimeZone, relevantExtensionKeys, tzData, uppercaseLinks }) {
	const requestedLocales = CanonicalizeLocaleList(locales);
	const options = CoerceOptionsToObject(opts);
	let opt = Object.create(null);
	let matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
	opt.localeMatcher = matcher;
	let calendar = GetOption(options, "calendar", "string", void 0, void 0);
	if (calendar !== void 0 && !IsUnicodeLocaleIdentifierType(calendar)) throw new RangeError("Malformed calendar");
	const internalSlots = getInternalSlots(dtf);
	opt.ca = calendar;
	const numberingSystem = GetOption(options, "numberingSystem", "string", void 0, void 0);
	if (numberingSystem !== void 0 && !IsUnicodeLocaleIdentifierType(numberingSystem)) throw new RangeError("Malformed numbering system");
	opt.nu = numberingSystem;
	const hour12 = GetOption(options, "hour12", "boolean", void 0, void 0);
	let hourCycle = GetOption(options, "hourCycle", "string", [
		"h11",
		"h12",
		"h23",
		"h24"
	], void 0);
	if (hour12 !== void 0) hourCycle = null;
	opt.hc = hourCycle;
	const r = ResolveLocale(availableLocales, requestedLocales, opt, relevantExtensionKeys, localeData, getDefaultLocale);
	internalSlots.locale = r.locale;
	calendar = r.ca;
	internalSlots.calendar = calendar;
	internalSlots.hourCycle = r.hc;
	internalSlots.numberingSystem = r.nu;
	const { dataLocale } = r;
	internalSlots.dataLocale = dataLocale;
	let { timeZone } = options;
	if (timeZone !== void 0) {
		timeZone = String(timeZone);
		if (!IsValidTimeZoneName(timeZone, {
			zoneNamesFromData: Object.keys(tzData),
			uppercaseLinks
		})) throw new RangeError("Invalid timeZoneName");
		timeZone = CanonicalizeTimeZoneName(timeZone, {
			zoneNames: Object.keys(tzData),
			uppercaseLinks
		});
	} else timeZone = getDefaultTimeZone();
	internalSlots.timeZone = timeZone;
	opt = Object.create(null);
	opt.weekday = GetOption(options, "weekday", "string", [
		"narrow",
		"short",
		"long"
	], void 0);
	opt.era = GetOption(options, "era", "string", [
		"narrow",
		"short",
		"long"
	], void 0);
	opt.year = GetOption(options, "year", "string", ["2-digit", "numeric"], void 0);
	opt.month = GetOption(options, "month", "string", [
		"2-digit",
		"numeric",
		"narrow",
		"short",
		"long"
	], void 0);
	opt.day = GetOption(options, "day", "string", ["2-digit", "numeric"], void 0);
	opt.dayPeriod = GetOption(options, "dayPeriod", "string", [
		"narrow",
		"short",
		"long"
	], void 0);
	opt.hour = GetOption(options, "hour", "string", ["2-digit", "numeric"], void 0);
	opt.minute = GetOption(options, "minute", "string", ["2-digit", "numeric"], void 0);
	opt.second = GetOption(options, "second", "string", ["2-digit", "numeric"], void 0);
	opt.fractionalSecondDigits = GetNumberOption(options, "fractionalSecondDigits", 1, 3, void 0);
	opt.timeZoneName = GetOption(options, "timeZoneName", "string", [
		"long",
		"short",
		"longOffset",
		"shortOffset",
		"longGeneric",
		"shortGeneric"
	], void 0);
	const rootLocaleData = localeData[dataLocale];
	invariant(!!rootLocaleData, `Missing locale data for ${dataLocale}`);
	const dataLocaleData = rootLocaleData.calendarData?.[calendar] ?? rootLocaleData;
	const formats = dataLocaleData.formats[calendar];
	if (!formats) throw new RangeError(`Calendar "${calendar}" is not supported. Try setting "calendar" to 1 of the following: ${Object.keys(dataLocaleData.formats).join(", ")}`);
	const formatMatcher = GetOption(options, "formatMatcher", "string", ["basic", "best fit"], "best fit");
	const dateStyle = GetOption(options, "dateStyle", "string", [
		"full",
		"long",
		"medium",
		"short"
	], void 0);
	internalSlots.dateStyle = dateStyle;
	const timeStyle = GetOption(options, "timeStyle", "string", [
		"full",
		"long",
		"medium",
		"short"
	], void 0);
	internalSlots.timeStyle = timeStyle;
	const temporalOptions = { ...opt };
	const temporalHourCycle = resolveHourCycle(internalSlots.hourCycle, dataLocaleData, hour12);
	let bestFormat;
	if (dateStyle === void 0 && timeStyle === void 0) {
		if ([
			"weekday",
			"year",
			"month",
			"day",
			"dayPeriod",
			"hour",
			"minute",
			"second",
			"fractionalSecondDigits"
		].every((key) => opt[key] === void 0)) {
			opt.year = "numeric";
			opt.month = "numeric";
			opt.day = "numeric";
		}
		if (formatMatcher === "basic") bestFormat = BasicFormatMatcher(opt, formats);
		else {
			if (isTimeRelated(opt)) {
				const hc = resolveHourCycle(internalSlots.hourCycle, dataLocaleData, hour12);
				opt.hour12 = hc === "h11" || hc === "h12";
			}
			bestFormat = BestFitFormatMatcher(opt, formats);
		}
	} else {
		for (const prop of DATE_TIME_PROPS) if (opt[prop] !== void 0) throw new TypeError(`Intl.DateTimeFormat can't set option ${prop} when ${dateStyle ? "dateStyle" : "timeStyle"} is used`);
		const hc = timeStyle !== void 0 ? resolveHourCycle(internalSlots.hourCycle, dataLocaleData, hour12) : void 0;
		bestFormat = DateTimeStyleFormat(dateStyle, timeStyle, dataLocaleData, formats, hc !== void 0 ? hc === "h11" || hc === "h12" : void 0);
	}
	internalSlots.format = bestFormat;
	for (const prop in opt) {
		const p = bestFormat[prop];
		if (p !== void 0) internalSlots[prop] = p;
	}
	if (opt.dayPeriod !== void 0) internalSlots.dayPeriod = opt.dayPeriod;
	if (opt.fractionalSecondDigits !== void 0) internalSlots.fractionalSecondDigits = opt.fractionalSecondDigits;
	let pattern;
	let rangePatterns;
	if (internalSlots.hour !== void 0) {
		const hc = resolveHourCycle(internalSlots.hourCycle, dataLocaleData, hour12);
		internalSlots.hourCycle = hc;
		if (hc === "h11" || hc === "h12") {
			pattern = bestFormat.pattern12;
			rangePatterns = bestFormat.rangePatterns12;
		} else {
			pattern = bestFormat.pattern;
			rangePatterns = bestFormat.rangePatterns;
		}
	} else {
		internalSlots.hourCycle = void 0;
		pattern = bestFormat.pattern;
		rangePatterns = bestFormat.rangePatterns;
	}
	pattern = applyExplicitTimePatternOptions(pattern, opt, internalSlots.locale, internalSlots.numberingSystem);
	internalSlots.pattern = pattern;
	internalSlots.rangePatterns = rangePatterns;
	const temporalFormats = /* @__PURE__ */ new Map();
	internalSlots.getTemporalFormat = (kind) => {
		const cached = temporalFormats.get(kind);
		if (cached) return cached;
		const dateFields = [
			"weekday",
			"year",
			"month",
			"day"
		];
		const timeFields = [
			"dayPeriod",
			"hour",
			"minute",
			"second",
			"fractionalSecondDigits"
		];
		const allFields = [...dateFields, ...timeFields];
		const required = kind === "PlainDate" ? [...dateFields] : kind === "PlainYearMonth" ? ["year", "month"] : kind === "PlainMonthDay" ? ["month", "day"] : kind === "PlainTime" ? [...timeFields] : allFields;
		const defaults = kind === "PlainDate" ? [
			"year",
			"month",
			"day"
		] : kind === "PlainTime" ? [
			"hour",
			"minute",
			"second"
		] : kind === "PlainYearMonth" || kind === "PlainMonthDay" ? required : [
			"year",
			"month",
			"day",
			"hour",
			"minute",
			"second"
		];
		const allowed = [...required];
		if (kind !== "PlainTime" && kind !== "PlainMonthDay") allowed.push("era");
		if (kind === "Instant") allowed.push("timeZoneName");
		const selectedOptions = Object.create(null);
		let selected;
		if (dateStyle !== void 0 || timeStyle !== void 0) {
			if (kind === "PlainTime" && timeStyle === void 0 || (kind === "PlainDate" || kind === "PlainYearMonth" || kind === "PlainMonthDay") && dateStyle === void 0) throw new TypeError("DateTimeFormat styles do not overlap the Temporal type");
			for (const key of allowed) Object.assign(selectedOptions, { [key]: bestFormat[key] });
			const conflicting = DATE_TIME_PROPS.some((key) => bestFormat[key] !== void 0 && !allowed.includes(key));
			selectedOptions.hour12 = temporalHourCycle === "h11" || temporalHourCycle === "h12";
			selected = !conflicting ? bestFormat : formatMatcher === "basic" ? BasicFormatMatcher(selectedOptions, formats) : BestFitFormatMatcher(selectedOptions, formats);
		} else {
			for (const key of allowed) Object.assign(selectedOptions, { [key]: temporalOptions[key] });
			if (required.every((key) => temporalOptions[key] === void 0)) {
				if (kind !== "Instant" && allFields.some((key) => temporalOptions[key] !== void 0)) throw new TypeError("DateTimeFormat options do not overlap the Temporal type");
				for (const key of defaults) Object.assign(selectedOptions, { [key]: "numeric" });
			}
			selectedOptions.hour12 = temporalHourCycle === "h11" || temporalHourCycle === "h12";
			selected = formatMatcher === "basic" ? BasicFormatMatcher(selectedOptions, formats) : BestFitFormatMatcher(selectedOptions, formats);
		}
		const slots = { ...internalSlots };
		for (const key of DATE_TIME_PROPS) Object.assign(slots, { [key]: selected[key] });
		if (selectedOptions.dayPeriod !== void 0) slots.dayPeriod = selectedOptions.dayPeriod;
		if (selectedOptions.fractionalSecondDigits !== void 0) slots.fractionalSecondDigits = selectedOptions.fractionalSecondDigits;
		slots.hourCycle = temporalHourCycle;
		const use12 = selected.hour !== void 0 && (temporalHourCycle === "h11" || temporalHourCycle === "h12");
		slots.pattern = applyExplicitTimePatternOptions(use12 ? selected.pattern12 : selected.pattern, selectedOptions, slots.locale, slots.numberingSystem);
		slots.rangePatterns = use12 ? selected.rangePatterns12 : selected.rangePatterns;
		slots.format = selected;
		temporalFormats.set(kind, slots);
		return slots;
	};
	return dtf;
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+tz@0.0.0/node_modules/@formatjs_generated/tz/links.js
var links_default = {
	"-5:00": "EST5EDT",
	"-6:00": "CST6CDT",
	"-7:00": "MST7MDT",
	"-8:00": "PST8PDT",
	"1918": "USback",
	"1942": "USback",
	"1945": "USback",
	"1967": "USback",
	"1974": "USback",
	"1975": "USback",
	"1976": "USback",
	"1987": "USback",
	"2007": "USback",
	"Africa/Accra": "Africa/Abidjan",
	"Africa/Addis_Ababa": "Africa/Nairobi",
	"Africa/Asmara": "Africa/Nairobi",
	"Africa/Asmera": "Africa/Nairobi",
	"Africa/Bamako": "Africa/Abidjan",
	"Africa/Bangui": "Africa/Lagos",
	"Africa/Banjul": "Africa/Abidjan",
	"Africa/Blantyre": "Africa/Maputo",
	"Africa/Brazzaville": "Africa/Lagos",
	"Africa/Bujumbura": "Africa/Maputo",
	"Africa/Conakry": "Africa/Abidjan",
	"Africa/Dakar": "Africa/Abidjan",
	"Africa/Dar_es_Salaam": "Africa/Nairobi",
	"Africa/Djibouti": "Africa/Nairobi",
	"Africa/Douala": "Africa/Lagos",
	"Africa/Freetown": "Africa/Abidjan",
	"Africa/Gaborone": "Africa/Maputo",
	"Africa/Harare": "Africa/Maputo",
	"Africa/Kampala": "Africa/Nairobi",
	"Africa/Kigali": "Africa/Maputo",
	"Africa/Kinshasa": "Africa/Lagos",
	"Africa/Libreville": "Africa/Lagos",
	"Africa/Lome": "Africa/Abidjan",
	"Africa/Luanda": "Africa/Lagos",
	"Africa/Lubumbashi": "Africa/Maputo",
	"Africa/Lusaka": "Africa/Maputo",
	"Africa/Malabo": "Africa/Lagos",
	"Africa/Maseru": "Africa/Johannesburg",
	"Africa/Mbabane": "Africa/Johannesburg",
	"Africa/Mogadishu": "Africa/Nairobi",
	"Africa/Niamey": "Africa/Lagos",
	"Africa/Nouakchott": "Africa/Abidjan",
	"Africa/Ouagadougou": "Africa/Abidjan",
	"Africa/Porto-Novo": "Africa/Lagos",
	"Africa/Timbuktu": "Africa/Abidjan",
	"America/Anguilla": "America/Puerto_Rico",
	"America/Antigua": "America/Puerto_Rico",
	"America/Argentina/ComodRivadavia": "America/Argentina/Catamarca",
	"America/Aruba": "America/Puerto_Rico",
	"America/Atikokan": "America/Panama",
	"America/Atka": "America/Adak",
	"America/Blanc-Sablon": "America/Puerto_Rico",
	"America/Buenos_Aires": "America/Argentina/Buenos_Aires",
	"America/Catamarca": "America/Argentina/Catamarca",
	"America/Cayman": "America/Panama",
	"America/Coral_Harbour": "America/Panama",
	"America/Cordoba": "America/Argentina/Cordoba",
	"America/Creston": "America/Phoenix",
	"America/Curacao": "America/Puerto_Rico",
	"America/Dominica": "America/Puerto_Rico",
	"America/Ensenada": "America/Tijuana",
	"America/Fort_Wayne": "America/Indiana/Indianapolis",
	"America/Godthab": "America/Nuuk",
	"America/Grenada": "America/Puerto_Rico",
	"America/Guadeloupe": "America/Puerto_Rico",
	"America/Indianapolis": "America/Indiana/Indianapolis",
	"America/Jujuy": "America/Argentina/Jujuy",
	"America/Knox_IN": "America/Indiana/Knox",
	"America/Kralendijk": "America/Puerto_Rico",
	"America/Louisville": "America/Kentucky/Louisville",
	"America/Lower_Princes": "America/Puerto_Rico",
	"America/Marigot": "America/Puerto_Rico",
	"America/Mendoza": "America/Argentina/Mendoza",
	"America/Montreal": "America/Toronto",
	"America/Montserrat": "America/Puerto_Rico",
	"America/Nassau": "America/Toronto",
	"America/Nipigon": "America/Toronto",
	"America/Pangnirtung": "America/Iqaluit",
	"America/Port_of_Spain": "America/Puerto_Rico",
	"America/Porto_Acre": "America/Rio_Branco",
	"America/Rainy_River": "America/Winnipeg",
	"America/Rosario": "America/Argentina/Cordoba",
	"America/Santa_Isabel": "America/Tijuana",
	"America/Shiprock": "America/Denver",
	"America/St_Barthelemy": "America/Puerto_Rico",
	"America/St_Kitts": "America/Puerto_Rico",
	"America/St_Lucia": "America/Puerto_Rico",
	"America/St_Thomas": "America/Puerto_Rico",
	"America/St_Vincent": "America/Puerto_Rico",
	"America/Thunder_Bay": "America/Toronto",
	"America/Tortola": "America/Puerto_Rico",
	"America/Virgin": "America/Puerto_Rico",
	"America/Yellowknife": "America/Edmonton",
	"Antarctica/DumontDUrville": "Pacific/Port_Moresby",
	"Antarctica/McMurdo": "Pacific/Auckland",
	"Antarctica/South_Pole": "Pacific/Auckland",
	"Antarctica/Syowa": "Asia/Riyadh",
	"Arctic/Longyearbyen": "Europe/Berlin",
	"Asia/Aden": "Asia/Riyadh",
	"Asia/Ashkhabad": "Asia/Ashgabat",
	"Asia/Bahrain": "Asia/Qatar",
	"Asia/Brunei": "Asia/Kuching",
	"Asia/Calcutta": "Asia/Kolkata",
	"Asia/Choibalsan": "Asia/Ulaanbaatar",
	"Asia/Chongqing": "Asia/Shanghai",
	"Asia/Chungking": "Asia/Shanghai",
	"Asia/Dacca": "Asia/Dhaka",
	"Asia/Harbin": "Asia/Shanghai",
	"Asia/Istanbul": "Europe/Istanbul",
	"Asia/Kashgar": "Asia/Urumqi",
	"Asia/Katmandu": "Asia/Kathmandu",
	"Asia/Kuala_Lumpur": "Asia/Singapore",
	"Asia/Kuwait": "Asia/Riyadh",
	"Asia/Macao": "Asia/Macau",
	"Asia/Muscat": "Asia/Dubai",
	"Asia/Phnom_Penh": "Asia/Bangkok",
	"Asia/Rangoon": "Asia/Yangon",
	"Asia/Saigon": "Asia/Ho_Chi_Minh",
	"Asia/Tel_Aviv": "Asia/Jerusalem",
	"Asia/Thimbu": "Asia/Thimphu",
	"Asia/Ujung_Pandang": "Asia/Makassar",
	"Asia/Ulan_Bator": "Asia/Ulaanbaatar",
	"Asia/Vientiane": "Asia/Bangkok",
	"Atlantic/Faeroe": "Atlantic/Faroe",
	"Atlantic/Jan_Mayen": "Europe/Berlin",
	"Atlantic/Reykjavik": "Africa/Abidjan",
	"Atlantic/St_Helena": "Africa/Abidjan",
	"Australia/ACT": "Australia/Sydney",
	"Australia/Canberra": "Australia/Sydney",
	"Australia/Currie": "Australia/Hobart",
	"Australia/LHI": "Australia/Lord_Howe",
	"Australia/NSW": "Australia/Sydney",
	"Australia/North": "Australia/Darwin",
	"Australia/Queensland": "Australia/Brisbane",
	"Australia/South": "Australia/Adelaide",
	"Australia/Tasmania": "Australia/Hobart",
	"Australia/Victoria": "Australia/Melbourne",
	"Australia/West": "Australia/Perth",
	"Australia/Yancowinna": "Australia/Broken_Hill",
	"Brazil/Acre": "America/Rio_Branco",
	"Brazil/DeNoronha": "America/Noronha",
	"Brazil/East": "America/Sao_Paulo",
	"Brazil/West": "America/Manaus",
	"CET": "Europe/Brussels",
	"Canada/Atlantic": "America/Halifax",
	"Canada/Central": "America/Winnipeg",
	"Canada/Eastern": "America/Toronto",
	"Canada/Mountain": "America/Edmonton",
	"Canada/Newfoundland": "America/St_Johns",
	"Canada/Pacific": "America/Vancouver",
	"Canada/Saskatchewan": "America/Regina",
	"Canada/Yukon": "America/Whitehorse",
	"Chile/Continental": "America/Santiago",
	"Chile/EasterIsland": "Pacific/Easter",
	"Cuba": "America/Havana",
	"EET": "Europe/Athens",
	"EST": "America/Panama",
	"Egypt": "Africa/Cairo",
	"Eire": "Europe/Dublin",
	"Etc/GMT+0": "Etc/GMT",
	"Etc/GMT-0": "Etc/GMT",
	"Etc/GMT0": "Etc/GMT",
	"Etc/Greenwich": "Etc/GMT",
	"Etc/UCT": "Etc/UTC",
	"Etc/Universal": "Etc/UTC",
	"Etc/Zulu": "Etc/UTC",
	"Europe/Amsterdam": "Europe/Brussels",
	"Europe/Belfast": "Europe/London",
	"Europe/Bratislava": "Europe/Prague",
	"Europe/Busingen": "Europe/Zurich",
	"Europe/Copenhagen": "Europe/Berlin",
	"Europe/Guernsey": "Europe/London",
	"Europe/Isle_of_Man": "Europe/London",
	"Europe/Jersey": "Europe/London",
	"Europe/Kiev": "Europe/Kyiv",
	"Europe/Ljubljana": "Europe/Belgrade",
	"Europe/Luxembourg": "Europe/Brussels",
	"Europe/Mariehamn": "Europe/Helsinki",
	"Europe/Monaco": "Europe/Paris",
	"Europe/Nicosia": "Asia/Nicosia",
	"Europe/Oslo": "Europe/Berlin",
	"Europe/Podgorica": "Europe/Belgrade",
	"Europe/San_Marino": "Europe/Rome",
	"Europe/Sarajevo": "Europe/Belgrade",
	"Europe/Skopje": "Europe/Belgrade",
	"Europe/Stockholm": "Europe/Berlin",
	"Europe/Tiraspol": "Europe/Chisinau",
	"Europe/Uzhgorod": "Europe/Kyiv",
	"Europe/Vaduz": "Europe/Zurich",
	"Europe/Vatican": "Europe/Rome",
	"Europe/Zagreb": "Europe/Belgrade",
	"Europe/Zaporozhye": "Europe/Kyiv",
	"GB": "Europe/London",
	"GB-Eire": "Europe/London",
	"GMT+0": "Etc/GMT",
	"GMT-0": "Etc/GMT",
	"GMT0": "Etc/GMT",
	"Greenwich": "Etc/GMT",
	"HST": "Pacific/Honolulu",
	"Hongkong": "Asia/Hong_Kong",
	"Iceland": "Africa/Abidjan",
	"Indian/Antananarivo": "Africa/Nairobi",
	"Indian/Christmas": "Asia/Bangkok",
	"Indian/Cocos": "Asia/Yangon",
	"Indian/Comoro": "Africa/Nairobi",
	"Indian/Kerguelen": "Indian/Maldives",
	"Indian/Mahe": "Asia/Dubai",
	"Indian/Mayotte": "Africa/Nairobi",
	"Indian/Reunion": "Asia/Dubai",
	"Iran": "Asia/Tehran",
	"Israel": "Asia/Jerusalem",
	"Jamaica": "America/Jamaica",
	"Japan": "Asia/Tokyo",
	"Kwajalein": "Pacific/Kwajalein",
	"Libya": "Africa/Tripoli",
	"MET": "Europe/Brussels",
	"MST": "America/Phoenix",
	"Mexico/BajaNorte": "America/Tijuana",
	"Mexico/BajaSur": "America/Mazatlan",
	"Mexico/General": "America/Mexico_City",
	"NZ": "Pacific/Auckland",
	"NZ-CHAT": "Pacific/Chatham",
	"Navajo": "America/Denver",
	"PRC": "Asia/Shanghai",
	"Pacific/Chuuk": "Pacific/Port_Moresby",
	"Pacific/Enderbury": "Pacific/Kanton",
	"Pacific/Funafuti": "Pacific/Tarawa",
	"Pacific/Johnston": "Pacific/Honolulu",
	"Pacific/Majuro": "Pacific/Tarawa",
	"Pacific/Midway": "Pacific/Pago_Pago",
	"Pacific/Pohnpei": "Pacific/Guadalcanal",
	"Pacific/Ponape": "Pacific/Guadalcanal",
	"Pacific/Saipan": "Pacific/Guam",
	"Pacific/Samoa": "Pacific/Pago_Pago",
	"Pacific/Truk": "Pacific/Port_Moresby",
	"Pacific/Wake": "Pacific/Tarawa",
	"Pacific/Wallis": "Pacific/Tarawa",
	"Pacific/Yap": "Pacific/Port_Moresby",
	"Poland": "Europe/Warsaw",
	"Portugal": "Europe/Lisbon",
	"ROC": "Asia/Taipei",
	"ROK": "Asia/Seoul",
	"Singapore": "Asia/Singapore",
	"Turkey": "Europe/Istanbul",
	"UCT": "Etc/UTC",
	"US/Alaska": "America/Anchorage",
	"US/Aleutian": "America/Adak",
	"US/Arizona": "America/Phoenix",
	"US/Central": "America/Chicago",
	"US/East-Indiana": "America/Indiana/Indianapolis",
	"US/Eastern": "America/New_York",
	"US/Hawaii": "Pacific/Honolulu",
	"US/Indiana-Starke": "America/Indiana/Knox",
	"US/Michigan": "America/Detroit",
	"US/Mountain": "America/Denver",
	"US/Pacific": "America/Los_Angeles",
	"US/Samoa": "Pacific/Pago_Pago",
	"UTC": "Etc/UTC",
	"Universal": "Etc/UTC",
	"W-SU": "Europe/Moscow",
	"WET": "Europe/Lisbon",
	"Zulu": "Etc/UTC"
};
//#endregion
//#region packages/intl-datetimeformat/get_internal_slots.ts
const internalSlotMap = /* @__PURE__ */ new WeakMap();
function getInternalSlots(x, initialize = false) {
	let internalSlots = internalSlotMap.get(x);
	if (!internalSlots && initialize) {
		internalSlots = Object.create(null);
		internalSlotMap.set(x, internalSlots);
	}
	if (!internalSlots) throw new TypeError("Receiver is not an initialized Intl.DateTimeFormat");
	return internalSlots;
}
//#endregion
//#region packages/intl-datetimeformat/unpack.ts
function unpack(data) {
	const abbrvs = data.abbrvs.split("|");
	const offsets = data.offsets.split("|").map((n) => parseInt(n, 36));
	const packedZones = data.zones;
	const zones = {};
	for (const d of packedZones) {
		const [zone, ...zoneData] = d.split("|");
		zones[zone] = zoneData.map((z) => z.split(",")).map(([ts, abbrvIndex, offsetIndex, dst]) => [
			ts === "" ? -Infinity : parseInt(ts, 36),
			abbrvs[+abbrvIndex],
			offsets[+offsetIndex],
			dst === "1"
		]);
	}
	return zones;
}
//#endregion
//#region packages/intl-datetimeformat/core.ts
const UPPERCASED_LINKS = Object.keys(links_default).reduce((all, l) => {
	all[l.toUpperCase()] = links_default[l];
	return all;
}, {});
const RESOLVED_OPTIONS_KEYS = [
	"locale",
	"calendar",
	"numberingSystem",
	"timeZone",
	"hourCycle",
	"weekday",
	"era",
	"year",
	"month",
	"day",
	"dayPeriod",
	"hour",
	"minute",
	"second",
	"fractionalSecondDigits",
	"timeZoneName",
	"dateStyle",
	"timeStyle"
];
function getDateTimeImplementationDetails() {
	return {
		getInternalSlots,
		localeData: DateTimeFormat.localeData,
		tzData: DateTimeFormat.tzData,
		calendarData: DateTimeFormat.calendarData,
		getDefaultTimeZone: DateTimeFormat.getDefaultTimeZone
	};
}
const formatDescriptor = {
	enumerable: false,
	configurable: true,
	get() {
		const internalSlots = getInternalSlots(this);
		const dtf = this;
		let boundFormat = internalSlots.boundFormat;
		if (boundFormat === void 0) {
			boundFormat = (date) => {
				let x;
				if (date === void 0) x = new Decimal$1(Date.now());
				else x = ToDateTimeFormattable(date);
				return FormatDateTime(dtf, x, getDateTimeImplementationDetails());
			};
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
const DateTimeFormat = function(locales, options) {
	if (!this || !OrdinaryHasInstance(DateTimeFormat, this)) return new DateTimeFormat(locales, options);
	InitializeDateTimeFormat(this, locales, options, {
		tzData: DateTimeFormat.tzData,
		uppercaseLinks: UPPERCASED_LINKS,
		availableLocales: DateTimeFormat.availableLocales,
		relevantExtensionKeys: DateTimeFormat.relevantExtensionKeys,
		getDefaultLocale: DateTimeFormat.getDefaultLocale,
		getDefaultTimeZone: DateTimeFormat.getDefaultTimeZone,
		getInternalSlots: (dtf) => getInternalSlots(dtf, true),
		localeData: DateTimeFormat.localeData
	});
	const dataLocale = getInternalSlots(this).dataLocale;
	const dataLocaleData = DateTimeFormat.localeData[dataLocale];
	invariant(dataLocaleData !== void 0, `Cannot load locale-dependent data for ${dataLocale}.`);
	/** IMPL END */
};
Object.defineProperty(DateTimeFormat, "prototype", { writable: false });
const { supportedLocalesOf } = { supportedLocalesOf(locales, options) {
	return SupportedLocales(DateTimeFormat.availableLocales, CanonicalizeLocaleList(locales), options);
} };
defineProperty(DateTimeFormat, "supportedLocalesOf", { value: supportedLocalesOf });
Object.defineProperty(supportedLocalesOf, "length", {
	value: 1,
	configurable: true
});
const { resolvedOptions } = { resolvedOptions() {
	const internalSlots = getInternalSlots(this);
	const ro = {};
	for (const key of RESOLVED_OPTIONS_KEYS) {
		let value = internalSlots[key];
		if (DATE_TIME_PROPS.indexOf(key) > -1) {
			if (internalSlots.dateStyle !== void 0 || internalSlots.timeStyle !== void 0) value = void 0;
		}
		if (value !== void 0) {
			createDataProperty(ro, key, value);
			if (key === "hourCycle") {
				const hour12 = value === "h11" || value === "h12" ? true : value === "h23" || value === "h24" ? false : void 0;
				if (hour12 !== void 0) createDataProperty(ro, "hour12", hour12);
			}
		}
	}
	return ro;
} };
defineProperty(DateTimeFormat.prototype, "resolvedOptions", { value: resolvedOptions });
const { formatToParts } = { formatToParts(date) {
	getInternalSlots(this);
	let x;
	if (date === void 0) x = new Decimal$1(Date.now());
	else x = ToDateTimeFormattable(date);
	return FormatDateTimeToParts(this, x, getDateTimeImplementationDetails());
} };
defineProperty(DateTimeFormat.prototype, "formatToParts", { value: formatToParts });
const { formatRangeToParts } = { formatRangeToParts(startDate, endDate) {
	const dtf = this;
	getInternalSlots(dtf);
	invariant(startDate !== void 0 && endDate !== void 0, "startDate/endDate cannot be undefined", TypeError);
	return FormatDateTimeRangeToParts(dtf, ToDateTimeFormattable(startDate), ToDateTimeFormattable(endDate), getDateTimeImplementationDetails());
} };
defineProperty(DateTimeFormat.prototype, "formatRangeToParts", { value: formatRangeToParts });
const { formatRange } = { formatRange(startDate, endDate) {
	const dtf = this;
	getInternalSlots(dtf);
	invariant(startDate !== void 0 && endDate !== void 0, "startDate/endDate cannot be undefined", TypeError);
	return FormatDateTimeRange(dtf, ToDateTimeFormattable(startDate), ToDateTimeFormattable(endDate), getDateTimeImplementationDetails());
} };
defineProperty(DateTimeFormat.prototype, "formatRange", { value: formatRange });
const DEFAULT_TIMEZONE = "UTC";
DateTimeFormat.__setDefaultTimeZone = (timeZone) => {
	if (timeZone !== void 0) {
		timeZone = String(timeZone);
		if (!IsValidTimeZoneName(timeZone, {
			zoneNamesFromData: Object.keys(DateTimeFormat.tzData),
			uppercaseLinks: UPPERCASED_LINKS
		})) throw new RangeError("Invalid timeZoneName");
		timeZone = CanonicalizeTimeZoneName(timeZone, {
			zoneNames: Object.keys(DateTimeFormat.tzData),
			uppercaseLinks: UPPERCASED_LINKS
		});
	} else timeZone = DEFAULT_TIMEZONE;
	DateTimeFormat.__defaultTimeZone = timeZone;
};
DateTimeFormat.relevantExtensionKeys = [
	"nu",
	"ca",
	"hc"
];
DateTimeFormat.__defaultTimeZone = DEFAULT_TIMEZONE;
DateTimeFormat.getDefaultTimeZone = () => DateTimeFormat.__defaultTimeZone;
/**
* GH #4535: When a format skeleton uses raw pattern form (e.g., "MMMEd, h:mm a")
* instead of canonical form (e.g., "MMMEd, hm"), interval formats won't match
* by exact key. This function finds the matching canonical interval format by
* normalizing the time portion of the skeleton.
*/
function findIntervalFormat(skeleton, intervalFormats) {
	const commaIdx = skeleton.indexOf(", ");
	if (commaIdx !== -1) {
		const datePart = skeleton.slice(0, commaIdx);
		const timePart = skeleton.slice(commaIdx + 2);
		let canonical = "";
		for (const c of timePart) if ((c >= "a" && c <= "z" || c >= "A" && c <= "Z") && c !== "a" && c !== "b" && c !== "B" && canonical[canonical.length - 1] !== c) canonical += c;
		const matched = intervalFormats[`${datePart}, ${canonical}`];
		if (matched) return matched;
	}
	const target = parseDateTimeSkeleton(skeleton);
	if (target.hour || target.minute || target.second || target.dayPeriod || target.timeZoneName) return void 0;
	for (const key of Object.keys(intervalFormats)) {
		if (typeof intervalFormats[key] !== "object") continue;
		const candidate = parseDateTimeSkeleton(key);
		const numericMonth = (month) => month === "numeric" || month === "2-digit";
		if (numericMonth(target.month) !== numericMonth(candidate.month)) continue;
		if (DATE_TIME_PROPS.every((field) => target[field] !== void 0 === (candidate[field] !== void 0))) return intervalFormats[key];
	}
}
function parseDateTimeStyles({ dateFormat, timeFormat, dateTimeFormat, intervalFormats }) {
	const parseStyle = (pattern) => parseDateTimeSkeleton(pattern, pattern, findIntervalFormat(pattern, intervalFormats), intervalFormats.intervalFormatFallback);
	return {
		dateFormat: {
			full: parseStyle(dateFormat.full),
			long: parseStyle(dateFormat.long),
			medium: parseStyle(dateFormat.medium),
			short: parseStyle(dateFormat.short)
		},
		timeFormat: {
			full: parseStyle(timeFormat.full),
			long: parseStyle(timeFormat.long),
			medium: parseStyle(timeFormat.medium),
			short: parseStyle(timeFormat.short)
		},
		dateTimeFormat: {
			full: parseDateTimeSkeleton(dateTimeFormat.full).pattern,
			long: parseDateTimeSkeleton(dateTimeFormat.long).pattern,
			medium: parseDateTimeSkeleton(dateTimeFormat.medium).pattern,
			short: parseDateTimeSkeleton(dateTimeFormat.short).pattern
		}
	};
}
const rawLocaleData = /* @__PURE__ */ new Map();
const calendarLocaleData = /* @__PURE__ */ new Map();
const processedLocales = /* @__PURE__ */ new Map();
const calendarPreferences = /* @__PURE__ */ new WeakMap();
function updateAvailableCalendars(data) {
	data.ca = calendarPreferences.get(data).filter((calendar) => Object.prototype.hasOwnProperty.call(data.formats, calendar) && (calendar === "gregory" || calendar === "iso8601" || Object.prototype.hasOwnProperty.call(DateTimeFormat.calendarData, calendar)));
}
DateTimeFormat.__addLocaleData = function __addLocaleData(...data) {
	for (const entry of data) {
		const { locale } = entry;
		rawLocaleData.set(locale, entry);
		const d = {
			...entry.data,
			ca: [...entry.data.ca],
			formats: { ...entry.data.formats },
			calendarData: { ...entry.data.calendarData }
		};
		for (const patch of calendarLocaleData.get(locale)?.values() ?? []) {
			if (d.ca.indexOf(patch.calendar) < 0) d.ca.push(patch.calendar);
			d.formats[patch.calendar] = patch.formats;
			d.calendarData[patch.calendar] = patch.data;
		}
		const { dateFormat, timeFormat, dateTimeFormat, formats, intervalFormats, calendarData, ...rawData } = d;
		const processedData = {
			...rawData,
			...parseDateTimeStyles({
				dateFormat,
				timeFormat,
				dateTimeFormat,
				intervalFormats
			}),
			intervalFormatFallback: intervalFormats.intervalFormatFallback,
			formats: {}
		};
		for (const calendar in formats) {
			if (calendar === "iso8601") continue;
			const calendarIntervals = calendarData?.[calendar]?.intervalFormats ?? intervalFormats;
			let parsed;
			Object.defineProperty(processedData.formats, calendar, {
				enumerable: true,
				get() {
					return parsed ??= Object.keys(formats[calendar]).map((skeleton) => parseDateTimeSkeleton(skeleton, formats[calendar][skeleton], calendarIntervals[skeleton] || findIntervalFormat(skeleton, calendarIntervals), calendarIntervals.intervalFormatFallback));
				}
			});
		}
		Object.defineProperty(processedData.formats, "iso8601", {
			enumerable: true,
			get: () => processedData.formats.gregory
		});
		if (calendarData) {
			processedData.calendarData = {};
			for (const calendar of Object.keys(calendarData)) {
				const { intervalFormats: calendarIntervals, ...calendarFields } = calendarData[calendar];
				let parsed;
				Object.defineProperty(processedData.calendarData, calendar, {
					enumerable: true,
					get() {
						return parsed ??= {
							...processedData,
							...calendarFields,
							formats: processedData.formats,
							...parseDateTimeStyles({
								...calendarFields,
								intervalFormats: calendarIntervals
							}),
							intervalFormatFallback: calendarIntervals.intervalFormatFallback,
							calendarData: void 0
						};
					}
				});
			}
		}
		calendarPreferences.set(processedData, [...d.ca]);
		updateAvailableCalendars(processedData);
		const previous = processedLocales.get(locale);
		if (previous) {
			for (const tag of Object.keys(DateTimeFormat.localeData)) if (DateTimeFormat.localeData[tag] === previous) DateTimeFormat.localeData[tag] = processedData;
		}
		processedLocales.set(locale, processedData);
		registerLocaleData(locale, processedData, DateTimeFormat.localeData, DateTimeFormat.availableLocales);
		if (!DateTimeFormat.__defaultLocale) DateTimeFormat.__defaultLocale = locale;
	}
};
Object.defineProperty(DateTimeFormat.prototype, "format", formatDescriptor);
DateTimeFormat.__defaultLocale = "";
DateTimeFormat.localeData = {};
DateTimeFormat.availableLocales = /* @__PURE__ */ new Set();
DateTimeFormat.getDefaultLocale = () => {
	return DateTimeFormat.__defaultLocale;
};
DateTimeFormat.polyfilled = true;
DateTimeFormat.calendarData = Object.create(null);
DateTimeFormat.__addCalendarData = function(...data) {
	for (const { calendar, dateFromTime } of data) createDataProperty(DateTimeFormat.calendarData, calendar, dateFromTime);
	for (const locale of new Set(Object.keys(DateTimeFormat.localeData).map((locale) => DateTimeFormat.localeData[locale]))) updateAvailableCalendars(locale);
};
DateTimeFormat.__addCalendarLocaleData = function(...data) {
	for (const patch of data) {
		let calendars = calendarLocaleData.get(patch.locale);
		if (!calendars) {
			calendars = /* @__PURE__ */ new Map();
			calendarLocaleData.set(patch.locale, calendars);
		}
		calendars.set(patch.calendar, patch);
		const raw = rawLocaleData.get(patch.locale);
		if (raw) DateTimeFormat.__addLocaleData(raw);
	}
};
DateTimeFormat.tzData = {};
DateTimeFormat.__addTZData = function(d) {
	DateTimeFormat.tzData = unpack(d);
};
try {
	if (typeof Symbol !== "undefined") Object.defineProperty(DateTimeFormat.prototype, Symbol.toStringTag, {
		value: "Intl.DateTimeFormat",
		writable: false,
		enumerable: false,
		configurable: true
	});
	Object.defineProperty(DateTimeFormat.prototype.constructor, "length", {
		value: 0,
		writable: false,
		enumerable: false,
		configurable: true
	});
} catch {}
//#endregion
//#region packages/ecma402-abstract/DateTimeFormat/ToDateTimeOptions.js
/**
* https://tc39.es/ecma402/#sec-todatetimeoptions
* @param options
* @param required
* @param defaults
*/
function ToDateTimeOptions(options, required, defaults) {
	if (options === void 0) options = null;
	else options = ToObject(options);
	options = Object.create(options);
	let needDefaults = true;
	if (required === "date" || required === "any") {
		for (const prop of [
			"weekday",
			"year",
			"month",
			"day"
		]) if (options[prop] !== void 0) needDefaults = false;
	}
	if (required === "time" || required === "any") {
		for (const prop of [
			"dayPeriod",
			"hour",
			"minute",
			"second",
			"fractionalSecondDigits"
		]) if (options[prop] !== void 0) needDefaults = false;
	}
	if (options.dateStyle !== void 0 || options.timeStyle !== void 0) needDefaults = false;
	if (required === "date" && options.timeStyle) throw new TypeError("Intl.DateTimeFormat date was required but timeStyle was included");
	if (required === "time" && options.dateStyle) throw new TypeError("Intl.DateTimeFormat time was required but dateStyle was included");
	if (needDefaults && (defaults === "date" || defaults === "all")) for (const prop of [
		"year",
		"month",
		"day"
	]) options[prop] = "numeric";
	if (needDefaults && (defaults === "time" || defaults === "all")) for (const prop of [
		"hour",
		"minute",
		"second"
	]) options[prop] = "numeric";
	return options;
}
//#endregion
//#region packages/intl-datetimeformat/to_locale_string.ts
/**
* https://tc39.es/ecma402/#sup-date.prototype.tolocalestring
*/
function toLocaleString(x, locales, options) {
	return new DateTimeFormat(locales, ToDateTimeOptions(options, "any", "all")).format(x);
}
/**
* https://tc39.es/ecma402/#sup-date.prototype.tolocaledatestring
*/
function toLocaleDateString(x, locales, options) {
	return new DateTimeFormat(locales, ToDateTimeOptions(options, "date", "date")).format(x);
}
/**
* https://tc39.es/ecma402/#sup-date.prototype.tolocaletimestring
*/
function toLocaleTimeString(x, locales, options) {
	return new DateTimeFormat(locales, ToDateTimeOptions(options, "time", "time")).format(x);
}
defineProperty(ensureIntl(), "DateTimeFormat", { value: DateTimeFormat });
defineProperty(Date.prototype, "toLocaleString", { value: function toLocaleString$1(locales, options) {
	try {
		return toLocaleString(this, locales, options);
	} catch {
		return "Invalid Date";
	}
} });
defineProperty(Date.prototype, "toLocaleDateString", { value: function toLocaleDateString$1(locales, options) {
	try {
		return toLocaleDateString(this, locales, options);
	} catch {
		return "Invalid Date";
	}
} });
defineProperty(Date.prototype, "toLocaleTimeString", { value: function toLocaleTimeString$1(locales, options) {
	try {
		return toLocaleTimeString(this, locales, options);
	} catch {
		return "Invalid Date";
	}
} });
for (const [key, register] of [["__FORMATJS_DATETIMEFORMAT_CALENDAR_DATA__", DateTimeFormat.__addCalendarData], ["__FORMATJS_DATETIMEFORMAT_CALENDAR_LOCALE_DATA__", DateTimeFormat.__addCalendarLocaleData]]) {
	const globals = globalThis;
	const queue = globals[key];
	if (queue) {
		for (const data of queue) register(data);
		delete globals[key];
	}
}
const buf = globalThis.__FORMATJS_DATETIMEFORMAT_DATA__;
if (buf) {
	for (const d of buf) DateTimeFormat.__addLocaleData(d);
	delete globalThis.__FORMATJS_DATETIMEFORMAT_DATA__;
}
//#endregion

//# sourceMappingURL=polyfill-force.js.map