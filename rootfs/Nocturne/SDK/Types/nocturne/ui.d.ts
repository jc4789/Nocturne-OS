/**
 * Type declarations for "nocturne/ui", the windows and widgets of js.
 *
 *     import { run, column, title, button } from "nocturne/ui";
 *     let count = 0;
 *     run({
 *         title: "Counter", width: 320, height: 200,
 *         view: () => column({padding: 24, gap: 16},
 *             title(`Clicked ${count} times`),
 *             button("Click", () => count++, {variant: "primary"})),
 *     });
 *
 * The view function describes the whole window as a tree of widgets. It runs
 * again after every event handler and after app.invalidate(), so the window
 * always shows the program's current state; hover, presses, focus, scrolling
 * and animations are kept by the system's interface engine (the same one C and
 * Go programs use), which lays the widgets out and paints them. Text is Noto Sans (with Noto Sans
 * CJK JP for Japanese, Chinese and Korean).
 */

/** A colour as 0xAARRGGBB (alpha 0xFF is opaque). */
export type Colour = number;

/** A widget made by one of the functions of this module. */
export interface Widget {
    readonly kind: string;
    readonly props: Readonly<Record<string, unknown>>;
    readonly children: readonly Widget[];
}

/** Children of a container: widgets, strings (shown as text), and nothing (skipped). */
export type Child = Widget | string | number | false | null | undefined | readonly Child[];

/** Space inside a box: all sides, [vertical, horizontal], or [top, right, bottom, left]. */
export type Padding = number | readonly [number, number] | readonly [number, number, number, number];

export type Align = "start" | "center" | "end" | "stretch";

/** What every widget accepts. */
export interface Common {
    /** Width in pixels, or "fill" for all the space across a column / along a row. */
    width?: number | "fill";
    /** Height in pixels, or "fill" for all the space along a column / across a row. */
    height?: number | "fill";
    minWidth?: number;
    minHeight?: number;
    /** Share of the free space along the parent (0 = only its natural size). */
    grow?: number;
    /** Own alignment across the parent (on a box, align is for its children; use self). */
    self?: Align;
    /** Own alignment across the parent (leaves only; on boxes see BoxProps.align). */
    align?: Align;
    /** Shown dimmed; takes no input. */
    disabled?: boolean;
    /** Identity that survives reordering (hover, scrolling and animations follow it). */
    key?: string | number;
    /** Scrolls the scroll box it is in until it can be seen, whenever this value changes (count up to ask again). */
    reveal?: number;
}

export interface BoxProps extends Common {
    padding?: Padding;
    /** Space between children (row default 8, column default 0). */
    gap?: number;
    /** Alignment of the children across the box (column default "stretch", row default "center"). */
    align?: Align;
    /** Placement of the children along the box when there is space left. */
    justify?: "start" | "center" | "end" | "between";
    background?: Colour;
    border?: Colour;
    radius?: number;
    /** A soft shadow under the box. */
    shadow?: boolean;
    /** Makes the whole box a button (it lights up under the pointer). */
    onClick?: () => void;
    hoverBackground?: Colour;
    pressedBackground?: Colour;
}

export interface ScrollProps extends BoxProps {
    /** The most height the area asks for; the rest scrolls. */
    maxHeight?: number;
}

export interface TextProps extends Common {
    /** Pixels per em (default 14). */
    size?: number;
    /** 0 regular, 1 bold. */
    weight?: 0 | 1;
    colour?: Colour;
    /** Break lines to fit (default true). */
    wrap?: boolean;
    textAlign?: "start" | "center" | "end";
    /** Line spacing as a multiple of the line box (default 1.25). */
    lineHeight?: number;
    /** In the monospaced face (Maple Mono). */
    mono?: boolean;
    /** One line; what does not fit is cut with "…". */
    ellipsis?: boolean;
}

export type IconName = "display" | "mouse" | "sound" | "language" | "info" | "check" | "plus" | "minus" | "close" |
    "chevron-right" | "chevron-left" | "chevron-up" | "chevron-down" | "settings" | "folder" | "clock" | "refresh" |
    "backspace" | "home" | "search";

export interface ButtonProps extends Common {
    /** primary: the accent colour; secondary (default): a framed button; ghost: no frame. */
    variant?: "primary" | "secondary" | "ghost";
    icon?: IconName;
    size?: number;
    /** Less space at the sides. */
    compact?: boolean;
    radius?: number;
    /** Instead of the variant's colours (hover and presses lighten or darken it). */
    background?: Colour;
    colour?: Colour;
}

export interface Option<T> { label: string; value: T; }

export interface SliderProps extends Common {
    /** Values snap to multiples of this (default 1; 0 for none). */
    step?: number;
    /** How much one wheel notch changes the value. */
    wheelStep?: number;
    /** Track length in pixels (default 280). */
    length?: number;
}

export interface FieldProps extends Common {
    placeholder?: string;
    /** Shows dots instead of the text. */
    secret?: boolean;
    length?: number;
    /** When this value changes: a positive number takes the keyboard (caret at the end), zero or less gives it up. */
    focus?: number;
    /** In the monospaced face (Maple Mono). */
    mono?: boolean;
    /** Getting the keyboard (by a click or by focus) selects all of its text. */
    selectOnFocus?: boolean;
    /** Enter in the field. */
    onSubmit?: (value: string) => void;
    /** The field got (true) or lost (false) the keyboard; cancelled: Escape gave it up. */
    onFocus?: (focused: boolean, cancelled: boolean) => void;
}

export interface ItemProps extends Common { icon?: IconName; }

export interface IconProps extends Common { size?: number; colour?: Colour; }

/** A key press that no focused text field took. */
export interface KeyEvent {
    /** "Escape", "Enter", "Backspace", "Tab", "ArrowUp" ... "F12", or the character typed. */
    key: string;
    shift: boolean;
    ctrl: boolean;
    alt: boolean;
}

export interface AppOptions {
    title: string;
    width: number;
    height: number;
    resizable?: boolean;
    /** The window background (default theme.background). */
    background?: Colour;
    /** Describes the window; runs again whenever something may have changed. */
    view: (app: App) => Child;
    /** Return true when the key was used. */
    onKey?: (event: KeyEvent, app: App) => boolean | void;
    /** A key that was let go (no text field took it). */
    onKeyUp?: (event: KeyEvent, app: App) => void;
    /** The window is closing. Return false to keep it open (when the program closes it itself, it closes anyway). */
    onClose?: (app: App) => boolean | void;
    /** Escape closes the window. */
    closeOnEscape?: boolean;
    /** The window got (true) or lost (false) the keyboard. */
    onFocus?: (focused: boolean, app: App) => void;
}

export interface App {
    /** Runs the view again soon (after a change the toolkit did not see, such as a timer). */
    invalidate(): void;
    close(): void;
    setTitle(title: string): void;
    readonly width: number;
    readonly height: number;
    readonly closed: boolean;
}

/** Opens a window. The program keeps running while a window is open. */
export function run(options: AppOptions): App;

export function column(props: BoxProps, ...children: Child[]): Widget;
export function column(...children: Child[]): Widget;
export function row(props: BoxProps, ...children: Child[]): Widget;
export function row(...children: Child[]): Widget;
/** A column that scrolls with the wheel and its scroll bar when its content is taller than its box. */
export function scroll(props: ScrollProps, ...children: Child[]): Widget;
export function scroll(...children: Child[]): Widget;
/** A column on a raised surface with a border and rounded corners. */
export function card(props: BoxProps, ...children: Child[]): Widget;
export function card(...children: Child[]): Widget;

export function text(content: string | number, props?: TextProps): Widget;
/** Bold text a little larger than body text. */
export function heading(content: string, props?: TextProps): Widget;
/** Large bold text. */
export function title(content: string, props?: TextProps): Widget;
/** Small dimmed text. */
export function caption(content: string, props?: TextProps): Widget;

export function button(label: string, onClick: () => void, props?: ButtonProps): Widget;
export function toggle(on: boolean, onChange: (on: boolean) => void, props?: Common): Widget;
export function checkbox(checked: boolean, label: string, onChange: (checked: boolean) => void, props?: Common): Widget;
/** done is false while the knob is dragged and true when the change is final. */
export function slider(value: number, min: number, max: number, onChange: (value: number, done: boolean) => void,
                       props?: SliderProps): Widget;
/** One choice of several, side by side; the chosen one slides into place. */
export function segmented<T>(options: readonly (Option<T> | T)[], value: T, onChange: (value: T) => void,
                             props?: Common & {size?: number}): Widget;
export function textField(value: string, onChange: (value: string) => void, props?: FieldProps): Widget;
/** A row of a navigation list or menu. */
export function listItem(label: string, selected: boolean, onClick: () => void, props?: ItemProps): Widget;
export function icon(name: IconName, props?: IconProps): Widget;
export function divider(props?: Common & {colour?: Colour}): Widget;
/** Fixed space, or (without a size) all the free space. */
export function spacer(size?: number): Widget;
/** A bar filled to value (0 to 1). */
export function progress(value: number, props?: Common & {length?: number}): Widget;

/** The colours and sizes the widgets use. */
export const theme: {
    readonly background: Colour; readonly sidebar: Colour; readonly surface: Colour; readonly surfaceHover: Colour;
    readonly surfaceActive: Colour; readonly control: Colour; readonly controlHover: Colour; readonly border: Colour;
    readonly divider: Colour; readonly text: Colour; readonly textDim: Colour; readonly textFaint: Colour;
    readonly accent: Colour; readonly accentHover: Colour; readonly accentPressed: Colour; readonly accentText: Colour;
    readonly warning: Colour; readonly danger: Colour; readonly success: Colour; readonly shadow: Colour;
    readonly radius: number; readonly controlRadius: number; readonly fontSize: number; readonly smallSize: number;
    readonly titleSize: number; readonly headingSize: number;
};

/** a and b mixed: t = 0 is a, 1 is b. */
export function mix(a: Colour, b: Colour, t: number): Colour;
/** The colour with the opacity alpha (0 to 1). */
export function withAlpha(colour: Colour, alpha: number): Colour;

/** Starts an app from the desktop, with one optional argument. The app is found by name in
    C:\Nocturne\System, then in each folder of C:\Programs ("notepad"), or by a path. */
export function launch(path: string, argument?: string): boolean;

/** The desktop's settings, as the Settings app shows them. The desktop applies a change at once
    and keeps it after a restart. */
export interface DesktopSettings {
    /** The desktop's size: the monitor's mode (ScreenModes), or a remote viewer's window. */
    readonly screenWidth: number;
    readonly screenHeight: number;
    /** -2 (slowest) to 2 (fastest). */
    pointerSpeed: number;
    /** The desktop drawn with more shades than the screen has and dithered onto it: less banding
        in gradients, photos and video, for some processor time. The user's wish; in effect while
        smoothGradientsAvailable. */
    smoothGradients: boolean;
    /** The screen shown now takes smooth gradients (it has 8 bits per colour channel). */
    readonly smoothGradientsAvailable: boolean;
}

/** The sound's way out. */
export interface SoundOutput {
    /** What plays the sound ("Remote desktop", or the sound card's name); null when nothing does:
        the volume is kept, but nothing is heard. */
    readonly device: string | null;
    /** 0 to 100. */
    readonly volume: number;
    readonly muted: boolean;
}

/** What changed:
    - "screen": anything get() or modes() reports (the desktop's size and settings, whether smooth
      gradients can be shown, the monitor and its modes, a trial beginning or ending, the mode kept
      for it); a trial counting down is no change.
    - "sound": anything sound() reports (the volume, muting, what plays the sound). */
export type SystemChange = "screen" | "sound";

/** The desktop, the monitor's modes and the sound. Each change asked for here goes to the part of
    the system that applies it (the desktop, the display modes, the sound), which applies it at
    once, keeps it after a restart and tells every program watching. A program holds none of them
    in a file of its own: it asks, and watches. */
export const desktop: {
    /** null when the desktop is not running. */
    get(): DesktopSettings | null;
    /** Applies the changeable settings; false when the desktop refused them. */
    set(settings: Pick<DesktopSettings, "pointerSpeed" | "smoothGradients">): boolean;
    /** What plays the sound, the volume and muting; null when the system will not say. */
    sound(): SoundOutput | null;
    /** Sets the volume (0 to 100; it also unmutes) and returns how the sound stands then. */
    setVolume(volume: number): SoundOutput | null;
    /** Mutes or unmutes, keeping the volume, and returns how the sound stands then. */
    setMuted(muted: boolean): SoundOutput | null;
    /** The monitor's modes; null when the desktop is not running. */
    modes(): ScreenModes | null;
    /** Shows mode (one of modes().modes) on trial: it goes back to the mode before unless
        keepMode() comes within 15 seconds (backMode(): at once). False when no display driver
        offers modes or the monitor has no such mode. */
    tryMode(mode: ScreenModeChoice): boolean;
    /** Keeps the mode on trial as the user's choice for this monitor (ScreenModes.chosen), shown
        whenever the monitor is driven and offers it, after a restart too. */
    keepMode(): boolean;
    /** Goes back to the mode before the trial now. */
    backMode(): boolean;
    /** Calls listener after something get(), modes() or sound() reports changed, whoever changed it
        (this program, another, the taskbar), with what changed. Returns the function that stops
        it. Watching alone does not keep the program running: its windows, timers or requests do. */
    watch(listener: (changed: readonly SystemChange[]) => void): () => void;
};

/** A resolution and refresh rate. */
export interface ScreenModeChoice {
    readonly width: number;
    readonly height: number;
    /** Refreshes per second times 1000: 59940 is 59.94 Hz. */
    readonly refreshMillihertz: number;
}

/** A mode the display driver can show on the monitor. */
export interface ScreenMode extends ScreenModeChoice {
    /** The monitor's own preferred mode (its EDID's). */
    readonly preferred: boolean;
}

/** What decides the desktop's size:
    - "firmware": the mode the computer started with; no display driver has run.
    - "driver": a display driver's mode (no modes: it finds no monitor).
    - "handedBack": the mode the computer started with again, while the display driver still runs:
      it gave the screen back. While it offers modes, trying one asks it to show that one again.
    - "driverStopped": the mode the computer started with again; the display driver ended.
    - "remote": a Remote Desktop window. */
export type ScreenSource = "firmware" | "driver" | "handedBack" | "driverStopped" | "remote";

/** The monitor and its modes, as the display driver offers them. */
export interface ScreenModes {
    readonly source: ScreenSource;
    /** The monitor, told apart by its EDID's maker, product and serial number ("" without one).
        What the system knows it by, not a name for people. */
    readonly monitor: string;
    /** The monitor's own name ("" when its EDID gives none). */
    readonly name: string;
    /** Empty while no display driver offers modes: the screen keeps the mode it has. */
    readonly modes: readonly ScreenMode[];
    /** The mode the screen shows: the driver's as it last said, or the one the computer started
        with (its refresh 0 when the monitor does not say it); null while unknown. */
    readonly shown: ScreenMode | null;
    /** The mode the system asks the driver to show; null while it asks for none. */
    readonly wanted: ScreenMode | null;
    /** Milliseconds until the mode on trial goes back; 0 when none is on trial. */
    readonly trialMs: number;
    /** The mode tried last could not be shown, and the one before is back. */
    readonly refused: boolean;
    /** The mode the user kept for this monitor, whether or not it is offered now; null while the
        user chose none (the monitor's preferred mode is shown). */
    readonly chosen: ScreenModeChoice | null;
}

/** The computer, as About shows it. */
export interface SystemInfo {
    /** The processor's name. */
    readonly processor: string;
    /** Processors the system runs on. */
    readonly processors: number;
    readonly system: string;
    /** Bytes. */
    readonly totalMemory: number;
    readonly freeMemory: number;
    /** Milliseconds since the computer started. */
    readonly uptimeMs: number;
    /** Programs and threads running now. */
    readonly tasks: number;
    readonly screenWidth: number;
    readonly screenHeight: number;
    readonly bitsPerChannel: number;
}

/** null when the system will not say. */
export function systemInfo(): SystemInfo | null;

/** Paints inside a canvas widget, in the widget's own coordinates. */
export interface Painter {
    rect(x: number, y: number, w: number, h: number, colour: Colour): void;
    /** A rounded rectangle; with bottom, a vertical gradient from colour to bottom. */
    round(x: number, y: number, w: number, h: number, radius: number, colour: Colour, bottom?: Colour): void;
    stroke(x: number, y: number, w: number, h: number, radius: number, width: number, colour: Colour): void;
    shadow(x: number, y: number, w: number, h: number, radius: number, blur: number, colour: Colour): void;
    circle(cx: number, cy: number, r: number, colour: Colour, bottom?: Colour): void;
    line(x0: number, y0: number, x1: number, y1: number, width: number, colour: Colour): void;
    /** Draws text with its top at y; returns its width. */
    text(x: number, y: number, content: string | number, options?: {size?: number; weight?: 0 | 1; mono?: boolean; colour?: Colour}): number;
    measure(content: string | number, options?: {size?: number; weight?: 0 | 1; mono?: boolean}): number;
    icon(name: IconName, x: number, y: number, size: number, colour?: Colour): void;
}

/** The pointer over a canvas, in its own coordinates. */
export interface PointerEvent {
    x: number;
    y: number;
    /** A button went down / came up (neither: the pointer moved). */
    down: boolean;
    up: boolean;
    /** The buttons held: 1 left, 2 right, 4 middle. */
    buttons: number;
    shift: boolean;
    ctrl: boolean;
    alt: boolean;
}

export interface CanvasProps extends Common {
    /** Moves and presses over the canvas; once a button is down there, the pointer is its until all are up. */
    onPointer?: (event: PointerEvent) => void;
    /** Wheel notches over the canvas (positive: towards the user). */
    onWheel?: (notches: number) => void;
}

/** Free drawing: draw runs on every repaint with the widget's size (default 100 × 100). */
export function canvas(draw: (painter: Painter, width: number, height: number) => void, props?: CanvasProps): Widget;
