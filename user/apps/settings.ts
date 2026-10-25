/* Settings: display, mouse and sound, all by mouse. Every change applies at
 * once and is kept in C:\Users\<name>\AppData\Settings\desktop.conf; at
 * startup init runs "js C:\Nocturne\System\settings.js --apply", which puts the
 * saved settings back before the desktop is used. Nothing here needs a
 * terminal or the boot command line. Each page shows only what the system can
 * do on this computer now, and says why when it cannot. */
import { args, createFolder, exists, exit, getSetting, readText, resolvePath, writeText } from "nocturne";
import {
    button, caption, card, column, desktop, heading, listItem, row, run, scroll, segmented, slider, text, theme, title,
    toggle, icon, type App, type Child, type DesktopSettings, type IconName, type ScreenMode, type ScreenModeChoice,
    type ScreenModes, type ScreenSource, type SoundOutput,
} from "nocturne/ui";

/* The user's folder is the launch setting USERFOLDER ("C:\" without it, as libc's n_user_folder). */
const FOLDER = resolvePath(`${getSetting("USERFOLDER") ?? "C:\\"}\\AppData\\Settings`);
const PATH = FOLDER + "\\desktop.conf";

type Section = "display" | "mouse" | "sound";
const SECTIONS: {id: Section; label: string; icon: IconName}[] = [
    {id: "display", label: "Display", icon: "display"},
    {id: "mouse", label: "Mouse", icon: "mouse"},
    {id: "sound", label: "Sound", icon: "sound"},
];

/* modes: the user's display mode for each monitor ("mode.<monitor>=<width>x<height>@<mHz>"). Keys
   an older Settings wrote that the system no longer has (refresh, pointer_y) are left out;
   deep_colour is what smooth_gradients was called. */
type Saved = {pointerSpeed?: number; smoothGradients?: boolean; volume?: number; muted?: boolean;
              modes: Map<string, ScreenModeChoice>};

/* ---- the saved file ---- */
function loadSaved(): Saved | null {
    let content: string;
    try { content = readText(PATH); } catch { return null; }
    const saved: Saved = {modes: new Map()};
    let oldSmooth: boolean | undefined;
    for (const raw of content.split("\n")) {
        const line = raw.trim();
        const eq = line.indexOf("=");
        if (line.startsWith("#") || eq < 0) continue;
        const key = line.slice(0, eq), value = line.slice(eq + 1).trim(), n = Number.parseInt(value, 10);
        const mode = /^(\d{1,5})x(\d{1,5})@(\d{1,7})$/.exec(value);
        if (key === "pointer_speed" && n >= -2 && n <= 2) saved.pointerSpeed = n;
        else if (key === "smooth_gradients") saved.smoothGradients = value === "on";
        else if (key === "deep_colour") oldSmooth = value === "on";
        else if (key === "volume" && n >= 0 && n <= 100) saved.volume = n;
        else if (key === "muted") saved.muted = value === "on";
        else if (key.startsWith("mode.") && key.length > 5 && mode && Number(mode[1]) && Number(mode[2]) && Number(mode[3]))
            saved.modes.set(key.slice(5), {width: Number(mode[1]), height: Number(mode[2]),
                                           refreshMillihertz: Number(mode[3])});
    }
    saved.smoothGradients ??= oldSmooth;
    return saved;
}

function save(s: DesktopSettings, sound: SoundOutput | null, modes: Map<string, ScreenModeChoice>): string | null {
    let content = "# Nocturne desktop settings, written by Settings\n" +
        `pointer_speed=${s.pointerSpeed}\nsmooth_gradients=${s.smoothGradients ? "on" : "off"}\n`;
    if (sound) content += `volume=${sound.volume}\nmuted=${sound.muted ? "on" : "off"}\n`;
    for (const [monitor, m] of modes) content += `mode.${monitor}=${m.width}x${m.height}@${m.refreshMillihertz}\n`;
    try {
        if (!exists(FOLDER)) createFolder(FOLDER, {recursive: true});
        writeText(PATH, content);
        return null;
    } catch (e) {
        return (e as Error).message;
    }
}

/* At startup: the saved settings over the system's own. Smooth gradients stay wished for while the
   screen cannot take them, and come on when it can. A monitor's mode is put back whenever a display
   driver drives that monitor, which may be later. The volume is put back even without a sound
   device, for when one plays the sound (a Remote Desktop viewer). */
function applySaved(): number {
    const now = desktop.get();
    if (!now) return 1;
    const saved = loadSaved();
    if (!saved) return 0;
    let ok = desktop.set({
        pointerSpeed: saved.pointerSpeed ?? now.pointerSpeed,
        smoothGradients: saved.smoothGradients ?? now.smoothGradients,
    });
    /* The volume first: setting it unmutes. */
    if (saved.volume !== undefined) ok = desktop.setVolume(saved.volume) !== null && ok;
    if (saved.muted !== undefined) ok = desktop.setMuted(saved.muted) !== null && ok;
    for (const [monitor, m] of saved.modes) ok = desktop.savedMode(monitor, m) && ok;
    return ok ? 0 : 1;
}

if (args[0] === "--apply") exit(applySaved());

/* ---- the window ---- */
const current = desktop.get();
if (!current) {
    console.error("settings: the desktop is not running");
    exit(1);
}
let settings: DesktopSettings = current;
let screen: ScreenModes | null = desktop.modes();
let sound: SoundOutput | null = desktop.sound();
const savedModes = loadSaved()?.modes ?? new Map<string, ScreenModeChoice>();
let section: Section = args[0] === "mouse" ? "mouse" : args[0] === "sound" ? "sound" : "display";
let status = "", statusError = false;
let dragging = false;
let tried: ScreenModeChoice | null = null; /* the mode on trial from this window */

/* Apply, read back what the system took, save. */
function change(next: Partial<Pick<DesktopSettings, "pointerSpeed" | "smoothGradients">>) {
    const ok = desktop.set({...settings, ...next});
    settings = desktop.get() ?? settings;
    if (ok) store();
    else {
        status = "The system did not accept that setting.";
        statusError = true;
    }
}

function store() {
    const error = save(settings, sound, savedModes);
    statusError = error !== null;
    status = error ? `Applied, but not saved: ${error}` : "Applied and saved. It stays this way after a restart.";
}

function setVolume(v: number, done: boolean) {
    sound = desktop.setVolume(Math.max(0, Math.min(100, Math.round(v)))) ?? sound;
    dragging = !done;
    if (done) store();
}

function setMuted(on: boolean) {
    sound = desktop.setMuted(on) ?? sound;
    store();
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
    const saved = s?.monitor ? savedModes.get(s.monitor) : undefined;
    const why = source === "driver" && listed ? "The monitor's own preferred mode is marked recommended."
              : source === "handedBack" && listed ? `${REASONS.handedBack} Pick a resolution to try again.`
              : REASONS[source];
    const words = column({gap: 4},
        heading(s?.name || "Screen"),
        text(shown),
        caption(why),
        !listed && saved && source !== "remote" &&
            caption(`Saved for this monitor: ${modeName(saved, [])}, used when a display driver runs.`));
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

/* What changed elsewhere: the taskbar speaker, a sound device coming or going, smooth gradients
   becoming available or not, the desktop's new size, the screen handed back, or the monitor's modes
   changing (a trial counting down or ending, a monitor plugged in). */
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
const poll = setInterval(() => {
    if (app.closed) { clearInterval(poll); return; }
    const now = desktop.get();
    if (!now) return;
    const nextScreen = desktop.modes(), nextSound = desktop.sound();
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
    if (changed) app.invalidate();
}, 500);
