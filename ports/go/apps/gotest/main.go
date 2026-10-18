// gotest exercises the Go port on Nocturne: output, time, goroutines on
// several CPUs, documents and folders, timers, the collector and apps;
// "gotest net" tries the network: DNS, TCP deadlines, HTTP and HTTPS.
package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

func main() {
	fmt.Println("hello from Go on Nocturne")
	for _, arg := range os.Args[1:] {
		switch {
		case strings.HasPrefix(arg, "procs="):
			n, _ := strconv.Atoi(arg[6:])
			runtime.GOMAXPROCS(n)
		case arg == "panic":
			panic("a test panic")
		case arg == "nil":
			var p *int
			fmt.Println(*p)
		case arg == "net":
			testNet()
			return
		case arg == "stdin":
			s := bufio.NewScanner(os.Stdin)
			lines := 0
			for s.Scan() {
				lines++
				fmt.Printf("stdin %d: %q\n", lines, s.Text())
			}
			fmt.Println("stdin lines:", lines, s.Err())
			return
		}
	}
	fmt.Println("GOOS/GOARCH:", runtime.GOOS, runtime.GOARCH, "CPUs:", runtime.NumCPU(), "GOMAXPROCS:", runtime.GOMAXPROCS(0))
	start := time.Now()
	fmt.Println("now:", start.Format(time.RFC3339))

	var wg sync.WaitGroup
	results := make([]int, 8)
	for i := range results {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s := 0
			for j := 0; j < 50_000_000; j++ {
				s += j % (i + 1)
			}
			results[i] = s
		}()
	}
	wg.Wait()
	fmt.Println("goroutines:", results[7], "in", time.Since(start).Round(time.Millisecond))

	t := time.Now()
	time.Sleep(250 * time.Millisecond)
	fmt.Println("sleep 250ms took", time.Since(t).Round(time.Millisecond))

	dir := filepath.Join(os.TempDir(), "gotest")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fmt.Println("mkdir:", err)
	}
	p := filepath.Join(dir, "a.txt")
	if err := os.WriteFile(p, []byte("document written by Go\n"), 0o644); err != nil {
		fmt.Println("write:", err)
	}
	f, err := os.OpenFile(p, os.O_APPEND|os.O_WRONLY, 0)
	if err == nil {
		fmt.Fprintln(f, "appended line")
		f.Close()
	}
	b, err := os.ReadFile(p)
	fmt.Printf("read back %q err=%v\n", b, err)
	fi, err := os.Stat(p)
	if err == nil {
		fmt.Println("stat:", fi.Name(), fi.Size(), fi.Mode(), fi.ModTime().Format(time.TimeOnly))
	}
	entries, err := os.ReadDir("/bin")
	fmt.Println("/bin entries:", len(entries), err)
	wd, _ := os.Getwd()
	fmt.Println("folder:", wd)
	_, err = os.Stat("/no/such/thing")
	fmt.Println("missing:", err, os.IsNotExist(err))

	ch := make(chan string)
	go func() { time.Sleep(100 * time.Millisecond); ch <- "ping" }()
	select {
	case m := <-ch:
		fmt.Println("channel:", m)
	case <-time.After(2 * time.Second):
		fmt.Println("channel: timeout")
	}

	var keep [][]byte
	for i := 0; i < 300; i++ {
		keep = append(keep, make([]byte, 1<<20))
		if len(keep) > 16 {
			keep = keep[1:]
		}
	}
	runtime.GC()
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	fmt.Println("GC cycles:", ms.NumGC, "heap MB:", ms.HeapAlloc>>20, "sys MB:", ms.Sys>>20)

	out, err := exec.Command("echo", "launched", "by", "Go").Output()
	fmt.Printf("exec: %q err=%v\n", out, err)

	fmt.Println("total", time.Since(start).Round(time.Millisecond))
}

func testNet() {
	ifs, err := net.Interfaces()
	for _, ifi := range ifs {
		addrs, _ := ifi.Addrs()
		fmt.Println("interface:", ifi.Name, ifi.HardwareAddr, ifi.Flags, addrs)
	}
	if err != nil {
		fmt.Println("interfaces:", err)
	}

	t := time.Now()
	ips, err := net.LookupIP("example.com")
	fmt.Println("lookup example.com:", ips, err, time.Since(t).Round(time.Millisecond))
	_, err = net.LookupHost("no-such-name.invalid")
	fmt.Println("lookup missing:", err)

	// Go's own DNS client, over UDP to the system resolver.
	r := &net.Resolver{PreferGo: true}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	mx, err := r.LookupMX(ctx, "gmail.com")
	cancel()
	if len(mx) > 0 {
		fmt.Println("MX gmail.com:", len(mx), mx[0].Host, err)
	} else {
		fmt.Println("MX gmail.com:", err)
	}

	// A read deadline on a quiet connection.
	c, err := net.DialTimeout("tcp", "example.com:80", 10*time.Second)
	if err == nil {
		fmt.Println("dial:", c.LocalAddr(), "->", c.RemoteAddr())
		c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		t = time.Now()
		_, err = c.Read(make([]byte, 16))
		ne, _ := err.(net.Error)
		fmt.Println("read deadline:", ne != nil && ne.Timeout(), time.Since(t).Round(10*time.Millisecond))
		c.Close()
	} else {
		fmt.Println("dial:", err)
	}

	for _, url := range []string{"http://example.com/", "https://example.com/"} {
		t = time.Now()
		resp, err := http.Get(url)
		if err != nil {
			fmt.Println("GET", url, err)
			continue
		}
		b, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		title := ""
		if i := strings.Index(string(b), "<title>"); i >= 0 {
			if j := strings.Index(string(b[i:]), "</title>"); j > 0 {
				title = string(b[i+7 : i+j])
			}
		}
		proto := ""
		if resp.TLS != nil {
			proto = tls.VersionName(resp.TLS.Version)
		}
		fmt.Println("GET", url, resp.Status, len(b), "bytes", proto, title, err, time.Since(t).Round(time.Millisecond))
	}
}
