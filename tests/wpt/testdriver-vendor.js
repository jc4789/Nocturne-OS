// Nocturne's testdriver backend, served as /resources/testdriver-vendor.js by scripts/wpt.py.
//
// A test tool, not a product feature. The browser gives a top-level window the function used
// here (__nocturne_automation) only when the WPT runner (tests/wptrunner.c) started it with
// --wpt-automation, and only then is navigator.webdriver true. The function puts input steps
// into the browser's own input queue, where the user's keyboard and mouse go, so the page gets
// real, trusted events through the browser's own hit testing, focus and default actions.
//
// click, send_keys and action_sequence follow WebDriver's Element Click, Element Send Keys and
// Perform Actions as far as the native input can carry them. Every other testdriver function
// keeps testdriver.js's refusal; in_automation makes it fail at once instead of waiting for a
// person.
(function() {
  "use strict";
  var internal = window.test_driver_internal;
  if (!internal) return;
  var own = typeof window.__nocturne_automation === "function";
  if (!own && !(window !== window.top && navigator.webdriver === true)) return;
  internal.in_automation = true;

  var FRAME_MS = 16; // a pointer move with a duration steps once a display frame

  function failure(code, message) { return new Error(code + ": " + message); }

  // One command at a time, in the order the test gave them.
  var queue = Promise.resolve();
  function serially(task) {
    var result = queue.then(task);
    queue = result.then(function() {}, function() {});
    return result;
  }

  // ------------------------------------------------------------------ where a frame is
  function contentOrigin(frame) {
    var rect = frame.getBoundingClientRect();
    var style = frame.ownerDocument.defaultView.getComputedStyle(frame);
    return {x: rect.left + frame.clientLeft + (parseFloat(style.paddingLeft) || 0),
            y: rect.top + frame.clientTop + (parseFloat(style.paddingTop) || 0)};
  }
  // How far win's viewport lies from its top-level viewport, as far as this realm can read:
  // up to the top, or to the first parent it may not read (a cross-origin one).
  function climb(win) {
    var x = 0, y = 0;
    for (;;) {
      if (win === win.top) return {window: win, x: x, y: y, top: true};
      var frame = null;
      try { frame = win.frameElement; } catch (e) { frame = null; }
      if (!frame) return {window: win, x: x, y: y, top: false};
      var origin = contentOrigin(frame);
      x += origin.x; y += origin.y;
      win = frame.ownerDocument.defaultView;
    }
  }
  function shifted(steps, dx, dy) {
    if (!dx && !dy) return steps;
    return steps.map(function(step) {
      if (step.type === "keyDown" || step.type === "keyUp") return step;
      var copy = Object.assign({}, step);
      copy.x = step.x + dx; copy.y = step.y + dy;
      return copy;
    });
  }

  // Steps given in win's viewport, done by the browser in its top-level viewport. A frame
  // under a cross-origin parent asks the parent's testdriver, which adds where the frame is.
  var relays = new Map(), relaySerial = 0;
  function perform(win, steps, duration) {
    var place = climb(win);
    steps = shifted(steps, place.x, place.y);
    if (place.top) {
      var port;
      try { port = place.window.__nocturne_automation; } catch (e) { port = undefined; }
      if (typeof port !== "function")
        return Promise.reject(failure("unsupported operation", "the browser is not driven for automation"));
      return Promise.resolve(port(steps, duration));
    }
    if (place.window !== window)
      return Promise.reject(failure("unsupported operation", "a frame under a cross-origin parent is driven by its own testdriver"));
    return new Promise(function(resolve, reject) {
      var id = ++relaySerial;
      relays.set(id, {resolve: resolve, reject: reject});
      window.parent.postMessage({nocturneTestdriver: "perform", id: id, steps: steps, duration: duration}, "*");
    });
  }
  function frameOf(source) {
    var found = null;
    (function walk(root) {
      var all = root.querySelectorAll("*");
      for (var i = 0; i < all.length && !found; i++) {
        var element = all[i];
        if ((element.localName === "iframe" || element.localName === "frame" || element.localName === "object") &&
            element.contentWindow === source) found = element;
        else if (element.shadowRoot) walk(element.shadowRoot);
      }
    })(document);
    return found;
  }
  // The testdriver's own messages; the test never sees them.
  window.addEventListener("message", function(event) {
    var data = event.data;
    if (!data || typeof data !== "object" || typeof data.nocturneTestdriver !== "string") return;
    event.stopImmediatePropagation();
    if (data.nocturneTestdriver === "done") {
      var waiting = event.source === window.parent ? relays.get(data.id) : undefined;
      if (!waiting) return;
      relays.delete(data.id);
      if (data.error) waiting.reject(new Error(data.error)); else waiting.resolve();
      return;
    }
    if (data.nocturneTestdriver !== "perform" || !event.source || !Array.isArray(data.steps)) return;
    var source = event.source, frame = frameOf(source);
    function reply(error) { source.postMessage({nocturneTestdriver: "done", id: data.id, error: error}, "*"); }
    if (!frame) { reply("no such frame: the message did not come from a child frame"); return; }
    var result;
    try {
      var origin = contentOrigin(frame);
      result = perform(window, shifted(data.steps, origin.x, origin.y), data.duration);
    } catch (e) { result = Promise.reject(e); }
    result.then(function() { reply(null); }, function(e) { reply(String(e && e.message || e)); });
  }, true);

  // ------------------------------------------------------------------ keys
  // WebDriver's keys that are another code point of the same key (its normalized key value).
  function normalized(code) {
    if (code >= 0xE050 && code <= 0xE053) return [0xE008, 0xE009, 0xE00A, 0xE03D][code - 0xE050];
    if (code >= 0xE054 && code <= 0xE05D) return code - 0xE054 + 0xE00E;
    if (code === 0xE006) return 0xE007;
    if (code === 0xE00D) return 0x20;
    if (code >= 0xE018 && code <= 0xE029) return ";=0123456789*+,-./".charCodeAt(code - 0xE018);
    return code;
  }
  var MODIFIERS = [0xE008, 0xE009, 0xE00A, 0xE03D, 0xE050, 0xE051, 0xE052, 0xE053];
  var SHIFTED = "~!@#$%^&*()_+{}|:\"<>?ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  function keyValue(value) {
    if (typeof value !== "string") throw failure("invalid argument", "a key value must be a string");
    var points = Array.from(value);
    if (points.length !== 1)
      throw failure(points.length ? "unsupported operation" : "invalid argument", "a key value must be one code point");
    return points[0].codePointAt(0);
  }

  // A key input source: which keys it holds (normalized key to the code point pressed).
  function KeySource() { this.pressed = new Map(); this.order = []; }
  KeySource.prototype.down = function(steps, raw, at) {
    var key = normalized(raw);
    if (!this.pressed.has(key)) this.order.push(key);
    this.pressed.set(key, raw); // a key held already repeats
    steps.push({type: "keyDown", at: at, key: raw});
  };
  KeySource.prototype.up = function(steps, raw, at) {
    var key = normalized(raw);
    if (!this.pressed.has(key)) return;
    this.pressed.delete(key);
    this.order.splice(this.order.indexOf(key), 1);
    steps.push({type: "keyUp", at: at, key: raw});
  };
  KeySource.prototype.shifted = function() { return this.pressed.has(0xE008); };
  KeySource.prototype.releaseAll = function(steps) {
    for (var i = this.order.length - 1; i >= 0; i--) this.up(steps, this.pressed.get(this.order[i]), 0);
  };

  // WebDriver's "dispatch actions for a string", code point by code point.
  function typeSteps(text) {
    var source = new KeySource(), steps = [], undo = [];
    function clearModifiers() {
      undo.sort(function(a, b) { return a - b; }).forEach(function(raw) { source.up(steps, raw, 0); });
      undo = [];
    }
    Array.from(text).forEach(function(ch) {
      var code = ch.codePointAt(0);
      if (code === 0xE000) { clearModifiers(); return; }
      if (MODIFIERS.indexOf(code) >= 0) {
        source.down(steps, code, 0);
        if (undo.indexOf(code) < 0) undo.push(code);
        return;
      }
      var shift = SHIFTED.indexOf(ch) >= 0;
      if (shift && !source.shifted()) source.down(steps, 0xE008, 0);
      if (!shift && source.shifted()) source.up(steps, 0xE008, 0);
      source.down(steps, code, 0);
      source.up(steps, code, 0);
    });
    clearModifiers();
    source.releaseAll(steps); // the implicit Shift ends with the source
    return steps;
  }

  // ------------------------------------------------------------------ elements
  function isFileInput(element) {
    return element.localName === "input" && String(element.type).toLowerCase() === "file";
  }
  function nonTypeable(element) {
    return element.localName === "input" && ["color", "range"].indexOf(String(element.type).toLowerCase()) >= 0;
  }
  function deepActive(doc) {
    var active = doc.activeElement;
    while (active && active.shadowRoot && active.shadowRoot.activeElement) active = active.shadowRoot.activeElement;
    return active;
  }
  function includes(ancestor, node) {
    for (var n = node; n; n = n.parentNode || (n.nodeType === 11 ? n.host : null)) if (n === ancestor) return true;
    return false;
  }
  function caretToEnd(element) {
    var doc = element.ownerDocument;
    if (element.isContentEditable) {
      var selection = doc.getSelection ? doc.getSelection() : null;
      if (!selection) return;
      var range = doc.createRange();
      range.selectNodeContents(element);
      range.collapse(false);
      selection.removeAllRanges();
      selection.addRange(range);
      return;
    }
    if (typeof element.setSelectionRange === "function" && typeof element.value === "string") {
      var end = element.value.length;
      try { element.setSelectionRange(end, end); } catch (e) {} // a type without a selection
    }
  }
  // WebDriver's in-view center point of the element's first box, in its window's viewport.
  function centre(element) {
    var rects = element.getClientRects();
    if (!rects.length) return null;
    var r = rects[0], win = element.ownerDocument.defaultView;
    var left = Math.max(0, Math.min(r.left, r.right)), right = Math.min(win.innerWidth, Math.max(r.left, r.right));
    var top = Math.max(0, Math.min(r.top, r.bottom)), bottom = Math.min(win.innerHeight, Math.max(r.top, r.bottom));
    if (right < left || bottom < top) return null;
    return {x: Math.floor((left + right) / 2), y: Math.floor((top + bottom) / 2)};
  }

  // ------------------------------------------------------------------ Element Click
  internal.click = function(element, coords) {
    return serially(function() {
      if (isFileInput(element)) throw failure("invalid argument", "a file input cannot be clicked");
      var x = Math.floor(coords.x), y = Math.floor(coords.y);
      return perform(element.ownerDocument.defaultView, [
        {type: "move", at: 0, x: x, y: y},
        {type: "down", at: 0, x: x, y: y, button: 0},
        {type: "up", at: 0, x: x, y: y, button: 0}], 0);
    });
  };

  // ------------------------------------------------------------------ Element Send Keys
  internal.send_keys = function(element, keys) {
    return serially(function() {
      var doc = element.ownerDocument;
      if (isFileInput(element)) throw failure("unsupported operation", "choosing files is not available");
      var wasActive = deepActive(doc) === element;
      if (!wasActive) element.focus();
      if (!includes(element, deepActive(doc)) && element !== doc.body && element !== doc.documentElement)
        throw failure("element not interactable", "the element cannot have the keyboard");
      if (nonTypeable(element)) {
        if (element.disabled || element.readOnly) throw failure("element not interactable", "the control is not mutable");
        element.value = String(keys);
        if (element.validity && element.validity.badInput) throw failure("invalid argument", "the value is bad input");
        return undefined;
      }
      if (!wasActive) caretToEnd(element);
      return perform(doc.defaultView, typeSteps(String(keys)), 0);
    });
  };

  // ------------------------------------------------------------------ Perform Actions
  // What the last action_sequence left pressed. Before the next one it is released, as the WPT
  // runner does (Release Actions), and the next one starts from fresh sources.
  var held = null;
  function release(state) {
    var steps = [];
    for (var i = state.order.length - 1; i >= 0; i--) {
      var token = state.order[i];
      if (token.button !== undefined) {
        if (state.buttons.has(token.button)) {
          state.buttons.delete(token.button);
          steps.push({type: "up", at: 0, x: state.x, y: state.y, button: token.button});
        }
      } else state.keys.up(steps, state.keys.pressed.has(token.key) ? state.keys.pressed.get(token.key) : token.key, 0);
    }
    if (!steps.length) return Promise.resolve();
    try { return perform(state.window, steps, 0).catch(function() {}); }
    catch (e) { return Promise.resolve(); } // its window is gone
  }
  function duration(value) {
    if (typeof value !== "number" || !isFinite(value) || value < 0 || Math.floor(value) !== value)
      throw failure("invalid argument", "a duration must be a non-negative integer");
    return value;
  }
  function coordinate(value, name) {
    if (typeof value !== "number" || !isFinite(value)) throw failure("invalid argument", name + " must be a number");
    return value;
  }
  function target(action, state, win, pointerOrigin) {
    var x = coordinate(action.x === undefined ? 0 : action.x, "x");
    var y = coordinate(action.y === undefined ? 0 : action.y, "y");
    var origin = action.origin === undefined ? "viewport" : action.origin;
    if (origin === "pointer" && pointerOrigin) { x += state.x; y += state.y; }
    else if (origin !== "viewport") {
      if (!origin || typeof origin !== "object" || typeof origin.getClientRects !== "function")
        throw failure("invalid argument", "unknown origin");
      if (!origin.isConnected) throw failure("stale element reference", "the origin element is not connected");
      var c = centre(origin);
      if (!c) throw failure("move target out of bounds", "the origin element is not in view");
      x += c.x; y += c.y;
    }
    if (x < 0 || x > win.innerWidth || y < 0 || y > win.innerHeight)
      throw failure("move target out of bounds", "(" + x + ", " + y + ") is outside the viewport");
    return {x: x, y: y};
  }
  function button(value) {
    if (typeof value !== "number" || value < 0 || Math.floor(value) !== value) throw failure("invalid argument", "a button must be a non-negative integer");
    if (value > 4) throw failure("unsupported operation", "the native pointer has buttons 0..4");
    return value;
  }
  // One action of one source in a tick, as steps (at: ms after the tick starts).
  function dispatch(source, action, state, tickDuration, win, steps) {
    var kind = source.type + "/" + action.type, b, d, to, raw;
    switch (kind) {
    case "none/pause": case "key/pause": case "pointer/pause": case "wheel/pause":
      return;
    case "key/keyDown":
      raw = keyValue(action.value);
      if (!state.keys.pressed.has(normalized(raw))) state.order.push({key: normalized(raw)});
      state.keys.down(steps, raw, 0);
      return;
    case "key/keyUp":
      state.keys.up(steps, keyValue(action.value), 0);
      return;
    case "pointer/pointerDown":
      b = button(action.button);
      if (state.buttons.has(b)) return;
      state.buttons.add(b);
      state.order.push({button: b});
      steps.push({type: "down", at: 0, x: state.x, y: state.y, button: b});
      return;
    case "pointer/pointerUp":
      b = button(action.button);
      if (!state.buttons.has(b)) return;
      state.buttons.delete(b);
      steps.push({type: "up", at: 0, x: state.x, y: state.y, button: b});
      return;
    case "pointer/pointerMove":
      to = target(action, state, win, true);
      d = action.duration !== undefined ? duration(action.duration) : tickDuration;
      for (var at = FRAME_MS; at < d; at += FRAME_MS)
        steps.push({type: "move", at: at, x: state.x + (to.x - state.x) * at / d, y: state.y + (to.y - state.y) * at / d});
      steps.push({type: "move", at: d, x: to.x, y: to.y});
      state.x = to.x; state.y = to.y;
      return;
    case "pointer/pointerCancel":
      throw failure("unsupported operation", "the native pointer cannot be cancelled");
    case "wheel/scroll":
      if (action.origin === "pointer") throw failure("invalid argument", "a scroll origin cannot be the pointer");
      to = target(action, state, win, false);
      d = action.duration !== undefined ? duration(action.duration) : tickDuration;
      steps.push({type: "wheel", at: d, x: to.x, y: to.y,
                  dx: coordinate(action.deltaX === undefined ? 0 : action.deltaX, "deltaX"),
                  dy: coordinate(action.deltaY === undefined ? 0 : action.deltaY, "deltaY")});
      return;
    }
    throw failure("invalid argument", "unknown action " + kind);
  }
  async function actionSequence(sequences, context) {
    if (!Array.isArray(sequences)) throw failure("invalid argument", "actions must be an array");
    var win = context, pointers = 0;
    sequences.forEach(function(sequence) {
      if (!sequence || typeof sequence !== "object" || !Array.isArray(sequence.actions))
        throw failure("invalid argument", "an input source needs a list of actions");
      if (["none", "key", "pointer", "wheel"].indexOf(sequence.type) < 0)
        throw failure("invalid argument", "unknown input source type " + sequence.type);
      if (sequence.type === "pointer") {
        var pointerType = sequence.parameters && sequence.parameters.pointerType || "mouse";
        if (pointerType !== "mouse") throw failure("unsupported operation", "the native input has no " + pointerType + " pointer");
        if (++pointers > 1) throw failure("unsupported operation", "the native input has one pointer");
      }
      sequence.actions.forEach(function(action) {
        if (!action || typeof action !== "object") throw failure("invalid argument", "an action must be an object");
        var origin = action.origin;
        if (origin && typeof origin === "object") {
          var owner = origin.ownerDocument && origin.ownerDocument.defaultView;
          if (!owner) throw failure("no such element", "the origin element has no window");
          if (win && win !== owner) throw failure("invalid argument", "Actions must be in a single context");
          win = owner;
        }
      });
    });
    win = win || window;
    if (held) { var previous = held; held = null; await release(previous); }
    var state = {window: win, keys: new KeySource(), buttons: new Set(), order: [], x: 0, y: 0};
    held = state;
    var ticks = 0;
    sequences.forEach(function(sequence) { ticks = Math.max(ticks, sequence.actions.length); });
    for (var t = 0; t < ticks; t++) {
      // WebDriver: a tick lasts as long as its longest pause, move or scroll.
      var tickDuration = 0;
      sequences.forEach(function(sequence) {
        var action = sequence.actions[t];
        if (action && action.duration !== undefined && (action.type === "pause" ||
            (sequence.type === "pointer" && action.type === "pointerMove") || (sequence.type === "wheel" && action.type === "scroll")))
          tickDuration = Math.max(tickDuration, duration(action.duration));
      });
      var steps = [];
      sequences.forEach(function(sequence) {
        var action = sequence.actions[t];
        if (action) dispatch(sequence, action, state, tickDuration, win, steps);
      });
      steps = steps.map(function(step, i) { return [step, i]; })
                   .sort(function(a, b) { return a[0].at - b[0].at || a[1] - b[1]; })
                   .map(function(pair) { return pair[0]; });
      await perform(win, steps, tickDuration);
    }
  }
  internal.action_sequence = function(actions, context) {
    return serially(function() { return actionSequence(actions, context || null); });
  };
})();
