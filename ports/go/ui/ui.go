// Package ui gives Go programs the windows and widgets of Nocturne.
//
// A program describes its window with a view function that returns a tree of
// widgets; the view runs again after every event handler and after
// Invalidate, so the window always shows the program's current state:
//
//	count := 0
//	ui.Run(ui.Options{
//		Title: "Counter", Width: 320, Height: 200,
//		View: func(a *ui.App) *ui.Widget {
//			return ui.Column(ui.Props{"padding": 24, "gap": 16},
//				ui.Title(fmt.Sprintf("Clicked %d times", count), nil),
//				ui.Button("Click", func() { count++ }, ui.Props{"variant": "primary"}))
//		},
//	})
//
// This package only translates. The system's interface engine (libc/ui.c, the
// same one C programs and /bin/js use) lays the widgets out, paints them and
// runs hover, presses, focus, scrolling and animations; Go reaches it through
// /bin/uihost over a message channel, in the format of
// docs/nocturne-native-abi.md §11. Properties are the engine's (Props keys are
// its property names: "width", "padding", "align" for a box's children, "self"
// for a widget's own alignment, "colour", ...).
//
// Event handlers, view functions and canvas drawing all run on the goroutine
// that called Run; another goroutine changes the program's state with App.Do.
package ui

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os"
	"sync"
	"syscall"
	"unsafe"
)

// Colour is 0xAARRGGBB (alpha 0xFF is opaque).
type Colour = uint32

// Props are a widget's properties by the engine's names. Values are numbers,
// booleans, strings, or slices of numbers or strings.
type Props map[string]any

// Widget is one node of a view.
type Widget struct {
	kind      string
	props     Props
	children  []*Widget
	onClick   func()
	onToggle  func(bool)
	onSlide   func(float64, bool)
	onChoose  func(int)
	onText    func(string)
	onSubmit  func(string)
	onPointer func(Pointer)
	onWheel   func(float64)
	onFocus   func(focused, cancelled bool)
	draw      func(p *Painter, width, height float64)
}

func newWidget(kind string, p Props, kids []*Widget) *Widget {
	w := &Widget{kind: kind, props: make(Props, len(p)+2)}
	for k, v := range p {
		w.props[k] = v
	}
	for _, k := range kids {
		if k != nil {
			w.children = append(w.children, k)
		}
	}
	return w
}

func (w *Widget) setDefault(name string, v any) *Widget {
	if _, ok := w.props[name]; !ok {
		w.props[name] = v
	}
	return w
}

// OnClick makes a box a button (it lights up under the pointer).
func (w *Widget) OnClick(f func()) *Widget { w.onClick = f; return w }

// OnSubmit is Enter in a text field.
func (w *Widget) OnSubmit(f func(value string)) *Widget { w.onSubmit = f; return w }

// OnPointer gets moves and presses over a canvas or surface view, in its own
// coordinates; once a button is down there, the pointer is its until all are up.
func (w *Widget) OnPointer(f func(p Pointer)) *Widget { w.onPointer = f; return w }

// OnWheel gets wheel notches over a canvas or surface view (positive: towards the user).
func (w *Widget) OnWheel(f func(notches float64)) *Widget { w.onWheel = f; return w }

// OnFocus is a text field getting (true) or losing (false) the keyboard (cancelled: Escape
// gave it up). Its "focus" property moves the keyboard from the program: a change to a
// positive number takes it (all text selected with "selectOnFocus"), to zero or less gives it
// up; "reveal" (any widget) scrolls the scroll view it is in until it can be seen whenever its
// value changes.
func (w *Widget) OnFocus(f func(focused, cancelled bool)) *Widget { w.onFocus = f; return w }

// Pointer is the pointer over a canvas or surface view.
type Pointer struct {
	X, Y float64
	// a button went down / came up (neither: the pointer moved)
	Down, Up bool
	// the buttons held: 1 left, 2 right, 4 middle
	Buttons          int
	Shift, Ctrl, Alt bool
}

func Column(p Props, kids ...*Widget) *Widget { return newWidget("column", p, kids) }
func Row(p Props, kids ...*Widget) *Widget    { return newWidget("row", p, kids) }

// Scroll is a column that scrolls when its content is taller than its box.
func Scroll(p Props, kids ...*Widget) *Widget { return newWidget("scroll", p, kids) }

// Card is a column on a raised surface with a border and rounded corners.
func Card(p Props, kids ...*Widget) *Widget { return newWidget("card", p, kids) }

func Text(s string, p Props) *Widget {
	w := newWidget("text", p, nil)
	w.props["text"] = s
	return w
}

// Heading is bold text a little larger than body text.
func Heading(s string, p Props) *Widget { return Text(s, p).setDefault("style", "heading") }

// Title is large bold text.
func Title(s string, p Props) *Widget { return Text(s, p).setDefault("style", "title") }

// Caption is small dimmed text.
func Caption(s string, p Props) *Widget { return Text(s, p).setDefault("style", "caption") }

func Button(label string, onClick func(), p Props) *Widget {
	w := newWidget("button", p, nil)
	w.props["text"] = label
	w.onClick = onClick
	return w
}

func Toggle(on bool, onChange func(on bool), p Props) *Widget {
	w := newWidget("toggle", p, nil)
	w.props["on"] = on
	w.onToggle = onChange
	return w
}

func Checkbox(on bool, label string, onChange func(on bool), p Props) *Widget {
	w := newWidget("checkbox", p, nil)
	w.props["on"] = on
	w.props["text"] = label
	w.onToggle = onChange
	return w
}

// Slider: done is false while the knob is dragged and true when the change is final.
func Slider(value, min, max float64, onChange func(value float64, done bool), p Props) *Widget {
	w := newWidget("slider", p, nil)
	w.props["value"], w.props["min"], w.props["max"] = value, min, max
	w.onSlide = onChange
	return w
}

// Segmented is one choice of several side by side; onChange gets the index.
func Segmented(options []string, selected int, onChange func(index int), p Props) *Widget {
	w := newWidget("segmented", p, nil)
	w.props["options"] = options
	w.props["selected"] = selected
	w.onChoose = onChange
	return w
}

func TextField(value string, onChange func(value string), p Props) *Widget {
	w := newWidget("field", p, nil)
	w.props["value"] = value
	w.onText = onChange
	return w
}

// ListItem is a row of a navigation list or menu.
func ListItem(label string, selected bool, onClick func(), p Props) *Widget {
	w := newWidget("item", p, nil)
	w.props["text"] = label
	w.props["selected"] = selected
	w.onClick = onClick
	return w
}

func Icon(name string, p Props) *Widget {
	w := newWidget("icon", p, nil)
	w.props["icon"] = name
	return w
}

func Divider(p Props) *Widget { return newWidget("divider", p, nil) }

// Spacer is fixed space, or (size 0) all the free space.
func Spacer(size float64) *Widget {
	if size == 0 {
		return newWidget("spacer", nil, nil)
	}
	return newWidget("spacer", Props{"size": size}, nil)
}

// Progress is a bar filled to value (0 to 1).
func Progress(value float64, p Props) *Widget {
	w := newWidget("progress", p, nil)
	w.props["value"] = value
	return w
}

// Canvas is free drawing: draw records what to paint inside the widget's box,
// given its size (its "width" and "height", or 100, until it is placed).
func Canvas(draw func(p *Painter, width, height float64), p Props) *Widget {
	w := newWidget("canvas", p, nil)
	w.draw = draw
	return w
}

// SurfaceView shows a surface's pixels, scaled to the widget's box.
func SurfaceView(s *Surface, p Props) *Widget {
	w := newWidget("surface", p, nil)
	w.props["surface"] = s.id
	return w
}

// ---- bytes ----

type writer []byte

func (w *writer) u8(v byte)      { *w = append(*w, v) }
func (w *writer) u16(v uint16)   { *w = binary.LittleEndian.AppendUint16(*w, v) }
func (w *writer) u32(v uint32)   { *w = binary.LittleEndian.AppendUint32(*w, v) }
func (w *writer) f64(v float64)  { *w = binary.LittleEndian.AppendUint64(*w, math.Float64bits(v)) }
func (w *writer) str(s string)   { w.u32(uint32(len(s))); *w = append(*w, s...) }
func (w *writer) name(s string)  { w.u8(byte(len(s))); *w = append(*w, s...) }
func (w *writer) bytes(b []byte) { w.u32(uint32(len(b))); *w = append(*w, b...) }

type reader struct {
	b   []byte
	bad bool
}

func (r *reader) take(n int) []byte {
	if r.bad || len(r.b) < n {
		r.bad = true
		if n > 8 { // a length read from a bad message must not become an allocation
			return nil
		}
		return make([]byte, n)
	}
	p := r.b[:n]
	r.b = r.b[n:]
	return p
}
func (r *reader) u8() byte     { return r.take(1)[0] }
func (r *reader) u32() uint32  { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *reader) f64() float64 { return math.Float64frombits(binary.LittleEndian.Uint64(r.take(8))) }
func (r *reader) str() string  { return string(r.take(int(r.u32()))) }

func number(v any) (float64, bool) {
	switch x := v.(type) {
	case float64:
		return x, true
	case float32:
		return float64(x), true
	case int:
		return float64(x), true
	case int32:
		return float64(x), true
	case int64:
		return float64(x), true
	case uint8:
		return float64(x), true
	case uint32:
		return float64(x), true
	case uint64:
		return float64(x), true
	case bool:
		if x {
			return 1, true
		}
		return 0, true
	}
	return 0, false
}

// prop writes one property; false for a value the format has no type for.
func prop(w *writer, name string, v any) bool {
	if f, ok := number(v); ok {
		w.name(name)
		w.u8(1)
		w.f64(f)
		return true
	}
	switch x := v.(type) {
	case string:
		w.name(name)
		w.u8(2)
		w.str(x)
	case []float64:
		w.name(name)
		w.u8(3)
		w.u32(uint32(len(x)))
		for _, f := range x {
			w.f64(f)
		}
	case []int:
		w.name(name)
		w.u8(3)
		w.u32(uint32(len(x)))
		for _, f := range x {
			w.f64(float64(f))
		}
	case []string:
		w.name(name)
		w.u8(4)
		w.u32(uint32(len(x)))
		for _, s := range x {
			w.str(s)
		}
	default:
		return false
	}
	return true
}

// ---- drawing ----

// Painter records what a canvas paints, in the canvas's own coordinates.
type Painter struct{ ops writer }

func (p *Painter) op(code byte, args ...float64) {
	p.ops.u8(code)
	for _, a := range args {
		p.ops.f64(a)
	}
}

func (p *Painter) Rect(x, y, w, h float64, c Colour) { p.op(1, x, y, w, h, float64(c)) }

// Round is a rounded rectangle.
func (p *Painter) Round(x, y, w, h, radius float64, c Colour) {
	p.op(2, x, y, w, h, radius, float64(c), float64(c))
}

// Gradient is a rounded rectangle whose colour goes from top to bottom.
func (p *Painter) Gradient(x, y, w, h, radius float64, top, bottom Colour) {
	p.op(2, x, y, w, h, radius, float64(top), float64(bottom))
}

func (p *Painter) Stroke(x, y, w, h, radius, width float64, c Colour) {
	p.op(3, x, y, w, h, radius, width, float64(c))
}

func (p *Painter) Shadow(x, y, w, h, radius, blur float64, c Colour) {
	p.op(4, x, y, w, h, radius, blur, float64(c))
}

func (p *Painter) Circle(cx, cy, r float64, c Colour) { p.Round(cx-r, cy-r, 2*r, 2*r, r, c) }

func (p *Painter) Line(x0, y0, x1, y1, width float64, c Colour) {
	p.op(5, x0, y0, x1, y1, width, float64(c))
}

// Text draws s with the top of its line at y (weight: TextBold, TextMono or both) and returns its width.
func (p *Painter) Text(x, y float64, s string, size float64, weight int, c Colour) float64 {
	p.op(6, x, y, size, float64(weight), float64(c))
	p.ops.str(s)
	return Measure(s, size, weight).Width
}

func (p *Painter) Icon(name string, x, y, size float64, c Colour) {
	p.op(7, x, y, size, float64(c))
	p.ops.str(name)
}

// Text styles for Painter.Text and Measure (a text widget takes "weight" and "mono").
const (
	TextBold = 1
	TextMono = 2
)

// Metrics of a text.
type Metrics struct{ Width, LineHeight, Baseline float64 }

type measureKey struct {
	s      string
	size   float64
	weight int
}

// Measure is how the engine sets s (weight: TextBold, TextMono or both).
func Measure(s string, size float64, weight int) Metrics {
	ss, err := start()
	if err != nil {
		return Metrics{}
	}
	key := measureKey{s, size, weight}
	if m, ok := ss.measures[key]; ok {
		return m
	}
	var w writer
	w.u8('M')
	w.u32(0)
	w.f64(size)
	w.u32(uint32(weight))
	w.str(s)
	r := ss.request(w, 'm')
	if r == nil {
		return Metrics{}
	}
	r.u32()
	m := Metrics{r.f64(), r.f64(), r.f64()}
	if len(ss.measures) > 4096 {
		clear(ss.measures)
	}
	ss.measures[key] = m
	return m
}

// ---- the session with /bin/uihost ----

type reply struct {
	b        []byte
	attached int
}

type session struct {
	ch       int
	replies  chan reply
	wake     chan struct{}
	mu       sync.Mutex // guards events, work, gone and App.dirty
	events   [][]byte
	work     []func()
	gone     bool
	theme    map[string]float64
	measures map[measureKey]Metrics
	apps     map[uint32]*App
	next     uint32
	running  bool
}

var (
	sess     *session
	sessErr  error
	sessOnce sync.Once
)

// ErrNoDesktop: the interface engine could not be started or has gone.
var ErrNoDesktop = errors.New("ui: the interface engine is not available")

func start() (*session, error) {
	sessOnce.Do(func() {
		a, b, err := syscall.ChannelPair()
		if err != nil {
			sessErr = err
			return
		}
		app, err := syscall.AppLaunch("/bin/uihost", nil, nil, []int{-1, -1, -1, b})
		syscall.Close(b)
		if err != nil {
			syscall.Close(a)
			sessErr = fmt.Errorf("ui: cannot start /bin/uihost: %w", err)
			return
		}
		syscall.Close(app)
		s := &session{ch: a, replies: make(chan reply, 16), wake: make(chan struct{}, 1),
			measures: map[measureKey]Metrics{}, apps: map[uint32]*App{}, theme: map[string]float64{}}
		go s.read()
		var w writer
		w.u8('Q')
		if r := s.request(w, 'q'); r != nil {
			for n := r.u32(); n > 0 && !r.bad; n-- {
				name := string(r.take(int(r.u8())))
				s.theme[name] = r.f64()
			}
		}
		sess = s
	})
	return sess, sessErr
}

func (s *session) signal() {
	select {
	case s.wake <- struct{}{}:
	default:
	}
}

// read takes the engine's messages: replies for request, events for the loop.
func (s *session) read() {
	buf := make([]byte, 4096)
	for {
		n, _, err := syscall.ChannelReceive(s.ch, nil, syscall.CHANNEL_WAIT|syscall.CHANNEL_PEEK)
		if err == nil && n > len(buf) {
			buf = make([]byte, n)
		}
		var att int
		if err == nil {
			n, att, err = syscall.ChannelReceive(s.ch, buf[:n], 0)
		}
		if err != nil {
			s.mu.Lock()
			s.gone = true
			s.mu.Unlock()
			close(s.replies)
			s.signal()
			return
		}
		if n == 0 {
			continue
		}
		msg := append([]byte(nil), buf[:n]...)
		switch {
		case msg[0] == 'e':
			s.mu.Lock()
			s.events = append(s.events, msg)
			s.mu.Unlock()
			s.signal()
		case msg[0] == 'x' && n >= 6 && msg[5] != 'O' && msg[5] != 'M' && msg[5] != 'Q' && msg[5] != 'S':
			fmt.Fprintf(os.Stderr, "ui: the engine refused a '%c' message (%d)\n", msg[5], int32(binary.LittleEndian.Uint32(msg[1:])))
		default:
			s.replies <- reply{msg, att}
		}
	}
}

func (s *session) send(w writer) {
	syscall.ChannelSend(s.ch, w, -1)
}

// request sends w and returns its reply after the type byte (nil if refused).
func (s *session) request(w writer, want byte) *reader {
	if _, err := syscall.ChannelSend(s.ch, w, -1); err != nil {
		return nil
	}
	r, ok := <-s.replies
	if !ok || len(r.b) == 0 || r.b[0] != want {
		if r.attached >= 0 && ok {
			syscall.Close(r.attached)
		}
		return nil
	}
	if r.attached >= 0 {
		syscall.Close(r.attached)
	}
	return &reader{b: r.b[1:]}
}

// requestHandle is request for a reply that carries a handle.
func (s *session) requestHandle(w writer, want byte) (*reader, int) {
	if _, err := syscall.ChannelSend(s.ch, w, -1); err != nil {
		return nil, -1
	}
	r, ok := <-s.replies
	if !ok || len(r.b) == 0 || r.b[0] != want {
		if ok && r.attached >= 0 {
			syscall.Close(r.attached)
		}
		return nil, -1
	}
	return &reader{b: r.b[1:]}, r.attached
}

// Theme is a colour or size of the system's look by its name ("accent",
// "text", "fontSize", ...; docs/nocturne-native-abi.md §11).
func Theme(name string) float64 {
	if s, err := start(); err == nil {
		return s.theme[name]
	}
	return 0
}

// ThemeColour is Theme for a colour.
func ThemeColour(name string) Colour { return Colour(Theme(name)) }

// ---- surfaces ----

// Surface is pixels the program draws itself (0xFFRRGGBB, row after row),
// shown by SurfaceView widgets.
type Surface struct {
	id     uint32
	Pix    []uint32
	Width  int
	Height int
	shared int
	bytes  uintptr
}

const (
	sysMemoryRelease = 167
	sysSharedMap     = 169
	memoryReadWrite  = 2
)

// NewSurface makes a surface of width x height pixels.
func NewSurface(width, height int) (*Surface, error) {
	s, err := start()
	if err != nil {
		return nil, err
	}
	s.next++
	sf := &Surface{id: s.next, shared: -1}
	return sf, sf.Resize(width, height)
}

// Resize gives the surface new pixels (their content is lost).
func (sf *Surface) Resize(width, height int) error {
	s, err := start()
	if err != nil {
		return err
	}
	var w writer
	w.u8('S')
	w.u32(sf.id)
	w.u32(uint32(width))
	w.u32(uint32(height))
	r, h := s.requestHandle(w, 's')
	if r == nil {
		return ErrNoDesktop
	}
	r.u32()
	if res := int32(r.u32()); res != 0 || h < 0 {
		if h >= 0 {
			syscall.Close(h)
		}
		return fmt.Errorf("ui: no surface of %dx%d (%d)", width, height, res)
	}
	sf.release()
	bytes := (uintptr(width)*uintptr(height)*4 + 4095) &^ 4095
	va, e := syscall.Syscall(sysSharedMap, uintptr(h), bytes, memoryReadWrite, 0, 0)
	if e != 0 {
		syscall.Close(h)
		return e
	}
	sf.shared, sf.bytes = h, bytes
	sf.Pix = unsafe.Slice((*uint32)(unsafe.Pointer(va)), width*height)
	sf.Width, sf.Height = width, height
	return nil
}

func (sf *Surface) release() {
	if sf.Pix != nil {
		syscall.Syscall(sysMemoryRelease, uintptr(unsafe.Pointer(&sf.Pix[0])), sf.bytes, 0, 0, 0)
		sf.Pix = nil
	}
	if sf.shared >= 0 {
		syscall.Close(sf.shared)
		sf.shared = -1
	}
}

// Changed shows what was drawn into the pixels.
func (sf *Surface) Changed() {
	if s, err := start(); err == nil {
		var w writer
		w.u8('P')
		w.u32(sf.id)
		s.send(w)
	}
}

// Drop lets the surface go.
func (sf *Surface) Drop() {
	sf.release()
	if s, err := start(); err == nil {
		var w writer
		w.u8('D')
		w.u32(sf.id)
		s.send(w)
	}
}

// ---- apps ----

const (
	eventClick = 1 + iota
	eventToggle
	eventSlide
	eventChoose
	eventText
	eventSubmit
	eventKey
	eventClose
	eventCanvas
	eventResize
	eventPointer
	eventWheel
	eventFocus
)

// Key is a key press that no focused text field took.
type Key struct {
	// "Escape", "Enter", "Backspace", "Tab", "ArrowUp" ... "F12", or the character typed.
	Name             string
	Shift, Ctrl, Alt bool
}

// Options describe a window.
type Options struct {
	Title         string
	Width, Height int
	Resizable     bool
	Background    Colour // 0: the theme's
	View          func(a *App) *Widget
	OnKey         func(k Key, a *App) bool   // true when the key was used
	OnKeyUp       func(k Key, a *App)        // a key that was let go
	OnClose       func(a *App) bool          // false keeps the window open
	OnFocus       func(focused bool, a *App) // the window got or lost the keyboard
	CloseOnEscape bool
}

// App is an open window.
type App struct {
	s        *session
	id       uint32
	opts     Options
	width    int
	height   int
	closed   bool
	dirty    bool
	views    map[uint32][]*Widget
	sent     uint32
	canvases map[any][2]float64
}

func (a *App) Width() int   { return a.width }
func (a *App) Height() int  { return a.height }
func (a *App) Closed() bool { return a.closed }

// Invalidate runs the view again soon; any goroutine may call it.
func (a *App) Invalidate() {
	a.s.mu.Lock()
	a.dirty = true
	a.s.mu.Unlock()
	a.s.signal()
}

// Do runs f on the goroutine of Run (where the view runs), then the view; any
// goroutine may call it.
func (a *App) Do(f func()) {
	a.s.mu.Lock()
	a.s.work = append(a.s.work, func() { f(); a.Invalidate() })
	a.s.mu.Unlock()
	a.s.signal()
}

func (a *App) SetTitle(title string) {
	if a.closed {
		return
	}
	var w writer
	w.u8('T')
	w.u32(a.id)
	w.str(title)
	a.s.send(w)
}

func (a *App) Close() {
	if a.closed {
		return
	}
	a.closed = true
	delete(a.s.apps, a.id)
	var w writer
	w.u8('C')
	w.u32(a.id)
	a.s.send(w)
}

// Open opens a window; it is shown and answered while Run runs.
func Open(o Options) (*App, error) {
	s, err := start()
	if err != nil {
		return nil, err
	}
	if o.View == nil {
		return nil, errors.New("ui: Open needs a View")
	}
	s.next++
	a := &App{s: s, id: s.next, opts: o, views: map[uint32][]*Widget{}, canvases: map[any][2]float64{}}
	var flags uint32 = 4 // WIN_CENTER
	if o.Resizable {
		flags |= 1
	}
	var w writer
	w.u8('O')
	w.u32(a.id)
	w.u32(uint32(o.Width))
	w.u32(uint32(o.Height))
	w.u32(flags)
	w.u32(o.Background)
	w.str(o.Title)
	r := s.request(w, 'o')
	if r == nil {
		return nil, ErrNoDesktop
	}
	r.u32()
	if res := int32(r.u32()); res != 0 {
		return nil, fmt.Errorf("ui: cannot open a window (%d; is the desktop running?)", res)
	}
	a.width, a.height = int(r.u32()), int(r.u32())
	s.apps[a.id] = a
	a.render()
	return a, nil
}

// Run opens a window and answers it (and any other window the program opens)
// until all are closed. Inside a handler it only opens the window.
func Run(o Options) error {
	a, err := Open(o)
	if err != nil {
		return err
	}
	if a.s.running {
		return nil
	}
	return a.s.loop()
}

func (s *session) loop() error {
	s.running = true
	defer func() { s.running = false }()
	for len(s.apps) > 0 {
		s.mu.Lock()
		events, work, gone := s.events, s.work, s.gone
		s.events, s.work = nil, nil
		s.mu.Unlock()
		for _, f := range work {
			f()
		}
		for _, m := range events {
			r := &reader{b: m[1:]}
			if a := s.apps[r.u32()]; a != nil {
				a.event(r)
			}
		}
		idle := true
		for _, a := range s.apps {
			s.mu.Lock()
			dirty := a.dirty
			a.dirty = false
			s.mu.Unlock()
			if dirty {
				a.render()
				idle = false
			}
		}
		if gone {
			return ErrNoDesktop
		}
		s.mu.Lock()
		idle = idle && len(s.events) == 0 && len(s.work) == 0
		s.mu.Unlock()
		if idle && len(events) == 0 && len(work) == 0 {
			<-s.wake
		}
	}
	return nil
}

func (a *App) canvasSize(key any, w *Widget) [2]float64 {
	if size, ok := a.canvases[key]; ok {
		return size
	}
	size := [2]float64{100, 100}
	if f, ok := number(w.props["width"]); ok {
		size[0] = f
	}
	if f, ok := number(w.props["height"]); ok {
		size[1] = f
	}
	return size
}

func canvasKey(w *Widget, index int) any {
	if k, ok := w.props["key"]; ok {
		return k
	}
	return index
}

// encode writes w and its children in pre-order; handlers[i] is the widget at place i.
func (a *App) encode(out *writer, w *Widget, handlers *[]*Widget) {
	index := len(*handlers)
	*handlers = append(*handlers, w)
	out.name(w.kind)
	at := len(*out)
	out.u16(0)
	out.u16(uint16(len(w.children)))
	n := 0
	for k, v := range w.props {
		if prop(out, k, v) {
			n++
		}
	}
	if w.onPointer != nil || w.onWheel != nil {
		prop(out, "pointer", 1)
		n++
	}
	if w.onClick != nil && (w.kind == "column" || w.kind == "row" || w.kind == "scroll" || w.kind == "card") {
		prop(out, "clickable", 1)
		n++
	}
	if w.draw != nil {
		size := a.canvasSize(canvasKey(w, index), w)
		p := &Painter{}
		w.draw(p, size[0], size[1])
		out.name("draw")
		out.u8(5)
		out.bytes(p.ops)
		prop(out, "drawn", size[:])
		n += 2
	}
	binary.LittleEndian.PutUint16((*out)[at:], uint16(n))
	for _, c := range w.children {
		a.encode(out, c, handlers)
	}
}

func (a *App) render() {
	if a.closed {
		return
	}
	root := a.opts.View(a)
	if root == nil {
		root = Column(nil)
	}
	var w writer
	w.u8('V')
	w.u32(a.id)
	var handlers []*Widget
	a.encode(&w, root, &handlers)
	a.s.send(w)
	a.sent++
	a.views[a.sent] = handlers
	delete(a.views, a.sent-8) // events refer to recent views
}

func (a *App) event(r *reader) {
	view, typ, index, mods, flags := r.u32(), r.u32(), r.u32(), r.u32(), r.u32()
	x, y, text := r.f64(), r.f64(), r.str()
	if r.bad {
		return
	}
	var w *Widget
	if hs := a.views[view]; index < uint32(len(hs)) {
		w = hs[index]
	}
	if w == nil {
		w = &Widget{}
	}
	switch typ {
	case eventClick:
		if w.onClick != nil {
			w.onClick()
		}
	case eventToggle:
		if w.onToggle != nil {
			w.onToggle(x != 0)
		}
	case eventSlide:
		if w.onSlide != nil {
			w.onSlide(x, flags&1 != 0)
		}
	case eventChoose:
		if w.onChoose != nil {
			w.onChoose(int(x))
		}
	case eventText:
		if w.onText != nil {
			w.onText(text)
		}
	case eventSubmit:
		if w.onSubmit != nil {
			w.onSubmit(text)
		}
	case eventKey:
		k := Key{Name: text, Shift: mods&1 != 0, Ctrl: mods&2 != 0, Alt: mods&4 != 0}
		if flags&8 != 0 {
			if a.opts.OnKeyUp != nil {
				a.opts.OnKeyUp(k, a)
			}
			break
		}
		used := a.opts.OnKey != nil && a.opts.OnKey(k, a)
		if !used && text == "Escape" && a.opts.CloseOnEscape {
			a.closeRequested(false)
			return
		}
	case eventClose:
		a.closeRequested(flags&2 != 0)
		return
	case eventCanvas:
		if w.kind != "" {
			a.canvases[canvasKey(w, int(index))] = [2]float64{x, y}
		}
	case eventResize:
		a.width, a.height = int(x), int(y)
	case eventPointer:
		if w.onPointer != nil {
			w.onPointer(Pointer{X: x, Y: y, Down: flags&4 != 0, Up: flags&8 != 0, Buttons: int(flags >> 8 & 0xFF),
				Shift: mods&1 != 0, Ctrl: mods&2 != 0, Alt: mods&4 != 0})
		}
	case eventWheel:
		if w.onWheel != nil {
			w.onWheel(x)
		}
	case eventFocus:
		if index == 0xFFFFFFFF && a.opts.OnFocus != nil {
			a.opts.OnFocus(x != 0, a)
		} else if w.onFocus != nil {
			w.onFocus(x != 0, flags&16 != 0)
		}
	}
	a.Invalidate()
}

func (a *App) closeRequested(gone bool) {
	if gone {
		a.closed = true // the engine has let the window go
		delete(a.s.apps, a.id)
		if a.opts.OnClose != nil {
			a.opts.OnClose(a)
		}
		return
	}
	if a.opts.OnClose != nil && !a.opts.OnClose(a) {
		return
	}
	a.Close()
}
