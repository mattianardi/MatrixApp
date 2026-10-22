use std::collections::HashMap;
use std::io::Write;
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut m: HashMap<String, usize> = HashMap::new();
    for (i, a) in args.iter().enumerate() { m.insert(a.clone(), i); }
    let v: Vec<u64> = (1..=10).map(|x| x * x).collect();
    println!("hello from Rust on NEOS: {} args, squares sum = {}", args.len(), v.iter().sum::<u64>());
    std::fs::write("/tmp/rs.txt", "written by rust\n").unwrap();
    let s = std::fs::read_to_string("/tmp/rs.txt").unwrap();
    print!("read back: {}", s);
    let h = std::thread::spawn(|| (1..=1000u64).sum::<u64>());
    println!("thread result: {}", h.join().unwrap());
    std::io::stdout().flush().unwrap();
    std::process::exit(if m.len() > 0 { 0 } else { 1 });
}
