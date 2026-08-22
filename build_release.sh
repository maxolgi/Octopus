#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p dist

echo "=== Building Octopus engine (C: standalone + static lib) ==="
make clean && make || { echo "Engine build failed"; exit 1; }
cp build/octopus dist/

echo "=== Building octopus_gui + octopus_cli (Rust, in-process engine) ==="
cargo build --release
cp target/release/octopus_gui target/release/octopus_cli dist/

echo "=== Web control surface (for the Python bridge / reference) ==="
cp web_gui.html web_gui_nemo.html web_gui_modern.html dist/ 2>/dev/null || cp web_gui.html dist/

echo ""
echo "=== Build complete ==="
echo "dist/ contents:"
ls -la dist/
