#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RING_BUFFER_SIZE 8192
#define MAX_PACKET_SIZE 8192
#define MAX_RESPONSE_SIZE 8192
#define MAX_OFFSET_RETRIES 5


typedef struct {
    char buffer[RING_BUFFER_SIZE];
    int head;
    int tail;
    int count;
    CRITICAL_SECTION lock;
} RingBuffer;

typedef struct {
    HANDLE hCom;
    RingBuffer* rxBuffer;
    volatile int running;
} SerialPort;

// Ring buffer functions
void ring_buffer_init(RingBuffer* rb) {
    memset(rb->buffer, 0, RING_BUFFER_SIZE);
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    InitializeCriticalSection(&rb->lock);
}

int ring_buffer_put(RingBuffer* rb, char data) {
    EnterCriticalSection(&rb->lock);

    if (rb->count >= RING_BUFFER_SIZE) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }

    rb->buffer[rb->head] = data;
    rb->head = (rb->head + 1) % RING_BUFFER_SIZE;
    rb->count++;

    LeaveCriticalSection(&rb->lock);
    return 1;
}

// Bulk write 'len' bytes from src into ring buffer (returns bytes written)
int ring_buffer_put_bulk(RingBuffer* rb, const char* src, int len) {
    EnterCriticalSection(&rb->lock);
    if (len <= 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    int freeSpace = RING_BUFFER_SIZE - rb->count;
    int toWrite = len > freeSpace ? freeSpace : len;
    if (toWrite <= 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    if (rb->head + toWrite <= RING_BUFFER_SIZE) {
        memcpy(rb->buffer + rb->head, src, toWrite);
        rb->head = (rb->head + toWrite) % RING_BUFFER_SIZE;
        rb->count += toWrite;
        LeaveCriticalSection(&rb->lock);
        return toWrite;
    }
    int first = RING_BUFFER_SIZE - rb->head;
    memcpy(rb->buffer + rb->head, src, first);
    int second = toWrite - first;
    if (second > 0) memcpy(rb->buffer, src + first, second);
    rb->head = (rb->head + toWrite) % RING_BUFFER_SIZE;
    rb->count += toWrite;
    LeaveCriticalSection(&rb->lock);
    return toWrite;
}

int ring_buffer_get(RingBuffer* rb, char* data) {
    EnterCriticalSection(&rb->lock);

    if (rb->count <= 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }

    *data = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
    rb->count--;

    LeaveCriticalSection(&rb->lock);
    return 1;
}

int ring_buffer_available(RingBuffer* rb) {
    return rb->count;
}

// Peek at a byte at 'index' (0..count-1) from tail without removing it.
// Returns 1 on success and sets *out, 0 if index out of range.
int ring_buffer_peek(RingBuffer* rb, int index, char* out) {
    EnterCriticalSection(&rb->lock);
    if (index < 0 || index >= rb->count) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    int pos = (rb->tail + index) % RING_BUFFER_SIZE;
    *out = rb->buffer[pos];
    LeaveCriticalSection(&rb->lock);
    return 1;
}

// Find first occurrence of 'ch' in buffer; returns zero-based index from tail or -1 if not found.
int ring_buffer_find_char(RingBuffer* rb, char ch) {
    EnterCriticalSection(&rb->lock);
    int cnt = rb->count;
    if (cnt <= 0) {
        LeaveCriticalSection(&rb->lock);
        return -1;
    }

    // If data is contiguous from tail, search in one block
    if (rb->tail + cnt <= RING_BUFFER_SIZE) {
        void* p = memchr(rb->buffer + rb->tail, (int)ch, (size_t)cnt);
        if (p) {
            int idx = (int)((char*)p - (rb->buffer + rb->tail));
            LeaveCriticalSection(&rb->lock);
            return idx;
        }
        LeaveCriticalSection(&rb->lock);
        return -1;
    }

    // Wrapped case: search first segment then second
    int first = RING_BUFFER_SIZE - rb->tail;
    void* p1 = memchr(rb->buffer + rb->tail, (int)ch, (size_t)first);
    if (p1) {
        int idx = (int)((char*)p1 - (rb->buffer + rb->tail));
        LeaveCriticalSection(&rb->lock);
        return idx;
    }
    int second = cnt - first;
    if (second > 0) {
        void* p2 = memchr(rb->buffer, (int)ch, (size_t)second);
        if (p2) {
            int idx = first + (int)((char*)p2 - rb->buffer);
            LeaveCriticalSection(&rb->lock);
            return idx;
        }
    }

    LeaveCriticalSection(&rb->lock);
    return -1;
}

// Read up to 'length' bytes from buffer into dest, removing them. Returns bytes read.
int ring_buffer_read_bulk(RingBuffer* rb, char* dest, int length) {
    EnterCriticalSection(&rb->lock);
    if (length <= 0 || rb->count == 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    int toRead = length;
    if (toRead > rb->count) toRead = rb->count;

    if (rb->tail + toRead <= RING_BUFFER_SIZE) {
        memcpy(dest, rb->buffer + rb->tail, toRead);
        rb->tail = (rb->tail + toRead) % RING_BUFFER_SIZE;
        rb->count -= toRead;
        LeaveCriticalSection(&rb->lock);
        return toRead;
    }

    int first = RING_BUFFER_SIZE - rb->tail;
    memcpy(dest, rb->buffer + rb->tail, first);
    int second = toRead - first;
    if (second > 0) memcpy(dest + first, rb->buffer, second);
    rb->tail = (rb->tail + toRead) % RING_BUFFER_SIZE;
    rb->count -= toRead;
    LeaveCriticalSection(&rb->lock);
    return toRead;
}

// Try to cancel an overlapped I/O operation. Prefer CancelIoEx when available,
// fall back to CancelIo which cancels all pending I/O for the thread.
int try_cancel_overlapped(HANDLE hCom, LPOVERLAPPED pov) {
    typedef BOOL(WINAPI* PCANCELIOEX)(HANDLE, LPOVERLAPPED);
    HMODULE h = GetModuleHandleA("kernel32.dll");
    if (h) {
        PCANCELIOEX p = (PCANCELIOEX)GetProcAddress(h, "CancelIoEx");
        if (p) {
            if (p(hCom, pov)) return 1;
            return 0;
        }
    }
    // Fallback
    CancelIo(hCom);
    return 1;
}

// Wait for either a specific pattern anywhere in the receive buffer OR a full
// line (ending with '\n'). If 'pattern' is found this returns 1 and places
// the consumed bytes (up to pattern end) into 'out' (null-terminated). If a
// full line is available but the pattern wasn't found, it consumes that line
// and returns 2 and places the line into 'out'. Returns 0 on timeout.
int wait_for_pattern_or_line(RingBuffer* rb, const char* pattern, char* out, int out_size, int timeout_ms) {
    DWORD start = GetTickCount();
    int pat_len = (int)strlen(pattern);
    if (out_size <= 0) return 0;

    while ((GetTickCount() - start) < (DWORD)timeout_ms) {
        int avail = ring_buffer_available(rb);
        if (avail > 0) {
            int toCopy = avail > RING_BUFFER_SIZE ? RING_BUFFER_SIZE : avail;
            // copy into temp buffer using peek
            char tmp[RING_BUFFER_SIZE + 1];
            int i;
            for (i = 0; i < toCopy; ++i) {
                ring_buffer_peek(rb, i, &tmp[i]);
            }
            tmp[toCopy] = '\0';

            // search for pattern
            char* p = strstr(tmp, pattern);
            if (p) {
                int pos = (int)(p - tmp);
                int consume = pos + pat_len;
                int toRead = consume < (out_size - 1) ? consume : (out_size - 1);
                int n = ring_buffer_read_bulk(rb, out, toRead);
                out[n] = '\0';
                return 1; // pattern found
            }

            // if there's a newline, return that line
            char* nl = strchr(tmp, '\n');
            if (nl) {
                int pos = (int)(nl - tmp) + 1;
                int toRead = pos < (out_size - 1) ? pos : (out_size - 1);
                int n = ring_buffer_read_bulk(rb, out, toRead);
                out[n] = '\0';
                return 2; // returned a line (no pattern)
            }
        }
        Sleep(10);
    }
    return 0; // timeout
}

// Serial receive thread (uses OVERLAPPED asynchronous reads to reduce blocking)
DWORD WINAPI serial_receive_thread(LPVOID param) {
    SerialPort* serial = (SerialPort*)param;
    DWORD bytesRead = 0;
    char readBuffer[256];
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (serial->running) {
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(serial->hCom, readBuffer, sizeof(readBuffer), &bytesRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD wait = WaitForSingleObject(ov.hEvent, 500);
                if (wait == WAIT_OBJECT_0) {
                    GetOverlappedResult(serial->hCom, &ov, &bytesRead, FALSE);
                }
                else {
                    // timeout or other
                    bytesRead = 0;
                }
            }
            else {
                // immediate error
                Sleep(1);
                bytesRead = 0;
            }
        }

        if (bytesRead > 0) {
            int remaining = (int)bytesRead;
            char* ptr = readBuffer;
            while (remaining > 0) {
                int w = ring_buffer_put_bulk(serial->rxBuffer, ptr, remaining);
                if (w <= 0) {
                    // buffer full, wait for consumer
                    Sleep(1);
                    continue;
                }
                ptr += w;
                remaining -= w;
            }
        }
    }

    CloseHandle(ov.hEvent);
    return 0;
}

// Serial port functions
HANDLE open_serial_port(const char* portName, int baudRate) {
    HANDLE hCom;
    char fullPortName[20];
    DCB dcb;
    COMMTIMEOUTS timeouts;

    sprintf_s(fullPortName, sizeof(fullPortName), "\\\\.\\%s", portName);

    // Open overlapped so we can do async I/O
    hCom = CreateFileA(fullPortName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (hCom == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    // Configure serial port parameters
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hCom, &dcb)) {
        CloseHandle(hCom);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(hCom, &dcb)) {
        CloseHandle(hCom);
        return INVALID_HANDLE_VALUE;
    }

    // Configure timeouts
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 10;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if (!SetCommTimeouts(hCom, &timeouts)) {
        CloseHandle(hCom);
        return INVALID_HANDLE_VALUE;
    }

    return hCom;
}

int send_at_command(HANDLE hCom, const char* command) {
    DWORD bytesWritten = 0;
    char fullCommand[256];
    sprintf_s(fullCommand, sizeof(fullCommand), "%s\r\n", command);

    // Use OVERLAPPED WriteFile to avoid blocking the caller. We open the port with
    // FILE_FLAG_OVERLAPPED, so this will be asynchronous when needed.
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    DWORD len = (DWORD)strlen(fullCommand);

    BOOL ok = WriteFile(hCom, fullCommand, len, &bytesWritten, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            // wait for completion with a modest timeout
            DWORD wait = WaitForSingleObject(ov.hEvent, 2000);
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(hCom, &ov, &bytesWritten, FALSE)) {
                    CloseHandle(ov.hEvent);
                    return 0;
                }
            }
            else {
                // timeout or error; cancel pending I/O
                CancelIo(hCom);
                CloseHandle(ov.hEvent);
                return 0;
            }
        }
        else {
            CloseHandle(ov.hEvent);
            return 0;
        }
    }

    CloseHandle(ov.hEvent);
    return (bytesWritten == len);
}

// Read a line from the ring buffer
int read_line_from_buffer(RingBuffer* rb, char* buffer, int bufferSize) {
    // Find newline without removing bytes first
    int idx = ring_buffer_find_char(rb, '\n');
    if (idx == -1) return 0; // no complete line yet

    int toCopy = idx + 1; // include the '\n'
    if (toCopy > bufferSize - 1) toCopy = bufferSize - 1; // avoid overflow

    int n = ring_buffer_read_bulk(rb, buffer, toCopy);
    if (n <= 0) return 0;
    buffer[n] = '\0';
    return 1;
}


// Wait for a specific response
int wait_for_response(RingBuffer* rb, const char* expected, int timeout_ms) {
    char line[256];
    DWORD startTime = GetTickCount();

    while ((GetTickCount() - startTime) < (DWORD)timeout_ms) {
        if (read_line_from_buffer(rb, line, sizeof(line))) {
            printf("Received: %s", line);

            if (strstr(line, expected) != NULL) {
                return 1;
            }
        }
        Sleep(1);
    }
    return 0;
}

// Parse numeric response
int parse_number_response(RingBuffer* rb, const char* prefix, int* value, int timeout_ms) {
    char line[256];
    DWORD startTime = GetTickCount();

    while ((GetTickCount() - startTime) < (DWORD)timeout_ms) {
        if (read_line_from_buffer(rb, line, sizeof(line))) {
            printf("Received: %s", line);

            const char* pos = strstr(line, prefix);
            if (pos != NULL) {
                pos += strlen(prefix);
                while (*pos && !isdigit(*pos)) pos++;
                if (*pos) {
                    *value = atoi(pos);
                    return 1;
                }
            }
        }
        Sleep(1);
    }
    return 0;
}

// Enumerate available serial ports
void enumerate_serial_ports() {
    printf("Available serial ports:\n");

    for (int i = 1; i <= 20; i++) {
        char portName[20];
        HANDLE hCom;

        sprintf_s(portName, sizeof(portName), "COM%d", i);
        hCom = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);

        if (hCom != INVALID_HANDLE_VALUE) {
            printf("  %s\n", portName);
            CloseHandle(hCom);
        }
    }
}

// Download file data
int download_file_data(HANDLE hCom, RingBuffer* rb, const char* filename, int total_size) {
    FILE* file;
    int offset = 0;
    int packet_size = 4096;
    char command[256];
    char line[256];
    int bytes_received = 0;

    if (fopen_s(&file, filename, "wb") != 0) {
        printf("Unable to create file %s\n", filename);
        return 0;
    }

    while (offset < total_size) {
        int current_size = (total_size - offset) > packet_size ? packet_size : (total_size - offset);
        int retries = 0;

        // Send download command
        sprintf_s(command, sizeof(command), "AT+HTTPREAD=0,10240");
        if (!send_at_command(hCom, command)) {
            printf("Failed to send command\n");
            fclose(file);
            return 0;
        }

        int data_received = 0;
        int expecting_data = 1;

        while (expecting_data) {
            if (!read_line_from_buffer(rb, line, sizeof(line))) {
                Sleep(1);
                continue;
            }

            printf("Received: %s", line);

            if (strstr(line, "+HTTPREAD: ") != NULL) {
                // Parse data length
                const char* data_pos = strstr(line, "+HTTPREAD: ");
                if (data_pos) {
                    data_pos += 11;
                    int data_len = atoi(data_pos);

                    if (data_len > 0) {
                        // Read binary data
                        char* data = (char*)malloc(data_len);
                        int bytes_read = 0;

                        while (bytes_read < data_len) {
                            if (ring_buffer_get(rb, &data[bytes_read])) {
                                bytes_read++;
                            }
                            else {
                                Sleep(1);
                            }
                        }

                        // Print 16-byte-per-line hex view with offset relative to bytes already received
                        for (int i = 0; i < data_len; ++i) {
                            if ((i % 16) == 0) {
                                // display the starting offset for this line
                                printf("\n%08X: ", bytes_received + i);
                            }
                            printf("%02X ", (unsigned char)data[i]);
                        }
                        printf("\n");

                        // Write to file
                        fwrite(data, 1, data_len, file);
                        fflush(file);

                        data_received += data_len;
                        bytes_received += data_len;
                        free(data);

                        printf("Received %d bytes, total progress: %d/%d (%.1f%%)\n",
                            data_len, bytes_received, total_size,
                            (float)bytes_received / total_size * 100);
                    }
                    else {
                        // No data length, end of data
                        expecting_data = 0;
                        offset += data_received;
                        break;
                    }
                }
            }
            else if (strstr(line, "ERROR") != NULL) {
                printf("Download error\n");
                fclose(file);
                return 0;
            }
        }
    }

    fclose(file);
    printf("File download complete, total size: %d bytes\n", bytes_received);
    return 1;
}

int main(int argc, char** argv) {
    SerialPort serial;
    RingBuffer rxBuffer;
    HANDLE hThread;
    char input[100];
    int file_size = 0;

    // Command-line parameters (positional): <COM> <HTTP_URL> <LOCAL_FILENAME> [BAUD]
    char portName[20] = { 0 };
    char http_url[260] = { 0 };
    char http_filename[100] = { 0 };
    int baudRate = 115200; // default baud rate

    // Accept partial CLI inputs; fall back to interactive prompts for missing values.
    if (argc >= 2) {
        snprintf(portName, sizeof(portName), "%s", argv[1]);
    }
    if (argc >= 3) {
        snprintf(http_url, sizeof(http_url), "%s", argv[2]);
    }
    if (argc >= 4) {
        snprintf(http_filename, sizeof(http_filename), "%s", argv[3]);
    }
    if (argc >= 5) {
        int b = atoi(argv[4]);
        if (b > 0) baudRate = b;
    }

    // If any required value is missing, prompt interactively. Also prompt for baud if not given.
    if (portName[0] == '\0' || http_url[0] == '\0' || http_filename[0] == '\0') {
        enumerate_serial_ports();

        if (portName[0] == '\0') {
            printf("\nEnter COM port to use (e.g., COM3): ");
            fgets(portName, sizeof(portName), stdin);
            portName[strcspn(portName, "\r\n")] = 0;
        }

        if (http_url[0] == '\0') {
            printf("Enter HTTP URL to download from (e.g., http://example.com/file.txt): ");
            fgets(http_url, sizeof(http_url), stdin);
            http_url[strcspn(http_url, "\r\n")] = 0;
        }

        if (http_filename[0] == '\0') {
            printf("Enter local filename to save as (e.g., file.txt): ");
            fgets(http_filename, sizeof(http_filename), stdin);
            http_filename[strcspn(http_filename, "\r\n")] = 0;
        }

        // If baud was not supplied on CLI, ask the user (allow empty to keep default)
        if (argc < 5) {
            char baud_input[32] = { 0 };
            printf("Enter baud rate (e.g., 115200) [default %d]: ", baudRate);
            fgets(baud_input, sizeof(baud_input), stdin);
            baud_input[strcspn(baud_input, "\r\n")] = 0;
            if (baud_input[0] != '\0') {
                int b = atoi(baud_input);
                if (b > 0) baudRate = b;
            }
        }
    }

    printf("=== SIMCOM HTTP File Download Tool ===\n\n");

    // Initialize ring buffer
    ring_buffer_init(&rxBuffer);
    // Open serial port
    // If no baud was provided on the command line, allow the user to enter it now
    printf("Opening serial port %s at %d baud...\n", portName, baudRate);
    serial.hCom = open_serial_port(portName, baudRate);
    serial.rxBuffer = &rxBuffer;

    if (serial.hCom == INVALID_HANDLE_VALUE) {
        printf("Unable to open serial port %s\n", portName);
        return 1;
    }

    printf("Serial port opened successfully\n");

    // Start receiver thread
    serial.running = 1;
    hThread = CreateThread(NULL, 0, serial_receive_thread, &serial, 0, NULL);
    if (hThread == NULL) {
        printf("Unable to create receiver thread\n");
        CloseHandle(serial.hCom);
        return 1;
    }

    // Execute AT command sequence
    printf("\nStarting AT command sequence...\n");

    // 1. Send AT
    printf("\n1. Sending AT command...\n");
    if (!send_at_command(serial.hCom, "AT") || !wait_for_response(&rxBuffer, "OK", 1000)) {
        printf("AT command failed\n");
        goto cleanup;
    }

    // After basic AT OK, query firmware version and subscribe (CGMR then CSUB)
    printf("\n1a. Querying firmware version (AT+CGMR)...\n");
    if (!send_at_command(serial.hCom, "AT+CGMR") || !wait_for_response(&rxBuffer, "OK", 2000)) {
        printf("AT+CGMR failed or no OK\n");
        goto cleanup;
    }

    printf("\n1b. Sending AT+CSUB...\n");
    if (!send_at_command(serial.hCom, "AT+CSUB") || !wait_for_response(&rxBuffer, "OK", 2000)) {
        printf("AT+CSUB failed or no OK\n");
        goto cleanup;
    }

    // 2. Send AT+HTTPINIT
    printf("\n2. Starting HTTP service...\n");
    if (!send_at_command(serial.hCom, "AT+HTTPINIT") || !wait_for_response(&rxBuffer, "OK", 5000)) {
        printf("Failed to start HTTP service\n");
        goto cleanup;
    }

    // 3. Send AT+HTTPPARA="SSLCFG",1
    printf("\n3. Set SSL configuration...\n");
    if (!send_at_command(serial.hCom, "AT+HTTPPARA=\"SSLCFG\",1") || !wait_for_response(&rxBuffer, "OK", 5000)) {
        printf("Failed to set SSL configuration\n");
        goto cleanup;
    }

    // 4. Send URL
    printf("\n4. Logging into HTTP server...\n");
    {
        char loginCmd[512];
        // Construct login command using HTTP parameters from CLI or interactive input
        sprintf_s(loginCmd, sizeof(loginCmd), "AT+HTTPPARA=\"URL\",\"%s\"", http_url);
        if (!send_at_command(serial.hCom, loginCmd) || !wait_for_response(&rxBuffer, "OK", 1000)) {
            printf("HTTP login failed\n");
            goto cleanup;
        }
    }

    // 5. Set AT+HTTPACTION
    printf("\n5. Set AT+HTTPACTION...\n");
    if (!send_at_command(serial.hCom, "AT+HTTPACTION=0") || !wait_for_response(&rxBuffer, "+HTTPACTION: 0,200", 10000)) {
        printf("Failed to set AT+HTTPACTION\n");
        goto cleanup;
    }

    // 6. Get file size 
    printf("\n6. Get file size...\n");
    char httphead_command[16];
    // Use filename from CLI or interactive input
    sprintf_s(httphead_command, sizeof(httphead_command), "AT+HTTPHEAD");
    if (!send_at_command(serial.hCom, httphead_command) ||
        !parse_number_response(&rxBuffer, "Content-Length: ", &file_size, 1000)) {
        printf("Failed to get file size\n");
        goto cleanup;
    }
    printf("Total file size: %d bytes\n", file_size);

    if(!wait_for_response(&rxBuffer, "OK", 1000)) {
        printf("Failed to complete HTTPHEAD command\n");
        goto cleanup;
    }

    // 7. Download file
    printf("\n7. Start downloading file...\n");
    if (!download_file_data(serial.hCom, &rxBuffer, http_filename, file_size)) {
        printf("File download failed\n");
        goto cleanup;
    }

    // After successful download, perform LFOTA upload sequence:
    // 1) Terminate HTTP
    printf("\n8. Terminating HTTP service...\n");
    if (!send_at_command(serial.hCom, "AT+HTTPTERM") || !wait_for_response(&rxBuffer, "OK", 5000)) {
        printf("AT+HTTPTERM failed\n");
        goto cleanup;
    }

    // 2) Notify module of incoming LFOTA size: AT+LFOTA=0,size
    {
        char lfota_cmd[64];
        sprintf_s(lfota_cmd, sizeof(lfota_cmd), "AT+LFOTA=0,%d", file_size);
        printf("Sending: %s\n", lfota_cmd);
        if (!send_at_command(serial.hCom, lfota_cmd) || !wait_for_response(&rxBuffer, "OK", 5000)) {
            printf("AT+LFOTA=0 failed\n");
            goto cleanup;
        }
    }

    // 3) Request to start LFOTA transfer: AT+LFOTA=1,size -> expect '>' prompt
    {
        char lfota_cmd[64];
        sprintf_s(lfota_cmd, sizeof(lfota_cmd), "AT+LFOTA=1,%d", file_size);
        printf("Sending: %s\n", lfota_cmd);
        if (!send_at_command(serial.hCom, lfota_cmd)) {
            printf("Failed to send AT+LFOTA=1 command\n");
            goto cleanup;
        }

        // Wait for '>' prompt indicating module is ready to receive binary data
        if (!wait_for_response(&rxBuffer, ">", 10000)) {
            printf("Did not receive '>' prompt for LFOTA data\n");
            goto cleanup;
        }

        // 4) Send entire file in a single write (more efficient than 1KB chunks)
        printf("Starting LFOTA upload of %d bytes (single write)...\n", file_size);
        FILE* f = NULL;
        if (fopen_s(&f, http_filename, "rb") != 0) {
            printf("Unable to open file for LFOTA: %s\n", http_filename);
            goto cleanup;
        }

        // Allocate buffer for whole file
        char* sendbuf_all = (char*)malloc((size_t)file_size);
        if (!sendbuf_all) {
            fclose(f);
            printf("Failed to allocate memory for LFOTA upload (%d bytes)\n", file_size);
            goto cleanup;
        }

        size_t total_read = fread(sendbuf_all, 1, (size_t)file_size, f);
        if ((int)total_read != file_size) {
            free(sendbuf_all);
            fclose(f);
            printf("Failed to read entire file for LFOTA (read %zu of %d)\n", total_read, file_size);
            goto cleanup;
        }

        // Use helper to write and drain the serial output queue
        int write_and_drain(HANDLE hCom, const char* buf, DWORD len, DWORD write_timeout_ms, DWORD drain_timeout_ms);
        printf("WriteFile (single) -> write_and_drain...\n");
        if (!write_and_drain(serial.hCom, sendbuf_all, (DWORD)total_read, 30000, 30000)) {
            free(sendbuf_all);
            fclose(f);
            printf("LFOTA single write or drain failed\n");
            goto cleanup;
        }

        // cleanup buffer and file
        free(sendbuf_all);
        fclose(f);

        // 5) After data sent, wait for final OK from module
        if (!wait_for_response(&rxBuffer, "OK", 20000)) {
            printf("LFOTA transfer did not complete (no OK)\n");
            goto cleanup;
        }

        // 6) Reboot module and monitor CFOTA progress
        printf("Sending AT+CRESET to reboot module...\n");
        if (!send_at_command(serial.hCom, "AT+CRESET")) {
            printf("Failed to send AT+CRESET\n");
            goto cleanup;
        }

        // Monitor module reports: +CFOTA: UPDATE:<process> and +CFOTA: UPDATE SUCCESS, then QCRDY
        printf("Waiting for CFOTA progress and completion (this may take several minutes)...\n");
        int got_update_success = 0;
        int got_qcrdy = 0;
        int last_progress = -1;
        char cfota_line[256];
        DWORD cfota_start = GetTickCount();
        const DWORD CFOTA_OVERALL_TIMEOUT_MS = 10 * 60 * 1000; // 10 minutes

        while (!got_qcrdy && (GetTickCount() - cfota_start) < CFOTA_OVERALL_TIMEOUT_MS) {
            if (read_line_from_buffer(&rxBuffer, cfota_line, sizeof(cfota_line))) {
                printf("Received: %s", cfota_line);

                // Check for progress lines like: +CFOTA: UPDATE:<n>
                const char* p = strstr(cfota_line, "+CFOTA: UPDATE:");
                if (p) {
                    p += strlen("+CFOTA: UPDATE:");
                    while (*p && !isdigit((unsigned char)*p)) p++;
                    if (*p) {
                        int v = atoi(p);
                        if (v != last_progress) {
                            last_progress = v;
                            printf("CFOTA progress: %d\n", v);
                        }
                        if (v >= 100) {
                            // progress indicates finished; wait for explicit SUCCESS message
                        }
                    }
                    continue;
                }

                // Check for explicit success message
                if (strstr(cfota_line, "+CFOTA: UPDATE SUCCESS") != NULL) {
                    got_update_success = 1;
                    printf("CFOTA update reported SUCCESS\n");
                    continue;
                }

                // Check for QCRDY (module ready after reboot/update)
                if (strstr(cfota_line, "QCRDY") != NULL) {
                    got_qcrdy = 1;
                    printf("Module reported QCRDY\n");
                    break;
                }
            }
            else {
                Sleep(200);
            }
        }

        if (!got_update_success) {
            printf("Did not observe CFOTA UPDATE SUCCESS within timeout\n");
            goto cleanup;
        }
        if (!got_qcrdy) {
            printf("Did not observe QCRDY within timeout\n");
            goto cleanup;
        }

        // After QCRDY, wait a short time, then query firmware and subscribe
        Sleep(2000);
        printf("Querying firmware version after update (AT+CGMR)...\n");
        if (!send_at_command(serial.hCom, "AT+CGMR") || !wait_for_response(&rxBuffer, "OK", 5000)) {
            printf("AT+CGMR failed or no OK after update\n");
            goto cleanup;
        }

        printf("Sending AT+CSUB after update...\n");
        if (!send_at_command(serial.hCom, "AT+CSUB") || !wait_for_response(&rxBuffer, "OK", 5000)) {
            printf("AT+CSUB failed or no OK after update\n");
            goto cleanup;
        }

        
    }

    printf("\n=== All operations completed ===\n");

cleanup:
    // Cleanup resources
    serial.running = 0;
    WaitForSingleObject(hThread, 1000);
    CloseHandle(hThread);
    CloseHandle(serial.hCom);
    DeleteCriticalSection(&rxBuffer.lock);

    return 0;
}

// Write buffer with overlapped I/O, wait for completion and drain the driver's output queue.
// Returns 1 on success, 0 on failure.
int write_and_drain(HANDLE hCom, const char* buf, DWORD len, DWORD write_timeout_ms, DWORD drain_timeout_ms) {
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset event
    if (!ov.hEvent) return 0;

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(hCom, buf, len, &bytesWritten, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(ov.hEvent, write_timeout_ms);
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(hCom, &ov, &bytesWritten, FALSE)) {
                    CloseHandle(ov.hEvent);
                    return 0;
                }
            }
            else {
                // timeout or error: cancel this overlapped I/O
                // Prefer CancelIoEx if available
                CancelIoEx(hCom, &ov);
                CloseHandle(ov.hEvent);
                return 0;
            }
        }
        else {
            CloseHandle(ov.hEvent);
            return 0;
        }
    }

    if (bytesWritten != len) {
        // Partial write
        CloseHandle(ov.hEvent);
        return 0;
    }

    // Now wait for driver's output queue to drain
    DWORD start = GetTickCount();
    while (1) {
        COMSTAT comStat;
        DWORD errors = 0;
        if (!ClearCommError(hCom, &errors, &comStat)) {
            CloseHandle(ov.hEvent);
            return 0;
        }
        if (comStat.cbOutQue == 0) {
            CloseHandle(ov.hEvent);
            return 1; // success
        }
        if ((GetTickCount() - start) > drain_timeout_ms) {
            // timed out waiting for drain
            CloseHandle(ov.hEvent);
            return 0;
        }
        Sleep(10);
    }
}