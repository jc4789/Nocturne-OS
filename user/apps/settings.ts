/* Settings: display, mouse and sound, all by mouse. Every change applies at
 * once and is kept in C:\Users\<name>\AppData\Settings\desktop.conf; at
 * startup init runs "js C:\Nocturne\System\settings.js --apply", which puts the
 * saved settings back before the desktop is used. Nothing here needs a
 * terminal or the boot command line. */
import { args, createFolder, exists, exit, getSetting, readText, resolvePath, writeText } from "nocturne";
import {
    button, caption, card, column, desktop, heading, listItem, row, run, scroll, segmented, slider, spacer, text,
    theme, title, toggle, icon, type App, type Child, type DesktopSettings, type IconName, type ScreenMode,
    type ScreenModeChoice, type ScreenModes,
} from "nocturne/ui";

/* The user's folder is the launch setting USERFOLDER ("C:\" without it, as libc's n_user_folder). */
const FOLDER = resolvePath(`${getSetting("USERFOLDER") ?? "C:\\"}\\AppData\\Settings`);
const PATH = FOLDER + "\\desktop.conf";
const POINTER_Y_KEYS = ["auto", "normal", "invert"] as const;

type Section = "display" | "mouse" | "sound";
const SECTIONS: {id: Section; label: string; icon: IconName}[] = [
    {id: "display", label: "Display", icon: "display"},
    {id: "mouse", label: "Mouse", icon: "mouse"},
    {id: "sound", label: "Sound", icon: "sound"},
];

/* modes: the user's display mode for each monitor ("mode.<monitor>=<width>x<height>@<mHz>"). */
type Saved = {refresh?: number; pointerSpeed?: number; pointerY?: number; deepColour?: boolean; volume?: number;
              modes: Map<string, ScreenModeChoice>};

/* ---- the saved file ---- */
function loadSaved(): Saved | null {
    let content: string;
    try { content = readText(PATH); } catch { return null; }
    const saved: Saved = {modes: new Map()};
    for (const raw of content.split("\n")) {
        const line = raw.trim();
        const eq = line.indexOf("=");
        if (line.startsWith("#") || eq < 0) continue;
        const key = line.slice(0, eq), value = line.slice(eq + 1).trim(), n = Number.parseInt(value, 10);
        const mode = /^(\d{1,5})x(\d{1,5})@(\d{1,7})$/.exec(value);
        if (key === "refresh" && n >= 0 && n <= 1000) saved.refresh = n;
        else if (key === "pointer_speed" && n >= -2 && n <= 2) saved.pointerSpeed = n;
        else if (key === "pointer_y" && POINTER_Y_KEYS.includes(value as typeof POINTER_Y_KEYS[number]))
            saved.pointerY = POINTER_Y_KEYS.indexOf(value as typeof POINTER_Y_KEYS[number]);
        else if (key === "deep_colour") saved.deepColour = value === "on";
        else if (key === "volume" && n >= 0 && n <= 100) saved.volume = n;
        else if (key.startsWith("mode.") && key.length > 5 && mode && Number(mode[1]) && Number(mode[2]) && Number(mode[3]))
            saved.modes.set(key.slice(5), {width: Number(mode[1]), height: Number(mode[2]),
                                           refreshMillihertz: Number(mode[3])});
    }
    return saved;
}

function save(s: DesktopSettings, volume: number | null, modes: Map<string, ScreenModeChoice>): string | null {
    let content = "# Nocturne desktop settings, written by Settings\n" +
        `refresh=${s.refreshRate}\npointer_speed=${s.pointerSpeed}\npointer_y=${POINTER_Y_KEYS[s.pointerY % 3]}\n` +
        `deep_colour=${s.deepColour ? "on" : "off"}\n`;
    if (volume !== null) content += `volume=${volume}\n`;
    for (const [monitor, m] of modes) content += `mode.${monitor}=${m.width}x${m.height}@${m.refreshMillihertz}\n`;
    try {
        if (!exists(FOLDER)) createFolder(FOLDER, {recursive: true});
        writeText(PATH, content);
        return null;
    } catch (e) {
        return (e as Error).message;
    }
}

/* At startup: the saved settings over the system's own. A monitor's mode is put back whenever a
   display driver drives that monitor, which may be later. */
function applySaved(): number {
    const now = desktop.get();
    if (!now) return 1;
    const saved = loadSaved();
    if (!saved) return 0;
    let ok = desktop.set({
        refreshRate: saved.refresh ?? now.refreshRate,
        pointerSpeed: saved.pointerSpeed ?? now.pointerSpeed,
        pointerY: saved.pointerY ?? now.pointerY,
        deepColour: (saved.deepColour ?? now.deepColour) && now.deepColourAvailable,
    });
    if (saved.volume !== undefined) desktop.setVolume(saved.volume);
    for (const [monitor, m] of saved.modes) ok = desktop.savedMode(monitor, m) && ok;
    return ok ? 0 : 1;
}

if (args[0] === "--apply") exit(applySaved());

/* ---- the window ---- */
let current = desktop.get();
if (!current) {
    console.error("settings: the desktop is not running");
    exit(1);
}
let settings: DesktopSettings = current;
let screen: ScreenModes | null = desktop.modes();
const savedModes = loadSaved()?.modes ?? new Map<string, ScreenModeChoice>();
let section: Section = args[0] === "mouse" ? "mouse" : args[0] === "sound" ? "sound" : "display";
let status = "", statusError = false;
let dragging = false;
let tried: ScreenModeChoice | null = null; /* the mode on trial from this window */

/* Apply, read back what the system took, save. */
function change(next: Partial<DesktopSettings>) {
    const wanted = {...settings, ...next};
    if (!desktop.set(wanted)) {
        status = "The system did not accept that setting.";
        statusError = true;
    } else {
        store();
    }
    settings = desktop.get() ?? settings;
}

function store() {
    const error = save(settings, settings.volume, savedModes);
    statusError = error !== null;
    status = error ? `Applied, but not saved: ${error}` : "Applied and saved. It stays this way after a restart.";
}

function setVolume(v: number, done: boolean) {
    const now = desktop.setVolume(Math.max(0, Math.min(100, Math.round(v))));
    if (now !== null) settings = {...settings, volume: now};
    dragging = !done;
    if (done) store();
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
const modeName = (m: ScreenModeChoice, modes: readonly ScreenMode[]) => `${sizeName(m)} at ${rateName(m, modes)}`;

function tryMode(m: ScreenModeChoice) {
    if (sameMode(m, screen?.wanted ?? null)) return;
    if (desktop.tryMode(m)) {
        tried = m;
        status = "";
        statusError = false;
    } else {
        status = "The system did not accept that mode.";
        statusError = true;
    }
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

function keepMode() {
    const monitor = screen?.monitor ?? "";
    if (!desktop.keepMode()) {
        status = "The mode had already gone back.";
        statusError = true;
    } else {
        screen = desktop.modes() ?? screen;
        if (screen?.wanted && monitor) savedModes.set(monitor, screen.wanted);
        store();
    }
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

function modesCard(): Child {
    const s = screen;
    if (!s || !s.modes.length)
        return setting("Resolution and refresh rate",
                       caption(`${settings.screenWidth} × ${settings.screenHeight}, set by the firmware when the ` +
                               "computer starts. A display driver for this screen lists the monitor's other modes."),
                       null, true);
    const modes = s.modes;
    const current = s.wanted ?? s.shown ?? modes.find(m => m.preferred) ?? modes[0];
    const sizes: ScreenMode[] = [];
    for (const m of modes) if (!sizes.some(o => sameSize(o, m))) sizes.push(m);
    sizes.sort((a, b) => b.width * b.height - a.width * a.height || b.width - a.width);
    const preferred = modes.find(m => m.preferred);
    const rates = modes.filter(m => sameSize(m, current)).sort((a, b) => b.refreshMillihertz - a.refreshMillihertz);
    const recommended = " (recommended)";
    const list = (items: Child[]) => scroll({maxHeight: 236, gap: 2}, items);
    return card({gap: 14},
        column({gap: 4},
               heading("Resolution and refresh rate"),
               caption(`${s.name || s.monitor}. The monitor's own preferred mode is marked recommended.`)),
        s.trialMs > 0 ? trialCard(s) : null,
        tried && !s.trialMs && s.refused &&
            caption(`The monitor could not show ${modeName(tried, modes)}; the mode before is back.`,
                    {colour: theme.warning}),
        row({gap: 16, align: "start"},
            column({grow: 1, gap: 6}, caption("Resolution"),
                   list(sizes.map(m => listItem(sizeName(m) + (preferred && sameSize(m, preferred) ? recommended : ""),
                                                sameSize(m, current), () => trySize(modes, m),
                                                {key: sizeName(m)})))),
            column({grow: 1, gap: 6}, caption("Refresh rate"),
                   list(rates.map(m => listItem(rateName(m, modes) + (m.preferred ? recommended : ""),
                                                sameMode(m, current), () => tryMode(m),
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

const FRAME_RATES = [30, 60, 75, 120, 144, 240, 0];
const frameRateName = (hz: number) => hz ? `${hz} Hz` : "Unlimited";

function displayPage(): Child {
    const s = settings;
    const depth = s.deepColourAvailable
        ? "Draws in 10 bits per channel, dithered for this 8 bpc screen: smoother gradients and 10-bit video."
        : s.nativeBitsPerChannel >= 10 ? "This screen already shows 10 bits per channel."
        : "Not available for this screen's pixel format.";
    const about = "How often the desktop is redrawn when the screen gives no vertical blanks, as the firmware's " +
                  "screen does not. A display driver's vertical blanks pace it at the monitor's refresh rate instead.";
    return [
        modesCard(),
        setting("10-bit colour", caption(depth),
                toggle(s.deepColour, on => change({deepColour: on}), {disabled: !s.deepColourAvailable})),
        setting("Desktop frame rate",
                caption(FRAME_RATES.includes(s.refreshRate) ? about : `Now ${s.refreshRate} Hz. ${about}`),
                segmented(FRAME_RATES.map(hz => ({label: frameRateName(hz), value: hz})), s.refreshRate,
                          hz => change({refreshRate: hz})), true),
        card({gap: 12},
             heading("Screen"),
             info("Size", `${s.screenWidth} × ${s.screenHeight}` +
                          (screen?.shown ? ` at ${rateName(screen.shown, screen.modes)}` : "")),
             info("Colour depth", `Screen ${s.nativeBitsPerChannel} bpc · desktop ${s.bitsPerChannel} bpc ` +
                                  `(${s.bitsPerChannel * 3}-bit colour)`)),
    ];
}

function info(name: string, value: string): Child {
    return row({gap: 12, align: "start"}, text(name, {width: 110, colour: theme.textDim}), text(value, {grow: 1}));
}

function mousePage(): Child {
    const s = settings;
    return [
        setting("Pointer speed", caption("How far the pointer moves for the same mouse movement."),
                segmented([{label: "Slowest", value: -2}, {label: "Slow", value: -1}, {label: "Normal", value: 0},
                           {label: "Fast", value: 1}, {label: "Fastest", value: 2}], s.pointerSpeed,
                          v => change({pointerSpeed: v})), true),
        setting("Up and down",
                column({gap: 2},
                       caption(s.pointerYDetected ? "Automatic: inverted, because this host reports it upside down."
                                                  : "Automatic: as the mouse reports it."),
                       caption("If the pointer moves the wrong way, choose Inverted.")),
                segmented([{label: "Automatic", value: 0}, {label: "Normal", value: 1}, {label: "Inverted", value: 2}],
                          s.pointerY, v => change({pointerY: v})), true),
    ];
}

function soundPage(): Child {
    const volume = settings.volume;
    if (volume === null) return setting("Volume", caption("No sound device was found."), null, true);
    return setting("Volume", caption("Drag or scroll the slider; the taskbar speaker does the same."),
                   row({gap: 14},
                       icon("sound", {size: 20}),
                       slider(volume, 0, 100, setVolume, {grow: 1, wheelStep: 5}),
                       text(`${volume}%`, {width: 44, textAlign: "end", weight: 1})), true);
}

function view(): Child {
    const page = section === "display" ? displayPage() : section === "mouse" ? mousePage() : soundPage();
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
        if (e.key === "ArrowUp" || e.key === "ArrowDown") {
            const i = SECTIONS.findIndex(s => s.id === section);
            section = SECTIONS[(i + (e.key === "ArrowDown" ? 1 : SECTIONS.length - 1)) % SECTIONS.length].id;
            status = "";
            return true;
        }
    },
});

/* The taskbar speaker, a depth switch the compositor just made, the desktop's new size, or the
   monitor's modes changing (a trial counting down or ending, a monitor plugged in). */
const modesKey = (s: ScreenModes | null) => JSON.stringify(s);
const poll = setInterval(() => {
    if (app.closed) { clearInterval(poll); return; }
    const now = desktop.get();
    if (!now) return;
    const nextScreen = desktop.modes();
    let changed = modesKey(nextScreen) !== modesKey(screen);
    if (changed && screen?.trialMs && nextScreen && !nextScreen.trialMs && tried && !nextScreen.refused &&
        !sameMode(nextScreen.wanted, tried)) {
        status = "The mode was not kept; the one before is back.";
        statusError = false;
        tried = null;
    }
    screen = nextScreen;
    if ((now.volume !== settings.volume && !dragging) || now.bitsPerChannel !== settings.bitsPerChannel ||
        now.screenWidth !== settings.screenWidth || now.screenHeight !== settings.screenHeight) {
        settings = {...settings, bitsPerChannel: now.bitsPerChannel, screenWidth: now.screenWidth,
                    screenHeight: now.screenHeight, volume: dragging ? settings.volume : now.volume};
        changed = true;
    }
    if (changed) app.invalidate();
}, 500);
