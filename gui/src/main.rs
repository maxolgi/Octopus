#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

//! Octopus GUI — hosts the C engine in-process and serves the web
//! control surface. No subprocess, no OSC port juggling.
//!
//! `--no-gui` runs headless (engine + web server, Ctrl+C to quit).

use eframe::egui;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use octopus_engine::web::WebServer;
use octopus_engine::{Engine, Options};

fn default_state_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
        .join("octopus_state.bin")
}

// ============================================================
// Headless mode (--no-gui)
// ============================================================

fn attach_console() {
    // Release builds use windows_subsystem = "windows" (no console).
    // When launched from a terminal, attach to its console so
    // engine output and Ctrl+C work in --no-gui mode.
    #[cfg(windows)]
    unsafe {
        // AttachConsole is not exposed by winapi 0.3 — declare it directly.
        extern "system" {
            fn AttachConsole(dw_process_id: u32) -> i32;
        }
        const ATTACH_PARENT_PROCESS: u32 = 0xFFFF_FFFF;
        if AttachConsole(ATTACH_PARENT_PROCESS) != 0 {
            winapi::um::processenv::SetStdHandle(winapi::um::winbase::STD_OUTPUT_HANDLE, {
                let handle =
                    winapi::um::processenv::GetStdHandle(winapi::um::winbase::STD_OUTPUT_HANDLE);
                handle
            });
            // Re-open C-level stdout/stderr so fprintf(stderr) in the C
            // engine lands in the console.
            let mode = "w";
            libc_freopen("CONOUT$", mode, 1);
            libc_freopen("CONOUT$", mode, 2);
        }
    }
}

#[cfg(windows)]
unsafe fn libc_freopen(path: &str, mode: &str, fd: i32) {
    // _wfreopen_s-style reopen via libc's freopen on the raw fds.
    // The C runtime fds 1/2 map to stdout/stderr.
    extern "C" {
        fn _wfreopen(
            path: *const u16,
            mode: *const u16,
            file: *mut std::ffi::c_void,
        ) -> *mut std::ffi::c_void;
        fn __acrt_iob_func(fd: i32) -> *mut std::ffi::c_void;
    }
    let mut wide: Vec<u16> = path.encode_utf16().collect();
    wide.push(0);
    let mut wmode: Vec<u16> = mode.encode_utf16().collect();
    wmode.push(0);
    let _ = _wfreopen(wide.as_ptr(), wmode.as_ptr(), __acrt_iob_func(fd));
}

fn print_help() {
    eprintln!("Usage: octopus_gui [options]");
    eprintln!();
    eprintln!("Options:");
    eprintln!("  --no-gui         Headless: engine + web server, no window (Ctrl+C to quit)");
    eprintln!("  --osc-port <n>   OSC UDP input port (default: 8000, 0 disables)");
    eprintln!("  --http-port <n>  Web GUI HTTP port (default: 8088, WS = port+1)");
    eprintln!("  --out-a <id>     MIDI output A device ID");
    eprintln!("  --out-b <id>     MIDI output B device ID");
    eprintln!("  --in <id>        MIDI input device ID");
    eprintln!("  --state <path>   State file (default: octopus_state.bin next to the exe)");
    eprintln!("  --autosave       Save state on exit");
    eprintln!("  --list-midi      List MIDI devices and exit");
    eprintln!("  --help           Show this help");
}

fn run_headless(args: &[String]) -> i32 {
    let mut opts = Options::default();
    opts.state_file = Some(default_state_path());
    let mut http_port: u16 = 8088;
    let mut autosave = false;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--osc-port" if i + 1 < args.len() => {
                opts.osc_udp_port = args[i + 1].parse().unwrap_or(8000);
                i += 1;
            }
            "--http-port" if i + 1 < args.len() => {
                http_port = args[i + 1].parse().unwrap_or(8088);
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
            _ => {}
        }
        i += 1;
    }

    let engine = match Engine::start(&opts) {
        Ok(e) => Arc::new(e),
        Err(e) => {
            eprintln!("Failed to start engine: {}", e);
            return 1;
        }
    };

    let mut web_server = match WebServer::start(engine.clone(), http_port, http_port + 1) {
        Ok(ws) => ws,
        Err(e) => {
            eprintln!("Failed to start web server: {}", e);
            engine.stop();
            return 1;
        }
    };

    eprintln!("Press Ctrl+C to stop.");

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
    0
}

struct OctopusApp {
    osc_port: String,
    http_port: String,
    autosave: bool,
    out_a: i32,
    out_b: i32,
    midi_in: i32,
    devices_out: Vec<(i32, String)>,
    devices_in: Vec<(i32, String)>,
    engine: Option<Arc<Engine>>,
    web_server: Option<WebServer>,
    stopped: bool,
    error: String,
}

impl OctopusApp {
    fn new() -> Self {
        let mut app = Self {
            osc_port: "8000".to_string(),
            http_port: "8088".to_string(),
            autosave: false,
            out_a: -1,
            out_b: -1,
            midi_in: -1,
            devices_out: vec![(-1, "<none>".into())],
            devices_in: vec![(-1, "<none>".into())],
            engine: None,
            web_server: None,
            stopped: false,
            error: String::new(),
        };
        app.refresh_devices();
        app
    }

    fn refresh_devices(&mut self) {
        let (outs, ins) = octopus_engine::list_midi_devices();
        self.devices_out = if outs.is_empty() {
            vec![(-1, "<none>".into())]
        } else {
            outs
        };
        self.devices_in = if ins.is_empty() {
            vec![(-1, "<none>".into())]
        } else {
            ins
        };
    }

    fn running(&self) -> bool {
        self.engine.is_some()
    }

    fn start(&mut self) {
        if self.running() || self.stopped {
            return;
        }
        let osc_port: u16 = self.osc_port.parse().unwrap_or(8000);
        let http_port: u16 = self.http_port.parse().unwrap_or(8088);

        let engine = match Engine::start(&Options {
            osc_udp_port: osc_port,
            out_a: self.out_a,
            out_b: self.out_b,
            midi_in: self.midi_in,
            state_file: Some(default_state_path()),
        }) {
            Ok(e) => Arc::new(e),
            Err(e) => {
                self.error = format!("Engine failed to start: {}", e);
                return;
            }
        };

        match WebServer::start(engine.clone(), http_port, http_port + 1) {
            Ok(ws) => {
                self.web_server = Some(ws);
                self.engine = Some(engine);
                self.error.clear();
            }
            Err(e) => {
                engine.stop();
                self.stopped = true;
                self.error = e;
            }
        }
    }

    fn stop(&mut self) {
        if let Some(mut ws) = self.web_server.take() {
            ws.stop();
        }
        if let Some(engine) = self.engine.take() {
            if self.autosave {
                engine.save_state();
            }
            engine.stop();
        }
        self.stopped = true;
    }

    fn open_browser(&self) {
        let port: u16 = self.http_port.parse().unwrap_or(8088);
        let url = format!("http://localhost:{}", port);
        #[cfg(target_os = "linux")]
        {
            let _ = std::process::Command::new("xdg-open").arg(url).spawn();
        }
        #[cfg(target_os = "windows")]
        {
            let _ = std::process::Command::new("cmd")
                .args(["/C", "start", &url])
                .spawn();
        }
        #[cfg(target_os = "macos")]
        {
            let _ = std::process::Command::new("open").arg(url).spawn();
        }
    }
}

impl eframe::App for OctopusApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Stop everything before the window closes — eframe may skip Drop
        if ctx.input(|i| i.viewport().close_requested()) {
            self.stop();
        }

        let running = self.running();

        // The web page can send /quit — reflect that here
        if running {
            if let Some(engine) = &self.engine {
                if engine.wants_quit() {
                    self.stop();
                }
            }
        }

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(8.0);

            ui.heading("Genoqs Octopus");
            ui.add_space(8.0);

            ui.horizontal(|ui| {
                ui.label("OSC UDP Port:");
                ui.add_enabled(
                    !running,
                    egui::TextEdit::singleline(&mut self.osc_port).desired_width(60.0),
                );
                ui.add_space(16.0);
                ui.label("Web Port:");
                ui.add_enabled(
                    !running,
                    egui::TextEdit::singleline(&mut self.http_port).desired_width(60.0),
                );
            });

            ui.add_space(8.0);
            ui.separator();
            ui.add_space(8.0);

            // --- MIDI section (live: switching while running reconnects
            //     the ALSA/winmm ports via OSC, no restart needed) ---
            let mut midi_change: Option<(&str, i32)> = None;
            ui.horizontal(|ui| {
                ui.label("MIDI Out A:");
                let name = self
                    .devices_out
                    .iter()
                    .find(|(id, _)| *id == self.out_a)
                    .map(|(_, n)| n.as_str())
                    .unwrap_or("<none>");
                egui::ComboBox::from_id_salt("out_a")
                    .selected_text(name)
                    .show_ui(ui, |ui| {
                        for (id, name) in &self.devices_out {
                            if ui.selectable_value(&mut self.out_a, *id, name).changed() {
                                midi_change = Some(("/midi/out_a", *id));
                            }
                        }
                    });
            });

            ui.horizontal(|ui| {
                ui.label("MIDI Out B:");
                let name = self
                    .devices_out
                    .iter()
                    .find(|(id, _)| *id == self.out_b)
                    .map(|(_, n)| n.as_str())
                    .unwrap_or("<none>");
                egui::ComboBox::from_id_salt("out_b")
                    .selected_text(name)
                    .show_ui(ui, |ui| {
                        for (id, name) in &self.devices_out {
                            if ui.selectable_value(&mut self.out_b, *id, name).changed() {
                                midi_change = Some(("/midi/out_b", *id));
                            }
                        }
                    });
            });

            ui.horizontal(|ui| {
                ui.label("MIDI Input:");
                let name = self
                    .devices_in
                    .iter()
                    .find(|(id, _)| *id == self.midi_in)
                    .map(|(_, n)| n.as_str())
                    .unwrap_or("<none>");
                egui::ComboBox::from_id_salt("midi_in")
                    .selected_text(name)
                    .show_ui(ui, |ui| {
                        for (id, name) in &self.devices_in {
                            if ui.selectable_value(&mut self.midi_in, *id, name).changed() {
                                midi_change = Some(("/midi/in", *id));
                            }
                        }
                    });
            });

            if let (Some((addr, id)), Some(engine)) = (midi_change, &self.engine) {
                engine.send_osc(&octopus_engine::osc_int(addr, id));
            }

            ui.horizontal(|ui| {
                if ui
                    .add_enabled(!running, egui::Button::new("Refresh Devices"))
                    .clicked()
                {
                    self.refresh_devices();
                }
            });

            ui.add_space(8.0);
            ui.separator();
            ui.add_space(8.0);

            // --- Engine + web server (one Start per application run) ---
            ui.horizontal(|ui| {
                if !running && !self.stopped {
                    if ui
                        .add(egui::Button::new("Start").min_size(egui::vec2(100.0, 28.0)))
                        .clicked()
                    {
                        self.start();
                    }
                }
                let color = if running {
                    egui::Color32::from_rgb(0, 180, 0)
                } else {
                    egui::Color32::from_rgb(180, 0, 0)
                };
                ui.colored_label(
                    color,
                    format!("● {}", if running { "Running" } else { "Stopped" }),
                );
            });

            ui.horizontal(|ui| {
                ui.checkbox(&mut self.autosave, "Save state on exit");
            });

            if running {
                ui.add_space(4.0);
                ui.horizontal(|ui| {
                    if ui.button("Open Browser").clicked() {
                        self.open_browser();
                    }
                    let port: u16 = self.http_port.parse().unwrap_or(8088);
                    ui.label(format!("http://localhost:{}", port));
                });
            }

            if self.stopped {
                ui.add_space(4.0);
                ui.colored_label(
                    egui::Color32::from_rgb(200, 160, 60),
                    "Engine stopped — restart the application to run it again.",
                );
            }

            if !self.error.is_empty() {
                ui.add_space(8.0);
                ui.colored_label(egui::Color32::from_rgb(200, 80, 80), &self.error);
            }
        });

        if running {
            ctx.request_repaint_after(std::time::Duration::from_millis(500));
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    if args.iter().any(|a| a == "--help" || a == "-h") {
        print_help();
        return;
    }

    if args.iter().any(|a| a == "--list-midi" || a == "-l") {
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

    if args.iter().any(|a| a == "--no-gui" || a == "--nogui") {
        attach_console();
        std::process::exit(run_headless(&args));
    }

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([440.0, 480.0])
            .with_resizable(true),
        ..Default::default()
    };
    let _ = eframe::run_native(
        "Octopus",
        options,
        Box::new(|_cc| Ok(Box::new(OctopusApp::new()))),
    );
}
