# Adaptive Reliable Sender (MP1)

**Recommended GitHub repository name:** `adaptive-reliable-sender`

This project implements a reliable file sender for the Computer Networks MP1 simulator. The sender reads an input file, splits it into frames, attaches a CRC checksum, sends each frame through `netsim`, and retries when the simulator returns `NAK`.

The implementation is optimized for noisy links by adapting the payload size based on recent `ACK`/`NAK` history.

## What it does

Each transmitted frame has the following format:

```text
[2-byte payload size, big-endian] [payload bytes] [4-byte CRC]
```

The sender:

- builds frames from arbitrary binary input files;
- computes CRC-32 over the size header and payload;
- waits for `ACK`/`NAK` from `send_frame()`;
- retransmits corrupted frames;
- estimates the current bit-error rate from recent attempts;
- adjusts payload size between 16 and 65,535 bytes to reduce expected transfer cost.

## Main source file

```text
sender_20211546.cc
```

This is the only file that should be submitted for grading. The simulator files are included only for local development and validation.

## Project layout

```text
.
├── sender_20211546.cc       # Reliable sender implementation
├── netsim.h                 # Simulator API header
├── netsim_lib.cc            # Simulator library used for local builds
├── netsim                   # Simulator executable
├── run_full_check.sh        # Local validation helper
├── validation_logs/         # Public validation run outputs
├── check_logs/              # Extended check summaries
├── edge_inputs/             # Small and edge-case input files
├── sherlock_holmes.txt      # Public validation input
├── harry_potter.txt         # Public validation input
└── cat_bgm.mp3              # Public validation input
```

## Build

Use the same grading-style compile command:

```bash
g++ -O2 -o sender_20211546 sender_20211546.cc netsim_lib.cc
```

If the simulator binary is not executable after unpacking the ZIP, run:

```bash
chmod +x netsim
```

## Run examples

```bash
./netsim ./sender_20211546 \
  --input sherlock_holmes.txt \
  --output out.rx \
  --ber 1e-6 \
  --seed 1001 \
  --max_bytes 386832300

diff sherlock_holmes.txt out.rx
```

```bash
./netsim ./sender_20211546 \
  --input cat_bgm.mp3 \
  --output out.rx \
  --ber 1e-4 \
  --seed 3003 \
  --max_bytes 316224000

diff cat_bgm.mp3 out.rx
```

## Full local check

```bash
chmod +x run_full_check.sh
./run_full_check.sh
```

The included logs show that the implementation completed the public validation scenarios successfully, with received output matching the original input files.

## Implementation notes

### CRC generation

`crc32_mod2()` builds a lookup table for polynomial `0x04C11DB7` and computes the checksum over:

```text
payload-size header + payload
```

The checksum is then appended in big-endian order.

### Adaptive payload sizing

`PayloadController` keeps a bounded history of recent transmission attempts. For each attempt, it records:

- the number of protected bits in the frame;
- whether the attempt ended with `ACK` or `NAK`.

From this history, it estimates the bit-error rate and chooses a payload size that minimizes the expected cost per delivered byte:

```text
(P + header + CRC + K) / delivered payload probability
```

where `P` is the payload size and `K` is the simulator's fixed round-trip cost.

### NAK handling

On repeated `NAK`s, the sender may reduce the current frame size instead of retransmitting the same large frame indefinitely. If only the prefix of the originally-read chunk is delivered, the input stream is rewound to the first undelivered byte so the remaining suffix is sent in the next frame.

## Submission note

For the actual MP1 submission, submit only:

```text
sender_<student_id>.cc
```

Do not submit simulator binaries, validation inputs, output files, or logs unless the course staff explicitly asks for them.
