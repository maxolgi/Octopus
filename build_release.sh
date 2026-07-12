#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p dist

echo "=== Building Octopus (C) ==="
make clean && make && cp build/octopus dist/ || { echo "Octopus build failed"; exit 1; }

echo "=== Building octopus_ui (Rust) ==="
cd ui && cargo build --release && cd ..
cp ui/target/release/octopus_ui dist/

echo ""
echo "=== Build complete ==="
echo "dist/ contents:"
ls -la dist/
