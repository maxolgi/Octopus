//! Octopus CLI — headless host: runs the C engine in-process and serves
//! the web control surface. Replaces the old cli_ui bridge + engine pair.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use octopus_engine::web::WebServer;
use octopus_engine::{Engine, Options};

fn print_help() {
    eprintln!("Usage: octopus_cli [options]");
    eprintln!();
    eprintln!("Options:");
    eprintln!("  --osc-port <n>   OSC UDP input port (default: 8000, 0 disables)");
    eprintln!("  --http-port <n>  Web GUI HTTP port (default: 8080, WS = port+1)");
    eprintln!("  --out-a <id>     MIDI output A device ID");
    eprintln!("  --out-b <id>     MIDI output B device ID");
    eprintln!("  --in <id>        MIDI input device ID");
    eprintln!("  --state <path>   State file (default: octopus_state.bin in cwd)");
    eprintln!("  --autosave       Save state on exit");
    eprintln!("  --list-midi      List MIDI devices and exit");
    eprintln!("  --help           Show this help");
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let mut opts = Options::default();
    let mut http_port: u16 = 8080;
    let mut autosave = false;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--osc-port" if i + 1 < args.len() => {
                opts.osc_udp_port = args[i + 1].parse().unwrap_or(8000);
                i += 1;
            }
            "--http-port" if i + 1 < args.len() => {
                http_port = args[i + 1].parse().unwrap_or(8080);
                i += 1;
            }
            "--out-a" if i + 1 < args.len() => {
                opts.out_a = args[i + 1].parse().unwrap_or(-1);
                i += 1;
            }
            "--out-b" if i + 1 < args.len() => {
                opts.out_b = args[i + 1].parse().unwrap_or(-1);
                i += 1;
            }
            "--in" if i + 1 < args.len() => {
                opts.midi_in = args[i + 1].parse().unwrap_or(-1);
                i += 1;
            }
            "--state" if i + 1 < args.len() => {
                opts.state_file = Some(PathBuf::from(&args[i + 1]));
                i += 1;
            }
            "--autosave" => autosave = true,
            "--list-midi" | "-l" => {
                let (outs, ins) = octopus_engine::list_midi_devices();
                println!("MIDI output devices:");
                println!("  [-1] <none>");
                for (id, name) in outs {
                    println!("  [{}] {}", id, name);
                }
                println!("MIDI input devices:");
                println!("  [-1] <none>");
                for (id, name) in ins {
                    println!("  [{}] {}", id, name);
                }
                return;
            }
            "--help" | "-h" => {
                print_help();
                return;
            }
            _ => {}
        }
        i += 1;
    }

    let engine = match Engine::start(&opts) {
        Ok(e) => Arc::new(e),
        Err(e) => {
            eprintln!("Failed to start engine: {}", e);
            std::process::exit(1);
        }
    };

    let mut web_server = match WebServer::start(engine.clone(), http_port, http_port + 1) {
        Ok(ws) => ws,
        Err(e) => {
            eprintln!("Failed to start web server: {}", e);
            engine.stop();
            std::process::exit(1);
        }
    };

    eprintln!("Press Ctrl+C to stop.");

    // Ctrl+C / SIGTERM / terminal close -> clean engine shutdown
    let should_stop = Arc::new(AtomicBool::new(false));
    {
        let should_stop = should_stop.clone();
        ctrlc::set_handler(move || {
            eprintln!("\nCaught Ctrl+C, shutting down...");
            should_stop.store(true, Ordering::SeqCst);
        })
        .expect("failed to install signal handler");
    }

    loop {
        if should_stop.load(Ordering::SeqCst) {
            break;
        }
        if engine.wants_quit() {
            eprintln!("\nQuit requested via OSC.");
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }

    eprintln!("Shutting down...");
    if autosave {
        engine.save_state();
    }
    web_server.stop();
    engine.stop();
    eprintln!("Done.");
}
