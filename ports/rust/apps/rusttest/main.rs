// Nocturne OS Rust std smoke test. Everything NAK-style code needs from std:
// printing, collections, Arc + Mutex + threads, OnceLock, env, fs under /tmp,
// Instant, CString/CStr, thread-locals. Exits with 0 and prints "rusttest: ok"
// when every check passed; otherwise it prints the failures and exits with 1.

use std::collections::{BTreeMap, BinaryHeap, HashMap, HashSet, VecDeque};
use std::ffi::{CStr, CString};
use std::io::{Read, Seek, SeekFrom, Write};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock, RwLock};
use std::thread;
use std::time::{Duration, Instant};

static GLOBAL: OnceLock<String> = OnceLock::new();
static COUNTER: AtomicUsize = AtomicUsize::new(0);

thread_local! {
    static LOCAL: std::cell::RefCell<Vec<u32>> = const { std::cell::RefCell::new(Vec::new()) };
}

struct Checks {
    failed: Vec<String>,
    passed: usize,
}

impl Checks {
    fn check(&mut self, name: &str, ok: bool) {
        if ok {
            self.passed += 1;
            println!("ok   {name}");
        } else {
            self.failed.push(name.to_string());
            println!("FAIL {name}");
        }
    }
}

fn main() {
    let mut c = Checks { failed: Vec::new(), passed: 0 };

    println!("rusttest: hello from Rust on Nocturne ({} {})", std::env::consts::OS, std::env::consts::ARCH);
    eprintln!("rusttest: this line goes to stderr");

    // Collections.
    let mut v: Vec<u64> = (0..1000).map(|i| i * i).collect();
    v.sort_by(|a, b| b.cmp(a));
    c.check("vec sort", v[0] == 999 * 999 && v[999] == 0);

    let mut map: HashMap<String, usize> = HashMap::new();
    for i in 0..500 {
        map.insert(format!("key{i}"), i);
    }
    c.check("hashmap", map.len() == 500 && map["key123"] == 123);

    let mut tree: BTreeMap<u32, &str> = BTreeMap::new();
    tree.insert(3, "three");
    tree.insert(1, "one");
    tree.insert(2, "two");
    c.check("btreemap order", tree.keys().copied().collect::<Vec<_>>() == [1, 2, 3]);

    let set: HashSet<u32> = [1, 2, 3, 2, 1].into_iter().collect();
    let mut heap = BinaryHeap::from(vec![5, 1, 9, 3]);
    let mut dq: VecDeque<u8> = VecDeque::new();
    dq.push_back(1);
    dq.push_front(0);
    c.check("hashset/heap/deque", set.len() == 3 && heap.pop() == Some(9) && dq[0] == 0);

    let big = vec![7u8; 8 << 20];
    c.check("8 MiB allocation", big.iter().all(|&b| b == 7));
    drop(big);

    // Alignment above the C library's 16 bytes.
    #[repr(align(64))]
    struct Aligned([u8; 64]);
    let boxed = Box::new(Aligned([1; 64]));
    c.check("align 64 allocation", (&*boxed as *const Aligned as usize) % 64 == 0);

    // Strings and formatting.
    let s = format!("{:>8.3}|{:#x}|{:?}", 3.14159f64, 255, "q\"uote");
    c.check("format", s == "   3.142|0xff|\"q\\\"uote\"");

    // OnceLock.
    let g = GLOBAL.get_or_init(|| "initialised once".to_string());
    c.check("oncelock", g == "initialised once" && GLOBAL.get().is_some());

    // Arc + Mutex over four threads, joined.
    let shared = Arc::new(Mutex::new(Vec::<usize>::new()));
    let handles: Vec<_> = (0..4)
        .map(|id| {
            let shared = Arc::clone(&shared);
            thread::spawn(move || {
                for i in 0..250 {
                    shared.lock().unwrap().push(id * 1000 + i);
                    COUNTER.fetch_add(1, Ordering::SeqCst);
                }
                LOCAL.with(|l| l.borrow_mut().push(id as u32));
                LOCAL.with(|l| l.borrow().len()) + id
            })
        })
        .collect();
    let results: Vec<usize> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    c.check("4 threads joined", results == [1, 2, 3, 4]);
    c.check("mutex vec", shared.lock().unwrap().len() == 1000);
    c.check("atomic counter", COUNTER.load(Ordering::SeqCst) == 1000);
    c.check("main thread-local untouched", LOCAL.with(|l| l.borrow().is_empty()));

    // Condvar hand-off and RwLock.
    let pair = Arc::new((Mutex::new(false), Condvar::new()));
    let pair2 = Arc::clone(&pair);
    let waker = thread::spawn(move || {
        thread::sleep(Duration::from_millis(20));
        *pair2.0.lock().unwrap() = true;
        pair2.1.notify_one();
    });
    let mut ready = pair.0.lock().unwrap();
    while !*ready {
        ready = pair.1.wait(ready).unwrap();
    }
    drop(ready);
    waker.join().unwrap();
    let rw = RwLock::new(5);
    {
        let a = rw.read().unwrap();
        let b = rw.read().unwrap();
        c.check("rwlock readers", *a + *b == 10);
    }
    *rw.write().unwrap() = 6;
    c.check("rwlock writer", *rw.read().unwrap() == 6);

    // Scoped threads and park/unpark.
    let mut parts = [0u64; 4];
    thread::scope(|scope| {
        for (i, p) in parts.iter_mut().enumerate() {
            scope.spawn(move || *p = (i as u64 + 1) * 10);
        }
    });
    c.check("scoped threads", parts == [10, 20, 30, 40]);
    let parked = thread::spawn(|| {
        thread::park();
        42
    });
    thread::sleep(Duration::from_millis(10));
    parked.thread().unpark();
    c.check("park/unpark", parked.join().unwrap() == 42);

    // Environment: the launch settings, set_var, remove_var.
    let path = std::env::var("PATH");
    println!("PATH={:?}", path);
    println!("vars: {}", std::env::vars().count());
    unsafe { std::env::set_var("RUSTTEST_VAR", "value 1") };
    c.check("env set_var", std::env::var("RUSTTEST_VAR").as_deref() == Ok("value 1"));
    c.check("env vars() lists it", std::env::vars().any(|(k, v)| k == "RUSTTEST_VAR" && v == "value 1"));
    unsafe { std::env::remove_var("RUSTTEST_VAR") };
    c.check("env remove_var", std::env::var("RUSTTEST_VAR").is_err());
    c.check("env missing", std::env::var_os("RUSTTEST_NOT_THERE").is_none());
    let args: Vec<String> = std::env::args().collect();
    println!("args: {:?}", args);
    c.check("args has the program", !args.is_empty());

    // File system under /tmp.
    let dir = std::path::PathBuf::from("/tmp/rusttest");
    let _ = std::fs::remove_dir_all(&dir);
    c.check("create_dir_all", std::fs::create_dir_all(dir.join("sub")).is_ok());
    let file = dir.join("hello.txt");
    c.check("fs::write", std::fs::write(&file, b"hello nocturne\nsecond line\n").is_ok());
    let text = std::fs::read_to_string(&file).unwrap_or_default();
    c.check("fs::read_to_string", text == "hello nocturne\nsecond line\n");
    c.check("metadata", std::fs::metadata(&file).map(|m| m.is_file() && m.len() == 27).unwrap_or(false));
    {
        let mut f = std::fs::OpenOptions::new().append(true).open(&file).unwrap();
        f.write_all(b"appended\n").unwrap();
    }
    {
        let mut f = std::fs::File::open(&file).unwrap();
        f.seek(SeekFrom::Start(6)).unwrap();
        let mut word = [0u8; 8];
        f.read_exact(&mut word).unwrap();
        c.check("seek + read_exact", &word == b"nocturne");
        let mut rest = String::new();
        f.read_to_string(&mut rest).unwrap();
        c.check("append visible", rest.ends_with("appended\n"));
    }
    c.check("fs::copy", std::fs::copy(&file, dir.join("sub/copy.txt")).is_ok());
    c.check("fs::rename", std::fs::rename(dir.join("sub/copy.txt"), dir.join("sub/moved.txt")).is_ok());
    let mut names: Vec<String> = std::fs::read_dir(dir.join("sub"))
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    c.check("read_dir", names == ["moved.txt"]);
    c.check("not found error", std::fs::File::open(dir.join("nope")).map(|_| ()).unwrap_err().kind() == std::io::ErrorKind::NotFound);
    c.check("remove_file", std::fs::remove_file(&file).is_ok() && !file.exists());
    c.check("remove_dir_all", std::fs::remove_dir_all(&dir).is_ok() && !dir.exists());

    // Time.
    let start = Instant::now();
    thread::sleep(Duration::from_millis(50));
    let elapsed = start.elapsed();
    println!("slept 50 ms, measured {:?}", elapsed);
    c.check("Instant after sleep", elapsed >= Duration::from_millis(45) && elapsed < Duration::from_millis(2000));
    let now = std::time::SystemTime::now();
    c.check("SystemTime after 2020", now.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() > 1_577_836_800).unwrap_or(false));

    // C strings.
    let cs = CString::new("c string").unwrap();
    let back: &CStr = cs.as_c_str();
    c.check("CString", back.to_str() == Ok("c string") && back.to_bytes_with_nul().len() == 9);

    // Hash maps use random keys from the system.
    let a = std::hash::RandomState::new();
    let b = std::hash::RandomState::new();
    use std::hash::BuildHasher;
    c.check("RandomState differs", a.hash_one(1u32) != b.hash_one(1u32) || a.hash_one(2u32) != b.hash_one(2u32));

    // catch_unwind with panic=abort just runs the closure.
    let r = std::panic::catch_unwind(|| 7);
    c.check("catch_unwind (abort)", matches!(r, Ok(7)));

    // Parallelism query.
    println!("available_parallelism: {:?}", thread::available_parallelism());

    std::io::stdout().flush().unwrap();
    if c.failed.is_empty() {
        println!("rusttest: ok ({} checks)", c.passed);
    } else {
        println!("rusttest: {} FAILED: {:?}", c.failed.len(), c.failed);
        std::process::exit(1);
    }
}
