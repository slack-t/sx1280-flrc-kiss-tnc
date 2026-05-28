mod kiss;

use clap::Parser;
use std::io::{Read, Write};
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;
use tun_rs::DeviceBuilder;

const RECONNECT_DELAY: Duration = Duration::from_secs(5);

#[derive(Parser, Debug)]
#[command(author, version, about = "KISS TNC ↔ tun0 bridge in Rust", long_about = None)]
struct Args {
    #[arg(long, default_value = "/dev/ttyACM0", help = "Serial port")]
    port: String,

    #[arg(long, default_value_t = 921600, help = "Baud rate")]
    baud: u32,

    #[arg(long, help = "IP/prefix for tun interface, e.g. 10.0.0.1/30")]
    addr: String,

    #[arg(long, default_value_t = 492, help = "MTU, must match firmware IP_MTU")]
    mtu: u16,

    #[arg(long, default_value = "tun0", help = "TUN interface name")]
    name: String,

    #[arg(
        long,
        help = "Log IPv4 header details for packets sent to and from the TUN"
    )]
    debug_ip: bool,

    #[arg(
        long,
        help = "Suppress per-packet and lifecycle logs for benchmark runs"
    )]
    quiet: bool,
}

#[derive(Debug, Clone, Copy)]
enum Direction {
    TunToRadio,
    RadioToTun,
}

impl Direction {
    fn label(self) -> &'static str {
        match self {
            Direction::TunToRadio => "tun->radio",
            Direction::RadioToTun => "radio->tun",
        }
    }
}

fn log_info(quiet: bool, message: impl AsRef<str>) {
    if !quiet {
        println!("{}", message.as_ref());
    }
}

fn describe_ipv4_packet(pkt: &[u8]) -> String {
    if pkt.len() < 20 {
        return format!("non-ip len={}", pkt.len());
    }

    let version = pkt[0] >> 4;
    let ihl = usize::from(pkt[0] & 0x0F) * 4;
    if version != 4 || ihl < 20 || pkt.len() < ihl {
        return format!("non-ip len={}", pkt.len());
    }

    let total_len = u16::from_be_bytes([pkt[2], pkt[3]]);
    let ident = u16::from_be_bytes([pkt[4], pkt[5]]);
    let frag_field = u16::from_be_bytes([pkt[6], pkt[7]]);
    let flags = frag_field >> 13;
    let frag_offset = (frag_field & 0x1FFF) * 8;
    let proto = pkt[9];

    let proto_name = match proto {
        1 => "ICMP".to_string(),
        6 => "TCP".to_string(),
        17 => "UDP".to_string(),
        other => other.to_string(),
    };
    let mut summary = format!(
        "ipv4 len={} id=0x{:04x} proto={}",
        total_len, ident, proto_name
    );

    if flags & 0x2 != 0 {
        summary.push_str(" DF");
    }
    if flags & 0x1 != 0 {
        summary.push_str(" MF");
    }
    if frag_offset != 0 {
        summary.push_str(&format!(" frag_off={}", frag_offset));
    }
    if proto == 1 && pkt.len() >= ihl + 2 {
        summary.push_str(&format!(" icmp={}/{}", pkt[ihl], pkt[ihl + 1]));
    }

    summary
}

fn log_packet(direction: Direction, pkt: &[u8], debug_ip: bool) {
    if debug_ip {
        println!(
            "[kiss_tun] {}: {}",
            direction.label(),
            describe_ipv4_packet(pkt)
        );
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    // Parse IP and prefix.
    let parts: Vec<&str> = args.addr.split('/').collect();
    let ip_addr: Ipv4Addr = parts[0].parse()?;
    let prefix_len = if parts.len() > 1 {
        parts[1].parse::<u8>().unwrap_or(30)
    } else {
        30
    };

    log_info(
        args.quiet,
        format!("[kiss_tun] Creating TUN interface {} ...", args.name),
    );
    let dev = DeviceBuilder::new()
        .name(&args.name)
        .ipv4(ip_addr, prefix_len, None)
        .mtu(args.mtu)
        .build_sync()?;

    dev.enabled(true)?;
    log_info(
        args.quiet,
        format!(
            "[kiss_tun] {} up - {}/{}  MTU {}",
            args.name, ip_addr, prefix_len, args.mtu
        ),
    );

    // Share the TUN device via Arc so both threads can access it
    let tun = Arc::new(dev);

    log_info(args.quiet, "[kiss_tun] Running - Ctrl-C to stop");

    loop {
        log_info(
            args.quiet,
            format!("[kiss_tun] Connecting to {} ...", args.port),
        );
        match serialport::new(&args.port, args.baud)
            .timeout(Duration::from_millis(500))
            .open()
        {
            Ok(port) => {
                log_info(args.quiet, "[kiss_tun] Connected.");

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
                let quiet_t1 = args.quiet;
                let quiet_t2 = args.quiet;
                let debug_ip_t1 = args.debug_ip;
                let debug_ip_t2 = args.debug_ip;

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
                                log_info(
                                    quiet_t1,
                                    format!(
                                        "[kiss_tun] {} -> radio: sending {} bytes",
                                        tun_name_t1, n
                                    ),
                                );
                                log_packet(Direction::TunToRadio, &packet_buf[..n], debug_ip_t1);
                                kiss::kiss_encode(&packet_buf[..n], &mut encoded_buf);
                                if let Err(e) = ser_writer.write_all(&encoded_buf) {
                                    eprintln!("[kiss_tun] tun->radio serial write error: {}", e);
                                    stop_t1.store(true, Ordering::Relaxed);
                                    break;
                                }
                                // Pacing: prevents flooding USB CDC
                                thread::sleep(Duration::from_millis(5));
                            }
                            Err(e) => {
                                if !stop_t1.load(Ordering::Relaxed) {
                                    eprintln!("[kiss_tun] tun->radio TUN read error: {}", e);
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
                                            log_info(
                                                quiet_t2,
                                                format!(
                                                    "[kiss_tun] radio -> {}: injecting {} bytes",
                                                    tun_name_t2,
                                                    decoded_buf.len()
                                                ),
                                            );
                                            log_packet(
                                                Direction::RadioToTun,
                                                &decoded_buf,
                                                debug_ip_t2,
                                            );
                                            if let Err(e) = tun_writer.send(&decoded_buf) {
                                                eprintln!(
                                                    "[kiss_tun] radio->tun: dropped invalid IP packet or write error: {}",
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
                                    eprintln!("[kiss_tun] radio->tun serial read error: {}", e);
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

                eprintln!(
                    "[kiss_tun] Connection lost - retrying in {} s ...",
                    RECONNECT_DELAY.as_secs()
                );
                thread::sleep(RECONNECT_DELAY);
            }
            Err(e) => {
                eprintln!(
                    "[kiss_tun] Cannot open {}: {} - retrying in {} s ...",
                    args.port,
                    e,
                    RECONNECT_DELAY.as_secs()
                );
                thread::sleep(RECONNECT_DELAY);
            }
        }
    }
}
