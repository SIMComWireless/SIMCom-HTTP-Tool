[Chinese](README_cn.md)

# SIMCom HTTP Tool

A small command-line C program that talks to a SIMCom cellular module over a serial (COM) port and downloads a file from an HTTP server via AT commands.

This project implements overlapped (asynchronous) serial I/O, a thread-safe ring buffer for received bytes, an AT-command sequence to start the module's HTTP client, and logic to download and save a file in chunks.

## Table of Contents

- [SIMCom HTTP Tool](#simcom-http-tool)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Key components](#key-components)
  - [Program flow (main)](#program-flow-main)
  - [Inputs and outputs](#inputs-and-outputs)
  - [Usage examples](#usage-examples)
  - [Error handling and edge cases](#error-handling-and-edge-cases)

## Overview

The tool opens a serial port to the SIMCom module, runs an AT command sequence to initialize the module's HTTP client, triggers an HTTP GET, and reads the returned data in chunks, saving it to a local file.

## Key components

- ### RingBuffer (thread-safe)
	- Fixed-size circular buffer: `RING_BUFFER_SIZE = 8192`.
	- Protected by a `CRITICAL_SECTION`.
	- Functions: `ring_buffer_init`, `ring_buffer_put`, `ring_buffer_put_bulk`, `ring_buffer_get`, `ring_buffer_read_bulk`, `ring_buffer_peek`, `ring_buffer_find_char`, `ring_buffer_available`.

- ### SerialPort (struct)
	- Holds `HANDLE hCom`, pointer to the Rx `RingBuffer`, and a `running` flag for the receive thread.

- ### serial_receive_thread
	- Background thread that performs overlapped `ReadFile` calls on the COM port.
	- Pushes received bytes into the `RingBuffer`.

- ### open_serial_port(const char* portName, int baudRate)
	- Opens COM port with `CreateFileA(..., FILE_FLAG_OVERLAPPED)`.
	- Configures `DCB` for baud, 8-N-1, DTR/RTS, and timeouts.
	- Returns `HANDLE` or `INVALID_HANDLE_VALUE` on error.

- ### send_at_command(HANDLE hCom, const char* command)
	- Sends an AT command (appends `\r\n`) with overlapped `WriteFile` and waits for completion.

- ### Helpers
	- `read_line_from_buffer`, `wait_for_response`, `parse_number_response` — read and parse newline-terminated responses from the ring buffer.
	- `enumerate_serial_ports` — quick probe of `COM1..COM20` to list available ports.

- ### download_file_data(HANDLE hCom, RingBuffer* rb, const char* filename, int total_size)
	- Uses an AT-based download protocol (`AT+CFTPSGET="<file>",<offset>,<len>`) to fetch file chunks.
	- Reads `+CFTPSGET: DATA,<len>` control lines, then reads the specified number of binary bytes from the ring buffer.
	- Writes chunks to a local file, prints a hex preview and progress, and retries on certain server return codes.

## Program flow (main)

1. Parse command-line arguments:
	 - Positional: `<COM> <HTTP_URL> <LOCAL_FILENAME> [BAUD]`.
	 - Missing values are requested interactively; default baud is `115200`.
2. Initialize the `RingBuffer`.
3. Open the serial port with `open_serial_port(portName, baudRate)`.
4. Start `serial_receive_thread` to collect incoming data.
5. Execute AT sequence (each send uses `send_at_command` and expects a response):
	 - `AT` (check alive)
	 - `AT+HTTPINIT`
	 - `AT+HTTPPARA="SSLCFG,1"` (example SSL config)
	 - `AT+HTTPPARA="URL","<http_url>"`
	 - `AT+HTTPACTION=0` (trigger HTTP GET)
6. (Optional/commented) Query file size with `AT+CFTPSSIZE`.
7. Call `download_file_data` to fetch and save the file.
8. Cleanup: stop thread, close handles, delete critical section.

## Inputs and outputs

- Inputs:
	- Command-line or interactive inputs: COM port (e.g., `COM3`), HTTP URL, local filename, optional baud rate.
	- Serial data from SIMCom module (AT responses and binary payloads).

- Outputs:
	- Console logs: commands, responses, hex previews, progress, and errors.
	- A local binary file saved with the downloaded content.

## Usage examples

- Non-interactive (all args):

```powershell
SIMCom_HTTP_Tool.exe COM3 http://example.com/file.bin file.bin 115200
```

- Interactive (leave args out and follow prompts):

```powershell
SIMCom_HTTP_Tool.exe
# then enter COM, URL, filename, and baud when prompted
```

## Error handling and edge cases

- Opening the serial port returns `INVALID_HANDLE_VALUE` on failure — the program reports and exits.
- Overlapped I/O uses events and timeouts; writes are cancelled on timeout.
- Ring buffer overflow causes the receive thread to wait (it sleeps briefly until space becomes available).
- `download_file_data` retries offsets for certain server response codes (bounded by `MAX_OFFSET_RETRIES`).
- The code relies on fixed timeouts and polling (`Sleep(1)`), so timing-dependent issues may arise for slow or very large transfers.

