# cbuf — Multi-Instance Circular Buffer Character Device

Linux kernel module exposing `/dev/cbuf0`..`/dev/cbufN` as stream-oriented
FIFOs backed by per-device circular buffers in kernel memory.

## Features

- **Multi-instance** — configurable number of independent devices
- **Blocking & non-blocking I/O** — `O_NONBLOCK` / `EAGAIN` semantics
- **poll/epoll/select** — multiplexed I/O readiness
- **fasync** — `SIGIO`-based async notification
- **ioctl** — flush buffer, query stats, live-resize
- **sysfs** — per-device attributes under `/sys/class/cbuf/cbufN/`
- **Dynamic allocation** — `kvzalloc`-backed buffers, power-of-2 sizing

## Build

```bash
make
# or
./build.sh
```

## Usage

Load the module:
```bash
sudo ./load.sh [num_devices] [buf_size]      # default: 4 devices, 4096 bytes
```

Read and write to the device:
```bash
echo "hello world" > /dev/cbuf0
cat /dev/cbuf0
```

Inspect sysfs metrics:
```bash
cat /sys/class/cbuf/cbuf0/buf_size
cat /sys/class/cbuf/cbuf0/occupancy
cat /sys/class/cbuf/cbuf0/total_written
cat /sys/class/cbuf/cbuf0/total_read
```

Unload when done:
```bash
sudo ./unload.sh
```

## Module Parameters

| Parameter      | Default | Description                       |
|----------------|---------|-----------------------------------|
| `num_devices`  | 4       | Number of devices (1–16)          |
| `buf_size`     | 4096    | Per-device buffer size (64–1 MiB) |

Readable at `/sys/module/cbuf/parameters/`.

## ioctl Interface

Defined in `cbuf.h`:

| Command          | Direction | Data               | Description              |
|------------------|-----------|--------------------|--------------------------|
| `CBUF_IOC_FLUSH` | —         | —                  | Clear buffer contents    |
| `CBUF_IOC_STATS` | read      | `struct cbuf_stats` | Snapshot device stats   |
| `CBUF_IOC_RESIZE`| write     | `__u32`            | Resize buffer (pow2)     |

`CBUF_IOC_RESIZE` fails with `EBUSY` if current data exceeds the new size.

## sysfs Attributes

Per-device at `/sys/class/cbuf/cbufN/`:

| Attribute       | Description                    |
|-----------------|--------------------------------|
| `buf_size`      | Current buffer capacity        |
| `occupancy`     | Bytes currently in the buffer  |
| `total_read`    | Lifetime bytes read            |
| `total_written` | Lifetime bytes written         |

## Architecture

```
 userspace:  write()  read()  poll()  ioctl()  /sys/class/cbuf/cbufN/*
               │        │       │       │                │
 ─────────── VFS ──────────────────────────── sysfs ─────┘
               │        │       │       │
 kernel:   cbuf_write cbuf_read cbuf_poll cbuf_ioctl
               │        │       │       │
               └────────┴───────┴───────┘
                         │
                  struct cbuf_device
                  ├── buf[size]  (circular)
                  ├── mutex
                  ├── rd_wait / wr_wait
                  ├── fasync_queue
                  └── stats (total_read, total_written, ...)
```

Each `struct cbuf_device` owns its buffer, lock, wait queues, and stats
independently. The `container_of` pattern links `struct cdev` back to
the parent `cbuf_device` at open time; `file->private_data` carries it
through subsequent operations.

## License

GPL-2.0
