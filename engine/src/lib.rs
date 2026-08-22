//! Safe Rust wrapper around the Octopus C engine (liboctopus.a).
//!
//! The engine is hosted in-process: OSC input is injected via
//! [`Engine::send_osc`], and output frames (MIR LED state, transport)
//! arrive through a channel fed by a C callback registered before
//! engine start.

pub mod web;

use std::ffi::{c_char, c_int, CStr, CString};
use std::path::PathBuf;
use std::sync::Mutex;
use std::sync::OnceLock;

use tokio::sync::mpsc::{unbounded_channel, UnboundedReceiver, UnboundedSender};

// ============================================================
// FFI
// ============================================================

#[repr(C)]
struct COpts {
    osc_udp_port: c_int,
    out_a: c_int,
    out_b: c_int,
    midi_in: c_int,
    state_file: *const c_char,
    hosted: c_int,
}

extern "C" {
    fn oct_engine_start(opts: *const COpts) -> c_int;
    fn oct_engine_stop();
    fn oct_engine_running() -> c_int;
    fn oct_engine_wants_quit() -> c_int;
    fn oct_send_osc(buf: *const u8, len: c_int);
    fn oct_save_state(path: *const c_char);
    fn midi_enum_devices(is_input: c_int, idx: c_int, name: *mut c_char, name_len: c_int) -> c_int;
    fn oct_set_frame_callback(cb: Option<unsafe extern "C" fn(*const u8, c_int)>);
}

// ============================================================
// Frame channel (C render thread -> Rust)
// ============================================================

static FRAME_SENDER: OnceLock<UnboundedSender<Vec<u8>>> = OnceLock::new();
static FRAME_RECEIVER: Mutex<Option<UnboundedReceiver<Vec<u8>>>> = Mutex::new(None);

unsafe extern "C" fn frame_trampoline(buf: *const u8, len: c_int) {
    if buf.is_null() || len <= 0 {
        return;
    }
    let data = std::slice::from_raw_parts(buf, len as usize).to_vec();
    if let Some(tx) = FRAME_SENDER.get() {
        let _ = tx.send(data);
    }
}

/// Take the engine output-frame receiver (packed OSC packets).
/// Must be called once, after [`Engine::start`].
pub fn take_frame_receiver() -> UnboundedReceiver<Vec<u8>> {
    FRAME_RECEIVER
        .lock()
        .unwrap()
        .take()
        .expect("frame receiver already taken")
}

// ============================================================
// Engine handle
// ============================================================

#[derive(Debug, Clone)]
pub struct Options {
    /// UDP OSC input port (external control surfaces / test scripts).
    /// 0 disables the UDP listener.
    pub osc_udp_port: u16,
    pub out_a: i32,
    pub out_b: i32,
    pub midi_in: i32,
    /// State file path. Defaults to `octopus_state.bin` in the cwd.
    pub state_file: Option<PathBuf>,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            osc_udp_port: 8000,
            out_a: -1,
            out_b: -1,
            midi_in: -1,
            state_file: None,
        }
    }
}

/// Handle to the running engine. One engine per process — the firmware
/// is initialized once and cannot be re-initialized.
pub struct Engine {
    state_path: CString,
}

impl Engine {
    /// Start the engine in hosted mode.
    pub fn start(opts: &Options) -> Result<Engine, String> {
        if unsafe { oct_engine_running() } != 0 {
            return Err("engine already started".into());
        }

        // Frame channel + callback must be in place before engine start.
        let (tx, rx) = unbounded_channel::<Vec<u8>>();
        let _ = FRAME_SENDER.set(tx);
        *FRAME_RECEIVER.lock().unwrap() = Some(rx);
        unsafe { oct_set_frame_callback(Some(frame_trampoline)) };

        let state_path = CString::new(
            opts.state_file
                .as_ref()
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_else(|| "octopus_state.bin".to_string()),
        )
        .map_err(|e| e.to_string())?;

        let copts = COpts {
            osc_udp_port: opts.osc_udp_port as c_int,
            out_a: opts.out_a,
            out_b: opts.out_b,
            midi_in: opts.midi_in,
            state_file: state_path.as_ptr(),
            hosted: 1,
        };

        let rc = unsafe { oct_engine_start(&copts) };
        if rc != 0 {
            return Err("engine failed to start".into());
        }
        Ok(Engine { state_path })
    }

    /// Inject a packed OSC packet into the engine dispatcher
    /// (same path as the UDP server). Thread-safe.
    pub fn send_osc(&self, bytes: &[u8]) {
        if !bytes.is_empty() {
            unsafe { oct_send_osc(bytes.as_ptr(), bytes.len() as c_int) };
        }
    }

    /// Save engine state to the configured state file.
    pub fn save_state(&self) {
        unsafe { oct_save_state(self.state_path.as_ptr()) };
    }

    /// True after a `/quit` OSC command asked the engine to stop.
    pub fn wants_quit(&self) -> bool {
        (unsafe { oct_engine_wants_quit() }) != 0
    }

    /// Stop the engine. Final — the engine cannot be restarted in this
    /// process (firmware limitation).
    pub fn stop(&self) {
        unsafe { oct_engine_stop() };
    }
}

/// Build a minimal OSC message with a single int argument
/// (e.g. runtime MIDI device switching: `/midi/out_a 128`).
pub fn osc_int(address: &str, value: i32) -> Vec<u8> {
    let mut msg = Vec::new();
    msg.extend_from_slice(address.as_bytes());
    msg.push(0);
    while msg.len() % 4 != 0 {
        msg.push(0);
    }
    msg.extend_from_slice(b",i\0");
    while msg.len() % 4 != 0 {
        msg.push(0);
    }
    msg.extend_from_slice(&value.to_be_bytes());
    msg
}

/// List MIDI devices: `(outputs, inputs)` as `(device_id, name)` pairs.
pub fn list_midi_devices() -> (Vec<(i32, String)>, Vec<(i32, String)>) {
    let mut outs = Vec::new();
    let mut ins = Vec::new();
    for is_input in [false, true] {
        let mut idx = 0;
        loop {
            let mut buf = [0u8; 256];
            let id = unsafe {
                midi_enum_devices(
                    is_input as c_int,
                    idx,
                    buf.as_mut_ptr() as *mut c_char,
                    buf.len() as c_int,
                )
            };
            if id < 0 {
                break;
            }
            let name = CStr::from_bytes_until_nul(&buf)
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default();
            if is_input {
                ins.push((id, name));
            } else {
                outs.push((id, name));
            }
            idx += 1;
        }
    }
    (outs, ins)
}
