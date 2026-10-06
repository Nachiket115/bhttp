# Binary HTTP Protocol Specification (bhttp/1)

## 1. Overview
The **bhttp** protocol is a lightweight, binary application-layer protocol providing request/response semantics over a single persistent TCP connection. Designed as a binary alternative to text-based HTTP/1.1, all messages are framed into explicit byte-aligned binary envelopes, eliminating message demarcation ambiguities and text-parsing overhead.

---

## 2. Frame Header
Every transmission across a bhttp connection begins with a fixed **8-byte frame header**. All multi-byte integers are encoded in network byte order (**big-endian**).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Version    |     Type      |     Flags     | Header Count  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Offset | Field | Type | Description |
|:---|:---|:---|:---|
| `0` | `version` | `uint8` | Protocol version. Current version is `0x01`. Frames with other versions must be rejected. |
| `1` | `type` | `uint8` | Frame type: `0x01` = **REQUEST**, `0x02` = **RESPONSE**. All other values are unrecognized. |
| `2` | `flags` | `uint8` | Reserved flags. Senders must emit `0x00`. Receivers must ignore unrecognized bits. |
| `3` | `header_count` | `uint8` | Number of header entries present in the frame payload. |
| `4–7` | `length` | `uint32` | Size of the payload immediately following this header in bytes (big-endian). |

Implementations impose a maximum payload size of 16 MB (`16,777,216` bytes) to prevent resource exhaustion.

---

## 3. Forward Compatibility Rule
Extensibility is guaranteed through a mandatory skip requirement:

> **A receiver that encounters an unrecognized frame type (any value other than `0x01` or `0x02`) MUST read exactly `length` payload bytes from the stream, discard them, and resume processing subsequent frames.**

Receivers must never close the connection or treat an unrecognized frame type as an error. This rule enables future revisions (such as a version 2 protocol introducing stream multiplexing or push frames) to interoperate safely with version 1 endpoints without breaking framing alignment.

---

## 4. REQUEST Payload Layout (`type = 0x01`)
A REQUEST frame implements GET-only semantics and carries no request body. Senders transmit the resource path followed by optional request headers:

```
+---------------+-------------------+-----------------------+
| path_len (1B) | path (path_len B) | headers (optional...) |
+---------------+-------------------+-----------------------+
```

| Field | Size | Description |
|:---|:---|:---|
| `path_len` | `1 byte` (`uint8`) | Byte length of the ASCII path string. Must be $\ge 1$. |
| `path` | `path_len` bytes | Target resource path (e.g. `/index.html`), without null-terminator. |
| `headers` | Variable | Exactly `header_count` consecutive encoded header records. Minimal clients emit `header_count = 0`. |

---

## 5. RESPONSE Payload Layout (`type = 0x02`)
A RESPONSE frame conveys the status code, response headers, and the response entity body:

```
+-------------+-----------------------+-----------------------------+
| status (2B) | headers (variable...) | body (length - offset bytes)|
+-------------+-----------------------+-----------------------------+
```

| Field | Size | Description |
|:---|:---|:---|
| `status` | `2 bytes` (`uint16` BE) | Numeric HTTP-style status code. |
| `headers` | Variable | Exactly `header_count` consecutive encoded header records. |
| `body` | Remainder | Raw entity body payload spanning from the end of the last header to the frame boundary. |

### Status Codes
- `200`: Success. The requested resource follows in the body.
- `400`: Bad Request. Frame malformed or invalid request parameters.
- `404`: Not Found. Resource does not exist under the server document root.

---

## 6. Header Entry Encoding
Each header entry uses an ID-based prefix inspired by static HPACK tables:

```
+--------+-------------------+--------------------+-------------------+
| ID(1B) | [name_len + name] | value_len (2B, BE) | value (value_len) |
+--------+-------------------+--------------------+-------------------+
```

- **ID (`uint8`)**:
  - `0`: Custom header follows. Must be immediately followed by `name_len` (`uint8`) and `name` (`name_len` ASCII bytes).
  - `1–10`: Predefined static table ID. Name is omitted from the wire.
- **Value Length (`uint16` BE)**: Byte length of the value.
- **Value**: Raw header value bytes (non-null terminated).

### Predefined Header Table
| ID | Header Name | ID | Header Name |
|:---|:---|:---|:---|
| `1` | `Content-Type` | `6` | `Server` |
| `2` | `Content-Length` | `7` | `User-Agent` |
| `3` | `Host` | `8` | `Accept` |
| `4` | `Connection` | `9` | `Cache-Control` |
| `5` | `Date` | `10` | `Last-Modified` |

This minimal static file server implementation only emits `Content-Type` (ID `1`) and `Content-Length` (ID `2`) on the wire. The remaining eight table entries are standardized for protocol completeness and forward use (e.g., future support for conditional requests, caching, or virtual hosting), ensuring consistent numeric assignments across compatible implementations.

---

## 7. Error Handling & Stream Synchronization
Because TCP is an un-delimited byte stream, errors are categorized into two tiers:

1. **Header-Level Errors (Stream Desynchronization)**:
   If an incoming frame header contains an unsupported version (`version != 0x01`) or a payload length exceeding safety boundaries (`length > 16 MB`), the receiver cannot reliably locate the next frame boundary. The receiver must transmit a `400 Bad Request` frame and **immediately terminate the TCP connection**.
2. **Payload-Level Errors**:
   If the frame header is valid, but the payload itself is malformed (e.g., truncated path, path traversal attempt `..`, invalid header length prefix, or non-existent file), the frame boundary remains intact. The server responds with `400 Bad Request` or `404 Not Found`, consumes any remaining payload bytes according to the header's `length`, and **keeps the connection open** for subsequent requests.

---

## 8. Design Rationale: Field Widths & Alignment
HTTP/2 specifies a 9-byte frame header packed across bit boundaries (`24` bits length, `8` bits type, `8` bits flags, `1` bit reserved, `31` bits stream ID). 

The **bhttp** protocol intentionally adopts an **8-byte, fully byte-aligned layout** (`8 / 8 / 8 / 8 / 32` bits). Every protocol field naturally aligns to standard 1-byte or 4-byte boundaries. This design decision was made because:
- Parsers can read and write fields directly via native byte offsets and standard socket primitives (`ntohl`/`htonl`) without bit-shifting, bitmask extraction, or packing logic.
- Hexdumps align cleanly to standard 8-byte and 16-byte terminal boundaries, facilitating debugging and protocol auditing without specialized packet dissection tools.
- For a dedicated, single-stream protocol, the trade-off of a few padding bytes per frame is negligible compared to the substantial reduction in implementation complexity.

---

## 9. Worked Example: GET /index.html
The following represents an actual captured exchange requesting a 216-byte HTML document.

### Request Frame (20 bytes total)
```
  0000  01 01 00 00 00 00 00 0c  0b 2f 69 6e 64 65 78 2e  |........./index.|
  0010  68 74 6d 6c                                       |html|
```
- `01`: `version = 0x01`
- `01`: `type = 0x01` (REQUEST)
- `00`: `flags = 0x00`
- `00`: `header_count = 0`
- `00 00 00 0c`: `length = 12` bytes
- `0b`: `path_len = 11`
- `2f 69 6e 64 65 78 2e 68 74 6d 6c`: ASCII string `"/index.html"`

### Response Frame (244 bytes total)
```
  0000  01 02 00 02 00 00 00 ec  00 c8 01 00 09 74 65 78  |.............tex|
  0010  74 2f 68 74 6d 6c 02 00  03 32 31 36 3c 21 44 4f  |t/html...216<!DO|
  0020  43 54 59 50 45 20 68 74  ...
```
- `01`: `version = 0x01`
- `02`: `type = 0x02` (RESPONSE)
- `00`: `flags = 0x00`
- `02`: `header_count = 2`
- `00 00 00 ec`: `length = 236` bytes
- `00 c8`: `status = 200`
- `01`: Header 1 ID = `1` (`Content-Type`)
- `00 09`: Header 1 value length = `9` bytes
- `74 65 78 74 2f 68 74 6d 6c`: Header 1 value = `"text/html"`
- `02`: Header 2 ID = `2` (`Content-Length`)
- `00 03`: Header 2 value length = `3` bytes
- `32 31 36`: Header 2 value = `"216"`
- Remaining 216 bytes (`3c 21 44 ...`): Entity body of `index.html`
