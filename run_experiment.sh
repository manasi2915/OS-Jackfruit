#!/bin/bash
# Scheduling Experiment: compare CPU-bound containers with different nice values

ENGINE=./boilerplate/engine
RESULTS_FILE="experiment_results.txt"

echo "=== Scheduling Experiment ===" | tee $RESULTS_FILE
echo "Date: $(date)" | tee -a $RESULTS_FILE
echo "" | tee -a $RESULTS_FILE

# Setup rootfs copies for experiment
cp -a ./rootfs-base ./rootfs-exp1 2>/dev/null || true
cp -a ./rootfs-base ./rootfs-exp2 2>/dev/null || true
cp boilerplate/cpu_hog ./rootfs-exp1/
cp boilerplate/cpu_hog ./rootfs-exp2/

echo "--- Experiment 1: Two CPU hogs, different nice values ---" | tee -a $RESULTS_FILE
echo "exp1: nice=-5 (high priority), exp2: nice=10 (low priority)" | tee -a $RESULTS_FILE
echo "" | tee -a $RESULTS_FILE

# Start both containers
sudo $ENGINE start exp1 ./rootfs-exp1 /cpu_hog --nice -5
sudo $ENGINE start exp2 ./rootfs-exp2 /cpu_hog --nice 10

echo "Both containers started. Sampling CPU usage for 15 seconds..." | tee -a $RESULTS_FILE
echo "" | tee -a $RESULTS_FILE

# Sample CPU usage 5 times over 15 seconds
for i in 1 2 3 4 5; do
    sleep 3
    echo "--- Sample $i (t=${i}*3s) ---" | tee -a $RESULTS_FILE
    ps -p $(pgrep -f cpu_hog | tr '\n' ',') -o pid,ni,pcpu,comm 2>/dev/null | tee -a $RESULTS_FILE
    echo "" | tee -a $RESULTS_FILE
done

echo "--- Final container states ---" | tee -a $RESULTS_FILE
sudo $ENGINE ps | tee -a $RESULTS_FILE

# Stop containers
sudo $ENGINE stop exp1
sudo $ENGINE stop exp2

echo "" | tee -a $RESULTS_FILE
echo "--- Experiment 2: CPU-bound vs I/O-bound ---" | tee -a $RESULTS_FILE

cp -a ./rootfs-base ./rootfs-exp3 2>/dev/null || true
cp boilerplate/io_pulse ./rootfs-exp3/

sudo $ENGINE start exp3 ./rootfs-exp3 /io_pulse --nice 0
sudo $ENGINE start exp1 ./rootfs-exp1 /cpu_hog  --nice 0

sleep 10

echo "CPU vs IO usage:" | tee -a $RESULTS_FILE
ps -p $(pgrep -f "cpu_hog\|io_pulse" | tr '\n' ',') -o pid,ni,pcpu,stat,comm 2>/dev/null | tee -a $RESULTS_FILE

sudo $ENGINE stop exp1
sudo $ENGINE stop exp3

echo "" | tee -a $RESULTS_FILE
echo "=== Experiment complete. Results saved to $RESULTS_FILE ===" | tee -a $RESULTS_FILE
