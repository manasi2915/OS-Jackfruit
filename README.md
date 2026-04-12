# OS-Jackfruit — Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|------|-----|
| Manasi Vipin | PES1UG24CS260 |
| Meghana Grandhi | PES1UG24CS268 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Clone and enter the repo
```bash
git clone https://github.com/manasi2915/OS-Jackfruit.git
cd OS-Jackfruit
```

### Prepare the Alpine base rootfs
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### Build everything
```bash
cd boilerplate
make
```

### Copy workload binaries into rootfs
```bash
sudo cp boilerplate/cpu_hog rootfs-alpha/
sudo cp boilerplate/cpu_hog rootfs-beta/
sudo cp boilerplate/memory_hog rootfs-alpha/
sudo cp boilerplate/memory_hog rootfs-beta/
sudo cp boilerplate/io_pulse rootfs-alpha/
sudo cp boilerplate/io_pulse rootfs-beta/
```

### Load the kernel module
```bash
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
```

### Start the supervisor (Terminal 1)
```bash
sudo ./boilerplate/engine supervisor ./rootfs-base
```

### Launch containers (Terminal 2)
```bash
sudo ./boilerplate/engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./boilerplate/engine start beta  ./rootfs-beta  /cpu_hog --soft-mib 32 --hard-mib 64
sudo ./boilerplate/engine ps
sudo ./boilerplate/engine logs alpha
sudo ./boilerplate/engine stop alpha
```

### Memory limit test
```bash
cp -a rootfs-base rootfs-memtest
sudo cp boilerplate/memory_hog rootfs-memtest/
sudo ./boilerplate/engine start memtest ./rootfs-memtest /memory_hog --soft-mib 20 --hard-mib 40
sudo dmesg | grep container_monitor
```

### Scheduling experiment
```bash
sudo ./boilerplate/engine start hipri ./rootfs-alpha /cpu_hog --nice -5 --soft-mib 48 --hard-mib 80
sudo ./boilerplate/engine start lopri ./rootfs-beta  /cpu_hog --nice 10 --soft-mib 48 --hard-mib 80
sleep 12
sudo ./boilerplate/engine logs hipri
sudo ./boilerplate/engine logs lopri
```

### Clean teardown
```bash
sudo ./boilerplate/engine stop alpha
sudo ./boilerplate/engine stop beta
# Press Ctrl+C in Terminal 1 to stop supervisor
sudo rmmod monitor
sudo dmesg | tail -5
ps aux | grep defunct
```

---

## 3. Demo with Screenshots

### Screenshot 1 and 2 — Multi-container supervision and metadata tracking
Two containers alpha and beta both in running state under a single supervisor. ps output shows ID, PID, STATE, SOFT(MB), HARD(MB).

![sc1_sc2](screenshots/sc1_sc2.png)

### Screenshot 3 — Bounded-buffer logging
Log file contents captured through the producer-consumer logging pipeline.

![sc3](screenshots/sc3.png)

### Screenshot 4 — CLI and IPC
Stop command issued via UNIX domain socket to supervisor, ps showing updated container state.

![sc4](screenshots/sc4.png)

### Screenshot 5 and 6 — Soft-limit warning and Hard-limit enforcement
dmesg showing SOFT LIMIT warning and HARD LIMIT kill events from the kernel module. ps showing memtest in killed state.

![sc5_sc6_dmesg](screenshots/sc5_sc6_dmesg.png)
![sc6_ps](screenshots/sc6_ps.png)

### Screenshot 7 — Scheduling experiment
hipri (nice=-5) vs lopri (nice=+10) log comparison showing hipri completed more CPU work due to higher CFS weight.

![sc7](screenshots/sc7.png)

### Screenshot 8 — Clean teardown
Module unloaded cleanly, no zombie processes after full shutdown.

![sc8](screenshots/sc8.png)

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms
The runtime uses clone() with CLONE_NEWPID, CLONE_NEWUTS, and CLONE_NEWNS flags to create isolated namespaces per container. CLONE_NEWPID gives each container its own PID namespace so container processes cannot see host processes. CLONE_NEWUTS allows each container its own hostname set via sethostname(). CLONE_NEWNS creates a private mount namespace so mounts inside the container do not affect the host. chroot() restricts the container filesystem view to its assigned rootfs directory. /proc is mounted inside each container so tools like ps work correctly. The host kernel is still shared across all containers — the scheduler, memory manager, network stack, and device drivers are not isolated.

### 2. Supervisor and Process Lifecycle
A long-running parent supervisor is necessary because only the parent of a process can call waitpid() on it. Without a persistent parent, exited containers become zombies. The supervisor installs a SIGCHLD handler that calls waitpid(-1, WNOHANG) to reap all exited children immediately without blocking. Each container metadata record stores PID, state, limits, exit code, and a stop_requested flag. When a container exits, the SIGCHLD handler updates its state to exited, stopped, or killed based on whether stop_requested was set and what signal terminated it.

### 3. IPC, Threads, and Synchronization
Two IPC mechanisms are used. Path A uses anonymous pipes: each container stdout/stderr connects to the supervisor via a pipe. A producer thread per container reads from the pipe and pushes log chunks into a bounded ring buffer. A consumer thread pops from the buffer and writes to per-container log files. Path B uses a UNIX domain socket: CLI client processes connect, send a control_request_t struct, receive a control_response_t, and exit. The bounded buffer uses a pthread_mutex_t protecting head, tail, and count, plus two condition variables not_full and not_empty. Without the mutex, concurrent push and pop would corrupt the indices. Condition variables avoid busy-waiting. A separate metadata_lock protects the container linked list.

### 4. Memory Management and Enforcement
RSS measures physical RAM pages currently mapped and present in memory. It does not measure virtual memory that was mapped but never accessed, pages swapped to disk, or shared library pages. Soft limits are early warning thresholds that log a message but allow the process to continue. Hard limits enforce a strict ceiling by sending SIGKILL. Enforcement belongs in kernel space because user-space polling has latency and a process could allocate memory faster than a user-space monitor reacts. The kernel timer fires every second regardless of container behavior and send_sig(SIGKILL) from kernel space cannot be caught or ignored.

### 5. Scheduling Behavior
Linux uses the Completely Fair Scheduler which allocates CPU time proportionally based on process weight derived from nice values. Nice -5 has weight ~335 and nice +10 has weight ~110 giving a ratio of approximately 3:1. Our experiment showed hipri (nice -5) completed significantly more iterations than lopri (nice +10) in the same wall-clock time confirming CFS weighted fairness. The I/O-bound container spent most time sleeping so CFS gave it immediate scheduling on wakeup demonstrating CFS responsiveness for I/O-bound workloads.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** chroot instead of pivot_root.
**Tradeoff:** chroot can be escaped by a privileged process. pivot_root is more secure but requires additional mount setup.
**Justification:** chroot provides sufficient isolation for a controlled lab environment with known workloads.

### Supervisor Architecture
**Choice:** Single-process supervisor with select-based event loop and separate logger thread.
**Tradeoff:** Command handling is serialised so simultaneous CLI calls queue up.
**Justification:** CLI commands are fast and low-frequency. Serialised handling eliminates concurrency bugs without meaningful performance cost.

### IPC and Logging
**Choice:** Anonymous pipes for logging, UNIX domain socket for control, fixed-size ring buffer between producer and consumer threads.
**Tradeoff:** Fixed 16-slot buffer means producers block if consumers fall behind.
**Justification:** Blocking producers provides backpressure rather than silent data loss. 64KB of buffering is sufficient for all workloads tested.

### Kernel Monitor
**Choice:** mutex instead of spinlock for the monitored list.
**Tradeoff:** Mutex cannot be used in hard IRQ context. Spinlock wastes CPU spinning.
**Justification:** get_task_mm() acquires a sleepable lock internally so a spinlock cannot be held while calling it. Mutex is the only correct choice.

### Scheduling Experiments
**Choice:** nice values via --nice flag at container launch time.
**Tradeoff:** nice affects CFS weight but does not provide hard CPU guarantees like cgroups would.
**Justification:** nice is the simplest mechanism available and produces clearly observable differences in CPU allocation between containers.

---

## 6. Scheduler Experiment Results

### Experiment 1 — Two CPU-bound containers with different nice values

| Container | Nice | Priority | Relative CPU share |
|-----------|------|----------|--------------------|
| hipri | -5 | High | ~75% |
| lopri | +10 | Low | ~25% |

hipri reached higher accumulator values in the same duration confirming CFS allocated proportionally more CPU to the higher weight process. Neither process was starved — lopri still made forward progress.

### Experiment 2 — CPU-bound vs I/O-bound at same priority

| Container | Type | Nice | Avg CPU% |
|-----------|------|------|----------|
| cpuwork | CPU-bound | 0 | ~90% |
| iowork | I/O-bound | 0 | ~10% |

The I/O-bound container spent most time sleeping between pulses. CFS immediately scheduled it on wakeup because its vruntime fell behind during sleep. This demonstrates CFS responsiveness for I/O-bound workloads even under CPU pressure.
