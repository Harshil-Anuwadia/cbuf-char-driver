# cbuf-char-driver

A Linux character device driver that creates circular buffer FIFOs under `/dev/` (e.g., `/dev/cbuf0`, `/dev/cbuf1`, etc.).

I built this project to get hands-on experience with Linux kernel development beyond basic "hello world" modules—specifically dealing with real kernel concurrency, wait queues, VFS callbacks, ioctl commands, and the sysfs subsystem.

## What it does

Each device node acts like an in-memory pipe backed by a circular buffer in kernel space:
- **Blocking reads/writes**: Readers sleep when the buffer is empty; writers sleep when it's full.
- **Non-blocking support**: Supports `O_NONBLOCK` (returns `-EAGAIN` instead of blocking).
- **I/O multiplexing**: Works out of the box with `poll`, `select`, and `epoll`.
- **Async notifications**: Supports `fasync` (`SIGIO`) for event-driven apps.
- **Multiple devices**: Spin up as many independent devices as you want (defaults to 4).
- **sysfs & ioctl**: Check buffer stats directly in `/sys/class/cbuf/` or resize/flush buffers at runtime via `ioctl`.

## Quick start

### 1. Build
Make sure you have your kernel headers installed (`kernel-devel` on Fedora/openSUSE or `linux-headers-$(uname -r)` on Debian/Ubuntu).

```bash
make
# or
./build.sh
```

### 2. Load the driver
Insert the module using root privileges. You can optionally specify how many devices you want and their buffer size:

```bash
# default: 4 devices, 4KB buffer each
sudo ./load.sh

# or customize: 2 devices with 8KB buffers
sudo ./load.sh 2 8192
```

This registers the devices and sets up `/dev/cbuf0` through `/dev/cbuf3`.

### 3. Try it out
You can test blocking I/O using two simple terminal windows:

**Terminal 1 (Reader):**
```bash
cat /dev/cbuf0
```
*(It will block and wait for incoming data)*

**Terminal 2 (Writer):**
```bash
echo "hello from userspace" > /dev/cbuf0
```
The text will immediately show up in Terminal 1.

### 4. Check stats in sysfs
You can inspect the state of any device in real-time under `/sys/class/cbuf/`:

```bash
cat /sys/class/cbuf/cbuf0/occupancy      # current bytes in buffer
cat /sys/class/cbuf/cbuf0/buf_size       # total capacity
cat /sys/class/cbuf/cbuf0/total_written  # lifetime bytes written
cat /sys/class/cbuf/cbuf0/total_read     # lifetime bytes read
```

### 5. Unload
When you're done:
```bash
sudo ./unload.sh
```

## How it works under the hood

- **Multi-device architecture**: Instead of using global static variables, each device gets its own `struct cbuf_device` allocated dynamically with `kvzalloc()`. When `/dev/cbufN` is opened, the driver uses `container_of` to retrieve the device struct from the inode's `cdev` and attaches it to `file->private_data`.
- **Locking & Wait Queues**: Buffer operations are protected by a per-device `mutex`. Two wait queues (`rd_wait` and `wr_wait`) handle putting processes to sleep and waking them up via `wait_event_interruptible()` and `wake_up_interruptible()`.
- **ioctl interface**:
  - `CBUF_IOC_FLUSH`: Clears the buffer pointers and unblocks waiting writers.
  - `CBUF_IOC_STATS`: Fills a `struct cbuf_stats` with live byte counters and reader/writer counts.
  - `CBUF_IOC_RESIZE`: Dynamically reallocates the buffer to a new power-of-two size while preserving any data currently queued up.

## License

GPL-2.0
