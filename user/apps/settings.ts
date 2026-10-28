/* Settings: display, mouse and sound, all by mouse. The window holds no value and no file of its
 * own, as the taskbar holds none: each change asked for here goes to the part of the system that
 * applies it (the desktop, the display modes, the sound), which applies it at once, keeps it in
 * its own document in the user's folder and puts it back at every start. What those parts report
 * is watched, so a change made elsewhere (the taskbar's volume, another program, another Settings
 * window) shows here at once. Nothing here needs a terminal or the boot command line. Each page
 * shows only what the system can do on this computer now, and says why when it cannot. */
import { args, exit } from "nocturne";
import {
    button, caption, card, column, desktop, heading, listItem, row, run, scroll, segmented, slider, text, theme, title,
    toggle, icon, type App, type Child, type DesktopSettings, type IconName, type ScreenMode, type ScreenModeChoice,
    type ScreenModes, type ScreenSource, type SoundOutput, type SystemChange,
} from "nocturne/ui";

type Section = "display" | "mouse" | "sound";
const SECTIONS: {id: Section; label: string; icon: IconName}[] = [
    {id: "display", label: "Display", icon: "display"},
    {id: "mouse", label: "Mouse", icon: "mouse"},
    {id: "sound", label: "Sound", icon: "sound"},
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
let section: Section = SECTIONS.find(s => s.id === args[0])?.id ?? "display";
let status = "", statusError = false;
let dragging = false; /* the volume's slider is held: the sound's changes are this window's own */
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
    countDown();
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
    countDown();
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
    countDown();
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

/* ---- what the system reports, read again when it changes ---- */
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

/* The desktop and the monitor's modes, read again; whether either changed. */
function rereadScreen(): boolean {
    const now = desktop.get();
    if (!now) return false;
    const next = desktop.modes();
    let changed = false;
    if (!same(now, settings)) {
        settings = now;
        changed = true;
    }
    if (!same(next, screen)) {
        /* The trial from this window ended with neither Keep nor Go back, and not refused: its time
           ran out. */
        if (screen?.trialMs && next && !next.trialMs && tried && !next.refused && !sameMode(next.wanted, tried)) {
            status = "The mode was not kept; the one before is back.";
            statusError = false;
            tried = null;
        }
        screen = next;
        changed = true;
    }
    return changed;
}

/* While a mode is on trial, the seconds left before it goes back are shown. Its counting down is
   no change the system reports (its beginning and end are), so the modes are read again just as
   each second shown runs out, and only then. */
let countdown = 0;
function countDown() {
    clearTimeout(countdown);
    countdown = 0;
    const left = screen?.trialMs ?? 0;
    if (app.closed || left <= 0) return;
    countdown = setTimeout(() => {
        countdown = 0;
        if (rereadScreen()) app.invalidate();
        countDown();
    }, (left - 1) % 1000 + 1);
}

/* What changed, whoever changed it: this window, the taskbar, another program or Settings window.
   While the volume's slider is held, the sound's changes are this window's own, and what the
   slider shows stays as the user moves it. */
function reread(changed: readonly SystemChange[]) {
    if (app.closed) {
        unwatch();
        return;
    }
    let shown = false;
    if (changed.includes("screen")) {
        shown = rereadScreen();
        countDown();
    }
    if (changed.includes("sound") && !dragging) {
        const next = desktop.sound();
        if (!same(next, sound)) {
            sound = next;
            shown = true;
        }
    }
    if (shown) app.invalidate();
}

const unwatch = desktop.watch(reread);
/* What changed between the first reading and the watch's beginning. */
reread(["screen", "sound"]);
