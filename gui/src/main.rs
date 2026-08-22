#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

//! Octopus GUI — hosts the C engine in-process and serves the web
//! control surface. No subprocess, no OSC port juggling.

use eframe::egui;
use std::path::PathBuf;
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
            http_port: "8080".to_string(),
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
        let http_port: u16 = self.http_port.parse().unwrap_or(8080);

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
        let port: u16 = self.http_port.parse().unwrap_or(8080);
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

            // --- MIDI section ---
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
                            ui.selectable_value(&mut self.out_a, *id, name);
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
                            ui.selectable_value(&mut self.out_b, *id, name);
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
                            ui.selectable_value(&mut self.midi_in, *id, name);
                        }
                    });
            });

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

            // --- Engine + web server (single switch) ---
            ui.horizontal(|ui| {
                if !running {
                    if !self.stopped
                        && ui
                            .add(egui::Button::new("Start").min_size(egui::vec2(100.0, 28.0)))
                            .clicked()
                    {
                        self.start();
                    }
                } else {
                    if ui
                        .add(egui::Button::new("Stop").min_size(egui::vec2(100.0, 28.0)))
                        .clicked()
                    {
                        self.stop();
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
                ui.checkbox(&mut self.autosave, "Save state on stop");
            });

            if running {
                ui.add_space(4.0);
                ui.horizontal(|ui| {
                    if ui.button("Open Browser").clicked() {
                        self.open_browser();
                    }
                    let port: u16 = self.http_port.parse().unwrap_or(8080);
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
