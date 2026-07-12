use std::sync::Arc;

use futures_util::{SinkExt, StreamExt};
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream, UdpSocket};
use tokio::sync::broadcast;
use tokio_tungstenite::tungstenite::Message;

const ENGINE_HOST: &str = "127.0.0.1";
const ENGINE_OSC_OUT: u16 = 9000;

const HTML_OCTOPUS: &str = include_str!("../../web_gui.html");
const HTML_NEMO: &str = include_str!("../../web_gui_nemo.html");

// ============================================================
// OSC pack / unpack
// ============================================================

enum OscArg {
    Int(i32),
    Str(String),
}

fn osc_pack(address: &str, args: &[OscArg]) -> Vec<u8> {
    let mut msg = Vec::new();
    msg.extend_from_slice(address.as_bytes());
    msg.push(0);
    while msg.len() % 4 != 0 {
        msg.push(0);
    }
    msg.push(b',');
    for a in args {
        match a {
            OscArg::Int(_) => msg.push(b'i'),
            OscArg::Str(_) => msg.push(b's'),
        }
    }
    msg.push(0);
    while msg.len() % 4 != 0 {
        msg.push(0);
    }
    for a in args {
        match a {
            OscArg::Int(v) => msg.extend_from_slice(&v.to_be_bytes()),
            OscArg::Str(s) => {
                msg.extend_from_slice(s.as_bytes());
                msg.push(0);
                while msg.len() % 4 != 0 {
                    msg.push(0);
                }
            }
        }
    }
    msg
}

fn osc_unpack(data: &[u8]) -> (String, Vec<Value>) {
    let addr_end = data.iter().position(|&b| b == 0).unwrap_or(data.len());
    let address = String::from_utf8_lossy(&data[..addr_end]).into_owned();
    let mut pos = (addr_end + 4) & !3;

    if pos >= data.len() || data.get(pos) != Some(&b',') {
        return (address, vec![]);
    }

    let type_end = data[pos..]
        .iter()
        .position(|&b| b == 0)
        .map(|i| i + pos)
        .unwrap_or(data.len());
    let types = &data[pos + 1..type_end];
    pos = (type_end + 4) & !3;

    let mut args = Vec::new();
    for &t in types {
        match t {
            b'i' => {
                if pos + 4 <= data.len() {
                    let val = i32::from_be_bytes([
                        data[pos],
                        data[pos + 1],
                        data[pos + 2],
                        data[pos + 3],
                    ]);
                    args.push(Value::from(val));
                    pos += 4;
                }
            }
            b'b' => {
                if pos + 4 <= data.len() {
                    let blen = i32::from_be_bytes([
                        data[pos],
                        data[pos + 1],
                        data[pos + 2],
                        data[pos + 3],
                    ]) as usize;
                    pos += 4;
                    let end = (pos + blen).min(data.len());
                    let hex: String = data[pos..end].iter().map(|b| format!("{:02x}", b)).collect();
                    args.push(Value::from(hex));
                    pos += (blen + 3) & !3;
                }
            }
            b's' => {
                let str_end = data[pos..]
                    .iter()
                    .position(|&b| b == 0)
                    .map(|i| i + pos)
                    .unwrap_or(data.len());
                let s = String::from_utf8_lossy(&data[pos..str_end]).into_owned();
                args.push(Value::from(s));
                pos = (str_end + 4) & !3;
            }
            _ => {}
        }
    }
    (address, args)
}

// ============================================================
// HTTP server
// ============================================================

async fn handle_http(mut stream: TcpStream, html: Arc<&str>, nemo_html: Arc<&str>) {
    let mut buf = [0u8; 2048];
    let n = match stream.read(&mut buf).await {
        Ok(n) if n > 0 => n,
        _ => return,
    };

    let request = std::str::from_utf8(&buf[..n]).unwrap_or("");
    let path = request
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .unwrap_or("/");

    let content = if path == "/nemo" || path == "/nemo/" {
        Some(&**nemo_html)
    } else if path == "/" || path == "/index.html" {
        Some(&**html)
    } else {
        None
    };

    let response = match content {
        Some(c) => format!(
            "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\n\r\n{}",
            c.len(),
            c
        ),
        None => "HTTP/1.0 404 Not Found\r\n\r\n".to_string(),
    };

    let _ = stream.write_all(response.as_bytes()).await;
}

// ============================================================
// WebSocket handler
// ============================================================

async fn handle_websocket(
    stream: TcpStream,
    udp: Arc<UdpSocket>,
    tx: broadcast::Sender<String>,
    engine_addr: String,
) {
    let ws_stream = match tokio_tungstenite::accept_async(stream).await {
        Ok(s) => s,
        Err(_) => return,
    };

    let (mut ws_sender, mut ws_receiver) = ws_stream.split();
    let mut rx = tx.subscribe();

    loop {
        tokio::select! {
            msg = ws_receiver.next() => {
                match msg {
                    Some(Ok(Message::Text(text))) => {
                        if let Some(p) = parse_ws_command(&text) {
                            let _ = udp.send_to(&p, &engine_addr).await;
                        }
                    }
                    Some(Ok(Message::Binary(data))) => {
                        if let Ok(text) = String::from_utf8(data.to_vec()) {
                            if let Some(p) = parse_ws_command(&text) {
                                let _ = udp.send_to(&p, &engine_addr).await;
                            }
                        }
                    }
                    Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                    _ => {}
                }
            }
            result = rx.recv() => {
                match result {
                    Ok(m) => {
                        if ws_sender.send(Message::Text(m.into())).await.is_err() {
                            break;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }
        }
    }
}

fn parse_ws_command(text: &str) -> Option<Vec<u8>> {
    let cmd: Value = serde_json::from_str(text).ok()?;
    let ctype = cmd.get("type").and_then(|v| v.as_str()).unwrap_or("");
    match ctype {
        "key" => {
            let idx = cmd.get("index").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
            let press = cmd.get("press").and_then(|v| v.as_bool()).unwrap_or(true);
            Some(osc_pack("/key", &[OscArg::Int(idx), OscArg::Int(if press { 1 } else { 0 })]))
        }
        "rotary" => {
            let idx = cmd.get("index").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
            let dir = cmd.get("direction").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
            Some(osc_pack("/rotary", &[OscArg::Int(idx), OscArg::Int(dir)]))
        }
        "transport" => {
            let s = cmd.get("cmd").and_then(|v| v.as_str()).unwrap_or("").to_string();
            Some(osc_pack("/transport", &[OscArg::Str(s)]))
        }
        "tempo" => {
            let bpm = cmd.get("bpm").and_then(|v| v.as_i64()).unwrap_or(120) as i32;
            Some(osc_pack("/tempo", &[OscArg::Int(bpm)]))
        }
        "zoom" => {
            let lvl = cmd.get("level").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
            Some(osc_pack("/zoom", &[OscArg::Int(lvl)]))
        }
        "quit" => Some(osc_pack("/quit", &[])),
        _ => None,
    }
}

// ============================================================
// OSC listener (engine -> WebSocket clients)
// ============================================================

async fn osc_listener(udp: Arc<UdpSocket>, tx: broadcast::Sender<String>) {
    let mut buf = [0u8; 4096];
    loop {
        if let Ok((n, _)) = udp.recv_from(&mut buf).await {
            let (address, args) = osc_unpack(&buf[..n]);
            let msg = serde_json::json!({"address": address, "args": args}).to_string();
            let _ = tx.send(msg);
        }
    }
}

// ============================================================
// WebServer — manages tokio runtime + all server tasks
// ============================================================

pub struct WebServer {
    runtime: Option<tokio::runtime::Runtime>,
}

impl WebServer {
    pub fn start(engine_osc_in: u16, http_port: u16, ws_port: u16) -> Result<Self, String> {
        let runtime = tokio::runtime::Runtime::new().map_err(|e| e.to_string())?;

        let (http_listener, ws_listener, udp) = runtime.block_on(async {
            let http = TcpListener::bind(format!("0.0.0.0:{}", http_port))
                .await
                .map_err(|e| format!("HTTP bind: {}", e))?;
            let ws = TcpListener::bind(format!("0.0.0.0:{}", ws_port))
                .await
                .map_err(|e| format!("WS bind: {}", e))?;
            let udp = UdpSocket::bind(format!("{}:{}", ENGINE_HOST, ENGINE_OSC_OUT))
                .await
                .map_err(|e| format!("OSC UDP bind: {}", e))?;
            Ok::<_, String>((http, ws, udp))
        })?;

        let html = Arc::new(HTML_OCTOPUS);
        let nemo_html = Arc::new(HTML_NEMO);
        let udp = Arc::new(udp);
        let (tx, _) = broadcast::channel::<String>(256);
        let engine_addr = format!("{}:{}", ENGINE_HOST, engine_osc_in);

        // HTTP server
        {
            let html = html.clone();
            let nemo = nemo_html.clone();
            runtime.spawn(async move {
                loop {
                    if let Ok((stream, _)) = http_listener.accept().await {
                        let html = html.clone();
                        let nemo = nemo.clone();
                        tokio::spawn(handle_http(stream, html, nemo));
                    }
                }
            });
        }

        // OSC listener
        {
            let udp = udp.clone();
            let tx = tx.clone();
            runtime.spawn(async move {
                osc_listener(udp, tx).await;
            });
        }

        // WebSocket server
        {
            let udp = udp.clone();
            let tx = tx.clone();
            let engine_addr = engine_addr.clone();
            runtime.spawn(async move {
                loop {
                    if let Ok((stream, _)) = ws_listener.accept().await {
                        let udp = udp.clone();
                        let tx = tx.clone();
                        let addr = engine_addr.clone();
                        tokio::spawn(handle_websocket(stream, udp, tx, addr));
                    }
                }
            });
        }

        eprintln!("Web GUI: http://localhost:{}", http_port);
        eprintln!("WebSocket: ws://localhost:{}", ws_port);
        eprintln!("OSC bridge: engine:{} -> engine:{}", engine_osc_in, ENGINE_OSC_OUT);

        Ok(Self {
            runtime: Some(runtime),
        })
    }

    pub fn stop(&mut self) {
        if let Some(rt) = self.runtime.take() {
            drop(rt);
            eprintln!("Web server stopped.");
        }
    }
}

impl Drop for WebServer {
    fn drop(&mut self) {
        self.stop();
    }
}
