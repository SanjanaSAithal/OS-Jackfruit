# Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|------|-----|
| Sanjana S Aithal | [Your SRN] |
| [Teammate Name] | [Teammate SRN] |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
- Ubuntu 22.04 or 24.04 (bare metal or VM)
- Secure Boot OFF
- Dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
git clone https://github.com/SanjanaSAithal/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate
make
```

This builds: `engine`, `monitor.ko`, `cpu_hog`, `io_pulse`, `memory_hog`.

### Prepare Root Filesystem

```bash
cd ..
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

Copy workload binaries into rootfs:

```bash
sudo cp boilerplate/cpu_hog rootfs/
sudo cp boilerplate/io_pulse rootfs/
sudo cp boilerplate/memory_hog rootfs/
```

### Load Kernel Module

```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor   # verify device created
sudo dmesg | tail -3            # verify module loaded
```

### Start Supervisor

In a dedicated terminal:

```bash
sudo ./engine supervisor ../rootfs
```

Expected output:
```
[supervisor] kernel monitor opened
[supervisor] ready on /tmp/mini_runtime.sock
```

### Launch Containers

In a second terminal:

```bash
# Start containers in background
sudo ./engine start alpha ../rootfs "echo hello from alpha"
sudo ./engine start beta ../rootfs "echo hello from beta"

# List tracked containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha
sudo ./engine logs beta

# Stop a container
sudo ./engine stop alpha

# Run a container in foreground (blocks until done)
sudo ./engine run test ../rootfs "echo done"
```

### Memory Limit Test

```bash
sudo ./engine start hogtest ../rootfs "/memory_hog" --soft-mib 10 --hard-mib 20
sudo dmesg | grep hogtest
```

### Scheduler Experiments

```bash
# Experiment 1: two CPU hogs at different nice values
sudo ./engine start cpu-low ../rootfs "/cpu_hog" --nice 0
sudo ./engine start cpu-high ../rootfs "/cpu_hog" --nice 10

# Experiment 2: CPU-bound vs I/O-bound
sudo ./engine start cpu-exp ../rootfs "/cpu_hog"
sudo ./engine start io-exp ../rootfs "/io_pulse"
```

### Teardown

```bash
# Stop supervisor with Ctrl+C in supervisor terminal

# Unload kernel module
sudo rmmod monitor
sudo dmesg | tail -5
```

---

## 3. Demo Screenshots

### Screenshot 1 — Multi-container supervision
Two containers (alpha and beta) running under one supervisor process, each started with `./engine start`.

### Screenshot 2 — Metadata tracking
Output of `./engine ps` showing container ID, PID, state, exit code, and start time for all tracked containers.

### Screenshot 3 — Bounded-buffer logging
Output of `./engine logs alpha` and `./engine logs beta` showing container stdout captured through the logging pipeline.

### Screenshot 4 — CLI and IPC
CLI commands (`start`, `ps`, `logs`) being issued and the supervisor responding over the UNIX domain socket at `/tmp/mini_runtime.sock`.

### Screenshot 5 — Soft-limit warning
`dmesg` output showing:
```
[container_monitor] SOFT LIMIT container=hogtest2 pid=21885 rss=17375232 limit=10485760
```

### Screenshot 6 — Hard-limit enforcement
`dmesg` output showing:
```
[container_monitor] HARD LIMIT container=hogtest2 pid=21885 rss=25763840 limit=20971520
```
Container was killed by the kernel module after exceeding the 20MB hard limit.

### Screenshot 7 — Scheduler experiment
Logs from cpu-low (nice 0) and cpu-high (nice 10) running concurrently, and logs from cpu-exp vs io-exp showing different iteration counts in the same time window.

### Screenshot 8 — Clean teardown
`ps aux | grep defunct` showing no zombie processes, and `dmesg` showing module unloaded cleanly with all entries freed.

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms

Each container is created using `clone()` with three namespace flags: `CLONE_NEWPID` gives the container its own PID namespace so processes inside see themselves as PID 1; `CLONE_NEWUTS` gives it an independent hostname set via `sethostname()`; `CLONE_NEWNS` gives it a private mount namespace so filesystem mounts don't propagate to the host. After cloning, `chroot()` redirects the container's root to the Alpine rootfs, and `/proc` is mounted inside so tools like `ps` work correctly within the container.

The host kernel is still shared across all containers — they use the same kernel code, system calls, and scheduler. Namespace isolation is purely a visibility boundary, not a security sandbox. The host can see all container PIDs from outside, which is why our kernel module uses host PIDs for RSS tracking.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers need a parent process to reap their exit status. Without a parent calling `waitpid()`, exited children become zombies — they stay in the process table indefinitely consuming a PID slot. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, WNOHANG)` in a loop to reap all exited children immediately.

The supervisor also maintains the metadata linked list. Each `container_record_t` tracks state transitions: `starting` → `running` → `exited` or `killed`. When a container is reaped, its exit code and signal are recorded. The metadata is protected by a mutex because the logger thread, producer threads, and the main event loop all access it concurrently.

### 3. IPC, Threads, and Synchronization

The project uses two IPC mechanisms. The first is a **pipe** per container: the container's stdout/stderr are redirected into the write end via `dup2()`, and the supervisor reads the read end in a dedicated producer thread. The second is a **UNIX domain socket** at `/tmp/mini_runtime.sock` for the CLI control plane — CLI commands connect, send a `control_request_t`, and receive a `control_response_t`.

The bounded buffer sits between producer threads (one per container) and one consumer (the logger thread). Without synchronization, producers and the consumer could corrupt the head/tail indices simultaneously. We use a `pthread_mutex_t` to protect the buffer state and two `pthread_cond_t` variables — `not_empty` (consumer waits here when buffer is empty) and `not_full` (producers wait here when buffer is full). This prevents both lost data and busy-waiting. The container metadata list uses a separate mutex because its access patterns (insert on start, update on SIGCHLD, read on ps) are independent from the log buffer.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped into a process's address space. It does not measure virtual memory that has been allocated but not yet touched, memory-mapped files that have been paged out, or shared libraries counted once per process. RSS is the right metric for memory enforcement because it reflects actual physical memory pressure.

Soft and hard limits serve different purposes. A soft limit is a warning threshold — when RSS crosses it, the kernel module logs a `KERN_WARNING` to dmesg once per container. This allows the operator to observe memory growth without disrupting the workload. A hard limit is a termination threshold — when RSS crosses it, the module sends `SIGKILL`. The enforcement belongs in kernel space because a user-space monitor could be delayed by scheduling, killed itself, or tricked by a process that allocates memory faster than the polling interval. The kernel timer fires reliably every second regardless of user-space load.

### 5. Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) which assigns CPU time proportional to each task's weight, derived from its nice value. A process with nice 0 has weight 1024; nice 10 has weight 110 — roughly a 9x difference in CPU share when competing on the same core.

In Experiment 1, both cpu-low (nice 0) and cpu-high (nice 10) ran for 10 seconds on a multi-core machine and completed similar amounts of work. This is because the Dell has multiple cores — CFS assigned each process its own core, so the nice difference had minimal effect. On a single-core machine the difference would be dramatic.

In Experiment 2, cpu-exp (CPU-bound) and io-exp (I/O-bound) ran concurrently. The cpu-exp ran its 10-second computation loop while io-exp completed 20 I/O iterations in the same window. The I/O-bound process voluntarily blocks on each write, causing it to yield the CPU. CFS rewards this by giving it a higher vruntime catch-up — when it wakes from I/O, it gets scheduled promptly because its virtual runtime is behind the CPU-bound process. This demonstrates CFS's bias toward interactive/I/O workloads for responsiveness.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` + `chroot()`.
**Tradeoff:** `chroot()` is simpler than `pivot_root()` but offers weaker isolation — a privileged process can escape a chroot. For a project runtime running as root this is acceptable.
**Justification:** Meets the project requirements for PID, UTS, and mount isolation while keeping the implementation straightforward.

### Supervisor Architecture
**Choice:** Single long-running supervisor process with a UNIX domain socket event loop using `select()`.
**Tradeoff:** Single-threaded event loop means one slow CLI request blocks others. A multi-threaded accept loop would be faster but more complex.
**Justification:** The project requires sequential command handling and the latency is acceptable for a demo runtime.

### IPC and Logging
**Choice:** Pipes for log data, UNIX domain socket for control plane, bounded circular buffer with mutex + condvar.
**Tradeoff:** Pipes are unidirectional and per-container, which means one producer thread per container. A shared socket would reduce threads but complicate routing.
**Justification:** Pipes are the natural fit for capturing stdout/stderr from a child process. The bounded buffer prevents unbounded memory growth if the consumer falls behind.

### Kernel Monitor
**Choice:** `mutex_lock` over `spinlock` for the monitored list.
**Tradeoff:** Mutexes can sleep, which means they cannot be used in hard interrupt context. Spinlocks can be used anywhere but waste CPU cycles spinning.
**Justification:** The timer callback runs in softirq-deferred context on modern kernels and the ioctl path may sleep during `copy_from_user`. A mutex is safe for both paths and avoids wasted CPU cycles.

### Scheduler Experiments
**Choice:** Used the provided `cpu_hog` and `io_pulse` workloads with nice values and concurrent execution.
**Tradeoff:** Results on a multi-core machine are less dramatic than single-core because CFS can isolate workloads to separate cores.
**Justification:** Demonstrates CFS behavior clearly — nice values affect single-core contention, and the CPU vs I/O comparison shows CFS's responsiveness bias regardless of core count.

---

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound workloads at different nice values

Both containers ran for 10 seconds on a multi-core machine.

| Container | Nice Value | Duration | Final Accumulator |
|-----------|-----------|----------|-------------------|
| cpu-low | 0 | 10s | 8943688564789692687 |
| cpu-high | 10 | 10s | 11229769910692783842 |

**Analysis:** Both completed in 10 seconds with similar accumulator values because the machine has multiple cores and CFS assigned each process its own core. The nice difference (weight ratio ~9:1) would show a significant CPU share difference on a single-core machine. On multi-core hardware, nice values primarily matter when all cores are saturated.

### Experiment 2: CPU-bound vs I/O-bound at same nice value

Both containers ran concurrently at nice 0.

| Container | Type | Duration | Work Completed |
|-----------|------|----------|----------------|
| cpu-exp | CPU-bound | 10s | 10 computation iterations |
| io-exp | I/O-bound | ~10s | 20 I/O iterations |

**Analysis:** The I/O-bound process completed more iterations because it voluntarily yields the CPU on each write operation. CFS tracks virtual runtime (vruntime) per process — when io-exp blocks on I/O its vruntime stops accumulating, so when it wakes up CFS schedules it quickly to catch up. This demonstrates CFS's design goal of fairness: processes that don't use their full time slice are rewarded with prompt scheduling when they need CPU again, improving responsiveness for interactive and I/O-bound workloads.
