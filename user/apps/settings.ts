/* Settings: display, mouse and sound, all by mouse. Every change applies at
 * once and is kept in C:\Users\<name>\AppData\Settings\desktop.conf; at
 * startup init runs "js C:\Nocturne\System\settings.js --apply", which puts the
 * saved settings back before the desktop is used. Nothing here needs a
 * terminal or the boot command line. */
import { args, createFolder, exists, exit, getSetting, readText, resolvePath, writeText } from "nocturne";
import {
    caption, card, column, desktop, heading, listItem, row, run, scroll, segmented, slider, spacer, text, theme, title,
    toggle, icon, type App, type Child, type DesktopSettings, type IconName,
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

type Saved = {refresh?: number; pointerSpeed?: number; pointerY?: number; deepColour?: boolean; volume?: number};

/* ---- the saved file ---- */
function loadSaved(): Saved | null {
    let content: string;
    try { content = readText(PATH); } catch { return null; }
    const saved: Saved = {};
    for (const raw of content.split("\n")) {
        const line = raw.trim();
        const eq = line.indexOf("=");
        if (line.startsWith("#") || eq < 0) continue;
        const key = line.slice(0, eq), value = line.slice(eq + 1).trim(), n = Number.parseInt(value, 10);
        if (key === "refresh" && n >= 0 && n <= 1000) saved.refresh = n;
        else if (key === "pointer_speed" && n >= -2 && n <= 2) saved.pointerSpeed = n;
        else if (key === "pointer_y" && POINTER_Y_KEYS.includes(value as typeof POINTER_Y_KEYS[number]))
            saved.pointerY = POINTER_Y_KEYS.indexOf(value as typeof POINTER_Y_KEYS[number]);
        else if (key === "deep_colour") saved.deepColour = value === "on";
        else if (key === "volume" && n >= 0 && n <= 100) saved.volume = n;
    }
    return saved;
}

function save(s: DesktopSettings, volume: number | null): string | null {
    let content = "# Nocturne desktop settings, written by Settings\n" +
        `refresh=${s.refreshRate}\npointer_speed=${s.pointerSpeed}\npointer_y=${POINTER_Y_KEYS[s.pointerY % 3]}\n` +
        `deep_colour=${s.deepColour ? "on" : "off"}\n`;
    if (volume !== null) content += `volume=${volume}\n`;
    try {
        if (!exists(FOLDER)) createFolder(FOLDER, {recursive: true});
        writeText(PATH, content);
        return null;
    } catch (e) {
        return (e as Error).message;
    }
}

/* At startup: the saved settings over the system's own. */
function applySaved(): number {
    const now = desktop.get();
    if (!now) return 1;
    const saved = loadSaved();
    if (!saved) return 0;
    const ok = desktop.set({
        refreshRate: saved.refresh ?? now.refreshRate,
        pointerSpeed: saved.pointerSpeed ?? now.pointerSpeed,
        pointerY: saved.pointerY ?? now.pointerY,
        deepColour: (saved.deepColour ?? now.deepColour) && now.deepColourAvailable,
    });
    if (saved.volume !== undefined) desktop.setVolume(saved.volume);
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
let section: Section = args[0] === "mouse" ? "mouse" : args[0] === "sound" ? "sound" : "display";
let status = "", statusError = false;
let dragging = false;

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
    const error = save(settings, settings.volume);
    statusError = error !== null;
    status = error ? `Applied, but not saved: ${error}` : "Applied and saved. It stays this way after a restart.";
}

function setVolume(v: number, done: boolean) {
    const now = desktop.setVolume(Math.max(0, Math.min(100, Math.round(v))));
    if (now !== null) settings = {...settings, volume: now};
    dragging = !done;
    if (done) store();
}

/* A setting: its name and explanation on the left, its control on the right or below. */
function setting(name: string, about: Child, control: Child, below = false): Child {
    const words = column({gap: 4, grow: below ? 0 : 1}, heading(name), about);
    return card({gap: 14}, below ? [words, control] : row({gap: 16}, words, control));
}

const REFRESH = [30, 60, 75, 120, 144, 240, 0];
const refreshName = (hz: number) => hz ? `${hz} Hz` : "Unlimited";

function displayPage(): Child {
    const s = settings;
    const depth = s.deepColourAvailable
        ? "Draws in 10 bits per channel, dithered for this 8 bpc screen: smoother gradients and 10-bit video."
        : s.nativeBitsPerChannel >= 10 ? "This screen already shows 10 bits per channel."
        : "Not available for this screen's pixel format.";
    return [
        setting("10-bit colour", caption(depth),
                toggle(s.deepColour, on => change({deepColour: on}), {disabled: !s.deepColourAvailable})),
        setting("Refresh rate",
                caption(REFRESH.includes(s.refreshRate)
                    ? "How often the desktop is redrawn. Pick your monitor's rate."
                    : `Now ${s.refreshRate} Hz. How often the desktop is redrawn. Pick your monitor's rate.`),
                segmented(REFRESH.map(hz => ({label: refreshName(hz), value: hz})), s.refreshRate,
                          hz => change({refreshRate: hz})), true),
        card({gap: 12},
             heading("Screen"),
             info("Resolution", `${s.screenWidth} × ${s.screenHeight}, chosen by the firmware when the computer starts`),
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
    title: "Settings", width: 880, height: 580, view,
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

/* The taskbar speaker, or a depth switch the compositor just made. */
const poll = setInterval(() => {
    if (app.closed) { clearInterval(poll); return; }
    const now = desktop.get();
    if (!now) return;
    if ((now.volume !== settings.volume && !dragging) || now.bitsPerChannel !== settings.bitsPerChannel) {
        settings = {...settings, bitsPerChannel: now.bitsPerChannel, volume: dragging ? settings.volume : now.volume};
        app.invalidate();
    }
}, 500);
