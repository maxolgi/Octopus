#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p dist

echo "=== Building Octopus engine (C: standalone + static lib) ==="
make clean && make || { echo "Engine build failed"; exit 1; }
cp build/octopus dist/

echo "=== Building octopus_gui (Rust, in-process engine; --no-gui for headless) ==="
cargo build --release
cp target/release/octopus_gui dist/

echo ""
echo "=== Build complete ==="
echo "dist/ contents:"
ls -la dist/
