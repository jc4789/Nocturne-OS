/* Settings: display, mouse, sound and language, all by mouse. This is only a
 * window onto the user's settings, which belong to the system: each change
 * asked for here applies at once, the system keeps it (in the user's
 * AppData\Nocturne\Settings.conf, which no program writes) and puts it back at
 * every start, and every program watching hears of it. A change made elsewhere
 * (the taskbar's volume, another program) shows here at once. Nothing here
 * needs a terminal or the boot command line. Each page shows only what the
 * system can do on this computer now, and says why when it cannot. */
import { args, exit } from "nocturne";
import {
    button, caption, card, column, desktop, divider, heading, listItem, row, run, scroll, segmented, slider, text,
    textField, theme, title, toggle, icon, type App, type Child, type DesktopSettings, type IconName, type ScreenMode,
    type ScreenModeChoice, type ScreenModes, type ScreenSource, type SoundOutput,
} from "nocturne/ui";

type Section = "display" | "mouse" | "sound" | "language";
const SECTIONS: {id: Section; label: string; icon: IconName}[] = [
    {id: "display", label: "Display", icon: "display"},
    {id: "mouse", label: "Mouse", icon: "mouse"},
    {id: "sound", label: "Sound", icon: "sound"},
    {id: "language", label: "Language", icon: "language"},
];

/* ---- the window ---- */
const current = desktop.get();
if (!current) {
    console.error("settings: the desktop is not running");
    exit(1);
}
let settings: DesktopSettings = current;
let screen: ScreenModes | null = desktop.modes();
let sound: SoundOutput | null = desktop.sound();
let languages: readonly string[] = desktop.languages() ?? ["en-US"];
let section: Section = SECTIONS.find(s => s.id === args[0])?.id ?? "display";
let status = "", statusError = false;
let dragging = false;
let tried: ScreenModeChoice | null = null; /* the mode on trial from this window */

function refused(what: string) {
    status = `The system did not accept ${what}.`;
    statusError = true;
}

/* Apply, then show what the system took. */
function change(next: Partial<Pick<DesktopSettings, "pointerSpeed" | "smoothGradients">>) {
    const ok = desktop.set({...settings, ...next});
    settings = desktop.get() ?? settings;
    if (!ok) refused("that setting");
}

function setVolume(v: number, done: boolean) {
    sound = desktop.setVolume(Math.max(0, Math.min(100, Math.round(v)))) ?? sound;
    dragging = !done;
}

function setMuted(on: boolean) {
    sound = desktop.setMuted(on) ?? sound;
}

/* ---- the monitor's modes ---- */
const sameSize = (a: ScreenModeChoice, b: ScreenModeChoice) => a.width === b.width && a.height === b.height;
const sameMode = (a: ScreenModeChoice | null, b: ScreenModeChoice | null) =>
    !!a && !!b && sameSize(a, b) && a.refreshMillihertz === b.refreshMillihertz;
const sizeName = (m: ScreenModeChoice) => `${m.width} × ${m.height}`;
/* Hz with two decimals, or three where two would not tell the monitor's rates at that size apart. */
function rateName(m: ScreenModeChoice, modes: readonly ScreenMode[]): string {
    const two = (mhz: number) => String(Math.round(mhz / 10) / 100);
    const clash = modes.some(o => sameSize(o, m) && o.refreshMillihertz !== m.refreshMillihertz &&
                                  two(o.refreshMillihertz) === two(m.refreshMillihertz));
    return `${clash ? String(m.refreshMillihertz / 1000) : two(m.refreshMillihertz)} Hz`;
}
/* The size, and the rate where it is known. */
const modeName = (m: ScreenModeChoice, modes: readonly ScreenMode[]) =>
    m.refreshMillihertz ? `${sizeName(m)} at ${rateName(m, modes)}` : sizeName(m);

function tryMode(m: ScreenModeChoice) {
    /* While the driver shows its own mode, the one it shows already is no trial; after it handed the
       screen back, any mode asks it to show one again. */
    if (screen?.source === "driver" && sameMode(m, screen.wanted)) return;
    if (desktop.tryMode(m)) {
        tried = m;
        status = "";
        statusError = false;
    } else refused("that mode");
    screen = desktop.modes() ?? screen;
}

/* A size chosen: its preferred mode if the monitor prefers one there, else the rate nearest the
   one shown now (the higher of two as near). */
function trySize(modes: readonly ScreenMode[], size: ScreenModeChoice) {
    const now = screen?.wanted?.refreshMillihertz ?? 60000;
    const at = modes.filter(m => sameSize(m, size));
    const best = at.find(m => m.preferred) ?? at.reduce((a, b) => {
        const da = Math.abs(a.refreshMillihertz - now), db = Math.abs(b.refreshMillihertz - now);
        return db < da || (db === da && b.refreshMillihertz > a.refreshMillihertz) ? b : a;
    });
    tryMode(best);
}

/* The mode on trial becomes the user's choice for this monitor, which the system keeps. */
function keepMode() {
    if (!desktop.keepMode()) {
        status = "The mode had already gone back.";
        statusError = true;
    } else screen = desktop.modes() ?? screen;
    tried = null;
}

function backMode() {
    if (!desktop.backMode()) {
        status = "The mode had already gone back.";
        statusError = true;
    } else {
        status = "Back to the mode before.";
        statusError = false;
    }
    tried = null;
    screen = desktop.modes() ?? screen;
}

/* A setting: its name and explanation on the left, its control on the right or below. */
function setting(name: string, about: Child, control: Child, below = false): Child {
    const words = column({gap: 4, grow: below ? 0 : 1}, heading(name), about);
    return card({gap: 14}, below ? [words, control] : row({gap: 16}, words, control));
}

/* Why the screen has the size it has, when no list of modes can change it (or, for a driver that
   handed the screen back, why its list is offered again). */
const REASONS: Record<ScreenSource, string> = {
    firmware: "No display driver is running, so the screen keeps the resolution the computer started with.",
    driver: "The display driver does not see a monitor.",
    handedBack: "The display driver handed the screen back, so it shows the resolution the computer started with.",
    driverStopped: "The display driver stopped, so the screen is back at the resolution the computer started with.",
    remote: "The size follows your Remote Desktop window.",
};

/* The screen: what it shows and what decides that; with a display driver's modes, the lists to
   choose from and the trial of the one chosen. */
function screenCard(): Child {
    const s = screen, source: ScreenSource = s?.source ?? "firmware", modes = s?.modes ?? [];
    const listed = source !== "remote" && modes.length > 0;
    const shown = source === "remote" || !s?.shown ? `${settings.screenWidth} × ${settings.screenHeight}`
                                                   : modeName(s.shown, modes);
    const why = source === "driver" && listed ? "The monitor's own preferred mode is marked recommended."
              : source === "handedBack" && listed ? `${REASONS.handedBack} Pick a resolution to try again.`
              : REASONS[source];
    const words = column({gap: 4},
        heading(s?.name || "Screen"),
        text(shown),
        caption(why),
        !listed && s?.chosen && source !== "remote" &&
            caption(`Kept for this monitor: ${modeName(s.chosen, [])}, used when a display driver runs.`));
    if (!listed || !s) return card({gap: 14}, words);

    /* The mode marked: the one the driver is asked for while it shows its own, else the one shown. */
    const preferred = modes.find(m => m.preferred);
    const marked = source === "driver" ? s.wanted ?? s.shown : s.shown;
    const at = (marked && modes.find(m => sameSize(m, marked))) ?? preferred ?? modes[0];
    const sizes: ScreenMode[] = [];
    for (const m of modes) if (!sizes.some(o => sameSize(o, m))) sizes.push(m);
    sizes.sort((a, b) => b.width * b.height - a.width * a.height || b.width - a.width);
    const rates = modes.filter(m => sameSize(m, at)).sort((a, b) => b.refreshMillihertz - a.refreshMillihertz);
    const recommended = " (recommended)";
    const list = (items: Child[]) => scroll({maxHeight: 236, gap: 2}, items);
    return card({gap: 14},
        words,
        s.trialMs > 0 ? trialCard(s) : null,
        tried && !s.trialMs && s.refused &&
            caption(`The monitor could not show ${modeName(tried, modes)}; the mode before is back.`,
                    {colour: theme.warning}),
        row({gap: 16, align: "start"},
            column({grow: 1, gap: 6}, caption("Resolution"),
                   list(sizes.map(m => listItem(sizeName(m) + (preferred && sameSize(m, preferred) ? recommended : ""),
                                                !!marked && sameSize(m, marked), () => trySize(modes, m),
                                                {key: sizeName(m)})))),
            column({grow: 1, gap: 6}, caption("Refresh rate"),
                   list(rates.map(m => listItem(rateName(m, modes) + (m.preferred ? recommended : ""),
                                                sameMode(m, marked), () => tryMode(m),
                                                {key: m.refreshMillihertz}))))));
}

function trialCard(s: ScreenModes): Child {
    const shown = s.wanted ? modeName(s.wanted, s.modes) : "This mode";
    return card({gap: 10, background: theme.sidebar},
        heading("Keep this display mode?"),
        caption(`${shown}. If you see this, keep it; otherwise the mode before comes back by itself.`),
        row({gap: 10},
            button("Keep", keepMode, {variant: "primary", minWidth: 96}),
            button("Go back", backMode, {minWidth: 96}),
            caption(`Going back in ${Math.ceil(s.trialMs / 1000)} s`)));
}

function displayPage(): Child {
    const s = settings;
    return [
        screenCard(),
        s.smoothGradientsAvailable &&
            setting("Smooth gradients",
                    caption("Shows gradients, photos and video with less banding. Uses more processor time."),
                    toggle(s.smoothGradients, on => change({smoothGradients: on}))),
    ];
}

function mousePage(): Child {
    return setting("Pointer speed", caption("How far the pointer moves for the same mouse movement."),
                   segmented([{label: "Slowest", value: -2}, {label: "Slow", value: -1}, {label: "Normal", value: 0},
                              {label: "Fast", value: 1}, {label: "Fastest", value: 2}], settings.pointerSpeed,
                             v => change({pointerSpeed: v})), true);
}

/* The same sound device as the taskbar's speaker names. */
function soundPage(): Child {
    const s = sound;
    if (!s?.device) return card({gap: 4}, heading("No sound device"), caption("Nocturne cannot play sound on this computer."));
    return [
        setting("Volume", caption(`Playing through ${s.device}.`),
                row({gap: 14},
                    icon("sound", {size: 20}),
                    slider(s.volume, 0, 100, setVolume, {grow: 1, wheelStep: 5}),
                    text(`${s.volume}%`, {width: 44, textAlign: "end", weight: 1})), true),
        setting("Mute", caption("Silences all sound. The volume stays as it is."), toggle(s.muted, setMuted)),
    ];
}

/* ---- languages ---- */

/* Languages to add, by BCP 47 language tag: each one's own name, and its name in English (what the
   search also finds). */
const LANGUAGES: readonly {tag: string; own: string; english: string}[] = [
    {tag: "af", own: "Afrikaans", english: "Afrikaans"},
    {tag: "am", own: "አማርኛ", english: "Amharic"},
    {tag: "ar", own: "العربية", english: "Arabic"},
    {tag: "az", own: "azərbaycan", english: "Azerbaijani"},
    {tag: "be", own: "беларуская", english: "Belarusian"},
    {tag: "bg", own: "български", english: "Bulgarian"},
    {tag: "bn", own: "বাংলা", english: "Bangla"},
    {tag: "br", own: "brezhoneg", english: "Breton"},
    {tag: "bs", own: "bosanski", english: "Bosnian"},
    {tag: "ca", own: "català", english: "Catalan"},
    {tag: "cs", own: "čeština", english: "Czech"},
    {tag: "cy", own: "Cymraeg", english: "Welsh"},
    {tag: "da", own: "dansk", english: "Danish"},
    {tag: "de", own: "Deutsch", english: "German"},
    {tag: "de-AT", own: "Deutsch (Österreich)", english: "German (Austria)"},
    {tag: "de-CH", own: "Deutsch (Schweiz)", english: "German (Switzerland)"},
    {tag: "de-DE", own: "Deutsch (Deutschland)", english: "German (Germany)"},
    {tag: "el", own: "Ελληνικά", english: "Greek"},
    {tag: "en", own: "English", english: "English"},
    {tag: "en-AU", own: "English (Australia)", english: "English (Australia)"},
    {tag: "en-CA", own: "English (Canada)", english: "English (Canada)"},
    {tag: "en-GB", own: "English (United Kingdom)", english: "English (United Kingdom)"},
    {tag: "en-IN", own: "English (India)", english: "English (India)"},
    {tag: "en-US", own: "English (United States)", english: "English (United States)"},
    {tag: "eo", own: "esperanto", english: "Esperanto"},
    {tag: "es", own: "español", english: "Spanish"},
    {tag: "es-419", own: "español (Latinoamérica)", english: "Spanish (Latin America)"},
    {tag: "es-ES", own: "español (España)", english: "Spanish (Spain)"},
    {tag: "es-MX", own: "español (México)", english: "Spanish (Mexico)"},
    {tag: "et", own: "eesti", english: "Estonian"},
    {tag: "eu", own: "euskara", english: "Basque"},
    {tag: "fa", own: "فارسی", english: "Persian"},
    {tag: "fi", own: "suomi", english: "Finnish"},
    {tag: "fil", own: "Filipino", english: "Filipino"},
    {tag: "fr", own: "français", english: "French"},
    {tag: "fr-CA", own: "français (Canada)", english: "French (Canada)"},
    {tag: "fr-FR", own: "français (France)", english: "French (France)"},
    {tag: "fy", own: "Frysk", english: "Western Frisian"},
    {tag: "ga", own: "Gaeilge", english: "Irish"},
    {tag: "gd", own: "Gàidhlig", english: "Scottish Gaelic"},
    {tag: "gl", own: "galego", english: "Galician"},
    {tag: "gu", own: "ગુજરાતી", english: "Gujarati"},
    {tag: "ha", own: "Hausa", english: "Hausa"},
    {tag: "he", own: "עברית", english: "Hebrew"},
    {tag: "hi", own: "हिन्दी", english: "Hindi"},
    {tag: "hr", own: "hrvatski", english: "Croatian"},
    {tag: "hu", own: "magyar", english: "Hungarian"},
    {tag: "hy", own: "հայերեն", english: "Armenian"},
    {tag: "id", own: "Bahasa Indonesia", english: "Indonesian"},
    {tag: "ig", own: "Igbo", english: "Igbo"},
    {tag: "is", own: "íslenska", english: "Icelandic"},
    {tag: "it", own: "italiano", english: "Italian"},
    {tag: "ja", own: "日本語", english: "Japanese"},
    {tag: "ka", own: "ქართული", english: "Georgian"},
    {tag: "kk", own: "қазақ тілі", english: "Kazakh"},
    {tag: "km", own: "ខ្មែរ", english: "Khmer"},
    {tag: "kn", own: "ಕನ್ನಡ", english: "Kannada"},
    {tag: "ko", own: "한국어", english: "Korean"},
    {tag: "ku", own: "kurdî", english: "Kurdish"},
    {tag: "ky", own: "кыргызча", english: "Kyrgyz"},
    {tag: "la", own: "Latina", english: "Latin"},
    {tag: "lb", own: "Lëtzebuergesch", english: "Luxembourgish"},
    {tag: "lo", own: "ລາວ", english: "Lao"},
    {tag: "lt", own: "lietuvių", english: "Lithuanian"},
    {tag: "lv", own: "latviešu", english: "Latvian"},
    {tag: "mk", own: "македонски", english: "Macedonian"},
    {tag: "ml", own: "മലയാളം", english: "Malayalam"},
    {tag: "mn", own: "монгол", english: "Mongolian"},
    {tag: "mr", own: "मराठी", english: "Marathi"},
    {tag: "ms", own: "Bahasa Melayu", english: "Malay"},
    {tag: "mt", own: "Malti", english: "Maltese"},
    {tag: "my", own: "မြန်မာ", english: "Burmese"},
    {tag: "nb", own: "norsk bokmål", english: "Norwegian Bokmål"},
    {tag: "ne", own: "नेपाली", english: "Nepali"},
    {tag: "nl", own: "Nederlands", english: "Dutch"},
    {tag: "nn", own: "norsk nynorsk", english: "Norwegian Nynorsk"},
    {tag: "oc", own: "occitan", english: "Occitan"},
    {tag: "pa", own: "ਪੰਜਾਬੀ", english: "Punjabi"},
    {tag: "pl", own: "polski", english: "Polish"},
    {tag: "ps", own: "پښتو", english: "Pashto"},
    {tag: "pt", own: "português", english: "Portuguese"},
    {tag: "pt-BR", own: "português (Brasil)", english: "Portuguese (Brazil)"},
    {tag: "pt-PT", own: "português (Portugal)", english: "Portuguese (Portugal)"},
    {tag: "ro", own: "română", english: "Romanian"},
    {tag: "ru", own: "русский", english: "Russian"},
    {tag: "si", own: "සිංහල", english: "Sinhala"},
    {tag: "sk", own: "slovenčina", english: "Slovak"},
    {tag: "sl", own: "slovenščina", english: "Slovenian"},
    {tag: "so", own: "Soomaali", english: "Somali"},
    {tag: "sq", own: "shqip", english: "Albanian"},
    {tag: "sr", own: "српски", english: "Serbian"},
    {tag: "sv", own: "svenska", english: "Swedish"},
    {tag: "sw", own: "Kiswahili", english: "Swahili"},
    {tag: "ta", own: "தமிழ்", english: "Tamil"},
    {tag: "te", own: "తెలుగు", english: "Telugu"},
    {tag: "tg", own: "тоҷикӣ", english: "Tajik"},
    {tag: "th", own: "ไทย", english: "Thai"},
    {tag: "tk", own: "türkmen dili", english: "Turkmen"},
    {tag: "tr", own: "Türkçe", english: "Turkish"},
    {tag: "tt", own: "татар", english: "Tatar"},
    {tag: "uk", own: "українська", english: "Ukrainian"},
    {tag: "ur", own: "اردو", english: "Urdu"},
    {tag: "uz", own: "oʻzbek", english: "Uzbek"},
    {tag: "vi", own: "Tiếng Việt", english: "Vietnamese"},
    {tag: "xh", own: "isiXhosa", english: "Xhosa"},
    {tag: "yo", own: "Èdè Yorùbá", english: "Yoruba"},
    {tag: "yue", own: "粵語", english: "Cantonese"},
    {tag: "zh", own: "中文", english: "Chinese"},
    {tag: "zh-CN", own: "中文（中国）", english: "Chinese (China)"},
    {tag: "zh-HK", own: "中文（香港）", english: "Chinese (Hong Kong)"},
    {tag: "zh-TW", own: "中文（台灣）", english: "Chinese (Taiwan)"},
    {tag: "zu", own: "isiZulu", english: "Zulu"},
];
const known = (tag: string) => LANGUAGES.find(l => l.tag.toLowerCase() === tag.toLowerCase());

let search = "";
let searchFocused = false;

/* Ask for the new list, then show what the system took. */
function setLanguages(next: readonly string[]) {
    if (!desktop.setLanguages(next)) refused("that list of languages");
    else {
        status = "";
        statusError = false;
    }
    languages = desktop.languages() ?? languages;
}

function moveLanguage(i: number, by: number) {
    const next = [...languages];
    [next[i], next[i + by]] = [next[i + by], next[i]];
    setLanguages(next);
}

function languageRow(tag: string, i: number): Child {
    const l = known(tag), last = i === languages.length - 1;
    return [
        i > 0 && divider(),
        row({gap: 8, key: tag},
            text(String(i + 1), {width: 22, colour: theme.textDim}),
            column({grow: 1, gap: 2},
                   text(l?.own ?? tag, {weight: 1}),
                   caption(l ? `${l.english} · ${tag}` : tag)),
            button("", () => moveLanguage(i, -1), {icon: "chevron-up", variant: "ghost", compact: true, disabled: i === 0}),
            button("", () => moveLanguage(i, 1), {icon: "chevron-down", variant: "ghost", compact: true, disabled: last}),
            button("", () => setLanguages(languages.filter(t => t !== tag)),
                   {icon: "close", variant: "ghost", compact: true, disabled: languages.length === 1})),
    ];
}

function languagePage(): Child {
    const q = search.trim().toLowerCase();
    const choices = LANGUAGES.filter(l => !languages.some(t => t.toLowerCase() === l.tag.toLowerCase()) &&
        (!q || l.own.toLowerCase().includes(q) || l.english.toLowerCase().includes(q) || l.tag.toLowerCase().startsWith(q)));
    return [
        card({gap: 12},
             column({gap: 4}, heading("Preferred languages"),
                    caption("Programs show text in the first of these languages they have, and web pages are asked " +
                            "for them in this order. The first is the one you want most.")),
             languages.map(languageRow)),
        card({gap: 12},
             column({gap: 4}, heading("Add a language"), caption("It is added at the end of the list.")),
             textField(search, v => { search = v; }, {placeholder: "Search languages", onFocus: on => { searchFocused = on; }}),
             choices.length
                 ? scroll({maxHeight: 260, gap: 2},
                          choices.map(l => listItem(l.own === l.english ? l.own : `${l.own} — ${l.english}`, false,
                                                    () => setLanguages([...languages, l.tag]), {key: l.tag})))
                 : caption(q ? "No language matches." : "Every language listed is already chosen.")),
    ];
}

function view(): Child {
    const page = section === "display" ? displayPage() : section === "mouse" ? mousePage()
               : section === "sound" ? soundPage() : languagePage();
    const label = SECTIONS.find(s => s.id === section)!.label;
    return row({gap: 0, align: "stretch"},
        column({width: 216, padding: [22, 12], gap: 4, background: theme.sidebar},
               row({gap: 10, padding: [0, 10, 18, 10]}, icon("settings", {size: 22, colour: theme.accent}),
                   title("Settings", {size: 19})),
               SECTIONS.map(s => listItem(s.label, s.id === section, () => { section = s.id; status = ""; },
                                          {icon: s.icon, key: s.id}))),
        column({grow: 1, gap: 0},
               scroll({grow: 1, padding: [26, 30], gap: 14}, title(label), page),
               row({padding: [12, 30], background: theme.sidebar},
                   caption(status || "Changes apply at once and are kept after a restart.",
                           {colour: statusError ? theme.warning : theme.textDim}))));
}

const app: App = run({
    title: "Settings", width: 880, height: 640, view,
    onKey: e => {
        if (e.key === "Escape") { app.close(); return true; }
        if ((e.key === "ArrowUp" || e.key === "ArrowDown") && !searchFocused) {
            const i = SECTIONS.findIndex(s => s.id === section);
            section = SECTIONS[(i + (e.key === "ArrowDown" ? 1 : SECTIONS.length - 1)) % SECTIONS.length].id;
            status = "";
            return true;
        }
    },
});

/* What the window shows, read again; whether any of it changed. */
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
function refresh(): boolean {
    const now = desktop.get();
    if (!now) return false;
    const nextScreen = desktop.modes(), nextSound = desktop.sound(), nextLanguages = desktop.languages() ?? languages;
    let changed = !same(nextScreen, screen);
    if (changed && screen?.trialMs && nextScreen && !nextScreen.trialMs && tried && !nextScreen.refused &&
        !sameMode(nextScreen.wanted, tried)) {
        status = "The mode was not kept; the one before is back.";
        statusError = false;
        tried = null;
    }
    screen = nextScreen;
    if (!same(now, settings)) {
        settings = now;
        changed = true;
    }
    if (!dragging && !same(nextSound, sound)) {
        sound = nextSound;
        changed = true;
    }
    if (!same(nextLanguages, languages)) {
        languages = nextLanguages;
        changed = true;
    }
    return changed;
}

/* The user's settings changed, here or elsewhere (the taskbar's volume, another program): the system
   says so at once. */
const unwatch = desktop.watch(() => { if (!app.closed && refresh()) app.invalidate(); });
/* What is the system's and no setting: a sound device coming or going, smooth gradients becoming
   available or not, the desktop's new size, the screen handed back, or the monitor's modes changing
   (a trial counting down or ending, a monitor plugged in). */
const poll = setInterval(() => {
    if (app.closed) {
        clearInterval(poll);
        unwatch();
        return;
    }
    if (refresh()) app.invalidate();
}, 500);
