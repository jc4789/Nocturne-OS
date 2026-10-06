import { memoize, strategies } from "@formatjs/fast-memoize";
//#region packages/ecma262-abstract/ToString.js
/**
* https://tc39.es/ecma262/#sec-tostring
*/
function ToString(o) {
	if (typeof o === "symbol") throw TypeError("Cannot convert a Symbol value to a string");
	return String(o);
}
//#endregion
//#region packages/intl-supportedvaluesof/memoize.ts
/**
* Implementation: Memoized factory for Intl.DateTimeFormat instances
*
* Creates and caches DateTimeFormat instances to avoid repeated instantiation overhead.
* This is critical for performance since we test many candidate values against formatters.
*
* Uses variadic memoization strategy to handle varying numbers of constructor arguments.
*/
const createMemoizedDateTimeFormat = memoize((...args) => new Intl.DateTimeFormat(...args), { strategy: strategies.variadic });
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.supported-values@0.0.0/node_modules/@formatjs_generated/cldr.supported-values/calendars.js
const calendars = [
	"buddhist",
	"chinese",
	"coptic",
	"dangi",
	"ethioaa",
	"ethiopic",
	"gregory",
	"hebrew",
	"indian",
	"islamic",
	"islamic-civil",
	"islamic-rgsa",
	"islamic-tbla",
	"islamic-umalqura",
	"islamicc",
	"iso8601",
	"japanese",
	"persian",
	"roc"
];
//#endregion
//#region packages/intl-supportedvaluesof/get-supported-calendars.ts
/**
* Implementation: Tests if a calendar is supported by attempting to create
* a DateTimeFormat with that calendar and verifying it was accepted.
*
* CLDR Data: Candidate values come from CLDR calendar types
*/
function isSupportedCalendar(item) {
	try {
		return createMemoizedDateTimeFormat(`en-u-ca-${item}`).resolvedOptions().calendar === item;
	} catch {}
	return false;
}
/**
* ECMA-402 Spec: Returns supported calendar identifiers
* ECMA-402 Spec: Results must be sorted lexicographically
*
* Implementation: Filters CLDR list against actual runtime support
*/
function getSupportedCalendars() {
	return calendars.filter(isSupportedCalendar).sort();
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.supported-values@0.0.0/node_modules/@formatjs_generated/cldr.supported-values/collations.js
const collations = [
	"big5han",
	"compat",
	"dict",
	"direct",
	"ducet",
	"emoji",
	"eor",
	"gb2312",
	"phonebk",
	"phonetic",
	"pinyin",
	"reformed",
	"search",
	"searchjl",
	"standard",
	"stroke",
	"trad",
	"unihan",
	"zhuyin"
];
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.collation@0.0.0/node_modules/@formatjs_generated/cldr.collation/locale-data.js
const collationCandidateLocales = {
	"compat": ["ar"],
	"trad": [
		"bn",
		"es",
		"fi",
		"kn",
		"sa",
		"sv",
		"vi"
	],
	"digits-after": ["cs"],
	"eor": [
		"de",
		"de-AT",
		"en"
	],
	"phonebk": ["de", "de-AT"],
	"private-kana": ["ja"],
	"unihan": [
		"ja",
		"ko",
		"zh",
		"zh-Hant"
	],
	"searchjl": ["ko"],
	"phonetic": ["ln"],
	"emoji": ["en"],
	"private-unihan": ["en"],
	"dict": ["si"],
	"pinyin": ["zh", "zh-Hant"],
	"private-pinyin": ["zh", "zh-Hant"],
	"stroke": ["zh", "zh-Hant"],
	"zhuyin": ["zh", "zh-Hant"]
};
//#endregion
//#region packages/intl-supportedvaluesof/get-supported-collations.ts
function isSupportedCollation(collation) {
	const candidates = collationCandidateLocales[collation] || ["en"];
	for (const locale of candidates) try {
		if (new Intl.Collator(`${locale}-u-co-${collation}`).resolvedOptions().collation === collation) return true;
	} catch {}
	return false;
}
function getSupportedCollations() {
	return collations.filter(isSupportedCollation).sort();
}
//#endregion
//#region packages/ecma402-abstract/utils.js
const createMemoizedNumberFormat = memoize((...args) => new Intl.NumberFormat(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.PluralRules(...args), { strategy: strategies.variadic });
memoize((...args) => new Intl.ListFormat(...args), { strategy: strategies.variadic });
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.supported-values@0.0.0/node_modules/@formatjs_generated/cldr.supported-values/currencies.js
const currencies = [
	"ADP",
	"AED",
	"AFA",
	"AFN",
	"ALK",
	"ALL",
	"AMD",
	"ANG",
	"AOA",
	"AOK",
	"AON",
	"AOR",
	"ARA",
	"ARL",
	"ARM",
	"ARP",
	"ARS",
	"ATS",
	"AUD",
	"AWG",
	"AZM",
	"AZN",
	"BAD",
	"BAM",
	"BAN",
	"BBD",
	"BDT",
	"BEC",
	"BEF",
	"BEL",
	"BGL",
	"BGM",
	"BGN",
	"BGO",
	"BHD",
	"BIF",
	"BMD",
	"BND",
	"BOB",
	"BOL",
	"BOP",
	"BOV",
	"BRB",
	"BRC",
	"BRE",
	"BRL",
	"BRN",
	"BRR",
	"BRZ",
	"BSD",
	"BTN",
	"BUK",
	"BWP",
	"BYB",
	"BYN",
	"BYR",
	"BZD",
	"CAD",
	"CDF",
	"CHE",
	"CHF",
	"CHW",
	"CLE",
	"CLF",
	"CLP",
	"CNH",
	"CNX",
	"CNY",
	"COP",
	"COU",
	"CRC",
	"CSD",
	"CSK",
	"CUC",
	"CUP",
	"CVE",
	"CYP",
	"CZK",
	"DDM",
	"DEM",
	"DJF",
	"DKK",
	"DOP",
	"DZD",
	"ECS",
	"ECV",
	"EEK",
	"EGP",
	"ERN",
	"ESA",
	"ESB",
	"ESP",
	"ETB",
	"EUR",
	"FIM",
	"FJD",
	"FKP",
	"FRF",
	"GBP",
	"GEK",
	"GEL",
	"GHC",
	"GHS",
	"GIP",
	"GMD",
	"GNF",
	"GNS",
	"GQE",
	"GRD",
	"GTQ",
	"GWE",
	"GWP",
	"GYD",
	"HKD",
	"HNL",
	"HRD",
	"HRK",
	"HTG",
	"HUF",
	"IDR",
	"IEP",
	"ILP",
	"ILR",
	"ILS",
	"INR",
	"IQD",
	"IRR",
	"ISJ",
	"ISK",
	"ITL",
	"JMD",
	"JOD",
	"JPY",
	"KES",
	"KGS",
	"KHR",
	"KMF",
	"KPW",
	"KRH",
	"KRO",
	"KRW",
	"KWD",
	"KYD",
	"KZT",
	"LAK",
	"LBP",
	"LKR",
	"LRD",
	"LSL",
	"LTL",
	"LTT",
	"LUC",
	"LUF",
	"LUL",
	"LVL",
	"LVR",
	"LYD",
	"MAD",
	"MAF",
	"MCF",
	"MDC",
	"MDL",
	"MGA",
	"MGF",
	"MKD",
	"MKN",
	"MLF",
	"MMK",
	"MNT",
	"MOP",
	"MRO",
	"MRU",
	"MTL",
	"MTP",
	"MUR",
	"MVP",
	"MVR",
	"MWK",
	"MXN",
	"MXP",
	"MXV",
	"MYR",
	"MZE",
	"MZM",
	"MZN",
	"NAD",
	"NGN",
	"NIC",
	"NIO",
	"NLG",
	"NOK",
	"NPR",
	"NZD",
	"OMR",
	"PAB",
	"PEI",
	"PEN",
	"PES",
	"PGK",
	"PHP",
	"PKR",
	"PLN",
	"PLZ",
	"PTE",
	"PYG",
	"QAR",
	"RHD",
	"ROL",
	"RON",
	"RSD",
	"RUB",
	"RUR",
	"RWF",
	"SAR",
	"SBD",
	"SCR",
	"SDD",
	"SDG",
	"SDP",
	"SEK",
	"SGD",
	"SHP",
	"SIT",
	"SKK",
	"SLE",
	"SLL",
	"SOS",
	"SRD",
	"SRG",
	"SSP",
	"STD",
	"STN",
	"SUR",
	"SVC",
	"SYP",
	"SZL",
	"THB",
	"TJR",
	"TJS",
	"TMM",
	"TMT",
	"TND",
	"TOP",
	"TPE",
	"TRL",
	"TRY",
	"TTD",
	"TWD",
	"TZS",
	"UAH",
	"UAK",
	"UGS",
	"UGX",
	"USD",
	"USN",
	"USS",
	"UYI",
	"UYP",
	"UYU",
	"UYW",
	"UZS",
	"VEB",
	"VED",
	"VEF",
	"VES",
	"VND",
	"VNN",
	"VUV",
	"WST",
	"XAF",
	"XAG",
	"XAU",
	"XBA",
	"XBB",
	"XBC",
	"XBD",
	"XCD",
	"XCG",
	"XDR",
	"XEU",
	"XFO",
	"XFU",
	"XOF",
	"XPD",
	"XPF",
	"XPT",
	"XRE",
	"XSU",
	"XTS",
	"XUA",
	"XXX",
	"YDD",
	"YER",
	"YUD",
	"YUM",
	"YUN",
	"YUR",
	"ZAL",
	"ZAR",
	"ZMK",
	"ZMW",
	"ZRN",
	"ZRZ",
	"ZWD",
	"ZWG",
	"ZWL",
	"ZWR"
];
//#endregion
//#region packages/intl-supportedvaluesof/get-supported-currencies.ts
/**
* Implementation: Tests if a currency is supported by attempting to create
* a NumberFormat with that currency and verifying it was accepted.
*
* CLDR Data: Candidate values come from CLDR currency codes (ISO 4217)
*/
function isSupportedCurrency(currency) {
	try {
		const format = createMemoizedNumberFormat("en", {
			style: "currency",
			currencyDisplay: "name",
			currency
		}).format(123);
		if (format.substring(0, 3) !== currency && format.substring(format.length - 3) !== currency) return true;
	} catch {}
	return false;
}
/**
* ECMA-402 Spec: Returns supported currency identifiers
* ECMA-402 Spec: Results must be sorted lexicographically
*
* Implementation: Filters CLDR list against actual runtime support
*/
function getSupportedCurrencies() {
	const ATOZ = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	const supportedCurrencies = [];
	for (const currency of currencies) if (currency.length === 3) {
		if (isSupportedCurrency(currency)) supportedCurrencies.push(currency);
	} else if (currency.length === 5 && currency[3] === "~") {
		const start = ATOZ.indexOf(currency[2]);
		const end = ATOZ.indexOf(currency[4]);
		for (let i = start; i <= end; i++) {
			const currentCurrency = currency.substring(0, 2) + ATOZ[i];
			if (isSupportedCurrency(currentCurrency)) supportedCurrencies.push(currentCurrency);
		}
	}
	return supportedCurrencies.sort();
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.number@0.0.0/node_modules/@formatjs_generated/cldr.number/numbering-systems.js
const numberingSystemNames = [
	"adlm",
	"ahom",
	"arab",
	"arabext",
	"armn",
	"armnlow",
	"bali",
	"beng",
	"bhks",
	"brah",
	"cakm",
	"cham",
	"cyrl",
	"deva",
	"diak",
	"ethi",
	"fullwide",
	"gara",
	"geor",
	"gong",
	"gonm",
	"grek",
	"greklow",
	"gujr",
	"gukh",
	"guru",
	"hanidays",
	"hanidec",
	"hans",
	"hansfin",
	"hant",
	"hantfin",
	"hebr",
	"hmng",
	"hmnp",
	"java",
	"jpan",
	"jpanfin",
	"jpanyear",
	"kali",
	"kawi",
	"khmr",
	"knda",
	"krai",
	"lana",
	"lanatham",
	"laoo",
	"latn",
	"lepc",
	"limb",
	"mathbold",
	"mathdbl",
	"mathmono",
	"mathsanb",
	"mathsans",
	"mlym",
	"modi",
	"mong",
	"mroo",
	"mtei",
	"mymr",
	"mymrepka",
	"mymrpao",
	"mymrshan",
	"mymrtlng",
	"nagm",
	"newa",
	"nkoo",
	"olck",
	"onao",
	"orya",
	"osma",
	"outlined",
	"rohg",
	"roman",
	"romanlow",
	"saur",
	"segment",
	"shrd",
	"sind",
	"sinh",
	"sora",
	"sund",
	"sunu",
	"takr",
	"talu",
	"taml",
	"tamldec",
	"telu",
	"thai",
	"tibt",
	"tirh",
	"tnsa",
	"tols",
	"vaii",
	"wara",
	"wcho"
];
//#endregion
//#region packages/intl-supportedvaluesof/get-supported-numbering-systems.ts
/**
* Implementation: Tests if a numbering system is supported by attempting to create
* a NumberFormat with that numbering system and verifying it was accepted.
*
* CLDR Data: Candidate values come from CLDR numbering system types
*/
function isSupportedNumberingSystem(system) {
	try {
		const numberFormat = createMemoizedNumberFormat(`en-u-nu-${system}`);
		if (numberFormat.resolvedOptions().numberingSystem === system && system === "latn" || numberFormat.format(123) !== "123") return true;
	} catch {}
	return false;
}
/**
* ECMA-402 Spec: Returns supported numbering system identifiers
* ECMA-402 Spec: Results must be sorted lexicographically
*
* Implementation: Filters CLDR list against actual runtime support
*/
function getSupportedNumberingSystems() {
	return numberingSystemNames.filter(isSupportedNumberingSystem).sort();
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.supported-values@0.0.0/node_modules/@formatjs_generated/cldr.supported-values/timezones.js
const timezones = [
	"Africa/Abidjan",
	"Africa/Accra",
	"Africa/Addis_Ababa",
	"Africa/Algiers",
	"Africa/Asmara",
	"Africa/Bamako",
	"Africa/Bangui",
	"Africa/Banjul",
	"Africa/Bissau",
	"Africa/Blantyre",
	"Africa/Brazzaville",
	"Africa/Bujumbura",
	"Africa/Cairo",
	"Africa/Casablanca",
	"Africa/Ceuta",
	"Africa/Conakry",
	"Africa/Dakar",
	"Africa/Dar_es_Salaam",
	"Africa/Djibouti",
	"Africa/Douala",
	"Africa/El_Aaiun",
	"Africa/Freetown",
	"Africa/Gaborone",
	"Africa/Harare",
	"Africa/Johannesburg",
	"Africa/Juba",
	"Africa/Kampala",
	"Africa/Khartoum",
	"Africa/Kigali",
	"Africa/Kinshasa",
	"Africa/Lagos",
	"Africa/Libreville",
	"Africa/Lome",
	"Africa/Luanda",
	"Africa/Lubumbashi",
	"Africa/Lusaka",
	"Africa/Malabo",
	"Africa/Maputo",
	"Africa/Maseru",
	"Africa/Mbabane",
	"Africa/Mogadishu",
	"Africa/Monrovia",
	"Africa/Nairobi",
	"Africa/Ndjamena",
	"Africa/Niamey",
	"Africa/Nouakchott",
	"Africa/Ouagadougou",
	"Africa/Porto-Novo",
	"Africa/Sao_Tome",
	"Africa/Tripoli",
	"Africa/Tunis",
	"Africa/Windhoek",
	"America/Adak",
	"America/Anchorage",
	"America/Anguilla",
	"America/Antigua",
	"America/Araguaina",
	"America/Argentina/Buenos_Aires",
	"America/Argentina/Catamarca",
	"America/Argentina/Cordoba",
	"America/Argentina/Jujuy",
	"America/Argentina/La_Rioja",
	"America/Argentina/Mendoza",
	"America/Argentina/Rio_Gallegos",
	"America/Argentina/Salta",
	"America/Argentina/San_Juan",
	"America/Argentina/San_Luis",
	"America/Argentina/Tucuman",
	"America/Argentina/Ushuaia",
	"America/Aruba",
	"America/Asuncion",
	"America/Atikokan",
	"America/Bahia",
	"America/Bahia_Banderas",
	"America/Barbados",
	"America/Belem",
	"America/Belize",
	"America/Blanc-Sablon",
	"America/Boa_Vista",
	"America/Bogota",
	"America/Boise",
	"America/Cambridge_Bay",
	"America/Campo_Grande",
	"America/Cancun",
	"America/Caracas",
	"America/Cayenne",
	"America/Cayman",
	"America/Chicago",
	"America/Chihuahua",
	"America/Ciudad_Juarez",
	"America/Costa_Rica",
	"America/Coyhaique",
	"America/Creston",
	"America/Cuiaba",
	"America/Curacao",
	"America/Danmarkshavn",
	"America/Dawson",
	"America/Dawson_Creek",
	"America/Denver",
	"America/Detroit",
	"America/Dominica",
	"America/Edmonton",
	"America/Eirunepe",
	"America/El_Salvador",
	"America/Fort_Nelson",
	"America/Fortaleza",
	"America/Glace_Bay",
	"America/Goose_Bay",
	"America/Grand_Turk",
	"America/Grenada",
	"America/Guadeloupe",
	"America/Guatemala",
	"America/Guayaquil",
	"America/Guyana",
	"America/Halifax",
	"America/Havana",
	"America/Hermosillo",
	"America/Indiana/Indianapolis",
	"America/Indiana/Knox",
	"America/Indiana/Marengo",
	"America/Indiana/Petersburg",
	"America/Indiana/Tell_City",
	"America/Indiana/Vevay",
	"America/Indiana/Vincennes",
	"America/Indiana/Winamac",
	"America/Inuvik",
	"America/Iqaluit",
	"America/Jamaica",
	"America/Juneau",
	"America/Kentucky/Louisville",
	"America/Kentucky/Monticello",
	"America/Kralendijk",
	"America/La_Paz",
	"America/Lima",
	"America/Los_Angeles",
	"America/Lower_Princes",
	"America/Maceio",
	"America/Managua",
	"America/Manaus",
	"America/Marigot",
	"America/Martinique",
	"America/Matamoros",
	"America/Mazatlan",
	"America/Menominee",
	"America/Merida",
	"America/Metlakatla",
	"America/Mexico_City",
	"America/Miquelon",
	"America/Moncton",
	"America/Monterrey",
	"America/Montevideo",
	"America/Montserrat",
	"America/Nassau",
	"America/New_York",
	"America/Nipigon",
	"America/Nome",
	"America/Noronha",
	"America/North_Dakota/Beulah",
	"America/North_Dakota/Center",
	"America/North_Dakota/New_Salem",
	"America/Nuuk",
	"America/Ojinaga",
	"America/Panama",
	"America/Pangnirtung",
	"America/Paramaribo",
	"America/Phoenix",
	"America/Port-au-Prince",
	"America/Port_of_Spain",
	"America/Porto_Velho",
	"America/Puerto_Rico",
	"America/Punta_Arenas",
	"America/Rainy_River",
	"America/Rankin_Inlet",
	"America/Recife",
	"America/Regina",
	"America/Resolute",
	"America/Rio_Branco",
	"America/Santarem",
	"America/Santiago",
	"America/Santo_Domingo",
	"America/Sao_Paulo",
	"America/Scoresbysund",
	"America/Sitka",
	"America/St_Barthelemy",
	"America/St_Johns",
	"America/St_Kitts",
	"America/St_Lucia",
	"America/St_Thomas",
	"America/St_Vincent",
	"America/Swift_Current",
	"America/Tegucigalpa",
	"America/Thule",
	"America/Thunder_Bay",
	"America/Tijuana",
	"America/Toronto",
	"America/Tortola",
	"America/Vancouver",
	"America/Whitehorse",
	"America/Winnipeg",
	"America/Yakutat",
	"America/Yellowknife",
	"Antarctica/Casey",
	"Antarctica/Davis",
	"Antarctica/DumontDUrville",
	"Antarctica/Macquarie",
	"Antarctica/Mawson",
	"Antarctica/McMurdo",
	"Antarctica/Palmer",
	"Antarctica/Rothera",
	"Antarctica/Syowa",
	"Antarctica/Troll",
	"Antarctica/Vostok",
	"Arctic/Longyearbyen",
	"Asia/Aden",
	"Asia/Almaty",
	"Asia/Amman",
	"Asia/Anadyr",
	"Asia/Aqtau",
	"Asia/Aqtobe",
	"Asia/Ashgabat",
	"Asia/Atyrau",
	"Asia/Baghdad",
	"Asia/Bahrain",
	"Asia/Baku",
	"Asia/Bangkok",
	"Asia/Barnaul",
	"Asia/Beirut",
	"Asia/Bishkek",
	"Asia/Brunei",
	"Asia/Chita",
	"Asia/Choibalsan",
	"Asia/Colombo",
	"Asia/Damascus",
	"Asia/Dhaka",
	"Asia/Dili",
	"Asia/Dubai",
	"Asia/Dushanbe",
	"Asia/Famagusta",
	"Asia/Gaza",
	"Asia/Hebron",
	"Asia/Ho_Chi_Minh",
	"Asia/Hong_Kong",
	"Asia/Hovd",
	"Asia/Irkutsk",
	"Asia/Jakarta",
	"Asia/Jayapura",
	"Asia/Jerusalem",
	"Asia/Kabul",
	"Asia/Kamchatka",
	"Asia/Karachi",
	"Asia/Kathmandu",
	"Asia/Khandyga",
	"Asia/Kolkata",
	"Asia/Krasnoyarsk",
	"Asia/Kuala_Lumpur",
	"Asia/Kuching",
	"Asia/Kuwait",
	"Asia/Macau",
	"Asia/Magadan",
	"Asia/Makassar",
	"Asia/Manila",
	"Asia/Muscat",
	"Asia/Nicosia",
	"Asia/Novokuznetsk",
	"Asia/Novosibirsk",
	"Asia/Omsk",
	"Asia/Oral",
	"Asia/Phnom_Penh",
	"Asia/Pontianak",
	"Asia/Pyongyang",
	"Asia/Qatar",
	"Asia/Qostanay",
	"Asia/Qyzylorda",
	"Asia/Riyadh",
	"Asia/Sakhalin",
	"Asia/Samarkand",
	"Asia/Seoul",
	"Asia/Shanghai",
	"Asia/Singapore",
	"Asia/Srednekolymsk",
	"Asia/Taipei",
	"Asia/Tashkent",
	"Asia/Tbilisi",
	"Asia/Tehran",
	"Asia/Thimphu",
	"Asia/Tokyo",
	"Asia/Tomsk",
	"Asia/Ulaanbaatar",
	"Asia/Urumqi",
	"Asia/Ust-Nera",
	"Asia/Vientiane",
	"Asia/Vladivostok",
	"Asia/Yakutsk",
	"Asia/Yangon",
	"Asia/Yekaterinburg",
	"Asia/Yerevan",
	"Atlantic/Azores",
	"Atlantic/Bermuda",
	"Atlantic/Canary",
	"Atlantic/Cape_Verde",
	"Atlantic/Faroe",
	"Atlantic/Madeira",
	"Atlantic/Reykjavik",
	"Atlantic/South_Georgia",
	"Atlantic/St_Helena",
	"Atlantic/Stanley",
	"Australia/Adelaide",
	"Australia/Brisbane",
	"Australia/Broken_Hill",
	"Australia/Currie",
	"Australia/Darwin",
	"Australia/Eucla",
	"Australia/Hobart",
	"Australia/Lindeman",
	"Australia/Lord_Howe",
	"Australia/Melbourne",
	"Australia/Perth",
	"Australia/Sydney",
	"Etc/GMT",
	"Etc/GMT+1",
	"Etc/GMT+10",
	"Etc/GMT+11",
	"Etc/GMT+12",
	"Etc/GMT+2",
	"Etc/GMT+3",
	"Etc/GMT+4",
	"Etc/GMT+5",
	"Etc/GMT+6",
	"Etc/GMT+7",
	"Etc/GMT+8",
	"Etc/GMT+9",
	"Etc/GMT-1",
	"Etc/GMT-10",
	"Etc/GMT-11",
	"Etc/GMT-12",
	"Etc/GMT-13",
	"Etc/GMT-14",
	"Etc/GMT-2",
	"Etc/GMT-3",
	"Etc/GMT-4",
	"Etc/GMT-5",
	"Etc/GMT-6",
	"Etc/GMT-7",
	"Etc/GMT-8",
	"Etc/GMT-9",
	"Etc/UTC",
	"Europe/Amsterdam",
	"Europe/Andorra",
	"Europe/Astrakhan",
	"Europe/Athens",
	"Europe/Belgrade",
	"Europe/Berlin",
	"Europe/Bratislava",
	"Europe/Brussels",
	"Europe/Bucharest",
	"Europe/Budapest",
	"Europe/Busingen",
	"Europe/Chisinau",
	"Europe/Copenhagen",
	"Europe/Dublin",
	"Europe/Gibraltar",
	"Europe/Guernsey",
	"Europe/Helsinki",
	"Europe/Isle_of_Man",
	"Europe/Istanbul",
	"Europe/Jersey",
	"Europe/Kaliningrad",
	"Europe/Kirov",
	"Europe/Kyiv",
	"Europe/Lisbon",
	"Europe/Ljubljana",
	"Europe/London",
	"Europe/Luxembourg",
	"Europe/Madrid",
	"Europe/Malta",
	"Europe/Mariehamn",
	"Europe/Minsk",
	"Europe/Monaco",
	"Europe/Moscow",
	"Europe/Oslo",
	"Europe/Paris",
	"Europe/Podgorica",
	"Europe/Prague",
	"Europe/Riga",
	"Europe/Rome",
	"Europe/Samara",
	"Europe/San_Marino",
	"Europe/Sarajevo",
	"Europe/Saratov",
	"Europe/Simferopol",
	"Europe/Skopje",
	"Europe/Sofia",
	"Europe/Stockholm",
	"Europe/Tallinn",
	"Europe/Tirane",
	"Europe/Ulyanovsk",
	"Europe/Uzhgorod",
	"Europe/Vaduz",
	"Europe/Vatican",
	"Europe/Vienna",
	"Europe/Vilnius",
	"Europe/Volgograd",
	"Europe/Warsaw",
	"Europe/Zagreb",
	"Europe/Zaporozhye",
	"Europe/Zurich",
	"Indian/Antananarivo",
	"Indian/Chagos",
	"Indian/Christmas",
	"Indian/Cocos",
	"Indian/Comoro",
	"Indian/Kerguelen",
	"Indian/Mahe",
	"Indian/Maldives",
	"Indian/Mauritius",
	"Indian/Mayotte",
	"Indian/Reunion",
	"Pacific/Apia",
	"Pacific/Auckland",
	"Pacific/Bougainville",
	"Pacific/Chatham",
	"Pacific/Chuuk",
	"Pacific/Easter",
	"Pacific/Efate",
	"Pacific/Fakaofo",
	"Pacific/Fiji",
	"Pacific/Funafuti",
	"Pacific/Galapagos",
	"Pacific/Gambier",
	"Pacific/Guadalcanal",
	"Pacific/Guam",
	"Pacific/Honolulu",
	"Pacific/Kanton",
	"Pacific/Kiritimati",
	"Pacific/Kosrae",
	"Pacific/Kwajalein",
	"Pacific/Majuro",
	"Pacific/Marquesas",
	"Pacific/Midway",
	"Pacific/Nauru",
	"Pacific/Niue",
	"Pacific/Norfolk",
	"Pacific/Noumea",
	"Pacific/Pago_Pago",
	"Pacific/Palau",
	"Pacific/Pitcairn",
	"Pacific/Pohnpei",
	"Pacific/Port_Moresby",
	"Pacific/Rarotonga",
	"Pacific/Saipan",
	"Pacific/Tahiti",
	"Pacific/Tarawa",
	"Pacific/Tongatapu",
	"Pacific/Wake",
	"Pacific/Wallis",
	"UTC"
];
//#endregion
//#region packages/intl-supportedvaluesof/get-supported-timezones.ts
/**
* Implementation: Tests if a timezone is supported by attempting to create
* a DateTimeFormat with that timezone and verifying it was accepted.
*
* CLDR Data: Candidate values come from IANA Time Zone Database
*/
function isSupportedTimeZone(timeZone) {
	try {
		return createMemoizedDateTimeFormat("en", { timeZone }).resolvedOptions().timeZone === timeZone;
	} catch {}
	return false;
}
/**
* ECMA-402 Spec: Returns supported timezone identifiers
* ECMA-402 Spec: Results must be sorted lexicographically
*
* Implementation: Filters CLDR list against actual runtime support
*/
function getSupportedTimeZones() {
	return timezones.filter(isSupportedTimeZone).sort();
}
//#endregion
//#region node_modules/.aspect_rules_js/@formatjs_generated+cldr.supported-values@0.0.0/node_modules/@formatjs_generated/cldr.supported-values/units.js
const units = [
	"degree",
	"acre",
	"hectare",
	"percent",
	"bit",
	"byte",
	"gigabit",
	"gigabyte",
	"kilobit",
	"kilobyte",
	"megabit",
	"megabyte",
	"petabyte",
	"terabit",
	"terabyte",
	"day",
	"hour",
	"microsecond",
	"millisecond",
	"minute",
	"month",
	"nanosecond",
	"second",
	"week",
	"year",
	"centimeter",
	"foot",
	"inch",
	"kilometer",
	"meter",
	"mile-scandinavian",
	"mile",
	"millimeter",
	"yard",
	"gram",
	"kilogram",
	"ounce",
	"pound",
	"stone",
	"celsius",
	"fahrenheit",
	"fluid-ounce",
	"gallon",
	"liter",
	"milliliter"
];
//#endregion
//#region packages/intl-supportedvaluesof/get-supported-units.ts
/**
* Implementation: Tests if a unit is supported by attempting to create
* a NumberFormat with that unit and verifying it was accepted.
*
* CLDR Data: Candidate values come from CLDR unit types
*/
function isSupportedUnit(unit) {
	try {
		return createMemoizedNumberFormat("en", {
			style: "unit",
			unit
		}).resolvedOptions().unit === unit;
	} catch {}
	return false;
}
/**
* ECMA-402 Spec: Returns supported unit identifiers
* ECMA-402 Spec: Results must be sorted lexicographically
*
* Implementation: Filters CLDR list against actual runtime support
*/
function getSupportedUnits() {
	return units.filter(isSupportedUnit).sort();
}
//#endregion
//#region packages/intl-supportedvaluesof/should-polyfill.ts
function shouldPolyfill() {
	return typeof Intl === "undefined" || !("supportedValuesOf" in Intl);
}
//#endregion
//#region packages/intl-supportedvaluesof/index.ts
/**
* ECMA-402 Spec: Intl.supportedValuesOf(key)
* ECMA-402 §8.3.2 Intl.supportedValuesOf, step 1.
* https://tc39.es/ecma402/#sec-intl.supportedvaluesof
* https://github.com/tc39/ecma402/blob/b1c961988b9a07894b1dc3dc2b5626ea48387d61/spec/intl.html#L103
*
* Returns an array containing the supported calendar, collation, currency,
* numbering systems, time zone, or unit values supported by the implementation.
*
* Implementation: We validate candidate values from CLDR against the actual
* Intl formatters to determine runtime support rather than using static lists.
* This ensures accuracy across different JavaScript engines and runtimes.
*
* @param key - The category of values to return
* @returns A sorted array of unique string values
*/
const supportedValuesOf = { supportedValuesOf(key) {
	key = ToString(key);
	switch (key) {
		case "calendar": return getSupportedCalendars();
		case "collation": return getSupportedCollations();
		case "currency": return getSupportedCurrencies();
		case "numberingSystem": return getSupportedNumberingSystems();
		case "timeZone": return getSupportedTimeZones();
		case "unit": return getSupportedUnits();
		default: throw RangeError("Invalid key: " + key);
	}
} }.supportedValuesOf;
//#endregion
export { shouldPolyfill, supportedValuesOf };

//# sourceMappingURL=index.js.map