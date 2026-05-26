mod kiss;

use clap::Parser;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;
use tun_rs::DeviceBuilder;

const RECONNECT_DELAY: Duration = Duration::from_secs(5);

#[derive(Parser, Debug)]
#[command(author, version, about = "KISS TNC ↔ tun0 bridge in Rust", long_about = None)]
struct Args {
    #[arg(long, default_value = "/dev/ttyACM0")]
    port: String,

    #[arg(long, default_value_t = 921600)]
    baud: u32,

    #[arg(long)]
    addr: String,

    #[arg(long, default_value_t = 496)]
    mtu: u16,

    #[arg(long, default_value = "tun0")]
    name: String,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    // Parse IP and prefix
    let parts: Vec<&str> = args.addr.split('/').collect();
    let ip_str = parts[0];
    let prefix_len = if parts.len() > 1 {
        parts[1].parse::<u8>().unwrap_or(30)
    } else {
        30
    };

    println!("[kiss_tun] Creating TUN interface {} ...", args.name);
    let dev = DeviceBuilder::new()
        .name(&args.name)
        .ipv4(ip_str, prefix_len, None)
        .mtu(args.mtu)
        .build_sync()?;

    dev.enabled(true)?;
    println!(
        "[kiss_tun] {} up — {}/{}  MTU {}",
        args.name, ip_str, prefix_len, args.mtu
    );

    // Share the TUN device via Arc so both threads can access it
    let tun = Arc::new(dev);

    println!("[kiss_tun] Running — Ctrl-C to stop");

    loop {
        println!("[kiss_tun] Connecting to {} ...", args.port);
        match serialport::new(&args.port, args.baud)
            .timeout(Duration::from_millis(500))
            .open()
        {
            Ok(port) => {
                println!("[kiss_tun] Connected.");

                let stop_signal = Arc::new(AtomicBool::new(false));

                // Clone handles for both threads
                let mut ser_reader = match port.try_clone() {
                    Ok(p) => p,
                    Err(e) => {
                        eprintln!("[kiss_tun] Failed to clone serial port: {}", e);
                        thread::sleep(RECONNECT_DELAY);
                        continue;
                    }
                };
                let mut ser_writer = port;

                let stop_t1 = Arc::clone(&stop_signal);
                let stop_t2 = Arc::clone(&stop_signal);

                let tun_reader = Arc::clone(&tun);
                let tun_writer = Arc::clone(&tun);

                let tun_name_t1 = args.name.clone();
                let tun_name_t2 = args.name.clone();

                let mtu = args.mtu;

                // Thread 1: TUN -> Radio (reads from TUN, writes to Serial)
                let t1 = thread::spawn(move || {
                    let mut packet_buf = [0u8; 65535];
                    let mut encoded_buf = Vec::new();

                    while !stop_t1.load(Ordering::Relaxed) {
                        match tun_reader.recv(&mut packet_buf) {
                            Ok(n) => {
                                if n == 0 {
                                    break;
                                }
                                if n > mtu as usize {
                                    eprintln!(
                                        "[kiss_tun] WARN dropped oversized packet ({} > {} bytes)",
                                        n, mtu
                                    );
                                    continue;
                                }
                                println!("[kiss_tun] {} → radio: sending {} bytes", tun_name_t1, n);
                                kiss::kiss_encode(&packet_buf[..n], &mut encoded_buf);
                                if let Err(e) = ser_writer.write_all(&encoded_buf) {
                                    eprintln!("[kiss_tun] tun→radio serial write error: {}", e);
                                    stop_t1.store(true, Ordering::Relaxed);
                                    break;
                                }
                                // Pacing: prevents flooding USB CDC
                                thread::sleep(Duration::from_millis(5));
                            }
                            Err(e) => {
                                if !stop_t1.load(Ordering::Relaxed) {
                                    eprintln!("[kiss_tun] tun→radio TUN read error: {}", e);
                                    stop_t1.store(true, Ordering::Relaxed);
                                }
                                break;
                            }
                        }
                    }
                });

                // Thread 2: Radio -> TUN (reads from Serial, writes to TUN)
                let t2 = thread::spawn(move || {
                    let mut decoder = kiss::KissDecoder::new(mtu as usize);
                    let mut read_buf = [0u8; 1024];
                    let mut decoded_buf = Vec::new();

                    while !stop_t2.load(Ordering::Relaxed) {
                        match ser_reader.read(&mut read_buf) {
                            Ok(0) => {
                                eprintln!("[kiss_tun] Serial connection EOF");
                                stop_t2.store(true, Ordering::Relaxed);
                                break;
                            }
                            Ok(n) => {
                                for &byte in &read_buf[..n] {
                                    if decoder.feed(byte, &mut decoded_buf) {
                                        if !decoded_buf.is_empty() {
                                            println!(
                                                "[kiss_tun] radio → {}: injecting {} bytes",
                                                tun_name_t2,
                                                decoded_buf.len()
                                            );
                                            if let Err(e) = tun_writer.send(&decoded_buf) {
                                                eprintln!(
                                                    "[kiss_tun] radio→tun: dropped invalid IP packet or write error: {}",
                                                    e
                                                );
                                            }
                                        }
                                    }
                                }
                            }
                            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {
                                continue;
                            }
                            Err(e) => {
                                if !stop_t2.load(Ordering::Relaxed) {
                                    eprintln!("[kiss_tun] radio→tun serial read error: {}", e);
                                    stop_t2.store(true, Ordering::Relaxed);
                                }
                                break;
                            }
                        }
                    }
                });

                // Wait for threads to exit
                let _ = t1.join();
                let _ = t2.join();

                println!(
                    "[kiss_tun] Connection lost — retrying in {} s ...",
                    RECONNECT_DELAY.as_secs()
                );
                thread::sleep(RECONNECT_DELAY);
            }
            Err(e) => {
                eprintln!(
                    "[kiss_tun] Cannot open {}: {} — retrying in {} s ...",
                    args.port,
                    e,
                    RECONNECT_DELAY.as_secs()
                );
                thread::sleep(RECONNECT_DELAY);
            }
        }
    }
}
