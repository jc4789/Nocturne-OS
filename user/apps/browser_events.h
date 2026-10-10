#ifndef NOCTURNE_BROWSER_EVENTS_H
#define NOCTURNE_BROWSER_EVENTS_H

/* A long native/author task can dequeue many hover positions before it is
 * safe to deliver any DOM event. Preserve the newest consecutive hover only.
 * Never merge across a button/key/wheel/focus boundary, a modifier change, or
 * a pressed-pointer drag. Native pointer acknowledgements remain unmerged. */
static void browser_hover_boundary(bool *eligible, const struct gui_event *event) {
    /* Chrome events can be consumed immediately instead of entering queue.
       They still break continuity between the queued old and new hover. */
    if (event->type != EV_MOUSE_MOVE || event->buttons) *eligible = false;
}
static bool browser_coalesce_hover(struct gui_event *queue, int count,
                                   const struct gui_event *event, bool eligible) {
    if (!eligible || count <= 0 || event->type != EV_MOUSE_MOVE || event->buttons) return false;
    struct gui_event *last = &queue[count - 1];
    if (last->type != EV_MOUSE_MOVE || last->buttons || last->mods != event->mods) return false;
    *last = *event;
    return true;
}
static bool browser_hover_tail(const struct gui_event *event) {
    return event->type == EV_MOUSE_MOVE && !event->buttons;
}

#endif
