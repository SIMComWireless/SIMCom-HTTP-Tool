
- [English](README.md)

# SIMCom HTTP Tool

一个小型的命令行 C 程序，通过串口（COM）与 SIMCom 蜂窝模块通信，并通过 AT 命令从 HTTP 服务器下载文件。

本项目实现了重叠（异步）串口 I/O、用于接收字节的线程安全环形缓冲区、用于启动模块 HTTP 客户端的 AT 命令序列，以及按块下载并保存文件的逻辑。

## 目录

- [Overview](#overview)
- [Key components](#key-components)
- [Program flow (main)](#program-flow-main)
- [Inputs and outputs](#inputs-and-outputs)
- [Usage examples](#usage-examples)
- [Error handling and edge cases](#error-handling-and-edge-cases)

## 概要

该工具打开与 SIMCom 模块的串口，执行一系列 AT 命令以初始化模块的 HTTP 客户端，触发 HTTP GET 请求，并按块读取返回数据，保存到本地文件。

## 主要组成

- ### RingBuffer（线程安全）
  - 固定大小的环形缓冲区：`RING_BUFFER_SIZE = 8192`。
  - 通过 `CRITICAL_SECTION` 保护。
  - 函数：`ring_buffer_init`、`ring_buffer_put`、`ring_buffer_put_bulk`、`ring_buffer_get`、`ring_buffer_read_bulk`、`ring_buffer_peek`、`ring_buffer_find_char`、`ring_buffer_available`。

- ### SerialPort（结构体）
  - 包含 `HANDLE hCom`、指向接收 `RingBuffer` 的指针以及用于接收线程的 `running` 标志。

- ### serial_receive_thread
  - 后台线程，在 COM 口上执行重叠（overlapped）`ReadFile` 调用。
  - 将接收到的字节推入 `RingBuffer`。

- ### open_serial_port(const char* portName, int baudRate)
  - 使用 `CreateFileA(..., FILE_FLAG_OVERLAPPED)` 打开 COM 口。
  - 配置 `DCB`（波特率、8-N-1、DTR/RTS）和超时参数。
  - 出错时返回 `INVALID_HANDLE_VALUE`，成功返回 `HANDLE`。

- ### send_at_command(HANDLE hCom, const char* command)
  - 发送 AT 命令（自动追加 `\r\n`），使用重叠的 `WriteFile` 并等待完成。

- ### 辅助函数
  - `read_line_from_buffer`、`wait_for_response`、`parse_number_response` — 从环形缓冲区读取并解析以换行结束的响应。
  - `enumerate_serial_ports` — 快速探测 `COM1..COM20` 并列出可用端口。

- ### download_file_data(HANDLE hCom, RingBuffer* rb, const char* filename, int total_size)
  - 使用基于 AT 的下载协议（`AT+CFTPSGET="<file>",<offset>,<len>`）获取文件块。
  - 读取 `+CFTPSGET: DATA,<len>` 控制行，然后从环形缓冲区读取指定数量的二进制字节。
  - 将数据块写入本地文件，打印十六进制预览和进度，并在特定服务器返回码下重试。

## 程序流程（main）

1. 解析命令行参数：
   - 位置参数：`<COM> <HTTP_URL> <LOCAL_FILENAME> [BAUD]`。
   - 缺失的值会以交互方式请求；默认波特率为 `115200`。
2. 初始化 `RingBuffer`。
3. 使用 `open_serial_port(portName, baudRate)` 打开串口。
4. 启动 `serial_receive_thread` 收集传入数据。
5. 执行 AT 命令序列（每个发送使用 `send_at_command` 并等待响应）：
   - `AT`（检测模块是否存活）
   - `AT+HTTPINIT`
   - `AT+HTTPPARA="SSLCFG,1"`（示例 SSL 配置）
   - `AT+HTTPPARA="URL","<http_url>"`
   - `AT+HTTPACTION=0`（触发 HTTP GET）
6. （可选/已注释）使用 `AT+CFTPSSIZE` 查询文件大小。
7. 调用 `download_file_data` 获取并保存文件。
8. 清理：停止线程、关闭句柄、删除临界区。

## 输入与输出

- 输入：
  - 命令行或交互输入：COM 端口（例如 `COM3`）、HTTP URL、本地保存文件名、可选波特率。
  - 来自 SIMCom 模块的串口数据（AT 响应与二进制负载）。

- 输出：
  - 控制台日志：命令、响应、十六进制预览、进度和错误信息。
  - 本地保存的二进制文件（下载内容）。

## 使用示例

- 非交互（全部参数）：

```powershell
SIMCom_HTTP_Tool.exe COM3 http://example.com/file.bin file.bin 115200
```

- 交互模式（不传参并按提示输入）：

```powershell
SIMCom_HTTP_Tool.exe
# 然后按提示输入 COM、URL、文件名和波特率
```

## 错误处理与边界情况

- 打开串口失败会返回 `INVALID_HANDLE_VALUE` —— 程序会报告并退出。
- 重叠 I/O 使用事件与超时；写入在超时情况下会被取消。
- 环形缓冲区溢出会导致接收线程等待（短暂 sleep）以腾出空间。
- `download_file_data` 在特定服务器返回码下对偏移进行重试（受 `MAX_OFFSET_RETRIES` 限制）。
- 代码依赖固定超时和轮询（`Sleep(1)`），对于较慢或超大传输可能出现时序相关的问题。

---