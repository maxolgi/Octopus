// Build script: compiles the C engine (src/*.c + firmware) into
// build/liboctopus.a, then links it statically into this crate.
use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().to_path_buf();
    let target = env::var("TARGET").unwrap();

    if target.contains("windows") {
        if !target.contains("gnu") {
            panic!(
                "The Octopus engine is built with MinGW gcc; build this crate with the \
                 x86_64-pc-windows-gnu target (e.g. `rustup target add \
                 x86_64-pc-windows-gnu` + `cargo build --target x86_64-pc-windows-gnu`)."
            );
        }
        let ok = Command::new("powershell")
            .args([
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                "build_win.ps1",
                "-Lib",
            ])
            .current_dir(&root)
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        if !ok {
            panic!("build_win.ps1 -Lib failed (requires MSYS2 ucrt64 gcc in PATH)");
        }
    } else {
        let ok = Command::new("make")
            .arg("build/liboctopus.a")
            .current_dir(&root)
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        if !ok {
            panic!("make build/liboctopus.a failed (requires gcc + libasound2-dev)");
        }
    }

    println!(
        "cargo:rustc-link-search=native={}",
        root.join("build").display()
    );
    println!("cargo:rustc-link-lib=static=octopus");

    if target.contains("windows") {
        println!("cargo:rustc-link-lib=dylib=winmm");
        println!("cargo:rustc-link-lib=dylib=ws2_32");
        println!("cargo:rustc-link-lib=dylib=avrt");
    } else if target.contains("linux") {
        println!("cargo:rustc-link-lib=dylib=asound");
        println!("cargo:rustc-link-lib=dylib=pthread");
        println!("cargo:rustc-link-lib=dylib=m");
        println!("cargo:rustc-link-lib=dylib=dl");
    }

    println!("cargo:rerun-if-changed={}", root.join("src").display());
    println!("cargo:rerun-if-changed={}", root.join("include").display());
    println!("cargo:rerun-if-changed={}", root.join("Makefile").display());
    println!(
        "cargo:rerun-if-changed={}",
        root.join("build_win.ps1").display()
    );
}
