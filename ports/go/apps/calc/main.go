// calc: the Calculator of the desktop, a window of nocturne/ui widgets.
// Buttons or the keyboard: digits, . + - * / %, Enter or =, Backspace, Escape or C.
package main

import (
	"math"
	"os"
	"strconv"
	"strings"

	"nocturne/ui"
)

var keys = [5][4]string{
	{"C", "+/-", "%", "/"},
	{"7", "8", "9", "*"},
	{"4", "5", "6", "-"},
	{"1", "2", "3", "+"},
	{"0", ".", "<", "="},
}

// what the keys show instead of their names
var shown = map[string]string{"/": "÷", "*": "×", "-": "−", "+/-": "±"}

type calc struct {
	display string // the number being typed or the last result
	line    string // the calculation so far, above it
	acc     float64
	op      string
	fresh   bool // the next digit starts a new number
}

func format(v float64) string {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return "Error"
	}
	return strconv.FormatFloat(v, 'g', 12, 64)
}

func value(s string) float64 {
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0
	}
	return v
}

func apply(a, b float64, op string) float64 {
	switch op {
	case "+":
		return a + b
	case "-":
		return a - b
	case "*":
		return a * b
	case "/":
		if b == 0 {
			return math.NaN()
		}
		return a / b
	}
	return b
}

func (c *calc) press(k string) {
	switch {
	case k >= "0" && k <= "9" && len(k) == 1, k == ".":
		if c.fresh || c.display == "0" || c.display == "Error" {
			c.display = k
			if k == "." {
				c.display = "0."
			}
			c.fresh = false
		} else if len(c.display) < 16 && !(k == "." && strings.Contains(c.display, ".")) {
			c.display += k
		}
	case k == "C":
		*c = calc{display: "0", fresh: true}
	case k == "<":
		if !c.fresh && len(c.display) > 1 {
			c.display = c.display[:len(c.display)-1]
		} else {
			c.display = "0"
		}
	case k == "+/-":
		c.display = format(-value(c.display))
	case k == "%":
		v := value(c.display) / 100
		if c.op == "+" || c.op == "-" {
			v *= c.acc
		}
		c.display = format(v)
	case k == "=":
		if c.op != "" {
			v := apply(c.acc, value(c.display), c.op)
			c.line = format(c.acc) + " " + shownOp(c.op) + " " + c.display + " ="
			c.display = format(v)
			c.op = ""
			c.acc = v
		}
		c.fresh = true
	default: // an operator
		v := value(c.display)
		switch {
		case c.op != "" && !c.fresh:
			c.acc = apply(c.acc, v, c.op)
		case c.op == "":
			c.acc = v
		}
		c.op = k
		c.line = format(c.acc) + " " + shownOp(k)
		c.display = format(c.acc)
		c.fresh = true
	}
}

func shownOp(op string) string {
	if s, ok := shown[op]; ok {
		return s
	}
	return op
}

func (c *calc) key(r, k int) *ui.Widget {
	name := keys[r][k]
	p := ui.Props{"width": "fill", "height": "fill", "grow": 1, "size": 18, "radius": 12}
	switch {
	case name == "=":
		p["background"] = ui.ThemeColour("warning")
		p["colour"] = ui.ThemeColour("background")
	case k == 3:
		p["variant"] = "primary"
	case r == 0:
		p["background"] = ui.ThemeColour("surfaceActive")
	}
	press := func() { c.press(name) }
	if name == "<" {
		p["icon"] = "backspace"
		return ui.Button("", press, p)
	}
	return ui.Button(shownOp(name), press, p)
}

func (c *calc) view(a *ui.App) *ui.Widget {
	size := 34.0
	if ui.Measure(c.display, size, 1).Width > float64(a.Width())-56 {
		size = 22
	}
	screen := ui.Column(ui.Props{"padding": []float64{12, 16}, "gap": 4, "background": ui.ThemeColour("sidebar"),
		"radius": ui.Theme("radius"), "height": 92, "justify": "end"},
		ui.Caption(c.line, ui.Props{"textAlign": "end", "wrap": false}),
		ui.Text(c.display, ui.Props{"size": size, "weight": 1, "textAlign": "end", "wrap": false}))
	rows := []*ui.Widget{screen}
	for r := range keys {
		var row []*ui.Widget
		for k := range keys[r] {
			row = append(row, c.key(r, k))
		}
		rows = append(rows, ui.Row(ui.Props{"gap": 8, "grow": 1, "align": "stretch"}, row...))
	}
	return ui.Column(ui.Props{"padding": 12, "gap": 8, "height": "fill"}, rows...)
}

func main() {
	c := &calc{display: "0", fresh: true}
	err := ui.Run(ui.Options{
		Title: "Calculator", Width: 300, Height: 416,
		View: c.view,
		OnKey: func(k ui.Key, a *ui.App) bool {
			switch name := k.Name; {
			case name == "Enter" || name == "=":
				c.press("=")
			case name == "Backspace":
				c.press("<")
			case name == "Escape" || name == "c" || name == "C":
				c.press("C")
			case len(name) == 1 && strings.Contains("0123456789.+-*/%", name):
				c.press(name)
			default:
				return false
			}
			return true
		},
	})
	if err != nil {
		os.Stderr.WriteString(err.Error() + "\n")
		os.Exit(1)
	}
}
