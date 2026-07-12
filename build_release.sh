#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p dist

echo "=== Building Octopus (C) ==="
make clean && make && cp build/octopus dist/ || { echo "Octopus build failed"; exit 1; }

echo "=== Building Nemo (C) ==="
make clean && make NEMO=1 && cp build/nemo dist/ || echo "Nemo build failed, skipping"

echo "=== Building octopus_ui (Rust) ==="
cd ui && cargo build --release && cd ..
cp ui/target/release/octopus_ui dist/

echo "=== Building web_gui (Rust) ==="
cd cli_ui && cargo build --release && cd ..
cp cli_ui/target/release/web_gui dist/

echo ""
echo "=== Build complete ==="
echo "dist/ contents:"
ls -la dist/
