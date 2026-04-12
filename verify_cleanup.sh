#!/bin/bash
ENGINE=./boilerplate/engine

echo "=== Task 6: Cleanup Verification ==="

echo "--- Containers before shutdown ---"
sudo $ENGINE ps

echo "--- Stopping all containers ---"
sudo $ENGINE stop alpha 2>/dev/null
sudo $ENGINE stop beta  2>/dev/null
sudo $ENGINE stop exp1  2>/dev/null
sudo $ENGINE stop exp2  2>/dev/null
sudo $ENGINE stop exp3  2>/dev/null
sudo $ENGINE stop memtest 2>/dev/null

sleep 2

echo "--- Checking for zombie processes ---"
ZOMBIES=$(ps aux | grep 'Z' | grep -v grep)
if [ -z "$ZOMBIES" ]; then
    echo "[OK] No zombie processes found"
else
    echo "[WARN] Zombies found:"
    echo "$ZOMBIES"
fi

echo "--- Checking for leftover engine processes ---"
ps aux | grep engine | grep -v grep

echo "--- Kernel module dmesg tail ---"
sudo dmesg | tail -10

echo "=== Cleanup verification complete ==="
