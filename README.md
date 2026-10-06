# bhttp

A lightweight, binary application-layer HTTP protocol operating over a single persistent TCP connection. See [spec.md](spec.md) for full protocol specification and wire format details.

## Build

Compile both binaries using `make`:

```bash
make
```

Outputs `bin/bserve` and `bin/bcurl`. Run `make clean` to remove build artifacts.

## Run

### 1. Start the server
Run `bserve` with a root directory and port number:

```bash
./bin/bserve www 9000
```

Serves static files from the specified directory on the given port, keeping connections open for sequential requests and logging activity to `stderr`.

### 2. Run the client
In another terminal, run `bcurl`:

```bash
./bin/bcurl -v localhost:9000/index.html
```

- `-v` hexdumps every request and response frame to `stderr`.
- The response entity body is written directly to `stdout`.
- Exits with a non-zero status code on `4xx` or `5xx` responses.
- Supports multiple paths over a single persistent TCP connection:

```bash
./bin/bcurl -v localhost:9000/index.html /style.css /hello.txt
```

## Repository Structure

- `src/` — C source files: shared protocol definitions (`bhttp.h`), server (`bserve.c`), and client (`bcurl.c`).
- `www/` — Sample document root with static test files (`index.html`, `style.css`, `hello.txt`).
- `hexdump/` — Raw captured output and field-by-field annotated hexdump of a live exchange (`annotated.txt`).
- `spec.md` — Complete binary protocol specification and design rationale.
