#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod web_server;

use eframe::egui;
use std::io::BufRead;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;

#[cfg(windows)]
use std::os::windows::process::CommandExt;
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;

const OSC_QUIT: &[u8] = b"/quit\0\0\0,\0\0\0";

#[derive(Clone, Copy, PartialEq)]
enum Variant {
    Octopus,
    Nemo,
}

impl Variant {
    fn binary_name(&self) -> &'static str {
        match self {
            Variant::Octopus => "octopus",
            Variant::Nemo => "nemo",
        }
    }
    fn from_str(s: &str) -> Self {
        if s.eq_ignore_ascii_case("nemo") {
            Variant::Nemo
        } else {
            Variant::Octopus
        }
    }
}

// ============================================================
// Binary path detection
// ============================================================

fn exe_name(name: &str) -> String {
    if cfg!(windows) { format!("{}.exe", name) } else { name.to_string() }
}

fn check_path(dir: &Path, name: &str) -> Option<PathBuf> {
    let p = dir.join(exe_name(name));
    if p.exists() { Some(p) } else { None }
}

fn find_sequencer(variant: Variant, exe_dir: &Path) -> Option<PathBuf> {
    let name = variant.binary_name();
    for d in [exe_dir, &exe_dir.join("build"), &exe_dir.join("..").join("build")] {
        if let Some(p) = check_path(d, name) {
            return Some(p);
        }
    }
    if let Ok(cwd) = std::env::current_dir() {
        if let Some(p) = check_path(&cwd, name) { return Some(p); }
        if let Some(p) = check_path(&cwd.join("build"), name) { return Some(p); }
    }
    None
}

// ============================================================
// MIDI device listing + parsing
// ============================================================

fn list_midi_devices(seq_path: &Path) -> Result<String, String> {
    let mut cmd = Command::new(seq_path);
    cmd.arg("--list-midi");
    cmd.stderr(Stdio::piped()).stdout(Stdio::piped());
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    let output = cmd.output().map_err(|e| e.to_string())?;
    Ok(String::from_utf8_lossy(&output.stderr).into_owned())
}

fn parse_midi_devices(output: &str) -> (Vec<(i32, String)>, Vec<(i32, String)>) {
    let mut outs = Vec::new();
    let mut ins = Vec::new();
    let mut section = 0u8;

    for line in output.lines() {
        let lower = line.to_lowercase();
        if lower.contains("output devices") { section = 1; continue; }
        if lower.contains("input devices") { section = 2; continue; }

        let trimmed = line.trim();
        if !trimmed.starts_with('[') { continue; }
        let close = match trimmed.find(']') { Some(p) => p, None => continue };
        let id: i32 = match trimmed[1..close].parse() { Ok(v) => v, Err(_) => continue };

        let name = trimmed[close + 1..].trim();
        let name = if let Some(p) = name.find("  (") {
            name[..p].trim().to_string()
        } else {
            name.to_string()
        };

        match section {
            1 => outs.push((id, name)),
            2 => ins.push((id, name)),
            _ => {}
        }
    }
    (outs, ins)
}

// ============================================================
// OSC quit
// ============================================================

fn send_osc_quit(port: u16) {
    if let Ok(sock) = std::net::UdpSocket::bind("127.0.0.1:0") {
        let _ = sock.send_to(OSC_QUIT, format!("127.0.0.1:{}", port));
    }
}

// ============================================================
// --list-midi mode
// ============================================================

fn run_list_midi(exe_dir: &Path) {
    let seq = match find_sequencer(Variant::Octopus, exe_dir) {
        Some(p) => p,
        None => {
            eprintln!("Sequencer binary not found.");
            std::process::exit(1);
        }
    };
    match list_midi_devices(&seq) {
        Ok(output) => print!("{}", output),
        Err(e) => {
            eprintln!("Failed to list MIDI devices: {}", e);
            std::process::exit(1);
        }
    }
}

// ============================================================
// --nogui mode
// ============================================================

fn run_nogui(args: &[String], exe_dir: &Path) {
    let mut variant = Variant::Octopus;
    let mut osc_port: u16 = 8000;
    let mut http_port: u16 = 8080;
    let mut host: String = "127.0.0.1".to_string();
    let mut out_a: i32 = -1;
    let mut out_b: i32 = -1;
    let mut midi_in: i32 = -1;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--variant" if i + 1 < args.len() => { variant = Variant::from_str(&args[i + 1]); i += 1; }
            "--osc-port" if i + 1 < args.len() => { osc_port = args[i + 1].parse().unwrap_or(8000); i += 1; }
            "--http-port" if i + 1 < args.len() => { http_port = args[i + 1].parse().unwrap_or(8080); i += 1; }
            "--host" if i + 1 < args.len() => { host = args[i + 1].to_string(); i += 1; }
            "--out-a" if i + 1 < args.len() => { out_a = args[i + 1].parse().unwrap_or(-1); i += 1; }
            "--out-b" if i + 1 < args.len() => { out_b = args[i + 1].parse().unwrap_or(-1); i += 1; }
            "--in" if i + 1 < args.len() => { midi_in = args[i + 1].parse().unwrap_or(-1); i += 1; }
            _ => {}
        }
        i += 1;
    }

    let is_local = host == "127.0.0.1" || host == "localhost";

    // Start web server
    let ws_port = http_port + 1;
    let mut web_server = match web_server::WebServer::start(osc_port, http_port, ws_port, &host) {
        Ok(ws) => ws,
        Err(e) => {
            eprintln!("Failed to start web server: {}", e);
            std::process::exit(1);
        }
    };

    // Spawn engine only if host is local
    let mut seq_process = None;
    if is_local {
        if let Some(seq) = find_sequencer(variant, exe_dir) {
            let mut seq_cmd = Command::new(&seq);
            seq_cmd
                .arg("--osc-port").arg(osc_port.to_string())
                .arg("--out-a").arg(out_a.to_string())
                .arg("--out-b").arg(out_b.to_string())
                .arg("--in").arg(midi_in.to_string());
            #[cfg(windows)]
            seq_cmd.creation_flags(CREATE_NO_WINDOW);

            match seq_cmd.spawn() {
                Ok(c) => seq_process = Some(c),
                Err(e) => eprintln!("Failed to start sequencer: {}", e),
            }
        } else {
            eprintln!("Sequencer binary not found. Running bridge only.");
        }
    } else {
        eprintln!("Remote host ({}), running bridge only.", host);
    }

    eprintln!("Press Ctrl+C to stop.");

    // Wait for Ctrl+C or sequencer exit
    let should_stop = std::sync::Arc::new(AtomicBool::new(false));
    let stop_clone = should_stop.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        rt.block_on(async { tokio::signal::ctrl_c().await.ok(); });
        stop_clone.store(true, Ordering::SeqCst);
    });

    loop {
        if should_stop.load(Ordering::SeqCst) { break; }
        if let Some(ref mut p) = seq_process {
            if let Ok(Some(_)) = p.try_wait() { break; }
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }

    // Cleanup
    eprintln!("\nShutting down...");
    send_osc_quit(osc_port);
    std::thread::sleep(std::time::Duration::from_millis(500));
    if let Some(mut p) = seq_process {
        let _ = p.kill();
        let _ = p.wait();
    }
    web_server.stop();
    eprintln!("Done.");
}

// ============================================================
// GUI mode
// ============================================================

struct LauncherApp {
    variant: Variant,
    osc_port: String,
    http_port: String,
    engine_host: String,
    out_a: i32,
    out_b: i32,
    midi_in: i32,
    devices_out: Vec<(i32, String)>,
    devices_in: Vec<(i32, String)>,
    seq_process: Option<Child>,
    web_server: Option<web_server::WebServer>,
    error: String,
    exe_dir: PathBuf,
    log_rx: Option<mpsc::Receiver<String>>,
    log_lines: Vec<String>,
}

impl LauncherApp {
    fn new(exe_dir: PathBuf) -> Self {
        let mut app = Self {
            variant: Variant::Octopus,
            osc_port: "8000".to_string(),
            http_port: "8080".to_string(),
            engine_host: "127.0.0.1".to_string(),
            out_a: -1,
            out_b: -1,
            midi_in: -1,
            devices_out: vec![(-1, "<none>".into())],
            devices_in: vec![(-1, "<none>".into())],
            seq_process: None,
            web_server: None,
            error: String::new(),
            exe_dir,
            log_rx: None,
            log_lines: Vec::new(),
        };
        app.refresh_devices();
        app
    }

    fn sequencer_path(&self) -> Option<PathBuf> {
        find_sequencer(self.variant, &self.exe_dir)
    }

    fn refresh_devices(&mut self) {
        let seq = match self.sequencer_path() {
            Some(p) => p,
            None => {
                self.error = "Sequencer binary not found.".into();
                return;
            }
        };
        match list_midi_devices(&seq) {
            Ok(output) => {
                let (outs, ins) = parse_midi_devices(&output);
                self.devices_out = if outs.is_empty() { vec![(-1, "<none>".into())] } else { outs };
                self.devices_in = if ins.is_empty() { vec![(-1, "<none>".into())] } else { ins };
                self.error.clear();
            }
            Err(e) => { self.error = format!("Failed to list MIDI devices: {}", e); }
        }
    }

    fn is_running(&mut self) -> bool {
        if let Some(p) = &mut self.seq_process {
            match p.try_wait() {
                Ok(Some(_)) => return false,
                Ok(None) => {}
                Err(_) => return false,
            }
        } else {
            return false;
        }
        true
    }

    fn start_engine(&mut self) {
        let seq = match self.sequencer_path() {
            Some(p) => p,
            None => { self.error = "Sequencer binary not found.".into(); return; }
        };
        let port: u16 = self.osc_port.parse().unwrap_or(8000);

        let mut seq_cmd = Command::new(&seq);
        seq_cmd
            .arg("--osc-port").arg(port.to_string())
            .arg("--out-a").arg(self.out_a.to_string())
            .arg("--out-b").arg(self.out_b.to_string())
            .arg("--in").arg(self.midi_in.to_string())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        seq_cmd.creation_flags(CREATE_NO_WINDOW);

        match seq_cmd.spawn() {
            Ok(mut child) => {
                if let Some(stderr) = child.stderr.take() {
                    let (tx, rx) = mpsc::channel::<String>();
                    std::thread::spawn(move || {
                        let reader = std::io::BufReader::new(stderr);
                        for line in reader.lines() {
                            match line {
                                Ok(l) => { if tx.send(l).is_err() { break; } }
                                Err(_) => break,
                            }
                        }
                    });
                    self.log_rx = Some(rx);
                }
                self.seq_process = Some(child);
                self.error.clear();
            }
            Err(e) => {
                self.error = format!("Failed to start sequencer: {}", e);
            }
        }
    }

    fn stop_engine(&mut self) {
        let port: u16 = self.osc_port.parse().unwrap_or(8000);
        send_osc_quit(port);
        std::thread::sleep(std::time::Duration::from_millis(500));

        if let Some(mut p) = self.seq_process.take() {
            let _ = p.kill();
            let _ = p.wait();
        }
        self.log_rx = None;
    }

    fn start_bridge(&mut self) {
        let port: u16 = self.osc_port.parse().unwrap_or(8000);
        let http_port: u16 = self.http_port.parse().unwrap_or(8080);
        let ws_port = http_port + 1;

        match web_server::WebServer::start(port, http_port, ws_port, &self.engine_host) {
            Ok(ws) => {
                self.web_server = Some(ws);
                self.error.clear();
            }
            Err(e) => {
                self.error = format!("Failed to start web server: {}", e);
            }
        }
    }

    fn stop_bridge(&mut self) {
        if let Some(mut ws) = self.web_server.take() {
            ws.stop();
        }
    }

    fn stop(&mut self) {
        self.stop_engine();
        self.stop_bridge();
    }

    fn open_browser(&self) {
        let port: u16 = self.http_port.parse().unwrap_or(8080);
        let url = format!("http://localhost:{}", port);
        #[cfg(target_os = "linux")]
        { let _ = Command::new("xdg-open").arg(url).spawn(); }
        #[cfg(target_os = "windows")]
        { let _ = Command::new("cmd").args(["/C", "start", &url]).spawn(); }
        #[cfg(target_os = "macos")]
        { let _ = Command::new("open").arg(url).spawn(); }
    }
}

impl Drop for LauncherApp {
    fn drop(&mut self) {
        self.stop();
    }
}

impl eframe::App for LauncherApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Kill sequencer before window closes — eframe may skip Drop on exit
        if ctx.input(|i| i.viewport().close_requested()) {
            self.stop();
        }

        let engine_running = self.is_running();
        let bridge_running = self.web_server.is_some();

        // Auto-detect engine crash
        if !engine_running && self.seq_process.is_some() {
            self.seq_process = None;
            self.log_rx = None;
        }

        // Drain log receiver
        if let Some(rx) = &self.log_rx {
            while let Ok(line) = rx.try_recv() {
                self.log_lines.push(line);
            }
            if self.log_lines.len() > 500 {
                let extra = self.log_lines.len() - 500;
                self.log_lines.drain(0..extra);
            }
        }

        // Console panel (bottom)
        egui::TopBottomPanel::bottom("console")
            .exact_height(180.0)
            .frame(egui::Frame::none().fill(egui::Color32::from_rgb(25, 25, 25)))
            .show(ctx, |ui| {
                ui.add_space(2.0);
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new("Console").color(egui::Color32::from_gray(120)).size(10.0));
                });
                egui::ScrollArea::vertical()
                    .stick_to_bottom(true)
                    .auto_shrink([false; 2])
                    .show(ui, |ui| {
                        for line in &self.log_lines {
                            ui.label(egui::RichText::new(line).monospace().color(egui::Color32::from_gray(200)).size(10.0));
                        }
                    });
            });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(8.0);

            ui.horizontal(|ui| {
                ui.label("Variant:");
                ui.radio_value(&mut self.variant, Variant::Octopus, "Octopus");
                ui.radio_value(&mut self.variant, Variant::Nemo, "Nemo");
            });

            ui.horizontal(|ui| {
                ui.label("OSC Port:");
                ui.add_enabled(!engine_running && !bridge_running, egui::TextEdit::singleline(&mut self.osc_port).desired_width(60.0));
                ui.add_space(16.0);
                ui.label("Web Port:");
                ui.add_enabled(!bridge_running, egui::TextEdit::singleline(&mut self.http_port).desired_width(60.0));
            });

            ui.horizontal(|ui| {
                ui.label("Engine Host:");
                ui.add_enabled(!bridge_running, egui::TextEdit::singleline(&mut self.engine_host).desired_width(120.0));
            });

            ui.add_space(8.0);
            ui.separator();
            ui.add_space(8.0);

            // --- MIDI section ---
            ui.horizontal(|ui| {
                ui.label("MIDI Out A:");
                let name = self.devices_out.iter().find(|(id, _)| *id == self.out_a).map(|(_, n)| n.as_str()).unwrap_or("<none>");
                egui::ComboBox::from_id_salt("out_a").selected_text(name).show_ui(ui, |ui| {
                    for (id, name) in &self.devices_out { ui.selectable_value(&mut self.out_a, *id, name); }
                });
            });

            ui.horizontal(|ui| {
                ui.label("MIDI Out B:");
                let name = self.devices_out.iter().find(|(id, _)| *id == self.out_b).map(|(_, n)| n.as_str()).unwrap_or("<none>");
                egui::ComboBox::from_id_salt("out_b").selected_text(name).show_ui(ui, |ui| {
                    for (id, name) in &self.devices_out { ui.selectable_value(&mut self.out_b, *id, name); }
                });
            });

            ui.horizontal(|ui| {
                ui.label("MIDI Input:");
                let name = self.devices_in.iter().find(|(id, _)| *id == self.midi_in).map(|(_, n)| n.as_str()).unwrap_or("<none>");
                egui::ComboBox::from_id_salt("midi_in").selected_text(name).show_ui(ui, |ui| {
                    for (id, name) in &self.devices_in { ui.selectable_value(&mut self.midi_in, *id, name); }
                });
            });

            ui.horizontal(|ui| {
                if ui.add_enabled(!engine_running, egui::Button::new("Refresh Devices")).clicked() {
                    self.refresh_devices();
                }
            });

            ui.add_space(8.0);
            ui.separator();
            ui.add_space(8.0);

            // --- Engine section ---
            ui.horizontal(|ui| {
                if !engine_running {
                    if ui.add(egui::Button::new("Start Engine").min_size(egui::vec2(100.0, 28.0))).clicked() {
                        self.start_engine();
                    }
                } else {
                    if ui.add(egui::Button::new("Stop Engine").min_size(egui::vec2(100.0, 28.0))).clicked() {
                        self.stop_engine();
                    }
                }
                ui.add_space(8.0);
                let color = if engine_running { egui::Color32::from_rgb(0, 180, 0) } else { egui::Color32::from_rgb(180, 0, 0) };
                ui.colored_label(color, format!("● Engine {}", if engine_running { "Running" } else { "Stopped" }));
            });

            ui.add_space(4.0);

            // --- Bridge section ---
            ui.horizontal(|ui| {
                if !bridge_running {
                    if ui.add(egui::Button::new("Start Bridge").min_size(egui::vec2(100.0, 28.0))).clicked() {
                        self.start_bridge();
                    }
                } else {
                    if ui.add(egui::Button::new("Stop Bridge").min_size(egui::vec2(100.0, 28.0))).clicked() {
                        self.stop_bridge();
                    }
                }
                if bridge_running {
                    if ui.button("Open Browser").clicked() {
                        self.open_browser();
                    }
                }
                ui.add_space(8.0);
                let color = if bridge_running { egui::Color32::from_rgb(0, 180, 0) } else { egui::Color32::from_rgb(180, 0, 0) };
                ui.colored_label(color, format!("● Bridge {}", if bridge_running { "Running" } else { "Stopped" }));
            });

            if !self.error.is_empty() {
                ui.add_space(8.0);
                ui.colored_label(egui::Color32::from_rgb(200, 80, 80), &self.error);
            }

            ui.add_space(8.0);
            ui.separator();
            ui.add_space(4.0);
            if let Some(p) = self.sequencer_path() {
                ui.label(format!("Sequencer: {}", p.display()));
            } else {
                ui.colored_label(egui::Color32::from_rgb(200, 80, 80), "Sequencer: not found");
            }
        });

        if engine_running {
            ctx.request_repaint_after(std::time::Duration::from_millis(500));
        }
    }
}

// ============================================================
// Main
// ============================================================

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let exe = std::env::current_exe().unwrap_or_default();
    let exe_dir = exe.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| PathBuf::from("."));

    if args.iter().any(|a| a == "--list-midi" || a == "-l") {
        run_list_midi(&exe_dir);
        return;
    }

    if args.iter().any(|a| a == "--nogui") {
        run_nogui(&args, &exe_dir);
        return;
    }

    if args.iter().any(|a| a == "--help" || a == "-h") {
        eprintln!("Usage: octopus_ui [options]\n");
        eprintln!("Options:");
        eprintln!("  --nogui              Run without GUI (web server + sequencer only)");
        eprintln!("  --variant <name>     Octopus or Nemo (default: Octopus)");
        eprintln!("  --osc-port <n>       OSC port (default: 8000)");
        eprintln!("  --http-port <n>      Web GUI HTTP port (default: 8080, WS = port+1)");
        eprintln!("  --host <addr>        Engine host IP (default: 127.0.0.1; if remote, bridge only)");
        eprintln!("  --out-a <id>         MIDI output A device ID");
        eprintln!("  --out-b <id>         MIDI output B device ID");
        eprintln!("  --in <id>            MIDI input device ID");
        eprintln!("  --list-midi          List MIDI devices and exit");
        eprintln!("  --help               Show this help");
        return;
    }

    // GUI mode
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([440.0, 580.0])
            .with_resizable(true),
        ..Default::default()
    };
    let _ = eframe::run_native(
        "Octopus UI",
        options,
        Box::new(|_cc| Ok(Box::new(LauncherApp::new(exe_dir)))),
    );
}
