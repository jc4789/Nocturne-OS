import { supportedValuesOf } from "@formatjs/intl-supportedvaluesof";
import { emitUnicodeLanguageId, emitUnicodeLocaleId, isStructurallyValidLanguageTag, isUnicodeLanguageSubtag, isUnicodeRegionSubtag, isUnicodeScriptSubtag, isUnicodeVariantSubtag, likelySubtags, parseUnicodeLanguageId, parseUnicodeLocaleId } from "@formatjs/intl-getcanonicallocales";
//#region packages/ecma262-abstract/ToString.js
/**
* https://tc39.es/ecma262/#sec-tostring
*/
function ToString(o) {
	if (typeof o === "symbol") throw TypeError("Cannot convert a Symbol value to a string");
	return String(o);
}
//#endregion
//#region packages/ecma402-abstract/IsUnicodeLocaleIdentifierType.js
/** Tests the Unicode locale identifier `type` grammar, not locale support. */
function IsUnicodeLocaleIdentifierType(value) {
	return /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*(?![\s\S])/i.test(value);
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
memoize((...args) => new Intl.NumberFormat(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.PluralRules(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.ListFormat(...args), { strategy: strategies.variadic });
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.locale@0.0.0/node_modules/@formatjs_generated/cldr.locale/character-orders.js
const characterOrders = {
	"aa": "left-to-right",
	"aa-DJ": "left-to-right",
	"aa-ER": "left-to-right",
	"ab": "left-to-right",
	"af": "left-to-right",
	"af-NA": "left-to-right",
	"agq": "left-to-right",
	"ak": "left-to-right",
	"am": "left-to-right",
	"an": "left-to-right",
	"ann": "left-to-right",
	"apc": "right-to-left",
	"ar": "right-to-left",
	"ar-AE": "right-to-left",
	"ar-BH": "right-to-left",
	"ar-DJ": "right-to-left",
	"ar-DZ": "right-to-left",
	"ar-EG": "right-to-left",
	"ar-EH": "right-to-left",
	"ar-ER": "right-to-left",
	"ar-IL": "right-to-left",
	"ar-IQ": "right-to-left",
	"ar-JO": "right-to-left",
	"ar-KM": "right-to-left",
	"ar-KW": "right-to-left",
	"ar-LB": "right-to-left",
	"ar-LY": "right-to-left",
	"ar-MA": "right-to-left",
	"ar-MR": "right-to-left",
	"ar-OM": "right-to-left",
	"ar-PS": "right-to-left",
	"ar-QA": "right-to-left",
	"ar-SA": "right-to-left",
	"ar-SD": "right-to-left",
	"ar-SO": "right-to-left",
	"ar-SS": "right-to-left",
	"ar-SY": "right-to-left",
	"ar-TD": "right-to-left",
	"ar-TN": "right-to-left",
	"ar-YE": "right-to-left",
	"arn": "left-to-right",
	"as": "left-to-right",
	"asa": "left-to-right",
	"ast": "left-to-right",
	"az": "left-to-right",
	"az-Arab": "right-to-left",
	"az-Arab-IQ": "right-to-left",
	"az-Arab-TR": "right-to-left",
	"az-Cyrl": "left-to-right",
	"az-Latn": "left-to-right",
	"ba": "left-to-right",
	"bal": "right-to-left",
	"bal-Arab": "right-to-left",
	"bal-Latn": "left-to-right",
	"bas": "left-to-right",
	"be": "left-to-right",
	"be-tarask": "left-to-right",
	"bem": "left-to-right",
	"bew": "left-to-right",
	"bez": "left-to-right",
	"bg": "left-to-right",
	"bgc": "left-to-right",
	"bgn": "right-to-left",
	"bgn-AE": "right-to-left",
	"bgn-AF": "right-to-left",
	"bgn-IR": "right-to-left",
	"bgn-OM": "right-to-left",
	"bho": "left-to-right",
	"blo": "left-to-right",
	"blt": "left-to-right",
	"bm": "left-to-right",
	"bm-Nkoo": "right-to-left",
	"bn": "left-to-right",
	"bn-IN": "left-to-right",
	"bo": "left-to-right",
	"bo-IN": "left-to-right",
	"bqi": "right-to-left",
	"br": "left-to-right",
	"brx": "left-to-right",
	"bs": "left-to-right",
	"bs-Cyrl": "left-to-right",
	"bs-Latn": "left-to-right",
	"bss": "left-to-right",
	"bua": "left-to-right",
	"byn": "left-to-right",
	"ca": "left-to-right",
	"ca-AD": "left-to-right",
	"ca-ES-valencia": "left-to-right",
	"ca-FR": "left-to-right",
	"ca-IT": "left-to-right",
	"cad": "left-to-right",
	"cch": "left-to-right",
	"ccp": "left-to-right",
	"ccp-IN": "left-to-right",
	"ce": "left-to-right",
	"ceb": "left-to-right",
	"cgg": "left-to-right",
	"cho": "left-to-right",
	"chr": "left-to-right",
	"cic": "left-to-right",
	"ckb": "right-to-left",
	"ckb-IR": "right-to-left",
	"co": "left-to-right",
	"cop": "left-to-right",
	"cs": "left-to-right",
	"csw": "left-to-right",
	"cu": "left-to-right",
	"cv": "left-to-right",
	"cy": "left-to-right",
	"da": "left-to-right",
	"da-GL": "left-to-right",
	"dav": "left-to-right",
	"de": "left-to-right",
	"de-AT": "left-to-right",
	"de-BE": "left-to-right",
	"de-CH": "left-to-right",
	"de-IT": "left-to-right",
	"de-LI": "left-to-right",
	"de-LU": "left-to-right",
	"dje": "left-to-right",
	"doi": "left-to-right",
	"dsb": "left-to-right",
	"dua": "left-to-right",
	"dv": "right-to-left",
	"dyo": "left-to-right",
	"dz": "left-to-right",
	"ebu": "left-to-right",
	"ee": "left-to-right",
	"ee-TG": "left-to-right",
	"el": "left-to-right",
	"el-CY": "left-to-right",
	"el-polyton": "left-to-right",
	"en": "left-to-right",
	"en-001": "left-to-right",
	"en-150": "left-to-right",
	"en-AE": "left-to-right",
	"en-AG": "left-to-right",
	"en-AI": "left-to-right",
	"en-AS": "left-to-right",
	"en-AT": "left-to-right",
	"en-AU": "left-to-right",
	"en-BB": "left-to-right",
	"en-BE": "left-to-right",
	"en-BI": "left-to-right",
	"en-BM": "left-to-right",
	"en-BS": "left-to-right",
	"en-BW": "left-to-right",
	"en-BZ": "left-to-right",
	"en-CA": "left-to-right",
	"en-CC": "left-to-right",
	"en-CH": "left-to-right",
	"en-CK": "left-to-right",
	"en-CM": "left-to-right",
	"en-CX": "left-to-right",
	"en-CY": "left-to-right",
	"en-CZ": "left-to-right",
	"en-DE": "left-to-right",
	"en-DG": "left-to-right",
	"en-DK": "left-to-right",
	"en-DM": "left-to-right",
	"en-Dsrt": "left-to-right",
	"en-EE": "left-to-right",
	"en-ER": "left-to-right",
	"en-ES": "left-to-right",
	"en-FI": "left-to-right",
	"en-FJ": "left-to-right",
	"en-FK": "left-to-right",
	"en-FM": "left-to-right",
	"en-FR": "left-to-right",
	"en-GB": "left-to-right",
	"en-GD": "left-to-right",
	"en-GE": "left-to-right",
	"en-GG": "left-to-right",
	"en-GH": "left-to-right",
	"en-GI": "left-to-right",
	"en-GM": "left-to-right",
	"en-GS": "left-to-right",
	"en-GU": "left-to-right",
	"en-GY": "left-to-right",
	"en-HK": "left-to-right",
	"en-HU": "left-to-right",
	"en-ID": "left-to-right",
	"en-IE": "left-to-right",
	"en-IL": "left-to-right",
	"en-IM": "left-to-right",
	"en-IN": "left-to-right",
	"en-IO": "left-to-right",
	"en-IT": "left-to-right",
	"en-JE": "left-to-right",
	"en-JM": "left-to-right",
	"en-JP": "left-to-right",
	"en-KE": "left-to-right",
	"en-KI": "left-to-right",
	"en-KN": "left-to-right",
	"en-KY": "left-to-right",
	"en-LC": "left-to-right",
	"en-LR": "left-to-right",
	"en-LS": "left-to-right",
	"en-LT": "left-to-right",
	"en-LV": "left-to-right",
	"en-MG": "left-to-right",
	"en-MH": "left-to-right",
	"en-MO": "left-to-right",
	"en-MP": "left-to-right",
	"en-MS": "left-to-right",
	"en-MT": "left-to-right",
	"en-MU": "left-to-right",
	"en-MV": "left-to-right",
	"en-MW": "left-to-right",
	"en-MY": "left-to-right",
	"en-NA": "left-to-right",
	"en-NF": "left-to-right",
	"en-NG": "left-to-right",
	"en-NL": "left-to-right",
	"en-NO": "left-to-right",
	"en-NR": "left-to-right",
	"en-NU": "left-to-right",
	"en-NZ": "left-to-right",
	"en-PG": "left-to-right",
	"en-PH": "left-to-right",
	"en-PK": "left-to-right",
	"en-PL": "left-to-right",
	"en-PN": "left-to-right",
	"en-PR": "left-to-right",
	"en-PT": "left-to-right",
	"en-PW": "left-to-right",
	"en-RO": "left-to-right",
	"en-RW": "left-to-right",
	"en-SB": "left-to-right",
	"en-SC": "left-to-right",
	"en-SD": "left-to-right",
	"en-SE": "left-to-right",
	"en-SG": "left-to-right",
	"en-SH": "left-to-right",
	"en-Shaw": "left-to-right",
	"en-SI": "left-to-right",
	"en-SK": "left-to-right",
	"en-SL": "left-to-right",
	"en-SS": "left-to-right",
	"en-SX": "left-to-right",
	"en-SZ": "left-to-right",
	"en-TC": "left-to-right",
	"en-TK": "left-to-right",
	"en-TO": "left-to-right",
	"en-TT": "left-to-right",
	"en-TV": "left-to-right",
	"en-TZ": "left-to-right",
	"en-UA": "left-to-right",
	"en-UG": "left-to-right",
	"en-UM": "left-to-right",
	"en-VC": "left-to-right",
	"en-VG": "left-to-right",
	"en-VI": "left-to-right",
	"en-VU": "left-to-right",
	"en-WS": "left-to-right",
	"en-ZA": "left-to-right",
	"en-ZM": "left-to-right",
	"en-ZW": "left-to-right",
	"eo": "left-to-right",
	"es": "left-to-right",
	"es-419": "left-to-right",
	"es-AR": "left-to-right",
	"es-BO": "left-to-right",
	"es-BR": "left-to-right",
	"es-BZ": "left-to-right",
	"es-CL": "left-to-right",
	"es-CO": "left-to-right",
	"es-CR": "left-to-right",
	"es-CU": "left-to-right",
	"es-DO": "left-to-right",
	"es-EA": "left-to-right",
	"es-EC": "left-to-right",
	"es-GQ": "left-to-right",
	"es-GT": "left-to-right",
	"es-HN": "left-to-right",
	"es-IC": "left-to-right",
	"es-MX": "left-to-right",
	"es-NI": "left-to-right",
	"es-PA": "left-to-right",
	"es-PE": "left-to-right",
	"es-PH": "left-to-right",
	"es-PR": "left-to-right",
	"es-PY": "left-to-right",
	"es-SV": "left-to-right",
	"es-US": "left-to-right",
	"es-UY": "left-to-right",
	"es-VE": "left-to-right",
	"et": "left-to-right",
	"eu": "left-to-right",
	"ewo": "left-to-right",
	"fa": "right-to-left",
	"fa-AF": "right-to-left",
	"ff": "left-to-right",
	"ff-Adlm": "right-to-left",
	"ff-Adlm-BF": "right-to-left",
	"ff-Adlm-CM": "right-to-left",
	"ff-Adlm-GH": "right-to-left",
	"ff-Adlm-GM": "right-to-left",
	"ff-Adlm-GW": "right-to-left",
	"ff-Adlm-LR": "right-to-left",
	"ff-Adlm-MR": "right-to-left",
	"ff-Adlm-NE": "right-to-left",
	"ff-Adlm-NG": "right-to-left",
	"ff-Adlm-SL": "right-to-left",
	"ff-Adlm-SN": "right-to-left",
	"ff-Latn": "left-to-right",
	"ff-Latn-BF": "left-to-right",
	"ff-Latn-CM": "left-to-right",
	"ff-Latn-GH": "left-to-right",
	"ff-Latn-GM": "left-to-right",
	"ff-Latn-GN": "left-to-right",
	"ff-Latn-GW": "left-to-right",
	"ff-Latn-LR": "left-to-right",
	"ff-Latn-MR": "left-to-right",
	"ff-Latn-NE": "left-to-right",
	"ff-Latn-NG": "left-to-right",
	"ff-Latn-SL": "left-to-right",
	"fi": "left-to-right",
	"fil": "left-to-right",
	"fo": "left-to-right",
	"fo-DK": "left-to-right",
	"fr": "left-to-right",
	"fr-BE": "left-to-right",
	"fr-BF": "left-to-right",
	"fr-BI": "left-to-right",
	"fr-BJ": "left-to-right",
	"fr-BL": "left-to-right",
	"fr-CA": "left-to-right",
	"fr-CD": "left-to-right",
	"fr-CF": "left-to-right",
	"fr-CG": "left-to-right",
	"fr-CH": "left-to-right",
	"fr-CI": "left-to-right",
	"fr-CM": "left-to-right",
	"fr-DJ": "left-to-right",
	"fr-DZ": "left-to-right",
	"fr-GA": "left-to-right",
	"fr-GF": "left-to-right",
	"fr-GN": "left-to-right",
	"fr-GP": "left-to-right",
	"fr-GQ": "left-to-right",
	"fr-HT": "left-to-right",
	"fr-KM": "left-to-right",
	"fr-LU": "left-to-right",
	"fr-MA": "left-to-right",
	"fr-MC": "left-to-right",
	"fr-MF": "left-to-right",
	"fr-MG": "left-to-right",
	"fr-ML": "left-to-right",
	"fr-MQ": "left-to-right",
	"fr-MR": "left-to-right",
	"fr-MU": "left-to-right",
	"fr-NC": "left-to-right",
	"fr-NE": "left-to-right",
	"fr-PF": "left-to-right",
	"fr-PM": "left-to-right",
	"fr-RE": "left-to-right",
	"fr-RW": "left-to-right",
	"fr-SC": "left-to-right",
	"fr-SN": "left-to-right",
	"fr-SY": "left-to-right",
	"fr-TD": "left-to-right",
	"fr-TG": "left-to-right",
	"fr-TN": "left-to-right",
	"fr-VU": "left-to-right",
	"fr-WF": "left-to-right",
	"fr-YT": "left-to-right",
	"frr": "left-to-right",
	"fur": "left-to-right",
	"fy": "left-to-right",
	"ga": "left-to-right",
	"ga-GB": "left-to-right",
	"gaa": "left-to-right",
	"gd": "left-to-right",
	"gez": "left-to-right",
	"gez-ER": "left-to-right",
	"gl": "left-to-right",
	"gn": "left-to-right",
	"gsw": "left-to-right",
	"gsw-FR": "left-to-right",
	"gsw-LI": "left-to-right",
	"gu": "left-to-right",
	"guz": "left-to-right",
	"gv": "left-to-right",
	"ha": "left-to-right",
	"ha-Arab": "right-to-left",
	"ha-Arab-SD": "right-to-left",
	"ha-GH": "left-to-right",
	"ha-NE": "left-to-right",
	"haw": "left-to-right",
	"he": "right-to-left",
	"hi": "left-to-right",
	"hi-Latn": "left-to-right",
	"hnj": "left-to-right",
	"hnj-Hmnp": "left-to-right",
	"hr": "left-to-right",
	"hr-BA": "left-to-right",
	"hsb": "left-to-right",
	"ht": "left-to-right",
	"hu": "left-to-right",
	"hy": "left-to-right",
	"ia": "left-to-right",
	"id": "left-to-right",
	"ie": "left-to-right",
	"ig": "left-to-right",
	"ii": "left-to-right",
	"io": "left-to-right",
	"is": "left-to-right",
	"it": "left-to-right",
	"it-CH": "left-to-right",
	"it-SM": "left-to-right",
	"it-VA": "left-to-right",
	"iu": "left-to-right",
	"iu-Latn": "left-to-right",
	"ja": "left-to-right",
	"jbo": "left-to-right",
	"jgo": "left-to-right",
	"jmc": "left-to-right",
	"jv": "left-to-right",
	"ka": "left-to-right",
	"kaa": "left-to-right",
	"kaa-Cyrl": "left-to-right",
	"kaa-Latn": "left-to-right",
	"kab": "left-to-right",
	"kaj": "left-to-right",
	"kam": "left-to-right",
	"kcg": "left-to-right",
	"kde": "left-to-right",
	"kea": "left-to-right",
	"kek": "left-to-right",
	"ken": "left-to-right",
	"kgp": "left-to-right",
	"khq": "left-to-right",
	"ki": "left-to-right",
	"kk": "left-to-right",
	"kk-Arab": "right-to-left",
	"kk-Cyrl": "left-to-right",
	"kk-KZ": "left-to-right",
	"kkj": "left-to-right",
	"kl": "left-to-right",
	"kln": "left-to-right",
	"km": "left-to-right",
	"kn": "left-to-right",
	"ko": "left-to-right",
	"ko-CN": "left-to-right",
	"ko-KP": "left-to-right",
	"kok": "left-to-right",
	"kok-Deva": "left-to-right",
	"kok-Latn": "left-to-right",
	"kpe": "left-to-right",
	"kpe-GN": "left-to-right",
	"ks": "right-to-left",
	"ks-Arab": "right-to-left",
	"ks-Deva": "left-to-right",
	"ksb": "left-to-right",
	"ksf": "left-to-right",
	"ksh": "left-to-right",
	"ku": "left-to-right",
	"ku-Arab": "right-to-left",
	"ku-Arab-IR": "right-to-left",
	"ku-Latn": "left-to-right",
	"ku-Latn-IQ": "left-to-right",
	"ku-Latn-SY": "left-to-right",
	"ku-TR": "left-to-right",
	"kw": "left-to-right",
	"kxv": "left-to-right",
	"kxv-Deva": "left-to-right",
	"kxv-Latn": "left-to-right",
	"kxv-Orya": "left-to-right",
	"kxv-Telu": "left-to-right",
	"ky": "left-to-right",
	"la": "left-to-right",
	"lag": "left-to-right",
	"lb": "left-to-right",
	"lg": "left-to-right",
	"lij": "left-to-right",
	"lkt": "left-to-right",
	"lld": "left-to-right",
	"lmo": "left-to-right",
	"ln": "left-to-right",
	"ln-AO": "left-to-right",
	"ln-CF": "left-to-right",
	"ln-CG": "left-to-right",
	"lo": "left-to-right",
	"lrc": "right-to-left",
	"lrc-IQ": "right-to-left",
	"lt": "left-to-right",
	"ltg": "left-to-right",
	"lu": "left-to-right",
	"luo": "left-to-right",
	"luy": "left-to-right",
	"lv": "left-to-right",
	"lzz": "left-to-right",
	"mai": "left-to-right",
	"mas": "left-to-right",
	"mas-TZ": "left-to-right",
	"mdf": "left-to-right",
	"mer": "left-to-right",
	"mfe": "left-to-right",
	"mg": "left-to-right",
	"mgh": "left-to-right",
	"mgo": "left-to-right",
	"mhn": "left-to-right",
	"mi": "left-to-right",
	"mic": "left-to-right",
	"mk": "left-to-right",
	"ml": "left-to-right",
	"mn": "left-to-right",
	"mn-Mong": "top-to-bottom",
	"mn-Mong-MN": "top-to-bottom",
	"mni": "left-to-right",
	"mni-Beng": "left-to-right",
	"mni-Mtei": "left-to-right",
	"moh": "left-to-right",
	"mr": "left-to-right",
	"ms": "left-to-right",
	"ms-Arab": "right-to-left",
	"ms-Arab-BN": "right-to-left",
	"ms-BN": "left-to-right",
	"ms-ID": "left-to-right",
	"ms-SG": "left-to-right",
	"mt": "left-to-right",
	"mua": "left-to-right",
	"mus": "left-to-right",
	"mww": "left-to-right",
	"mww-Hmnp": "left-to-right",
	"my": "left-to-right",
	"myv": "left-to-right",
	"mzn": "right-to-left",
	"naq": "left-to-right",
	"nb": "left-to-right",
	"nb-SJ": "left-to-right",
	"nd": "left-to-right",
	"nds": "left-to-right",
	"nds-NL": "left-to-right",
	"ne": "left-to-right",
	"ne-IN": "left-to-right",
	"nl": "left-to-right",
	"nl-AW": "left-to-right",
	"nl-BE": "left-to-right",
	"nl-BQ": "left-to-right",
	"nl-CW": "left-to-right",
	"nl-SR": "left-to-right",
	"nl-SX": "left-to-right",
	"nmg": "left-to-right",
	"nn": "left-to-right",
	"nnh": "left-to-right",
	"no": "left-to-right",
	"nqo": "right-to-left",
	"nr": "left-to-right",
	"nso": "left-to-right",
	"nus": "left-to-right",
	"nv": "left-to-right",
	"ny": "left-to-right",
	"nyn": "left-to-right",
	"oc": "left-to-right",
	"oc-ES": "left-to-right",
	"oka": "left-to-right",
	"oka-US": "left-to-right",
	"om": "left-to-right",
	"om-KE": "left-to-right",
	"or": "left-to-right",
	"os": "left-to-right",
	"os-RU": "left-to-right",
	"osa": "left-to-right",
	"pa": "left-to-right",
	"pa-Arab": "right-to-left",
	"pa-Guru": "left-to-right",
	"pap": "left-to-right",
	"pap-AW": "left-to-right",
	"pcm": "left-to-right",
	"pi": "left-to-right",
	"pi-Latn": "left-to-right",
	"pis": "left-to-right",
	"pl": "left-to-right",
	"pms": "left-to-right",
	"prg": "left-to-right",
	"ps": "right-to-left",
	"ps-PK": "right-to-left",
	"pt": "left-to-right",
	"pt-AO": "left-to-right",
	"pt-CH": "left-to-right",
	"pt-CV": "left-to-right",
	"pt-GQ": "left-to-right",
	"pt-GW": "left-to-right",
	"pt-LU": "left-to-right",
	"pt-MO": "left-to-right",
	"pt-MZ": "left-to-right",
	"pt-PT": "left-to-right",
	"pt-ST": "left-to-right",
	"pt-TL": "left-to-right",
	"qu": "left-to-right",
	"qu-BO": "left-to-right",
	"qu-EC": "left-to-right",
	"quc": "left-to-right",
	"raj": "left-to-right",
	"rhg": "right-to-left",
	"rhg-Rohg": "right-to-left",
	"rhg-Rohg-BD": "right-to-left",
	"rif": "left-to-right",
	"rm": "left-to-right",
	"rn": "left-to-right",
	"ro": "left-to-right",
	"ro-MD": "left-to-right",
	"rof": "left-to-right",
	"ru": "left-to-right",
	"ru-BY": "left-to-right",
	"ru-KG": "left-to-right",
	"ru-KZ": "left-to-right",
	"ru-MD": "left-to-right",
	"ru-UA": "left-to-right",
	"rw": "left-to-right",
	"rwk": "left-to-right",
	"sa": "left-to-right",
	"sah": "left-to-right",
	"saq": "left-to-right",
	"sat": "left-to-right",
	"sat-Deva": "left-to-right",
	"sat-Olck": "left-to-right",
	"sbp": "left-to-right",
	"sc": "left-to-right",
	"scn": "left-to-right",
	"sd": "right-to-left",
	"sd-Arab": "right-to-left",
	"sd-Deva": "left-to-right",
	"sdh": "right-to-left",
	"sdh-IQ": "right-to-left",
	"se": "left-to-right",
	"se-FI": "left-to-right",
	"se-SE": "left-to-right",
	"seh": "left-to-right",
	"ses": "left-to-right",
	"sg": "left-to-right",
	"sgs": "left-to-right",
	"shi": "left-to-right",
	"shi-Latn": "left-to-right",
	"shi-Tfng": "left-to-right",
	"shn": "left-to-right",
	"shn-TH": "left-to-right",
	"si": "left-to-right",
	"sid": "left-to-right",
	"sk": "left-to-right",
	"skr": "right-to-left",
	"sl": "left-to-right",
	"sma": "left-to-right",
	"sma-NO": "left-to-right",
	"smj": "left-to-right",
	"smj-NO": "left-to-right",
	"smn": "left-to-right",
	"sms": "left-to-right",
	"sn": "left-to-right",
	"so": "left-to-right",
	"so-DJ": "left-to-right",
	"so-ET": "left-to-right",
	"so-KE": "left-to-right",
	"sq": "left-to-right",
	"sq-MK": "left-to-right",
	"sq-XK": "left-to-right",
	"sr": "left-to-right",
	"sr-Cyrl": "left-to-right",
	"sr-Cyrl-BA": "left-to-right",
	"sr-Cyrl-ME": "left-to-right",
	"sr-Cyrl-XK": "left-to-right",
	"sr-Latn": "left-to-right",
	"sr-Latn-BA": "left-to-right",
	"sr-Latn-ME": "left-to-right",
	"sr-Latn-XK": "left-to-right",
	"ss": "left-to-right",
	"ss-SZ": "left-to-right",
	"ssy": "left-to-right",
	"st": "left-to-right",
	"st-LS": "left-to-right",
	"su": "left-to-right",
	"su-Latn": "left-to-right",
	"suz": "left-to-right",
	"suz-Deva": "left-to-right",
	"suz-Sunu": "left-to-right",
	"sv": "left-to-right",
	"sv-AX": "left-to-right",
	"sv-FI": "left-to-right",
	"sw": "left-to-right",
	"sw-CD": "left-to-right",
	"sw-KE": "left-to-right",
	"sw-UG": "left-to-right",
	"syr": "right-to-left",
	"syr-SY": "right-to-left",
	"szl": "left-to-right",
	"ta": "left-to-right",
	"ta-LK": "left-to-right",
	"ta-MY": "left-to-right",
	"ta-SG": "left-to-right",
	"te": "left-to-right",
	"teo": "left-to-right",
	"teo-KE": "left-to-right",
	"tg": "left-to-right",
	"th": "left-to-right",
	"ti": "left-to-right",
	"ti-ER": "left-to-right",
	"tig": "left-to-right",
	"tk": "left-to-right",
	"tn": "left-to-right",
	"tn-BW": "left-to-right",
	"to": "left-to-right",
	"tok": "left-to-right",
	"tpi": "left-to-right",
	"tr": "left-to-right",
	"tr-CY": "left-to-right",
	"trv": "left-to-right",
	"trw": "right-to-left",
	"ts": "left-to-right",
	"tt": "left-to-right",
	"twq": "left-to-right",
	"tyv": "left-to-right",
	"tzm": "left-to-right",
	"ug": "right-to-left",
	"uk": "left-to-right",
	"und": "left-to-right",
	"ur": "right-to-left",
	"ur-IN": "right-to-left",
	"uz": "left-to-right",
	"uz-Arab": "right-to-left",
	"uz-Cyrl": "left-to-right",
	"uz-Latn": "left-to-right",
	"vai": "left-to-right",
	"vai-Latn": "left-to-right",
	"vai-Vaii": "left-to-right",
	"ve": "left-to-right",
	"vec": "left-to-right",
	"vi": "left-to-right",
	"vmw": "left-to-right",
	"vo": "left-to-right",
	"vun": "left-to-right",
	"wa": "left-to-right",
	"wae": "left-to-right",
	"wal": "left-to-right",
	"wbp": "left-to-right",
	"wo": "left-to-right",
	"xh": "left-to-right",
	"xnr": "left-to-right",
	"xog": "left-to-right",
	"yav": "left-to-right",
	"yi": "right-to-left",
	"yo": "left-to-right",
	"yo-BJ": "left-to-right",
	"yrl": "left-to-right",
	"yrl-CO": "left-to-right",
	"yrl-VE": "left-to-right",
	"yue": "left-to-right",
	"yue-Hans": "left-to-right",
	"yue-Hant": "left-to-right",
	"yue-Hant-CN": "left-to-right",
	"yue-Hant-MO": "left-to-right",
	"za": "left-to-right",
	"zgh": "left-to-right",
	"zh": "left-to-right",
	"zh-Hans": "left-to-right",
	"zh-Hans-HK": "left-to-right",
	"zh-Hans-MO": "left-to-right",
	"zh-Hans-MY": "left-to-right",
	"zh-Hans-SG": "left-to-right",
	"zh-Hant": "left-to-right",
	"zh-Hant-HK": "left-to-right",
	"zh-Hant-MO": "left-to-right",
	"zh-Hant-MY": "left-to-right",
	"zh-Latn": "left-to-right",
	"zu": "left-to-right"
};
//#endregion
//#region packages/intl-locale/get_internal_slots.ts
const internalSlotMap = /* @__PURE__ */ new WeakMap();
function getInternalSlotsIfPresent(x) {
	return internalSlotMap.get(x);
}
function getInternalSlots(x, internalSlotsList) {
	let internalSlots = internalSlotMap.get(x);
	if (!internalSlots) {
		if (internalSlotsList === void 0) throw new TypeError("Intl.Locale method called on incompatible receiver");
		internalSlots = Object.create(null, internalSlotsList.reduce((all, prop) => {
			all[prop] = {
				enumerable: false,
				writable: true,
				configurable: true
			};
			return all;
		}, {}));
		internalSlotMap.set(x, internalSlots);
	}
	return internalSlots;
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.locale@0.0.0/node_modules/@formatjs_generated/cldr.locale/numbering-systems.js
const numberingSystems = {
	"aa": ["latn"],
	"aa-DJ": ["latn"],
	"aa-ER": ["latn"],
	"ab": ["latn"],
	"af": ["latn"],
	"af-NA": ["latn"],
	"agq": ["latn"],
	"ak": ["latn"],
	"am": ["latn", "ethi"],
	"an": ["latn"],
	"ann": ["latn"],
	"apc": ["latn"],
	"ar": ["latn", "arab"],
	"ar-AE": ["latn", "arab"],
	"ar-BH": ["arab"],
	"ar-DJ": ["arab"],
	"ar-DZ": ["latn", "arab"],
	"ar-EG": ["arab"],
	"ar-EH": ["latn", "arab"],
	"ar-ER": ["arab"],
	"ar-IL": ["arab"],
	"ar-IQ": ["arab"],
	"ar-JO": ["arab"],
	"ar-KM": ["arab"],
	"ar-KW": ["arab"],
	"ar-LB": ["arab"],
	"ar-LY": ["latn", "arab"],
	"ar-MA": ["latn", "arab"],
	"ar-MR": ["arab"],
	"ar-OM": ["arab"],
	"ar-PS": ["arab"],
	"ar-QA": ["arab"],
	"ar-SA": ["arab"],
	"ar-SD": ["arab"],
	"ar-SO": ["arab"],
	"ar-SS": ["arab"],
	"ar-SY": ["arab"],
	"ar-TD": ["arab"],
	"ar-TN": ["latn", "arab"],
	"ar-YE": ["arab"],
	"arn": ["latn"],
	"as": ["beng"],
	"asa": ["latn"],
	"ast": ["latn"],
	"az": ["latn"],
	"az-Arab": ["arabext"],
	"az-Arab-IQ": ["arabext"],
	"az-Arab-TR": ["arabext"],
	"az-Cyrl": ["latn"],
	"az-Latn": ["latn"],
	"ba": ["latn"],
	"bal": ["latn"],
	"bal-Arab": ["latn"],
	"bal-Latn": ["latn"],
	"bas": ["latn"],
	"be": ["latn"],
	"be-tarask": ["latn"],
	"bem": ["latn"],
	"bew": ["latn"],
	"bez": ["latn"],
	"bg": ["latn"],
	"bgc": ["deva", "latn"],
	"bgn": ["arabext"],
	"bgn-AE": ["arabext"],
	"bgn-AF": ["arabext"],
	"bgn-IR": ["arabext"],
	"bgn-OM": ["arabext"],
	"bho": ["deva", "latn"],
	"blo": ["latn"],
	"blt": ["latn"],
	"bm": ["latn"],
	"bm-Nkoo": ["latn", "nkoo"],
	"bn": ["beng"],
	"bn-IN": ["beng"],
	"bo": ["latn", "tibt"],
	"bo-IN": ["latn", "tibt"],
	"bqi": ["latn"],
	"br": ["latn"],
	"brx": ["latn", "deva"],
	"bs": ["latn"],
	"bs-Cyrl": ["latn"],
	"bs-Latn": ["latn"],
	"bss": ["latn"],
	"bua": ["latn"],
	"byn": ["latn", "ethi"],
	"ca": ["latn"],
	"ca-AD": ["latn"],
	"ca-ES-valencia": ["latn"],
	"ca-FR": ["latn"],
	"ca-IT": ["latn"],
	"cad": ["latn"],
	"cch": ["latn"],
	"ccp": ["cakm"],
	"ccp-IN": ["cakm"],
	"ce": ["latn"],
	"ceb": ["latn"],
	"cgg": ["latn"],
	"cho": ["latn"],
	"chr": ["latn"],
	"cic": ["latn"],
	"ckb": ["arab"],
	"ckb-IR": ["arab"],
	"co": ["latn"],
	"cop": ["latn"],
	"cs": ["latn"],
	"csw": ["latn"],
	"cu": ["latn", "cyrl"],
	"cv": ["latn"],
	"cy": ["latn"],
	"da": ["latn"],
	"da-GL": ["latn"],
	"dav": ["latn"],
	"de": ["latn"],
	"de-AT": ["latn"],
	"de-BE": ["latn"],
	"de-CH": ["latn"],
	"de-IT": ["latn"],
	"de-LI": ["latn"],
	"de-LU": ["latn"],
	"dje": ["latn"],
	"doi": ["latn", "deva"],
	"dsb": ["latn"],
	"dua": ["latn"],
	"dv": ["latn", "arab"],
	"dyo": ["latn"],
	"dz": ["tibt"],
	"ebu": ["latn"],
	"ee": ["latn"],
	"ee-TG": ["latn"],
	"el": ["latn", "grek"],
	"el-CY": ["latn", "grek"],
	"el-polyton": ["latn", "grek"],
	"en": ["latn"],
	"en-001": ["latn"],
	"en-150": ["latn"],
	"en-AE": ["latn"],
	"en-AG": ["latn"],
	"en-AI": ["latn"],
	"en-AS": ["latn"],
	"en-AT": ["latn"],
	"en-AU": ["latn"],
	"en-BB": ["latn"],
	"en-BE": ["latn"],
	"en-BI": ["latn"],
	"en-BM": ["latn"],
	"en-BS": ["latn"],
	"en-BW": ["latn"],
	"en-BZ": ["latn"],
	"en-CA": ["latn"],
	"en-CC": ["latn"],
	"en-CH": ["latn"],
	"en-CK": ["latn"],
	"en-CM": ["latn"],
	"en-CX": ["latn"],
	"en-CY": ["latn"],
	"en-CZ": ["latn"],
	"en-DE": ["latn"],
	"en-DG": ["latn"],
	"en-DK": ["latn"],
	"en-DM": ["latn"],
	"en-Dsrt": ["latn"],
	"en-EE": ["latn"],
	"en-ER": ["latn"],
	"en-ES": ["latn"],
	"en-FI": ["latn"],
	"en-FJ": ["latn"],
	"en-FK": ["latn"],
	"en-FM": ["latn"],
	"en-FR": ["latn"],
	"en-GB": ["latn"],
	"en-GD": ["latn"],
	"en-GE": ["latn"],
	"en-GG": ["latn"],
	"en-GH": ["latn"],
	"en-GI": ["latn"],
	"en-GM": ["latn"],
	"en-GS": ["latn"],
	"en-GU": ["latn"],
	"en-GY": ["latn"],
	"en-HK": ["latn"],
	"en-HU": ["latn"],
	"en-ID": ["latn"],
	"en-IE": ["latn"],
	"en-IL": ["latn"],
	"en-IM": ["latn"],
	"en-IN": ["latn"],
	"en-IO": ["latn"],
	"en-IT": ["latn"],
	"en-JE": ["latn"],
	"en-JM": ["latn"],
	"en-JP": ["latn"],
	"en-KE": ["latn"],
	"en-KI": ["latn"],
	"en-KN": ["latn"],
	"en-KY": ["latn"],
	"en-LC": ["latn"],
	"en-LR": ["latn"],
	"en-LS": ["latn"],
	"en-LT": ["latn"],
	"en-LV": ["latn"],
	"en-MG": ["latn"],
	"en-MH": ["latn"],
	"en-MO": ["latn"],
	"en-MP": ["latn"],
	"en-MS": ["latn"],
	"en-MT": ["latn"],
	"en-MU": ["latn"],
	"en-MV": ["latn"],
	"en-MW": ["latn"],
	"en-MY": ["latn"],
	"en-NA": ["latn"],
	"en-NF": ["latn"],
	"en-NG": ["latn"],
	"en-NL": ["latn"],
	"en-NO": ["latn"],
	"en-NR": ["latn"],
	"en-NU": ["latn"],
	"en-NZ": ["latn"],
	"en-PG": ["latn"],
	"en-PH": ["latn"],
	"en-PK": ["latn"],
	"en-PL": ["latn"],
	"en-PN": ["latn"],
	"en-PR": ["latn"],
	"en-PT": ["latn"],
	"en-PW": ["latn"],
	"en-RO": ["latn"],
	"en-RW": ["latn"],
	"en-SB": ["latn"],
	"en-SC": ["latn"],
	"en-SD": ["latn"],
	"en-SE": ["latn"],
	"en-SG": ["latn"],
	"en-SH": ["latn"],
	"en-SI": ["latn"],
	"en-SK": ["latn"],
	"en-SL": ["latn"],
	"en-SS": ["latn"],
	"en-SX": ["latn"],
	"en-SZ": ["latn"],
	"en-Shaw": ["latn"],
	"en-TC": ["latn"],
	"en-TK": ["latn"],
	"en-TO": ["latn"],
	"en-TT": ["latn"],
	"en-TV": ["latn"],
	"en-TZ": ["latn"],
	"en-UA": ["latn"],
	"en-UG": ["latn"],
	"en-UM": ["latn"],
	"en-VC": ["latn"],
	"en-VG": ["latn"],
	"en-VI": ["latn"],
	"en-VU": ["latn"],
	"en-WS": ["latn"],
	"en-ZA": ["latn"],
	"en-ZM": ["latn"],
	"en-ZW": ["latn"],
	"eo": ["latn"],
	"es": ["latn"],
	"es-419": ["latn"],
	"es-AR": ["latn"],
	"es-BO": ["latn"],
	"es-BR": ["latn"],
	"es-BZ": ["latn"],
	"es-CL": ["latn"],
	"es-CO": ["latn"],
	"es-CR": ["latn"],
	"es-CU": ["latn"],
	"es-DO": ["latn"],
	"es-EA": ["latn"],
	"es-EC": ["latn"],
	"es-GQ": ["latn"],
	"es-GT": ["latn"],
	"es-HN": ["latn"],
	"es-IC": ["latn"],
	"es-MX": ["latn"],
	"es-NI": ["latn"],
	"es-PA": ["latn"],
	"es-PE": ["latn"],
	"es-PH": ["latn"],
	"es-PR": ["latn"],
	"es-PY": ["latn"],
	"es-SV": ["latn"],
	"es-US": ["latn"],
	"es-UY": ["latn"],
	"es-VE": ["latn"],
	"et": ["latn"],
	"eu": ["latn"],
	"ewo": ["latn"],
	"fa": ["arabext"],
	"fa-AF": ["arabext"],
	"ff": ["latn"],
	"ff-Adlm": ["adlm"],
	"ff-Adlm-BF": ["adlm"],
	"ff-Adlm-CM": ["adlm"],
	"ff-Adlm-GH": ["adlm"],
	"ff-Adlm-GM": ["adlm"],
	"ff-Adlm-GW": ["adlm"],
	"ff-Adlm-LR": ["adlm"],
	"ff-Adlm-MR": ["adlm"],
	"ff-Adlm-NE": ["adlm"],
	"ff-Adlm-NG": ["adlm"],
	"ff-Adlm-SL": ["adlm"],
	"ff-Adlm-SN": ["adlm"],
	"ff-Latn": ["latn"],
	"ff-Latn-BF": ["latn"],
	"ff-Latn-CM": ["latn"],
	"ff-Latn-GH": ["latn"],
	"ff-Latn-GM": ["latn"],
	"ff-Latn-GN": ["latn"],
	"ff-Latn-GW": ["latn"],
	"ff-Latn-LR": ["latn"],
	"ff-Latn-MR": ["latn"],
	"ff-Latn-NE": ["latn"],
	"ff-Latn-NG": ["latn"],
	"ff-Latn-SL": ["latn"],
	"fi": ["latn"],
	"fil": ["latn"],
	"fo": ["latn"],
	"fo-DK": ["latn"],
	"fr": ["latn"],
	"fr-BE": ["latn"],
	"fr-BF": ["latn"],
	"fr-BI": ["latn"],
	"fr-BJ": ["latn"],
	"fr-BL": ["latn"],
	"fr-CA": ["latn"],
	"fr-CD": ["latn"],
	"fr-CF": ["latn"],
	"fr-CG": ["latn"],
	"fr-CH": ["latn"],
	"fr-CI": ["latn"],
	"fr-CM": ["latn"],
	"fr-DJ": ["latn"],
	"fr-DZ": ["latn"],
	"fr-GA": ["latn"],
	"fr-GF": ["latn"],
	"fr-GN": ["latn"],
	"fr-GP": ["latn"],
	"fr-GQ": ["latn"],
	"fr-HT": ["latn"],
	"fr-KM": ["latn"],
	"fr-LU": ["latn"],
	"fr-MA": ["latn"],
	"fr-MC": ["latn"],
	"fr-MF": ["latn"],
	"fr-MG": ["latn"],
	"fr-ML": ["latn"],
	"fr-MQ": ["latn"],
	"fr-MR": ["latn"],
	"fr-MU": ["latn"],
	"fr-NC": ["latn"],
	"fr-NE": ["latn"],
	"fr-PF": ["latn"],
	"fr-PM": ["latn"],
	"fr-RE": ["latn"],
	"fr-RW": ["latn"],
	"fr-SC": ["latn"],
	"fr-SN": ["latn"],
	"fr-SY": ["latn"],
	"fr-TD": ["latn"],
	"fr-TG": ["latn"],
	"fr-TN": ["latn"],
	"fr-VU": ["latn"],
	"fr-WF": ["latn"],
	"fr-YT": ["latn"],
	"frr": ["latn"],
	"fur": ["latn"],
	"fy": ["latn"],
	"ga": ["latn"],
	"ga-GB": ["latn"],
	"gaa": ["latn"],
	"gd": ["latn"],
	"gez": ["latn"],
	"gez-ER": ["latn"],
	"gl": ["latn"],
	"gn": ["latn"],
	"gsw": ["latn"],
	"gsw-FR": ["latn"],
	"gsw-LI": ["latn"],
	"gu": ["latn", "gujr"],
	"guz": ["latn"],
	"gv": ["latn"],
	"ha": ["latn"],
	"ha-Arab": ["latn", "arab"],
	"ha-Arab-SD": ["latn", "arab"],
	"ha-GH": ["latn"],
	"ha-NE": ["latn"],
	"haw": ["latn"],
	"he": ["latn", "hebr"],
	"hi": ["latn", "deva"],
	"hi-Latn": ["latn"],
	"hnj": ["hmnp", "latn"],
	"hnj-Hmnp": ["hmnp", "latn"],
	"hr": ["latn"],
	"hr-BA": ["latn"],
	"hsb": ["latn"],
	"ht": ["latn"],
	"hu": ["latn"],
	"hy": ["latn", "armn"],
	"ia": ["latn"],
	"id": ["latn"],
	"ie": ["latn"],
	"ig": ["latn"],
	"ii": ["latn"],
	"io": ["latn"],
	"is": ["latn"],
	"it": ["latn"],
	"it-CH": ["latn"],
	"it-SM": ["latn"],
	"it-VA": ["latn"],
	"iu": ["latn"],
	"iu-Latn": ["latn"],
	"ja": [
		"latn",
		"jpan",
		"jpanfin"
	],
	"jbo": ["latn"],
	"jgo": ["latn"],
	"jmc": ["latn"],
	"jv": ["latn", "java"],
	"ka": ["latn", "geor"],
	"kaa": ["latn"],
	"kaa-Cyrl": ["latn"],
	"kaa-Latn": ["latn"],
	"kab": ["latn"],
	"kaj": ["latn"],
	"kam": ["latn"],
	"kcg": ["latn"],
	"kde": ["latn"],
	"kea": ["latn"],
	"kek": ["latn"],
	"ken": ["latn"],
	"kgp": ["latn"],
	"khq": ["latn"],
	"ki": ["latn"],
	"kk": ["latn"],
	"kk-Arab": ["latn"],
	"kk-Cyrl": ["latn"],
	"kk-KZ": ["latn"],
	"kkj": ["latn"],
	"kl": ["latn"],
	"kln": ["latn"],
	"km": ["latn", "khmr"],
	"kn": ["latn", "knda"],
	"ko": ["latn"],
	"ko-CN": ["latn"],
	"ko-KP": ["latn"],
	"kok": ["latn", "deva"],
	"kok-Deva": ["latn", "deva"],
	"kok-Latn": ["latn"],
	"kpe": ["latn"],
	"kpe-GN": ["latn"],
	"ks": ["arabext"],
	"ks-Arab": ["arabext"],
	"ks-Deva": ["latn"],
	"ksb": ["latn"],
	"ksf": ["latn"],
	"ksh": ["latn"],
	"ku": ["latn"],
	"ku-Arab": ["latn"],
	"ku-Arab-IR": ["latn"],
	"ku-Latn": ["latn"],
	"ku-Latn-IQ": ["latn"],
	"ku-Latn-SY": ["latn"],
	"ku-TR": ["latn"],
	"kw": ["latn"],
	"kxv": ["latn"],
	"kxv-Deva": ["latn", "deva"],
	"kxv-Latn": ["latn"],
	"kxv-Orya": ["latn", "orya"],
	"kxv-Telu": ["latn", "telu"],
	"ky": ["latn"],
	"la": ["latn"],
	"lag": ["latn"],
	"lb": ["latn"],
	"lg": ["latn"],
	"lij": ["latn"],
	"lkt": ["latn"],
	"lld": ["latn"],
	"lmo": ["latn"],
	"ln": ["latn"],
	"ln-AO": ["latn"],
	"ln-CF": ["latn"],
	"ln-CG": ["latn"],
	"lo": ["latn", "laoo"],
	"lrc": ["arabext"],
	"lrc-IQ": ["arabext"],
	"lt": ["latn"],
	"ltg": ["latn"],
	"lu": ["latn"],
	"luo": ["latn"],
	"luy": ["latn"],
	"lv": ["latn"],
	"lzz": ["latn"],
	"mai": ["latn", "deva"],
	"mas": ["latn"],
	"mas-TZ": ["latn"],
	"mdf": ["latn"],
	"mer": ["latn"],
	"mfe": ["latn"],
	"mg": ["latn"],
	"mgh": ["latn"],
	"mgo": ["latn"],
	"mhn": ["latn"],
	"mi": ["latn"],
	"mic": ["latn"],
	"mk": ["latn"],
	"ml": ["latn", "mlym"],
	"mn": ["latn"],
	"mn-Mong": ["latn", "mong"],
	"mn-Mong-MN": ["latn", "mong"],
	"mni": ["beng", "latn"],
	"mni-Beng": ["beng", "latn"],
	"mni-Mtei": ["mtei", "latn"],
	"moh": ["latn"],
	"mr": ["deva"],
	"ms": ["latn"],
	"ms-Arab": ["latn"],
	"ms-Arab-BN": ["latn"],
	"ms-BN": ["latn"],
	"ms-ID": ["latn"],
	"ms-SG": ["latn"],
	"mt": ["latn"],
	"mua": ["latn"],
	"mus": ["latn"],
	"mww": ["hmnp", "latn"],
	"mww-Hmnp": ["hmnp", "latn"],
	"my": ["mymr"],
	"myv": ["latn"],
	"mzn": ["arabext"],
	"naq": ["latn"],
	"nb": ["latn"],
	"nb-SJ": ["latn"],
	"nd": ["latn"],
	"nds": ["latn"],
	"nds-NL": ["latn"],
	"ne": ["deva"],
	"ne-IN": ["deva"],
	"nl": ["latn"],
	"nl-AW": ["latn"],
	"nl-BE": ["latn"],
	"nl-BQ": ["latn"],
	"nl-CW": ["latn"],
	"nl-SR": ["latn"],
	"nl-SX": ["latn"],
	"nmg": ["latn"],
	"nn": ["latn"],
	"nnh": ["latn"],
	"no": ["latn"],
	"nqo": ["nkoo"],
	"nr": ["latn"],
	"nso": ["latn"],
	"nus": ["latn"],
	"nv": ["latn"],
	"ny": ["latn"],
	"nyn": ["latn"],
	"oc": ["latn"],
	"oc-ES": ["latn"],
	"oka": ["latn"],
	"oka-US": ["latn"],
	"om": ["latn", "ethi"],
	"om-KE": ["latn", "ethi"],
	"or": ["latn", "orya"],
	"os": ["latn"],
	"os-RU": ["latn"],
	"osa": ["latn"],
	"pa": ["latn", "guru"],
	"pa-Arab": ["arabext"],
	"pa-Guru": ["latn", "guru"],
	"pap": ["latn"],
	"pap-AW": ["latn"],
	"pcm": ["latn"],
	"pi": ["latn"],
	"pi-Latn": ["latn"],
	"pis": ["latn"],
	"pl": ["latn"],
	"pms": ["latn"],
	"prg": ["latn"],
	"ps": ["arabext"],
	"ps-PK": ["arabext"],
	"pt": ["latn"],
	"pt-AO": ["latn"],
	"pt-CH": ["latn"],
	"pt-CV": ["latn"],
	"pt-GQ": ["latn"],
	"pt-GW": ["latn"],
	"pt-LU": ["latn"],
	"pt-MO": ["latn"],
	"pt-MZ": ["latn"],
	"pt-PT": ["latn"],
	"pt-ST": ["latn"],
	"pt-TL": ["latn"],
	"qu": ["latn"],
	"qu-BO": ["latn"],
	"qu-EC": ["latn"],
	"quc": ["latn"],
	"raj": ["deva", "latn"],
	"rhg": ["latn"],
	"rhg-Rohg": ["latn"],
	"rhg-Rohg-BD": ["latn"],
	"rif": ["latn"],
	"rm": ["latn"],
	"rn": ["latn"],
	"ro": ["latn"],
	"ro-MD": ["latn"],
	"rof": ["latn"],
	"ru": ["latn"],
	"ru-BY": ["latn"],
	"ru-KG": ["latn"],
	"ru-KZ": ["latn"],
	"ru-MD": ["latn"],
	"ru-UA": ["latn"],
	"rw": ["latn"],
	"rwk": ["latn"],
	"sa": ["deva"],
	"sah": ["latn"],
	"saq": ["latn"],
	"sat": ["olck", "latn"],
	"sat-Deva": ["deva", "latn"],
	"sat-Olck": ["olck", "latn"],
	"sbp": ["latn"],
	"sc": ["latn"],
	"scn": ["latn"],
	"sd": ["arab"],
	"sd-Arab": ["arab"],
	"sd-Deva": ["latn"],
	"sdh": ["arab"],
	"sdh-IQ": ["arab"],
	"se": ["latn"],
	"se-FI": ["latn"],
	"se-SE": ["latn"],
	"seh": ["latn"],
	"ses": ["latn"],
	"sg": ["latn"],
	"sgs": ["latn"],
	"shi": ["latn"],
	"shi-Latn": ["latn"],
	"shi-Tfng": ["latn"],
	"shn": ["latn"],
	"shn-TH": ["latn"],
	"si": ["latn"],
	"sid": ["latn"],
	"sk": ["latn"],
	"skr": ["latn"],
	"sl": ["latn"],
	"sma": ["latn"],
	"sma-NO": ["latn"],
	"smj": ["latn"],
	"smj-NO": ["latn"],
	"smn": ["latn"],
	"sms": ["latn"],
	"sn": ["latn"],
	"so": ["latn"],
	"so-DJ": ["latn"],
	"so-ET": ["latn"],
	"so-KE": ["latn"],
	"sq": ["latn"],
	"sq-MK": ["latn"],
	"sq-XK": ["latn"],
	"sr": ["latn"],
	"sr-Cyrl": ["latn"],
	"sr-Cyrl-BA": ["latn"],
	"sr-Cyrl-ME": ["latn"],
	"sr-Cyrl-XK": ["latn"],
	"sr-Latn": ["latn"],
	"sr-Latn-BA": ["latn"],
	"sr-Latn-ME": ["latn"],
	"sr-Latn-XK": ["latn"],
	"ss": ["latn"],
	"ss-SZ": ["latn"],
	"ssy": ["latn"],
	"st": ["latn"],
	"st-LS": ["latn"],
	"su": ["latn"],
	"su-Latn": ["latn"],
	"suz": ["latn"],
	"suz-Deva": ["latn"],
	"suz-Sunu": ["latn"],
	"sv": ["latn"],
	"sv-AX": ["latn"],
	"sv-FI": ["latn"],
	"sw": ["latn"],
	"sw-CD": ["latn"],
	"sw-KE": ["latn"],
	"sw-UG": ["latn"],
	"syr": ["latn"],
	"syr-SY": ["latn"],
	"szl": ["latn"],
	"ta": [
		"latn",
		"tamldec",
		"taml"
	],
	"ta-LK": [
		"latn",
		"tamldec",
		"taml"
	],
	"ta-MY": [
		"latn",
		"tamldec",
		"taml"
	],
	"ta-SG": [
		"latn",
		"tamldec",
		"taml"
	],
	"te": ["latn", "telu"],
	"teo": ["latn"],
	"teo-KE": ["latn"],
	"tg": ["latn"],
	"th": ["latn", "thai"],
	"ti": ["latn", "ethi"],
	"ti-ER": ["latn", "ethi"],
	"tig": ["latn", "ethi"],
	"tk": ["latn"],
	"tn": ["latn"],
	"tn-BW": ["latn"],
	"to": ["latn"],
	"tok": ["latn"],
	"tpi": ["latn"],
	"tr": ["latn"],
	"tr-CY": ["latn"],
	"trv": ["latn"],
	"trw": ["latn"],
	"ts": ["latn"],
	"tt": ["latn"],
	"twq": ["latn"],
	"tyv": ["latn"],
	"tzm": ["latn"],
	"ug": ["latn", "arabext"],
	"uk": ["latn"],
	"und": ["latn"],
	"ur": ["latn", "arabext"],
	"ur-IN": ["arabext"],
	"uz": ["latn"],
	"uz-Arab": ["arabext"],
	"uz-Cyrl": ["latn"],
	"uz-Latn": ["latn"],
	"vai": ["latn", "vaii"],
	"vai-Latn": ["latn", "vaii"],
	"vai-Vaii": ["latn", "vaii"],
	"ve": ["latn"],
	"vec": ["latn"],
	"vi": ["latn"],
	"vmw": ["latn"],
	"vo": ["latn"],
	"vun": ["latn"],
	"wa": ["latn"],
	"wae": ["latn"],
	"wal": ["latn", "ethi"],
	"wbp": ["latn"],
	"wo": ["latn"],
	"xh": ["latn"],
	"xnr": ["latn", "deva"],
	"xog": ["latn"],
	"yav": ["latn"],
	"yi": ["latn", "hebr"],
	"yo": ["latn"],
	"yo-BJ": ["latn"],
	"yrl": ["latn"],
	"yrl-CO": ["latn"],
	"yrl-VE": ["latn"],
	"yue": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"yue-Hans": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"yue-Hant": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"yue-Hant-CN": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"yue-Hant-MO": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"za": ["latn"],
	"zgh": ["latn"],
	"zh": [
		"latn",
		"hanidec",
		"hans",
		"hansfin"
	],
	"zh-Hans": [
		"latn",
		"hanidec",
		"hans",
		"hansfin"
	],
	"zh-Hans-HK": [
		"latn",
		"hanidec",
		"hans",
		"hansfin"
	],
	"zh-Hans-MO": [
		"latn",
		"hanidec",
		"hans",
		"hansfin"
	],
	"zh-Hans-MY": [
		"latn",
		"hanidec",
		"hans",
		"hansfin"
	],
	"zh-Hans-SG": [
		"latn",
		"hanidec",
		"hans",
		"hansfin"
	],
	"zh-Hant": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"zh-Hant-HK": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"zh-Hant-MO": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"zh-Hant-MY": [
		"latn",
		"hanidec",
		"hant",
		"hantfin"
	],
	"zh-Latn": ["latn"],
	"zu": ["latn"]
};
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.locale@0.0.0/node_modules/@formatjs_generated/cldr.locale/timezones.js
const timezones = {
	"ad": ["Europe/Andorra"],
	"ae": ["Asia/Dubai"],
	"af": ["Asia/Kabul"],
	"ag": ["America/Antigua"],
	"ai": ["America/Anguilla"],
	"al": ["Europe/Tirane"],
	"am": ["Asia/Yerevan"],
	"an": ["America/Curacao"],
	"ao": ["Africa/Luanda"],
	"aq": [
		"Antarctica/McMurdo",
		"Antarctica/Casey",
		"Antarctica/Davis",
		"Antarctica/DumontDUrville",
		"Antarctica/Mawson",
		"Antarctica/McMurdo",
		"Antarctica/Palmer",
		"Antarctica/Rothera",
		"Antarctica/Syowa",
		"Antarctica/Troll",
		"Antarctica/Vostok"
	],
	"ar": [
		"America/Buenos_Aires",
		"America/Cordoba",
		"America/Catamarca",
		"America/Argentina/La_Rioja",
		"America/Jujuy",
		"America/Argentina/San_Luis",
		"America/Mendoza",
		"America/Argentina/Rio_Gallegos",
		"America/Argentina/Salta",
		"America/Argentina/Tucuman",
		"America/Argentina/San_Juan",
		"America/Argentina/Ushuaia"
	],
	"as": ["Pacific/Pago_Pago"],
	"at": ["Europe/Vienna"],
	"au": [
		"Australia/Adelaide",
		"Australia/Broken_Hill",
		"Australia/Brisbane",
		"Australia/Darwin",
		"Australia/Eucla",
		"Australia/Hobart",
		"Australia/Hobart",
		"Australia/Lindeman",
		"Australia/Lord_Howe",
		"Australia/Melbourne",
		"Antarctica/Macquarie",
		"Australia/Perth",
		"Australia/Sydney"
	],
	"aw": ["America/Aruba"],
	"az": ["Asia/Baku"],
	"ba": ["Europe/Sarajevo"],
	"bb": ["America/Barbados"],
	"bd": ["Asia/Dhaka"],
	"be": ["Europe/Brussels"],
	"bf": ["Africa/Ouagadougou"],
	"bg": ["Europe/Sofia"],
	"bh": ["Asia/Bahrain"],
	"bi": ["Africa/Bujumbura"],
	"bj": ["Africa/Porto-Novo"],
	"bm": ["Atlantic/Bermuda"],
	"bn": ["Asia/Brunei"],
	"bo": ["America/La_Paz"],
	"bq": ["America/Kralendijk"],
	"br": [
		"America/Araguaina",
		"America/Belem",
		"America/Boa_Vista",
		"America/Cuiaba",
		"America/Campo_Grande",
		"America/Eirunepe",
		"America/Noronha",
		"America/Fortaleza",
		"America/Manaus",
		"America/Maceio",
		"America/Porto_Velho",
		"America/Rio_Branco",
		"America/Recife",
		"America/Sao_Paulo",
		"America/Bahia",
		"America/Santarem"
	],
	"bs": ["America/Nassau"],
	"bt": ["Asia/Thimphu"],
	"bw": ["Africa/Gaborone"],
	"by": ["Europe/Minsk"],
	"bz": ["America/Belize"],
	"ca": [
		"America/Creston",
		"America/Edmonton",
		"America/Winnipeg",
		"America/Fort_Nelson",
		"America/Glace_Bay",
		"America/Goose_Bay",
		"America/Halifax",
		"America/Iqaluit",
		"America/Moncton",
		"America/Toronto",
		"America/Toronto",
		"America/Iqaluit",
		"America/Resolute",
		"America/Regina",
		"America/St_Johns",
		"America/Toronto",
		"America/Toronto",
		"America/Vancouver",
		"America/Winnipeg",
		"America/Blanc-Sablon",
		"America/Cambridge_Bay",
		"America/Dawson",
		"America/Dawson_Creek",
		"America/Rankin_Inlet",
		"America/Inuvik",
		"America/Whitehorse",
		"America/Swift_Current",
		"America/Edmonton",
		"America/Coral_Harbour"
	],
	"cc": ["Indian/Cocos"],
	"cd": ["Africa/Lubumbashi", "Africa/Kinshasa"],
	"cf": ["Africa/Bangui"],
	"cg": ["Africa/Brazzaville"],
	"ch": ["Europe/Zurich"],
	"ci": ["Africa/Abidjan"],
	"ck": ["Pacific/Rarotonga"],
	"cl": [
		"America/Coyhaique",
		"Pacific/Easter",
		"America/Punta_Arenas",
		"America/Santiago"
	],
	"cm": ["Africa/Douala"],
	"cn": [
		"Asia/Shanghai",
		"Asia/Shanghai",
		"Asia/Urumqi",
		"Asia/Shanghai",
		"Asia/Urumqi"
	],
	"co": ["America/Bogota"],
	"cr": ["America/Costa_Rica"],
	"cs": ["America/Chicago"],
	"cu": ["America/Havana"],
	"cv": ["Atlantic/Cape_Verde"],
	"cx": ["Indian/Christmas"],
	"cy": ["Asia/Famagusta", "Asia/Nicosia"],
	"cz": ["Europe/Prague"],
	"de": ["Europe/Berlin", "Europe/Busingen"],
	"dj": ["Africa/Djibouti"],
	"dk": ["Europe/Copenhagen"],
	"dm": ["America/Dominica"],
	"do": ["America/Santo_Domingo"],
	"dz": ["Africa/Algiers"],
	"ec": ["Pacific/Galapagos", "America/Guayaquil"],
	"ee": ["Europe/Tallinn"],
	"eg": ["Africa/Cairo"],
	"eh": ["Africa/El_Aaiun"],
	"er": ["Africa/Asmera"],
	"es": [
		"Africa/Ceuta",
		"Atlantic/Canary",
		"Europe/Madrid",
		"America/New_York"
	],
	"et": ["Africa/Addis_Ababa"],
	"fi": ["Europe/Helsinki", "Europe/Mariehamn"],
	"fj": ["Pacific/Fiji"],
	"fk": ["Atlantic/Stanley"],
	"fm": [
		"Pacific/Kosrae",
		"Pacific/Ponape",
		"Pacific/Truk"
	],
	"fo": ["Atlantic/Faeroe"],
	"fr": ["Europe/Paris"],
	"ga": [
		"Africa/Libreville",
		"Asia/Gaza",
		"Asia/Gaza"
	],
	"gb": ["Europe/London"],
	"gd": ["America/Grenada"],
	"ge": ["Asia/Tbilisi"],
	"gf": ["America/Cayenne"],
	"gg": ["Europe/Guernsey"],
	"gh": ["Africa/Accra"],
	"gi": ["Europe/Gibraltar"],
	"gl": [
		"America/Danmarkshavn",
		"America/Godthab",
		"America/Scoresbysund",
		"America/Thule"
	],
	"gm": ["Africa/Banjul", "Etc/GMT"],
	"gn": ["Africa/Conakry"],
	"gp": [
		"America/Guadeloupe",
		"America/Marigot",
		"America/St_Barthelemy"
	],
	"gq": ["Africa/Malabo"],
	"gr": ["Europe/Athens"],
	"gs": ["Atlantic/South_Georgia"],
	"gt": ["America/Guatemala"],
	"gu": ["Pacific/Guam"],
	"gw": ["Africa/Bissau"],
	"gy": ["America/Guyana"],
	"he": ["Asia/Hebron"],
	"hk": ["Asia/Hong_Kong"],
	"hn": ["America/Tegucigalpa"],
	"hr": ["Europe/Zagreb"],
	"ht": ["America/Port-au-Prince"],
	"hu": ["Europe/Budapest"],
	"id": [
		"Asia/Jayapura",
		"Asia/Jakarta",
		"Asia/Makassar",
		"Asia/Pontianak"
	],
	"ie": ["Europe/Dublin"],
	"im": ["Europe/Isle_of_Man"],
	"in": ["Asia/Calcutta"],
	"io": ["Indian/Chagos"],
	"iq": ["Asia/Baghdad"],
	"ir": ["Asia/Tehran"],
	"is": ["Atlantic/Reykjavik"],
	"it": ["Europe/Rome"],
	"je": ["Asia/Jerusalem", "Europe/Jersey"],
	"jm": ["America/Jamaica"],
	"jo": ["Asia/Amman"],
	"jp": ["Asia/Tokyo"],
	"ke": ["Africa/Nairobi"],
	"kg": ["Asia/Bishkek"],
	"kh": ["Asia/Phnom_Penh"],
	"ki": [
		"Pacific/Kiritimati",
		"Pacific/Enderbury",
		"Pacific/Tarawa"
	],
	"km": ["Indian/Comoro"],
	"kn": ["America/St_Kitts"],
	"kp": ["Asia/Pyongyang"],
	"kr": ["Asia/Seoul"],
	"kw": ["Asia/Kuwait"],
	"ky": ["America/Cayman"],
	"kz": [
		"Asia/Aqtau",
		"Asia/Aqtobe",
		"Asia/Almaty",
		"Asia/Atyrau",
		"Asia/Qostanay",
		"Asia/Qyzylorda",
		"Asia/Oral"
	],
	"la": ["Asia/Vientiane"],
	"lb": ["Asia/Beirut"],
	"lc": ["America/St_Lucia"],
	"li": ["Europe/Vaduz"],
	"lk": ["Asia/Colombo"],
	"lr": ["Africa/Monrovia"],
	"ls": ["Africa/Maseru"],
	"lt": ["Europe/Vilnius"],
	"lu": ["Europe/Luxembourg"],
	"lv": ["Europe/Riga"],
	"ly": ["Africa/Tripoli"],
	"ma": ["Africa/Casablanca"],
	"mc": ["Europe/Monaco"],
	"md": ["Europe/Chisinau"],
	"me": ["Europe/Podgorica"],
	"mg": ["Indian/Antananarivo"],
	"mh": ["Pacific/Kwajalein", "Pacific/Majuro"],
	"mk": ["Europe/Skopje"],
	"ml": ["Africa/Bamako"],
	"mm": ["Asia/Rangoon"],
	"mn": [
		"Asia/Ulaanbaatar",
		"Asia/Hovd",
		"Asia/Ulaanbaatar"
	],
	"mo": ["Asia/Macau"],
	"mp": ["Pacific/Saipan"],
	"mq": ["America/Martinique"],
	"mr": ["Africa/Nouakchott"],
	"ms": ["America/Montserrat", "America/Denver"],
	"mt": ["Europe/Malta"],
	"mu": ["Indian/Mauritius"],
	"mv": ["Indian/Maldives"],
	"mw": ["Africa/Blantyre"],
	"mx": [
		"America/Chihuahua",
		"America/Ciudad_Juarez",
		"America/Cancun",
		"America/Hermosillo",
		"America/Matamoros",
		"America/Mexico_City",
		"America/Merida",
		"America/Monterrey",
		"America/Mazatlan",
		"America/Ojinaga",
		"America/Bahia_Banderas",
		"America/Tijuana",
		"America/Tijuana"
	],
	"my": ["Asia/Kuching", "Asia/Kuala_Lumpur"],
	"mz": ["Africa/Maputo"],
	"na": ["Africa/Windhoek"],
	"nc": ["Pacific/Noumea"],
	"ne": ["Africa/Niamey"],
	"nf": ["Pacific/Norfolk"],
	"ng": ["Africa/Lagos"],
	"ni": ["America/Managua"],
	"nl": ["Europe/Amsterdam"],
	"no": ["Europe/Oslo"],
	"np": ["Asia/Katmandu"],
	"nr": ["Pacific/Nauru"],
	"nu": ["Pacific/Niue"],
	"nz": ["Pacific/Auckland", "Pacific/Chatham"],
	"om": ["Asia/Muscat"],
	"pa": ["America/Panama"],
	"pe": ["America/Lima"],
	"pf": [
		"Pacific/Gambier",
		"Pacific/Marquesas",
		"Pacific/Tahiti"
	],
	"pg": ["Pacific/Port_Moresby", "Pacific/Bougainville"],
	"ph": ["Asia/Manila"],
	"pk": ["Asia/Karachi"],
	"pl": ["Europe/Warsaw"],
	"pm": ["America/Miquelon"],
	"pn": ["Pacific/Pitcairn"],
	"pr": ["America/Puerto_Rico"],
	"ps": ["America/Los_Angeles"],
	"pt": [
		"Atlantic/Madeira",
		"Europe/Lisbon",
		"Atlantic/Azores"
	],
	"pw": ["Pacific/Palau"],
	"py": ["America/Asuncion"],
	"qa": ["Asia/Qatar"],
	"re": ["Indian/Reunion"],
	"ro": ["Europe/Bucharest"],
	"rs": ["Europe/Belgrade"],
	"ru": [
		"Europe/Astrakhan",
		"Asia/Barnaul",
		"Asia/Chita",
		"Asia/Anadyr",
		"Asia/Magadan",
		"Asia/Irkutsk",
		"Europe/Kaliningrad",
		"Asia/Khandyga",
		"Asia/Krasnoyarsk",
		"Europe/Samara",
		"Europe/Kirov",
		"Europe/Moscow",
		"Asia/Novokuznetsk",
		"Asia/Omsk",
		"Asia/Novosibirsk",
		"Asia/Kamchatka",
		"Europe/Saratov",
		"Asia/Srednekolymsk",
		"Asia/Tomsk",
		"Europe/Ulyanovsk",
		"Asia/Ust-Nera",
		"Asia/Sakhalin",
		"Europe/Volgograd",
		"Asia/Vladivostok",
		"Asia/Yekaterinburg",
		"Asia/Yakutsk"
	],
	"rw": ["Africa/Kigali"],
	"sa": ["Asia/Riyadh"],
	"sb": ["Pacific/Guadalcanal"],
	"sc": ["Indian/Mahe"],
	"sd": ["Africa/Khartoum"],
	"se": ["Europe/Stockholm"],
	"sg": ["Asia/Singapore"],
	"sh": ["Atlantic/St_Helena"],
	"si": ["Europe/Ljubljana"],
	"sj": ["Arctic/Longyearbyen"],
	"sk": ["Europe/Bratislava"],
	"sl": ["Africa/Freetown"],
	"sm": ["Europe/San_Marino"],
	"sn": ["Africa/Dakar"],
	"so": ["Africa/Mogadishu"],
	"sr": ["America/Paramaribo"],
	"ss": ["Africa/Juba"],
	"st": ["Africa/Sao_Tome"],
	"sv": ["America/El_Salvador"],
	"sx": ["America/Lower_Princes"],
	"sy": ["Asia/Damascus"],
	"sz": ["Africa/Mbabane"],
	"tc": ["America/Grand_Turk"],
	"td": ["Africa/Ndjamena"],
	"tf": ["Indian/Kerguelen"],
	"tg": ["Africa/Lome"],
	"th": ["Asia/Bangkok"],
	"tj": ["Asia/Dushanbe"],
	"tk": ["Pacific/Fakaofo"],
	"tl": ["Asia/Dili"],
	"tm": ["Asia/Ashgabat"],
	"tn": ["Africa/Tunis"],
	"to": ["Pacific/Tongatapu"],
	"tr": ["Europe/Istanbul"],
	"tt": ["America/Port_of_Spain"],
	"tv": ["Pacific/Funafuti"],
	"tw": ["Asia/Taipei"],
	"tz": ["Africa/Dar_es_Salaam"],
	"ua": [
		"Europe/Kiev",
		"Europe/Kiev",
		"Europe/Simferopol",
		"Europe/Kiev"
	],
	"ug": ["Africa/Kampala"],
	"um": [
		"Pacific/Wake",
		"Pacific/Honolulu",
		"Pacific/Midway"
	],
	"un": ["Etc/Unknown"],
	"us": [
		"America/Adak",
		"America/Indiana/Marengo",
		"America/Anchorage",
		"America/Boise",
		"America/Chicago",
		"America/Denver",
		"America/Detroit",
		"Pacific/Honolulu",
		"America/Indianapolis",
		"America/Indiana/Vevay",
		"America/Juneau",
		"America/Indiana/Knox",
		"America/Los_Angeles",
		"America/Louisville",
		"America/Menominee",
		"America/Kentucky/Monticello",
		"America/Metlakatla",
		"America/Denver",
		"America/North_Dakota/Center",
		"America/North_Dakota/New_Salem",
		"America/New_York",
		"America/Indiana/Vincennes",
		"America/Nome",
		"America/Phoenix",
		"America/Sitka",
		"America/Indiana/Tell_City",
		"America/Indiana/Winamac",
		"America/Indiana/Petersburg",
		"America/North_Dakota/Beulah",
		"America/Yakutat"
	],
	"ut": [
		"Etc/UTC",
		"Etc/GMT-1",
		"Etc/GMT-2",
		"Etc/GMT-3",
		"Etc/GMT-4",
		"Etc/GMT-5",
		"Etc/GMT-6",
		"Etc/GMT-7",
		"Etc/GMT-8",
		"Etc/GMT-9",
		"Etc/GMT-10",
		"Etc/GMT-11",
		"Etc/GMT-12",
		"Etc/GMT-13",
		"Etc/GMT-14",
		"Etc/GMT+1",
		"Etc/GMT+2",
		"Etc/GMT+3",
		"Etc/GMT+4",
		"Etc/GMT+5",
		"Etc/GMT+6",
		"Etc/GMT+7",
		"Etc/GMT+8",
		"Etc/GMT+9",
		"Etc/GMT+10",
		"Etc/GMT+11",
		"Etc/GMT+12"
	],
	"uy": ["America/Montevideo"],
	"uz": ["Asia/Samarkand", "Asia/Tashkent"],
	"va": ["Europe/Vatican"],
	"vc": ["America/St_Vincent"],
	"ve": ["America/Caracas"],
	"vg": ["America/Tortola"],
	"vi": ["America/St_Thomas"],
	"vn": ["Asia/Saigon"],
	"vu": ["Pacific/Efate"],
	"wf": ["Pacific/Wallis"],
	"ws": ["Pacific/Apia"],
	"ye": ["Asia/Aden"],
	"yt": ["Indian/Mayotte"],
	"za": ["Africa/Johannesburg"],
	"zm": ["Africa/Lusaka"],
	"zw": ["Africa/Harare"]
};
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.locale@0.0.0/node_modules/@formatjs_generated/cldr.locale/hour-cycles.js
const hourCycles = {
	"001": ["h23", "h12"],
	"419": ["h12", "h23"],
	"AC": ["h23", "h12"],
	"AD": ["h23"],
	"AE": ["h12", "h23"],
	"AF": ["h23", "h12"],
	"AG": ["h12", "h23"],
	"AI": ["h23", "h12"],
	"AL": ["h12", "h23"],
	"AM": ["h23"],
	"AO": ["h23"],
	"AR": ["h12", "h23"],
	"AS": ["h12", "h23"],
	"AT": ["h23"],
	"AU": ["h12", "h23"],
	"AW": ["h23"],
	"AX": ["h23"],
	"AZ": ["h23", "h12"],
	"BA": ["h23", "h12"],
	"BB": ["h12", "h23"],
	"BD": ["h12", "h23"],
	"BE": ["h23"],
	"BF": ["h23"],
	"BG": ["h23", "h12"],
	"BH": ["h12", "h23"],
	"BI": ["h23", "h12"],
	"BJ": ["h23"],
	"BL": ["h23"],
	"BM": ["h12", "h23"],
	"BN": ["h12", "h23"],
	"BO": ["h12", "h23"],
	"BQ": ["h23"],
	"BR": ["h23"],
	"BS": ["h12", "h23"],
	"BT": ["h12", "h23"],
	"BW": ["h23", "h12"],
	"BY": ["h23", "h12"],
	"BZ": ["h23", "h12"],
	"CA": ["h12", "h23"],
	"CC": ["h23", "h12"],
	"CD": ["h23"],
	"CF": ["h23", "h12"],
	"CG": ["h23"],
	"CH": ["h23", "h12"],
	"CI": ["h23"],
	"CK": ["h23", "h12"],
	"CL": ["h12", "h23"],
	"CM": ["h23", "h12"],
	"CN": ["h23", "h12"],
	"CO": ["h12", "h23"],
	"CP": ["h23"],
	"CR": ["h12", "h23"],
	"CU": ["h12", "h23"],
	"CV": ["h23"],
	"CW": ["h23"],
	"CX": ["h23", "h12"],
	"CY": ["h12", "h23"],
	"CZ": ["h23"],
	"DE": ["h23"],
	"DG": ["h23", "h12"],
	"DJ": ["h12", "h23"],
	"DK": ["h23"],
	"DM": ["h12", "h23"],
	"DO": ["h12", "h23"],
	"DZ": ["h12", "h23"],
	"EA": ["h23", "h12"],
	"EC": ["h12", "h23"],
	"EE": ["h23"],
	"EG": ["h12", "h23"],
	"EH": ["h12", "h23"],
	"ER": ["h12", "h23"],
	"ES": ["h23", "h12"],
	"ET": ["h12", "h23"],
	"FI": ["h23"],
	"FJ": ["h12", "h23"],
	"FK": ["h23", "h12"],
	"FM": ["h12", "h23"],
	"FO": ["h23", "h12"],
	"FR": ["h23"],
	"GA": ["h23"],
	"GB": ["h23", "h12"],
	"GD": ["h12", "h23"],
	"GE": ["h23", "h12"],
	"GF": ["h23"],
	"GG": ["h23", "h12"],
	"GH": ["h12", "h23"],
	"GI": ["h23", "h12"],
	"GL": ["h23", "h12"],
	"GM": ["h12", "h23"],
	"GN": ["h23"],
	"GP": ["h23"],
	"GQ": ["h23", "h12"],
	"GR": ["h12", "h23"],
	"GS": ["h23", "h12"],
	"GT": ["h12", "h23"],
	"GU": ["h12", "h23"],
	"GW": ["h23"],
	"GY": ["h12", "h23"],
	"HK": ["h12", "h23"],
	"HN": ["h12", "h23"],
	"HR": ["h23"],
	"HU": ["h23", "h12"],
	"IC": ["h23", "h12"],
	"ID": ["h23"],
	"IE": ["h23", "h12"],
	"IL": ["h23"],
	"IM": ["h23", "h12"],
	"IN": ["h12", "h23"],
	"IO": ["h23", "h12"],
	"IQ": ["h12", "h23"],
	"IR": ["h23"],
	"IS": ["h23"],
	"IT": ["h23"],
	"JE": ["h23", "h12"],
	"JM": ["h12", "h23"],
	"JO": ["h12", "h23"],
	"JP": [
		"h23",
		"h11",
		"h12"
	],
	"KE": ["h23", "h12"],
	"KG": ["h23", "h12"],
	"KH": ["h12", "h23"],
	"KI": ["h12", "h23"],
	"KM": ["h23", "h12"],
	"KN": ["h12", "h23"],
	"KP": ["h12", "h23"],
	"KR": ["h12", "h23"],
	"KW": ["h12", "h23"],
	"KY": ["h12", "h23"],
	"KZ": ["h23"],
	"LA": ["h23", "h12"],
	"LB": ["h12", "h23"],
	"LC": ["h12", "h23"],
	"LI": ["h23", "h12"],
	"LK": ["h23", "h12"],
	"LR": ["h12", "h23"],
	"LS": ["h12", "h23"],
	"LT": ["h23", "h12"],
	"LU": ["h23", "h12"],
	"LV": ["h23", "h12"],
	"LY": ["h12", "h23"],
	"MA": ["h23", "h12"],
	"MC": ["h23"],
	"MD": ["h23"],
	"ME": ["h23", "h12"],
	"MF": ["h23"],
	"MG": ["h23", "h12"],
	"MH": ["h12", "h23"],
	"MK": ["h23", "h12"],
	"ML": ["h23"],
	"MM": ["h23", "h12"],
	"MN": ["h23", "h12"],
	"MO": ["h12", "h23"],
	"MP": ["h12", "h23"],
	"MQ": ["h23"],
	"MR": ["h12", "h23"],
	"MS": ["h23", "h12"],
	"MT": ["h23", "h12"],
	"MU": ["h23", "h12"],
	"MV": ["h23", "h12"],
	"MW": ["h12", "h23"],
	"MX": ["h12", "h23"],
	"MY": ["h12", "h23"],
	"MZ": ["h23"],
	"NA": ["h12", "h23"],
	"NC": ["h23"],
	"NE": ["h23"],
	"NF": ["h23", "h12"],
	"NG": ["h23", "h12"],
	"NI": ["h12", "h23"],
	"NL": ["h23"],
	"NO": ["h23", "h12"],
	"NP": ["h23", "h12"],
	"NR": ["h23", "h12"],
	"NU": ["h23", "h12"],
	"NZ": ["h12", "h23"],
	"OM": ["h12", "h23"],
	"PA": ["h12", "h23"],
	"PE": ["h12", "h23"],
	"PF": ["h23", "h12"],
	"PG": ["h12", "h23"],
	"PH": ["h12", "h23"],
	"PK": ["h12", "h23"],
	"PL": ["h23", "h12"],
	"PM": ["h23"],
	"PN": ["h23", "h12"],
	"PR": ["h12", "h23"],
	"PS": ["h12", "h23"],
	"PT": ["h23"],
	"PW": ["h12", "h23"],
	"PY": ["h12", "h23"],
	"QA": ["h12", "h23"],
	"RE": ["h23"],
	"RO": ["h23"],
	"RS": ["h23", "h12"],
	"RU": ["h23"],
	"RW": ["h23", "h12"],
	"SA": ["h12", "h23"],
	"SB": ["h12", "h23"],
	"SC": ["h23", "h12"],
	"SD": ["h12", "h23"],
	"SE": ["h23"],
	"SG": ["h12", "h23"],
	"SH": ["h23", "h12"],
	"SI": ["h23"],
	"SJ": ["h23"],
	"SK": ["h23"],
	"SL": ["h12", "h23"],
	"SM": ["h23", "h12"],
	"SN": ["h23", "h12"],
	"SO": ["h12", "h23"],
	"SR": ["h23"],
	"SS": ["h12", "h23"],
	"ST": ["h23"],
	"SV": ["h12", "h23"],
	"SX": ["h23", "h12"],
	"SY": ["h12", "h23"],
	"SZ": ["h12", "h23"],
	"TA": ["h23", "h12"],
	"TC": ["h12", "h23"],
	"TD": ["h12", "h23"],
	"TF": ["h23", "h12"],
	"TG": ["h23"],
	"TH": ["h23", "h12"],
	"TJ": ["h23", "h12"],
	"TL": ["h23", "h12"],
	"TM": ["h23", "h12"],
	"TN": ["h12", "h23"],
	"TO": ["h12", "h23"],
	"TR": ["h23"],
	"TT": ["h12", "h23"],
	"TW": ["h12", "h23"],
	"TZ": ["h23", "h12"],
	"UA": ["h23", "h12"],
	"UG": ["h23", "h12"],
	"UM": ["h12", "h23"],
	"US": ["h12", "h23"],
	"UY": ["h12", "h23"],
	"UZ": ["h23", "h12"],
	"VA": ["h23", "h12"],
	"VC": ["h12", "h23"],
	"VE": ["h12", "h23"],
	"VG": ["h12", "h23"],
	"VI": ["h12", "h23"],
	"VN": ["h23", "h12"],
	"VU": ["h12", "h23"],
	"WF": ["h23"],
	"WS": ["h12", "h23"],
	"XK": ["h23", "h12"],
	"YE": ["h12", "h23"],
	"YT": ["h23"],
	"ZA": ["h23", "h12"],
	"ZM": ["h12", "h23"],
	"ZW": ["h23", "h12"],
	"af-ZA": ["h23", "h12"],
	"ar-001": ["h12", "h23"],
	"ca-ES": ["h23", "h12"],
	"en-001": ["h12", "h23"],
	"en-HK": ["h12", "h23"],
	"en-IL": ["h23", "h12"],
	"en-MY": ["h12", "h23"],
	"es-BR": ["h23", "h12"],
	"es-ES": ["h23", "h12"],
	"es-GQ": ["h23", "h12"],
	"fr-CA": ["h23", "h12"],
	"gl-ES": ["h23", "h12"],
	"gu-IN": ["h12", "h23"],
	"hi-IN": ["h12", "h23"],
	"it-CH": ["h23", "h12"],
	"it-IT": ["h23", "h12"],
	"kn-IN": ["h12", "h23"],
	"ku-SY": ["h23"],
	"ml-IN": ["h12", "h23"],
	"mr-IN": ["h12", "h23"],
	"pa-IN": ["h12", "h23"],
	"ta-IN": ["h12", "h23"],
	"te-IN": ["h12", "h23"],
	"zu-ZA": ["h23", "h12"]
};
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.locale@0.0.0/node_modules/@formatjs_generated/cldr.locale/calendars.js
const calendars = {
	"001": ["gregorian"],
	"AE": [
		"gregorian",
		"islamic-umalqura",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"AF": [
		"persian",
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"AL": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"AZ": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"BD": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"BH": [
		"gregorian",
		"islamic-umalqura",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"CN": ["gregorian", "chinese"],
	"CX": ["gregorian", "chinese"],
	"DJ": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"DZ": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"EG": [
		"gregorian",
		"coptic",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"EH": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"ER": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"ET": ["gregorian", "ethiopic"],
	"HK": ["gregorian", "chinese"],
	"ID": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"IL": [
		"gregorian",
		"hebrew",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"IN": ["gregorian", "indian"],
	"IQ": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"IR": [
		"persian",
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"JO": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"JP": ["gregorian", "japanese"],
	"KM": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"KR": ["gregorian", "dangi"],
	"KW": [
		"gregorian",
		"islamic-umalqura",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"LB": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"LY": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"MA": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"MO": ["gregorian", "chinese"],
	"MR": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"MV": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"MY": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"NE": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"OM": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"PK": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"PS": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"QA": [
		"gregorian",
		"islamic-umalqura",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"SA": [
		"gregorian",
		"islamic-umalqura",
		"islamic",
		"islamic-rgsa"
	],
	"SD": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"SG": ["gregorian", "chinese"],
	"SY": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"TD": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"TH": ["buddhist", "gregorian"],
	"TJ": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"TM": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"TN": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	],
	"TR": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"TW": [
		"gregorian",
		"roc",
		"chinese"
	],
	"UZ": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"XK": [
		"gregorian",
		"islamic-civil",
		"islamic-tbla"
	],
	"YE": [
		"gregorian",
		"islamic",
		"islamic-civil",
		"islamic-tbla"
	]
};
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.locale@0.0.0/node_modules/@formatjs_generated/cldr.locale/week-data.js
const weekData = {
	"001": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AD": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"AE": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AF": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [4, 5]
	},
	"AG": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AI": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AL": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AO": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AQ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AS": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AT": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"AU": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AW": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"AX": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"AZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BB": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BD": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"BF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BG": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"BH": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"BI": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BJ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BL": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BO": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BQ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BR": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BS": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BT": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BV": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BW": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BY": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"BZ": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CA": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CD": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CH": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"CI": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CK": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CL": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CO": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CP": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CQ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CU": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CV": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CW": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CX": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CY": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"CZ": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"DE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"DG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"DJ": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"DK": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"DM": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"DO": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"DZ": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"EA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"EC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"EE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"EG": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"EH": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ER": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ES": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"ET": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"FI": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"FJ": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"FK": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"FM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"FO": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"FR": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GB": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GD": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GE": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GF": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GG": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GH": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GI": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GL": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GP": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GQ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GR": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"GS": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GT": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GU": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GW": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"GY": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"HK": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"HM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"HN": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"HR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"HT": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"HU": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"IC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ID": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"IE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"IL": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"IM": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"IN": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [7]
	},
	"IO": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"IQ": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"IR": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5]
	},
	"IS": {
		"firstDay": 7,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"IT": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"JE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"JM": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"JO": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"JP": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KE": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KH": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KI": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KP": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KR": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KW": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"KY": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"KZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LA": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LB": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LI": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"LK": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LS": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LT": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"LU": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"LV": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"LY": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"MA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MC": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"MD": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ME": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MH": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MK": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ML": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MM": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MO": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MP": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MQ": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"MR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MS": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MT": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MU": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MV": {
		"firstDay": 5,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MW": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MX": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MY": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"MZ": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NE": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NI": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NL": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"NO": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"NP": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NU": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"NZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"OM": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"PA": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PE": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PH": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PK": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PL": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"PM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PR": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PS": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PT": {
		"firstDay": 7,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"PW": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"PY": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"QA": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"RE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"RO": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"RS": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"RU": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"RW": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SA": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"SB": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SD": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"SE": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"SG": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SH": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SI": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SJ": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"SK": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"SL": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SM": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"SN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SO": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SS": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ST": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SV": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SX": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"SY": {
		"firstDay": 6,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"SZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TD": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TH": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TJ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TK": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TL": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TO": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TR": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TT": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TV": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TW": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"TZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"UA": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"UG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [7]
	},
	"UM": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"US": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"UY": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"UZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"VA": {
		"firstDay": 1,
		"minimalDays": 4,
		"weekend": [6, 7]
	},
	"VC": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"VE": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"VG": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"VI": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"VN": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"VU": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"WF": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"WS": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"XK": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"YE": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [5, 6]
	},
	"YT": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ZA": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ZM": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ZW": {
		"firstDay": 7,
		"minimalDays": 1,
		"weekend": [6, 7]
	},
	"ZZ": {
		"firstDay": 1,
		"minimalDays": 1,
		"weekend": [6, 7]
	}
};
//#endregion
//#region packages/intl-locale/preference-data.ts
function getCalendarPreferenceDataForRegion(region, regionOverride, language) {
	for (const candidate of [regionOverride, region]) {
		if (!candidate) continue;
		const territory = candidate.toUpperCase();
		const values = calendars[`${language}-${territory}`] || calendars[territory];
		if (values) return values.map((c) => c === "gregorian" ? "gregory" : c);
	}
	return ["gregory"];
}
function getHourCyclesPreferenceDataForLocaleOrRegion(language, region, regionOverride) {
	for (const candidate of [regionOverride, region]) {
		if (!candidate) continue;
		const territory = candidate.toUpperCase();
		const values = hourCycles[`${language}-${territory}`] || hourCycles[territory];
		if (values) return [...values];
	}
	return ["h23"];
}
function getTimeZonePreferenceForRegion(region) {
	const territory = region.toLowerCase();
	if (timezones[territory]) return [...timezones[territory]];
	return [];
}
function getWeekDataForRegion(region, regionOverride) {
	const _region = region ? region.toUpperCase() : "";
	const override = (regionOverride || "").toUpperCase();
	return weekData[override] || weekData[_region] || weekData["001"];
}
//#endregion
//#region packages/intl-locale/index.ts
const RELEVANT_EXTENSION_KEYS = [
	"ca",
	"co",
	"hc",
	"kf",
	"kn",
	"nu",
	"fw"
];
function applyOptionsToTag(tag, options) {
	invariant(typeof tag === "string", "language tag must be a string");
	invariant(isStructurallyValidLanguageTag(tag), "malformed language tag", RangeError);
	tag = Intl.getCanonicalLocales(tag)[0];
	const language = GetOption(options, "language", "string", void 0, void 0);
	if (language !== void 0) invariant(isUnicodeLanguageSubtag(language), "Malformed unicode_language_subtag", RangeError);
	const script = GetOption(options, "script", "string", void 0, void 0);
	if (script !== void 0) invariant(isUnicodeScriptSubtag(script), "Malformed unicode_script_subtag", RangeError);
	const region = GetOption(options, "region", "string", void 0, void 0);
	if (region !== void 0) invariant(isUnicodeRegionSubtag(region), "Malformed unicode_region_subtag", RangeError);
	const variants = GetOption(options, "variants", "string", void 0, void 0);
	const languageId = parseUnicodeLanguageId(tag);
	if (variants !== void 0) {
		const subtags = variants.replace(/[A-Z]/g, (character) => character.toLowerCase()).split("-");
		if (subtags.some((variant) => !isUnicodeVariantSubtag(variant)) || new Set(subtags).size !== subtags.length) throw new RangeError("Malformed unicode_variant_subtag");
		languageId.variants = subtags;
	}
	if (language !== void 0) languageId.lang = language;
	if (script !== void 0) languageId.script = script;
	if (region !== void 0) languageId.region = region;
	return Intl.getCanonicalLocales(emitUnicodeLocaleId({
		...parseUnicodeLocaleId(tag),
		lang: languageId
	}))[0];
}
function applyUnicodeExtensionToTag(tag, options, relevantExtensionKeys) {
	let unicodeExtension;
	let keywords = [];
	const ast = parseUnicodeLocaleId(tag);
	for (const ext of ast.extensions) if (ext.type === "u") {
		unicodeExtension = ext;
		if (Array.isArray(ext.keywords)) keywords = ext.keywords;
	}
	const result = Object.create(null);
	for (const key of relevantExtensionKeys) {
		let value, entry;
		for (const keyword of keywords) if (keyword[0] === key) {
			entry = keyword;
			value = entry[1];
		}
		invariant(key in options, `${key} must be in options`);
		const optionsValue = options[key];
		if (optionsValue !== void 0) {
			invariant(typeof optionsValue === "string", `Value for ${key} must be a string`);
			value = optionsValue;
			if (entry) entry[1] = value;
			else keywords.push([key, value]);
		}
		result[key] = value;
	}
	if (!unicodeExtension) {
		if (keywords.length) ast.extensions.push({
			type: "u",
			keywords,
			attributes: []
		});
	} else unicodeExtension.keywords = keywords;
	result.locale = Intl.getCanonicalLocales(emitUnicodeLocaleId(ast))[0];
	const canonicalExtension = parseUnicodeLocaleId(result.locale).extensions.find((extension) => extension.type === "u");
	for (const key of relevantExtensionKeys) result[key] = canonicalExtension?.keywords.find((keyword) => keyword[0] === key)?.[1];
	return result;
}
function mergeUnicodeLanguageId(lang, script, region, variants = [], replacement) {
	if (!replacement) return {
		lang: lang || "und",
		script,
		region,
		variants
	};
	return {
		lang: !lang || lang === "und" ? replacement.lang : lang,
		script: script || replacement.script,
		region: region || replacement.region,
		variants: [...variants, ...replacement.variants]
	};
}
function addLikelySubtags(tag) {
	const ast = parseUnicodeLocaleId(tag);
	const { lang, variants } = ast.lang;
	const script = ast.lang.script === "Zzzz" ? void 0 : ast.lang.script;
	const region = ast.lang.region === "ZZ" ? void 0 : ast.lang.region;
	if (lang !== "und" && script && region) return tag;
	for (const candidate of [
		{
			lang,
			script,
			region,
			variants: []
		},
		{
			lang,
			script,
			variants: []
		},
		{
			lang,
			region,
			variants: []
		},
		{
			lang,
			variants: []
		}
	]) {
		const match = likelySubtags[emitUnicodeLanguageId(candidate)];
		if (!match) continue;
		ast.lang = mergeUnicodeLanguageId(lang, script, region, variants, parseUnicodeLanguageId(match));
		return emitUnicodeLocaleId(ast);
	}
	return tag;
}
/**
* From: https://github.com/unicode-org/icu/blob/4231ca5be053a22a1be24eb891817458c97db709/icu4j/main/classes/core/src/com/ibm/icu/util/ULocale.java#L2395
* @param tag
*/
function removeLikelySubtags(tag) {
	let maxLocale = addLikelySubtags(tag);
	if (!maxLocale) return tag;
	const ast = parseUnicodeLocaleId(maxLocale);
	maxLocale = emitUnicodeLanguageId({
		...parseUnicodeLanguageId(maxLocale),
		variants: []
	});
	const { lang: { lang, script, region, variants } } = ast;
	if (addLikelySubtags(emitUnicodeLanguageId({
		lang,
		variants: []
	})) === maxLocale) return emitUnicodeLocaleId({
		...ast,
		lang: mergeUnicodeLanguageId(lang, void 0, void 0, variants)
	});
	if (region) {
		if (addLikelySubtags(emitUnicodeLanguageId({
			lang,
			region,
			variants: []
		})) === maxLocale) return emitUnicodeLocaleId({
			...ast,
			lang: mergeUnicodeLanguageId(lang, void 0, region, variants)
		});
	}
	if (script) {
		if (addLikelySubtags(emitUnicodeLanguageId({
			lang,
			script,
			variants: []
		})) === maxLocale) return emitUnicodeLocaleId({
			...ast,
			lang: mergeUnicodeLanguageId(lang, script, void 0, variants)
		});
	}
	return emitUnicodeLocaleId(ast);
}
function createArrayFromListOrRestricted(list, restricted) {
	let result = list;
	if (restricted !== void 0) result = [restricted];
	return Array.from(result);
}
function calendarsOfLocale(loc) {
	const locInternalSlots = getInternalSlots(loc);
	const restricted = locInternalSlots.calendar;
	const locale = locInternalSlots.locale;
	const { region, regionOverride } = regionPreference(loc);
	return createArrayFromListOrRestricted(getCalendarPreferenceDataForRegion(region, regionOverride, parseUnicodeLanguageId(locale).lang), restricted);
}
function collationsOfLocale(loc) {
	const locInternalSlots = getInternalSlots(loc);
	const restricted = locInternalSlots.collation;
	if (restricted !== void 0) return [restricted];
	const locale = locInternalSlots.locale;
	if (!Intl.Collator.supportedLocalesOf([locale], { localeMatcher: "lookup" }).length) return ["emoji", "eor"];
	const supportedCollations = supportedValuesOf("collation").filter((co) => co !== "standard" && co !== "search" && new Intl.Collator(locale, {
		collation: co,
		localeMatcher: "lookup"
	}).resolvedOptions().collation === co);
	supportedCollations.sort();
	return createArrayFromListOrRestricted(supportedCollations, restricted);
}
function hourCyclesOfLocale(loc) {
	const locInternalSlots = getInternalSlots(loc);
	const restricted = locInternalSlots.hourCycle;
	const locale = locInternalSlots.locale;
	const { region, regionOverride } = regionPreference(loc);
	return createArrayFromListOrRestricted(getHourCyclesPreferenceDataForLocaleOrRegion(parseUnicodeLanguageId(locale).lang, region, regionOverride), restricted);
}
function numberingSystemsOfLocale(loc) {
	const locInternalSlots = getInternalSlots(loc);
	const restricted = locInternalSlots.numberingSystem;
	const locale = locInternalSlots.locale;
	const language = loc.language;
	const localeNumberingSystems = numberingSystems[locale] ?? numberingSystems[language];
	if (localeNumberingSystems) return createArrayFromListOrRestricted([...localeNumberingSystems], restricted);
	return createArrayFromListOrRestricted([], restricted);
}
function timeZonesOfLocale(loc) {
	const locale = getInternalSlots(loc).locale;
	const region = parseUnicodeLanguageId(locale).region;
	if (!region) return;
	const preferredTimeZones = getTimeZonePreferenceForRegion(region);
	preferredTimeZones.sort();
	return Array.from(preferredTimeZones);
}
function translateCharacterOrder(order) {
	if (order === "right-to-left") return "rtl";
	return "ltr";
}
function characterDirectionOfLocale(loc) {
	const locale = loc.minimize().toString();
	return translateCharacterOrder(characterOrders[locale]);
}
function regionPreference(loc) {
	const locale = getInternalSlots(loc).locale;
	const ast = parseUnicodeLocaleId(locale);
	const extension = ast.extensions.find((ext) => ext.type === "u");
	const subdivisionRegion = (key) => {
		const match = (extension?.keywords.find((entry) => entry[0] === key)?.[1])?.match(/^([a-z]{2}|[0-9]{3})[a-z0-9]{1,4}$/i);
		return match ? new Locale(`und-${match[1]}`).region : void 0;
	};
	return {
		region: ast.lang.region || subdivisionRegion("sd") || parseUnicodeLanguageId(addLikelySubtags(locale)).region || "001",
		regionOverride: subdivisionRegion("rg")
	};
}
function weekInfoOfLocale(loc) {
	const { region, regionOverride } = regionPreference(loc);
	return getWeekDataForRegion(region, regionOverride);
}
const TABLE_1 = [
	"sun",
	"mon",
	"tue",
	"wed",
	"thu",
	"fri",
	"sat"
];
function weekdayToString(fw) {
	return /^[0-7]$/.test(fw) ? TABLE_1[Number(fw) % 7] : fw;
}
var Locale = class Locale {
	constructor(tag, opts) {
		if (!(this && this instanceof Locale ? this.constructor : void 0)) throw new TypeError("Intl.Locale must be called with 'new'");
		const { relevantExtensionKeys } = Locale;
		const internalSlotsList = [
			"initializedLocale",
			"locale",
			"calendar",
			"collation",
			"hourCycle",
			"numberingSystem"
		];
		if (relevantExtensionKeys.indexOf("kf") > -1) internalSlotsList.push("caseFirst");
		if (relevantExtensionKeys.indexOf("kn") > -1) internalSlotsList.push("numeric");
		if (tag === void 0) throw new TypeError("First argument to Intl.Locale constructor can't be empty or missing");
		if (tag === null || typeof tag !== "string" && typeof tag !== "object" && typeof tag !== "function") throw new TypeError("tag must be a string or object");
		let tagInternalSlots;
		if (typeof tag === "object" && (tagInternalSlots = getInternalSlotsIfPresent(tag)) && HasOwnProperty(tagInternalSlots, "initializedLocale")) tag = tagInternalSlots.locale;
		else tag = ToString(tag);
		let internalSlots = getInternalSlots(this, internalSlotsList);
		let options = CoerceOptionsToObject(opts);
		tag = applyOptionsToTag(tag, options);
		const opt = Object.create(null);
		const calendar = GetOption(options, "calendar", "string", void 0, void 0);
		if (calendar !== void 0) {
			if (!IsUnicodeLocaleIdentifierType(calendar)) throw new RangeError("invalid calendar");
		}
		opt.ca = calendar;
		const collation = GetOption(options, "collation", "string", void 0, void 0);
		if (collation !== void 0) {
			if (!IsUnicodeLocaleIdentifierType(collation)) throw new RangeError("invalid collation");
		}
		opt.co = collation;
		let fw = GetOption(options, "firstDayOfWeek", "string", void 0, void 0);
		if (fw !== void 0) {
			fw = weekdayToString(fw);
			if (!IsUnicodeLocaleIdentifierType(fw)) throw new RangeError("Invalid firstDayOfWeek");
		}
		opt.fw = fw?.toLowerCase();
		opt.hc = GetOption(options, "hourCycle", "string", [
			"h11",
			"h12",
			"h23",
			"h24"
		], void 0);
		opt.kf = GetOption(options, "caseFirst", "string", [
			"upper",
			"lower",
			"false"
		], void 0);
		const _kn = GetOption(options, "numeric", "boolean", void 0, void 0);
		let kn;
		if (_kn !== void 0) kn = String(_kn);
		opt.kn = kn;
		const numberingSystem = GetOption(options, "numberingSystem", "string", void 0, void 0);
		if (numberingSystem !== void 0) {
			if (!IsUnicodeLocaleIdentifierType(numberingSystem)) throw new RangeError("Invalid numberingSystem");
		}
		opt.nu = numberingSystem;
		const r = applyUnicodeExtensionToTag(tag, opt, relevantExtensionKeys);
		internalSlots.locale = r.locale;
		internalSlots.calendar = r.ca;
		internalSlots.collation = r.co;
		internalSlots.firstDayOfWeek = r.fw;
		internalSlots.hourCycle = r.hc;
		if (relevantExtensionKeys.indexOf("kf") > -1) internalSlots.caseFirst = r.kf;
		if (relevantExtensionKeys.indexOf("kn") > -1) internalSlots.numeric = r.kn === "" || SameValue(r.kn, "true");
		internalSlots.numberingSystem = r.nu;
	}
	/**
	* https://www.unicode.org/reports/tr35/#Likely_Subtags
	*/
	maximize() {
		const locale = getInternalSlots(this).locale;
		try {
			const maximizedLocale = addLikelySubtags(locale);
			return new Locale(maximizedLocale);
		} catch {
			return new Locale(locale);
		}
	}
	/**
	* https://www.unicode.org/reports/tr35/#Likely_Subtags
	*/
	minimize() {
		const locale = getInternalSlots(this).locale;
		try {
			const minimizedLocale = removeLikelySubtags(locale);
			return new Locale(minimizedLocale);
		} catch {
			return new Locale(locale);
		}
	}
	toString() {
		const slots = getInternalSlots(this);
		if (!HasOwnProperty(slots, "initializedLocale")) throw new TypeError("Error uninitialized locale");
		return slots.locale;
	}
	get baseName() {
		const locale = getInternalSlots(this).locale;
		return emitUnicodeLanguageId(parseUnicodeLanguageId(locale));
	}
	get calendar() {
		return getInternalSlots(this).calendar;
	}
	get collation() {
		return getInternalSlots(this).collation;
	}
	get caseFirst() {
		return getInternalSlots(this).caseFirst;
	}
	get numeric() {
		return getInternalSlots(this).numeric;
	}
	get numberingSystem() {
		return getInternalSlots(this).numberingSystem;
	}
	/**
	* https://tc39.es/proposal-intl-locale/#sec-Intl.Locale.prototype.language
	*/
	get language() {
		const locale = getInternalSlots(this).locale;
		return parseUnicodeLanguageId(locale).lang;
	}
	/**
	* https://tc39.es/proposal-intl-locale/#sec-Intl.Locale.prototype.script
	*/
	get script() {
		const locale = getInternalSlots(this).locale;
		return parseUnicodeLanguageId(locale).script;
	}
	/**
	* https://tc39.es/proposal-intl-locale/#sec-Intl.Locale.prototype.region
	*/
	get region() {
		const locale = getInternalSlots(this).locale;
		return parseUnicodeLanguageId(locale).region;
	}
	/**
	* Returns the variant subtags of the locale, or undefined if none exist.
	* Multiple variants are joined with hyphens in alphabetical order.
	*
	* Added in ECMA-402 via PR #960 to align with other subtag accessors.
	* @see https://tc39.es/ecma402/#sec-Intl.Locale.prototype.variants
	* @see https://github.com/tc39/ecma402/pull/960
	* @see https://github.com/tc39/ecma402/issues/900
	*/
	get variants() {
		const locale = getInternalSlots(this).locale;
		const variants = parseUnicodeLanguageId(locale).variants;
		if (!variants || variants.length === 0) return;
		return variants.join("-");
	}
	get firstDayOfWeek() {
		const internalSlots = getInternalSlots(this);
		if (!HasOwnProperty(internalSlots, "initializedLocale")) throw new TypeError("Error uninitialized locale");
		return internalSlots.firstDayOfWeek;
	}
	get hourCycle() {
		const internalSlots = getInternalSlots(this);
		if (!HasOwnProperty(internalSlots, "initializedLocale")) throw new TypeError("Error uninitialized locale");
		return internalSlots.hourCycle;
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getCalendars
	* https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getCalendars
	*/
	getCalendars() {
		return calendarsOfLocale(this);
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getCollations
	* https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getCollations
	*/
	getCollations() {
		return collationsOfLocale(this);
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getHourCycles
	* https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getHourCycles
	*/
	getHourCycles() {
		if (!HasOwnProperty(getInternalSlots(this), "initializedLocale")) throw new TypeError("Error uninitialized locale");
		return hourCyclesOfLocale(this);
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getNumberingSystems
	* https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getNumberingSystems
	*/
	getNumberingSystems() {
		return numberingSystemsOfLocale(this);
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getTimeZones
	* https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getTimeZones
	*/
	getTimeZones() {
		return timeZonesOfLocale(this);
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getTextInfo
	* https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getTextInfo
	*/
	getTextInfo() {
		const info = Object.create(Object.prototype);
		createDataProperty(info, "direction", characterDirectionOfLocale(this));
		return info;
	}
	/**
	* https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/getWeekInfo
	* ECMA-402 §15.3.22 Intl.Locale.prototype.getWeekInfo, steps 3–7.
	* https://tc39.es/ecma402/#sec-Intl.Locale.prototype.getWeekInfo
	* https://github.com/tc39/ecma402/blob/b1c961988b9a07894b1dc3dc2b5626ea48387d61/spec/locale.html#L420-L424
	*/
	getWeekInfo() {
		const info = Object.create(Object.prototype);
		const internalSlots = getInternalSlots(this);
		if (!HasOwnProperty(internalSlots, "initializedLocale")) throw new TypeError("Error uninitialized locale");
		const wi = weekInfoOfLocale(this);
		const we = wi.weekend;
		createDataProperty(info, "firstDay", wi.firstDay);
		createDataProperty(info, "weekend", [...we]);
		const fw = internalSlots.firstDayOfWeek;
		const day = TABLE_1.indexOf(fw);
		if (day !== -1) info.firstDay = day || 7;
		return info;
	}
	static {
		this.relevantExtensionKeys = RELEVANT_EXTENSION_KEYS;
	}
	static {
		this.polyfilled = true;
	}
};
try {
	if (typeof Symbol !== "undefined") Object.defineProperty(Locale.prototype, Symbol.toStringTag, {
		value: "Intl.Locale",
		writable: false,
		enumerable: false,
		configurable: true
	});
	Object.defineProperty(Locale.prototype.constructor, "length", {
		value: 1,
		writable: false,
		enumerable: false,
		configurable: true
	});
} catch {}
defineProperty(ensureIntl(), "Locale", { value: Locale });
//#endregion

//# sourceMappingURL=polyfill-force.js.map