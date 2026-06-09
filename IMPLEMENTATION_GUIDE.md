# USB/IP Remote Control - Complete Implementation Guide

A comprehensive step-by-step guide to implementing, building, deploying, and auto-running the USB/IP TCP server on Raspberry Pi with a Windows client.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Phase 1: Prerequisites & Environment Setup](#phase-1-prerequisites--environment-setup)
3. [Phase 2: Server-Side Implementation (Raspberry Pi)](#phase-2-server-side-implementation-raspberry-pi)
4. [Phase 3: Client-Side Implementation (Windows)](#phase-3-client-side-implementation-windows)
5. [Phase 4: Building Both Components](#phase-4-building-both-components)
6. [Phase 5: Deployment & Configuration](#phase-5-deployment--configuration)
7. [Phase 6: Auto-Run Setup](#phase-6-auto-run-setup)
8. [Phase 7: Testing & Verification](#phase-7-testing--verification)
9. [Complete Workflow Summary](#complete-workflow-summary)

---

## 1. Architecture Overview

### System Communication Flow

```
┌─────────────────┐                      ┌──────────────────────┐
│   Windows PC    │                      │  Raspberry Pi (Linux)│
│                 │                      │                      │
│  usbip_client   │  TCP Port 5000       │   usbip_server      │
│                 │◄────────────────────►│                      │
│                 │                      │  (runs as root)      │
│  - Connect      │  Plain Text Commands │                      │
│  - Send Command │  ("list_usb\n")      │  - Listen Socket     │
│  - Read Output  │                      │  - popen() execution │
│                 │  Responses           │  - Capture stdout    │
│                 │  + "END_OF_RESPONSE" │  - Send results      │
└─────────────────┘                      └──────────────────────┘
```

### Key Components

| Component | File | Platform | Purpose |
|-----------|------|----------|---------|
| **Server** | `code/main.c` | Raspberry Pi (Linux) | TCP listener, command executor |
| **Client** | `clinent_side_code/client.c` | Windows PC | TCP connector, UI interface |
| **Build Script** | `build.sh` | Linux/Bash | Server compilation automation |
| **Run Script** | `run.sh` | Linux/Bash | Server execution wrapper |

---

## Phase 1: Prerequisites & Environment Setup

### 1.1 Raspberry Pi Server Setup

#### Step 1: Install USB/IP Tools

```bash
# SSH into Raspberry Pi
ssh pi@<rpi_ip>

# Update package lists
sudo apt update
sudo apt upgrade -y

# Install USB/IP tools and dependencies
sudo apt install -y usbip linux-tools-generic build-essential
sudo apt install -y gcc git

# Verify installation
usbip --version
gcc --version
```

#### Step 2: Verify USB/IP Daemon

```bash
# Check if usbip modules are available
lsmod | grep usbip

# If not loaded, you can load them manually:
sudo modprobe usbip_core
sudo modprobe usbip_host

# Verify daemon can run
sudo usbipd -v
```

#### Step 3: Clone/Copy Project

```bash
# Option A: Clone from git
cd ~
git clone <your_repo_url> usbip_pro
cd usbip_pro

# Option B: Manual copy
# Transfer files via SCP or USB to /home/pi/usbip_pro
scp -r /local/path/usbip_pro pi@<rpi_ip>:/home/pi/
```

### 1.2 Windows Client Setup

#### Step 1: Install Visual Studio Build Tools

- Download from: https://visualstudio.microsoft.com/downloads/
- Install "Desktop Development with C++" workload
- Includes MSVC compiler and build tools

#### Step 2: Install USB/IP for Windows

- Download: https://github.com/dorssel/usbipd-win/releases
- Run installer (Administrator required)
- Add to PATH if not automatic:
  ```cmd
  setx PATH "%PATH%;C:\Program Files\usbipd-win"
  ```

#### Step 3: Verify Installation

```cmd
# Open Command Prompt as Administrator
cl.exe /?          # Check MSVC availability
usbip.exe --version    # Check USB/IP tools
```

---

## Phase 2: Server-Side Implementation (Raspberry Pi)

### 2.1 Server Code Architecture (main.c)

#### Core Components:

```
main.c (Raspberry Pi TCP Server)
├── Configuration
│   ├── SERVER_PORT: 5000
│   ├── BACKLOG: 5 (max connections)
│   ├── CMD_BUF_SIZE: 256
│   └── OUT_BUF_SIZE: 65536 (64 KiB output buffer)
│
├── Utility Functions
│   ├── run_cmd() - Execute shell command, capture output
│   ├── send_all() - Reliable socket send (handles partial writes)
│   ├── send_response() - Send data + END_OF_RESPONSE sentinel
│   ├── rtrim() - Remove trailing whitespace
│   └── buf_append() - Safe string concatenation
│
├── Command Handlers
│   ├── cmd_usbserver_init() - Load modules, start daemon
│   ├── cmd_list_usb() - List USB devices
│   ├── cmd_bind_all() - Auto-bind all USB devices
│   ├── cmd_bind_busid() - Bind specific device by busid
│   ├── cmd_unbind_busid() - Unbind specific device
│   ├── cmd_list_exportable() - List devices ready to export
│   ├── cmd_export_*() - Export device to remote host
│   └── cmd_help() - Display command list
│
├── Main Event Loop
│   ├── socket() - Create TCP listener
│   ├── bind() - Bind to port 5000
│   ├── listen() - Accept connections
│   ├── accept() - Wait for client
│   ├── recv() - Read command from client
│   └── dispatch - Route to appropriate handler
│
└── Cleanup
    ├── close() - Close connections
    └── exit handlers - Signal handling
```

### 2.2 Command Execution Model

#### How Commands Are Processed:

```
Client: "list_usb\n"
         ↓
recv() on server socket
         ↓
Parse command: "list_usb"
         ↓
Find handler: cmd_list_usb()
         ↓
Handler runs: run_cmd("usbip list -l", buffer, 65536)
         ↓
popen() → execute shell command
         ↓
Capture ALL output (stdout + stderr)
         ↓
send_response(sock, buffer)
         ├─ send_all(sock, buffer, strlen)
         └─ send_all(sock, "END_OF_RESPONSE\n", 16)
         ↓
Client detects sentinel → Display output
```

### 2.3 Root Privilege Requirements

All USB/IP operations require **root** access:

```bash
# Commands that need sudo:
sudo modprobe usbip_core      # Load kernel modules
sudo modprobe usbip_host      # Load host driver
sudo usbipd -D                # Start USB/IP daemon
sudo usbip bind -b <busid>   # Bind USB device
sudo usbip unbind -b <busid> # Unbind USB device
sudo usbip list -r            # List remote/exported devices
```

**Server must run as root:**
```bash
sudo ./build/main
```

Or configure sudoers for passwordless commands (advanced):
```bash
sudo visudo
# Add: pi ALL=(ALL) NOPASSWD: /usr/bin/modprobe, /usr/bin/usbipd, /usr/bin/usbip
```

### 2.4 Key Implementation Details

#### Output Buffering Strategy

```c
#define OUT_BUF_SIZE 65536  /* 64 KiB max command output */

/* Why large buffer?
   - Some USB listings can be verbose
   - Multiple device statuses = large output
   - Ensures complete response is sent
   - Silently truncates if exceeded
*/
```

#### End-of-Response Sentinel

```c
#define SENTINEL "END_OF_RESPONSE\n"

/* Why use sentinel?
   1. Network packets may arrive fragmented
   2. Client doesn't know response size in advance
   3. Sentinel clearly marks response boundary
   4. Works regardless of output content
   5. Prevents client from hanging indefinitely
*/
```

#### Error Handling

```c
/* Server gracefully handles:
   - popen() failures → returns -1, error message
   - Partial socket sends → retry until all bytes sent
   - Client disconnection → cleanup and wait for next
   - Invalid commands → "unknown command" response
   - Buffer overflow → silently truncate output
   - Root permission issues → error message to client
*/
```

---

## Phase 3: Client-Side Implementation (Windows)

### 3.1 Client Code Architecture (client.c)

#### Core Components:

```
client.c (Windows TCP Client)
├── Configuration
│   ├── DEFAULT_PORT: 5000
│   ├── CMD_BUF_SIZE: 256
│   ├── RECV_BUF_SIZE: 4096 (read chunk size)
│   ├── OUT_BUF_SIZE: 65536
│   └── SENTINEL: "END_OF_RESPONSE\n"
│
├── Utility Functions
│   ├── rtrim() - Remove whitespace from input
│   ├── send_all() - Reliable socket send
│   ├── recv_response() - Receive until sentinel
│   ├── connect_to_server() - TCP connection
│   └── parse_busid_list() - Parse USB device list
│
├── Command Handlers
│   ├── cmd_usbserver_init() - Remote initialization
│   ├── cmd_list_usb() - Remote list USB
│   ├── cmd_bind_all() - Remote bind all
│   ├── cmd_bind_busid() - Remote bind specific
│   ├── cmd_unbind_busid() - Remote unbind
│   ├── cmd_local_list() - Local USB listing
│   ├── cmd_local_attach() - Local USB attachment
│   └── cmd_local_detach() - Local USB detachment
│
├── Main Interactive Loop
│   ├── Connect to server
│   ├── Display menu
│   ├── Read user input
│   ├── Send command to server
│   ├── Display server response
│   └── Loop until exit
│
└── Cleanup
    ├── closesocket() - Close TCP connection
    ├── WSACleanup() - Windows socket cleanup
    └── Exit gracefully
```

### 3.2 Sentinel Detection Algorithm

#### Challenge: Sentinel May Straddle Packets

```
Scenario 1: Sentinel received completely in one recv()
  recv() returns: "output...\nEND_OF_RESPONSE\n"
  ✓ Easy to detect

Scenario 2: Sentinel split across two recv() calls
  recv() #1 returns: "output...\nEND_OF_RES"
  recv() #2 returns: "PONSE\n"
  ✗ Would miss sentinel without special handling!
```

#### Solution: Sliding Window with Tail Buffer

```c
char tail[SENTINEL_LEN + 1];  /* Keep last 15 bytes */
int tail_len = 0;             /* How many bytes in tail */

while (1) {
    recv(sock, buf, RECV_BUF_SIZE, 0)
    
    /* Create search window: old tail + new data */
    window = tail + buf
    
    /* Search for sentinel in window */
    if (strstr(window, "END_OF_RESPONSE\n")) {
        /* Found! Print up to sentinel, then exit loop */
        break;
    }
    
    /* Save last 15 bytes of window into tail for next iteration */
    memcpy(tail, window + (window_size - 15), 15)
    tail_len = 15
}
```

### 3.3 Windows Socket API Details

```c
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

/* Initialization */
WSADATA wsaData;
WSAStartup(MAKEWORD(2, 2), &wsaData);

/* Create socket */
SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

/* Connect */
struct sockaddr_in serv_addr;
serv_addr.sin_family = AF_INET;
serv_addr.sin_port = htons(port);
inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);
connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

/* Send/Receive */
send(sock, cmd, strlen(cmd), 0);
recv(sock, buffer, size, 0);

/* Cleanup */
closesocket(sock);
WSACleanup();
```

---

## Phase 4: Building Both Components

### 4.1 Build the Server (Raspberry Pi)

#### Automated Build (Recommended)

```bash
cd /home/pi/usbip_pro

# Make build script executable
chmod +x build.sh

# Run build
./build.sh

# Output:
# Compiling C code with GCC...
# ✓ Compilation successful!
# Executable created: build/main
```

#### Manual Build

```bash
# Single command
gcc -Wall -Wextra -O2 -o build/main code/main.c

# Flags explained:
# -Wall -Wextra    = Enable all warnings
# -O2              = Optimization level 2
# -o build/main    = Output to build/main
```

#### Verify Build

```bash
# Check executable exists
ls -la build/main

# Should output something like:
# -rwxr-xr-x 1 pi pi 45680 Jun 9 10:23 build/main

# Check file type
file build/main
# Should output: ELF 32-bit LSB executable, ARM, version 1 (SYSV), ...
```

### 4.2 Build the Client (Windows)

#### Using MSVC (Visual Studio Command Prompt)

```cmd
REM Open "Developer Command Prompt for Visual Studio"

REM Navigate to project
cd C:\path\to\usbip_pro\clinent_side_code

REM Compile
cl client.c /W4 /O2 /Fe:usbip_client.exe Ws2_32.lib

REM Flags explained:
REM /W4              = Warnings level 4 (most strict)
REM /O2              = Optimization level 2
REM /Fe:usbip_client.exe = Output filename
REM Ws2_32.lib       = Windows socket library

REM Verify build
dir usbip_client.exe
```

#### Verify Build

```cmd
# Check executable exists
dir usbip_client.exe

# Test basic execution (no args = show usage)
usbip_client.exe
```

### 4.3 Build Verification Checklist

```
☐ Server (Linux):
  ☐ /home/pi/usbip_pro/build/main exists
  ☐ File size > 30KB (not stripped)
  ☐ Executable bit set (chmod +x)
  ☐ ELF format verified (file command)

☐ Client (Windows):
  ☐ C:\path\to\usbip_client.exe exists
  ☐ File size > 20KB
  ☐ No build warnings/errors
  ☐ Can run without DLL errors
```

---

## Phase 5: Deployment & Configuration

### 5.1 Server Deployment to Raspberry Pi

#### Step 1: Prepare Server Directory

```bash
# On Raspberry Pi
mkdir -p /home/pi/usbip_pro/build
cd /home/pi/usbip_pro

# Verify project structure
tree -L 2
# Should show:
# ├── build/
# │   └── main          ← Executable
# ├── code/
# │   └── main.c
# ├── build.sh
# └── README.md
```

#### Step 2: Test Manual Execution

```bash
# Run server as root (required for USB/IP)
sudo /home/pi/usbip_pro/build/main

# Expected output (server waiting for connections):
# Server listening on port 5000...
# Waiting for connections...

# Leave running, test from Windows (next section)
# Press Ctrl+C to stop
```

#### Step 3: Verify Port Binding

```bash
# In another terminal on Raspberry Pi
netstat -tuln | grep 5000
# Should show: tcp  0  0 0.0.0.0:5000  0.0.0.0:*  LISTEN

# Or using ss command:
ss -tuln | grep 5000
```

### 5.2 Client Deployment to Windows

#### Step 1: Copy Executable

```cmd
REM Create folder for client
mkdir C:\USB-IP\
copy usbip_client.exe C:\USB-IP\

REM Verify
dir C:\USB-IP\usbip_client.exe
```

#### Step 2: Get Raspberry Pi IP Address

```bash
# On Raspberry Pi, find IP address
hostname -I
# Example output: 192.168.1.100

# Or from Windows (ping Raspberry Pi hostname):
ping raspberrypi.local
```

#### Step 3: Test Client Connection

```cmd
REM From Windows Command Prompt (NOT necessarily admin)
C:\USB-IP\usbip_client.exe 192.168.1.100

REM Expected output:
REM Connected to server 192.168.1.100:5000
REM
REM USB/IP Remote Control Menu:
REM   1. init_server      - Initialize USB/IP server
REM   2. list_usb         - List USB devices
REM   3. bind_all         - Bind all USB devices
REM   4. bind_<busid>     - Bind specific device
REM   ... (more commands)
REM
REM Enter command:
```

### 5.3 Network Configuration

#### Firewall Rules (Raspberry Pi)

```bash
# Check if firewall is enabled
sudo ufw status
# If enabled:

# Allow port 5000 from specific Windows IP:
sudo ufw allow from 192.168.1.50 to any port 5000

# Or allow from entire network:
sudo ufw allow 5000/tcp

# Verify rule added:
sudo ufw status numbered
```

#### Network Testing

```bash
# From Windows, test connection
ping 192.168.1.100           # Verify reachability
telnet 192.168.1.100 5000    # Verify port 5000 open

# Expected: Connection established (or times out if port closed)

# From Raspberry Pi
netstat -tuln | grep 5000    # Verify listening
sudo netstat -tulnp | grep main  # Show process holding port
```

---

## Phase 6: Auto-Run Setup

See [auto_run.md](auto_run.md) for complete auto-start instructions.

### 6.1 Quick Setup: Systemd Service

#### Step 1: Create Service File

```bash
sudo nano /etc/systemd/system/usbip_server.service
```

Add this content:

```ini
[Unit]
Description=USB/IP Server
After=network.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/pi/usbip_pro
ExecStart=/home/pi/usbip_pro/build/main
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

#### Step 2: Enable and Start

```bash
# Reload systemd
sudo systemctl daemon-reload

# Enable auto-start on boot
sudo systemctl enable usbip_server.service

# Start service now
sudo systemctl start usbip_server.service

# Verify running
sudo systemctl status usbip_server.service
# Should show: Active: active (running)
```

#### Step 3: Check Logs

```bash
# View recent logs
sudo journalctl -u usbip_server.service -n 20

# Follow logs in real-time
sudo journalctl -u usbip_server.service -f
```

#### Step 4: Test After Boot

```bash
# Reboot Raspberry Pi
sudo reboot

# After reboot, connect from Windows client
usbip_client.exe 192.168.1.100

# If connected successfully, auto-start is working!
```

---

## Phase 7: Testing & Verification

### 7.1 Pre-Connection Verification

#### Raspberry Pi Checklist

```bash
# Verify build
ls -la /home/pi/usbip_pro/build/main
chmod +x /home/pi/usbip_pro/build/main

# Verify USB/IP tools installed
which usbip
which usbipd
usbip --version

# Verify kernel modules available
modinfo usbip_core
modinfo usbip_host

# Verify permissions
groups pi
# Should include sudo group, or add: sudo usermod -aG sudo pi
```

#### Windows Checklist

```cmd
# Verify client built
dir usbip_client.exe

# Verify USB/IP for Windows installed
usbip.exe --version

# Verify network connectivity
ping 192.168.1.100
ipconfig          (find your IP)

# Verify ports available
netstat -an | findstr "5000"   (should be empty if port not in use)
```

### 7.2 Step-by-Step Connection Test

#### Step 1: Start Server

```bash
# On Raspberry Pi (SSH terminal)
sudo /home/pi/usbip_pro/build/main

# Expected output:
# Server listening on port 5000...
# Waiting for connections...
```

#### Step 2: Connect Client

```cmd
# On Windows (separate terminal)
usbip_client.exe 192.168.1.100

# Expected: Interactive menu appears
```

#### Step 3: Send Commands

```
Connected to server 192.168.1.100:5000

Enter command: init_server

(output shows module loading, daemon startup)

Enter command: list_usb

(output shows connected USB devices)

Enter command: help

(output shows available commands)
```

### 7.3 Full Workflow Test

```
1. Initialize Server:
   Command: init_server
   Expected: Modules loaded, usbipd running
   
2. List USB Devices:
   Command: list_usb
   Expected: Shows connected USB devices with busids
   
3. Bind All Devices:
   Command: bind_all
   Expected: All devices bound successfully
   
4. List Exportable:
   Command: list_exportable
   Expected: Shows devices available for export
   
5. Disconnect and Reconnect:
   Type: exit
   Command: usbip_client.exe 192.168.1.100
   Expected: Connection successful
```

### 7.4 Troubleshooting Test Cases

#### Test: Connection Refused

```
Error: Connection refused / Cannot connect

Diagnosis:
☐ Server running?
  ssh pi@192.168.1.100
  ps aux | grep main
  
☐ Port listening?
  sudo netstat -tuln | grep 5000
  
☐ Firewall blocking?
  sudo ufw status
  sudo ufw allow 5000/tcp
  
☐ Network reachable?
  ping 192.168.1.100
  telnet 192.168.1.100 5000
```

#### Test: Sentinel Not Detected

```
Client hangs after sending command

Diagnosis:
☐ Server responds?
  Check server terminal for errors
  
☐ Response includes sentinel?
  Can inject test: echo "test\nEND_OF_RESPONSE\n"
  
☐ Socket still open?
  Check both client and server netstat
```

#### Test: Root Permission Denied

```
Error: "usbip: permission denied"

Solution:
☐ Run server as root:
  sudo ./build/main
  
☐ Or configure sudoers (risky):
  sudo visudo
  Add: pi ALL=(ALL) NOPASSWD: /usr/bin/usbip
```

---

## Complete Workflow Summary

### Quick Start: 5 Phases

```
PHASE 1: Build Server (Raspberry Pi)
├─ SSH into Pi: ssh pi@192.168.1.100
├─ Navigate: cd /home/pi/usbip_pro
├─ Build: bash build.sh
└─ Verify: ls -la build/main

PHASE 2: Build Client (Windows)
├─ Open Developer Command Prompt
├─ Navigate: cd C:\path\to\project\clinent_side_code
├─ Compile: cl client.c /W4 /O2 /Fe:usbip_client.exe Ws2_32.lib
└─ Verify: dir usbip_client.exe

PHASE 3: Deploy Server
├─ Test manual: sudo /home/pi/usbip_pro/build/main
├─ Verify port: sudo netstat -tuln | grep 5000
├─ Check firewall: sudo ufw allow 5000/tcp
└─ Leave running

PHASE 4: Test Client
├─ Run: usbip_client.exe 192.168.1.100
├─ Command: init_server
├─ Command: list_usb
└─ Command: exit

PHASE 5: Setup Auto-Run
├─ Create service: sudo nano /etc/systemd/system/usbip_server.service
├─ Enable: sudo systemctl enable usbip_server.service
├─ Start: sudo systemctl start usbip_server.service
└─ Verify: sudo systemctl status usbip_server.service

RESULT: Server auto-starts on Pi, Client connects on demand from Windows
```

### File Organization After Implementation

```
Raspberry Pi (/home/pi/usbip_pro/)
├── code/
│   └── main.c                         ← Server source
├── build/
│   └── main                           ← Server executable
├── build.sh                           ← Build automation
├── README.md                          ← Original docs
├── auto_run.md                        ← Auto-start guide
└── IMPLEMENTATION_GUIDE.md            ← This file

Windows (C:\USB-IP\)
├── usbip_client.exe                   ← Client executable
└── client.c                           ← Client source (optional)

Systemd Unit (/etc/systemd/system/)
└── usbip_server.service               ← Auto-start configuration
```

### Key Implementation Decisions

| Aspect | Decision | Reason |
|--------|----------|--------|
| **Protocol** | Plain text + sentinel | Simple, language-agnostic, reliable framing |
| **Port** | 5000 | Non-privileged, configurable, not reserved |
| **Buffer Size** | 65KB | Balances completeness with memory efficiency |
| **Execution Model** | popen() + capture | Simplest way to execute and get output |
| **Root Access** | Required | USB/IP needs kernel module access |
| **Error Handling** | Graceful degradation | Server never crashes, always responds |
| **Auto-start** | Systemd | Modern, reliable, standard on Raspberry Pi OS |

---

## Post-Implementation Checklist

```
✓ Server builds without errors
✓ Client builds without errors
✓ Server listens on port 5000
✓ Client connects successfully
✓ Commands execute remotely
✓ Output transmitted correctly
✓ Sentinel detection works
✓ Server survives disconnections
✓ Auto-start configured
✓ Auto-start tested after reboot
✓ Firewall rules allow traffic
✓ Logging works (journalctl or log files)
✓ Multiple connections handled correctly
✓ Root privileges configured appropriately
```

---

## References

- **Original README:** [README.md](README.md)
- **Auto-Run Setup:** [auto_run.md](auto_run.md)
- **USB/IP Documentation:** https://usbip.sourceforge.net/
- **USB/IP for Windows:** https://github.com/dorssel/usbipd-win
- **Raspberry Pi OS:** https://www.raspberrypi.org/

---

## Support & Troubleshooting

For complete troubleshooting information, see the "Verification & Troubleshooting" section of [auto_run.md](auto_run.md) and the "Troubleshooting Test Cases" in this guide.
