# miniredis

A small, from-scratch **key-value store server written in modern C++20**, wire-compatible with the [Redis](https://redis.io) protocol (RESP2). Because it speaks the real protocol, you can talk to it with the official `redis-cli` and drive load against it with `redis-benchmark`.

This is a learning-oriented reimplementation of the core ideas behind an in-memory data store: TCP networking, protocol parsing, concurrent access to shared state, and a clean layered design.

## Features

- **RESP2 protocol** — connect with the standard `redis-cli`, no custom client needed
- **Concurrent** — one thread per client over a thread-safe store; serves many clients at once
- **Incremental parser** — correctly handles TCP fragmentation (half commands, batched commands)
- **Commands:** `PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS`, `DBSIZE`, `COMMAND`
- **Zero dependencies** beyond the standard library and the OS sockets API

## Architecture

The code is split into layers so each piece can be understood and tested on its own:

| File | Responsibility |
|------|----------------|
| `store.{hpp,cpp}` | Thread-safe key/value map (the data) |
| `resp.{hpp,cpp}` | RESP protocol parsing and reply encoding (the wire format) |
| `commands.{hpp,cpp}` | Command execution — pure logic, knows nothing about sockets |
| `server.{hpp,cpp}` | TCP accept loop and per-client threads (the networking) |
| `main.cpp` | Entry point / argument parsing |

The `commands` layer takes a `Store` and a parsed argument list and returns reply bytes, so it can be unit-tested without any network.

## Build

Requires a C++20 compiler and CMake 3.15+.

```bash
cmake -S . -B build
cmake --build build
```

On Windows with MinGW, add `-G "MinGW Makefiles"` to the configure step.

## Run

```bash
./build/miniredis        # listens on port 6379 (Redis default)
./build/miniredis 6400   # or a port of your choice
```

Then, from another terminal:

```bash
redis-cli -p 6379
> SET name Ada
OK
> GET name
"Ada"
> EXISTS name missing
(integer) 1
```

## Roadmap

- [x] **Phase 1** — TCP server, RESP protocol, core commands, concurrency
- [ ] **Phase 2** — key expiration (TTL), more data types, sharded locking
- [ ] **Phase 3** — persistence (append-only file / snapshots)
- [ ] **Phase 4** — unit tests, benchmarks, CI

## License

MIT
