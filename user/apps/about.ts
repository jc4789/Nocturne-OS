/* About Nocturne: the version, and what this computer is running on. */
import { button, canvas, caption, column, mix, row, run, systemInfo, text, theme, withAlpha, type App, type Child, type Painter } from "nocturne/ui";

const WIDTH = 500, HEIGHT = 440, SKY = 150;

/* The night sky at the top: a gradient, stars, and the crescent moon in its glow. */
function sky(p: Painter, w: number, h: number) {
    p.round(0, 0, w, h, 0, 0xFF15123A, 0xFF3A2463);
    for (let i = 0; i < 70; i++) {
        const x = (i * 7919) % w, y = (i * 104729) % (h - 10) + 4, r = i % 7 === 0 ? 1.3 : i % 3 === 0 ? 0.9 : 0.6;
        p.circle(x + 0.5, y + 0.5, r, withAlpha(0xFFDCDCF5, i % 7 === 0 ? 0.95 : 0.55));
    }
    const cx = 78, cy = h / 2, r = 36;
    for (let i = 6; i > 0; i--) p.circle(cx, cy, r + i * 7, withAlpha(0xFFC8BEFF, 0.035));
    p.circle(cx, cy, r, 0xFFF6ECC4, 0xFFE8D9A0);
    p.circle(cx + r * 0.42, cy - r * 0.28, r * 0.9, mix(0xFF15123A, 0xFF3A2463, (cy - r * 0.28) / h));
    p.text(144, cy - 38, "Nocturne", {size: 38, weight: 1, colour: 0xFFFFFFFF});
    p.text(147, cy + 12, 'version 0.9 "Moonrise"', {size: 14, colour: 0xFFC9BEFF});
}

const MIB = 1024 * 1024;

function uptime(ms: number): string {
    const s = Math.floor(ms / 1000), two = (n: number) => String(n).padStart(2, "0");
    return `${Math.floor(s / 3600)}:${two(Math.floor(s / 60) % 60)}:${two(s % 60)}`;
}

function info(name: string, value: string): Child {
    return row({gap: 12, align: "start"},
               text(name, {width: 88, colour: theme.textDim}),
               text(value, {grow: 1}));
}

function view(app: App): Child {
    const si = systemInfo();
    const rows: Child[] = [info("Kernel", "x86_64, preemptive, written from scratch")];
    if (si) {
        const free = Math.round(si.freeMemory / MIB), total = Math.round(si.totalMemory / MIB);
        rows.push(
            info("Processor", si.processors > 1 ? `${si.processor.trim()} × ${si.processors}` : si.processor.trim()),
            info("Memory", `${free.toLocaleString()} MiB free of ${total.toLocaleString()} MiB`),
            info("Display", `${si.screenWidth} × ${si.screenHeight}`),
            info("Uptime", uptime(si.uptimeMs)),
            info("Tasks", `${si.tasks} running`));
    }
    return column({gap: 0},
        canvas(sky, {width: "fill", height: SKY}),
        column({padding: [22, 28, 20, 28], gap: 10, grow: 1},
               rows,
               column({grow: 1}),
               row({gap: 12},
                   caption("Made with care on a quiet night.", {grow: 1, colour: 0xFFAAA0D2}),
                   button("Close", () => app.close(), {variant: "primary", minWidth: 96}))));
}

const app = run({
    title: "About Nocturne", width: WIDTH, height: HEIGHT, view, closeOnEscape: true,
    onKey: e => {
        if (e.key === "Enter") { app.close(); return true; }
    },
});

/* Uptime, memory and tasks change; show them as they are. */
const tick = setInterval(() => {
    if (app.closed) clearInterval(tick);
    else app.invalidate();
}, 1000);
