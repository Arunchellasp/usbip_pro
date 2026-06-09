# Auto-Run Server on Raspberry Pi

This guide explains how to automatically run the USB/IP server on Raspberry Pi at startup or in the background.

---

## Table of Contents

1. [Build the Server](#build-the-server)
2. [Method 1: Systemd Service (Recommended)](#method-1-systemd-service-recommended)
3. [Method 2: Cron Job](#method-2-cron-job)
4. [Method 3: rc.local](#method-3-rclocal)
5. [Method 4: Init.d Script](#method-4-initd-script)
6. [Verification & Troubleshooting](#verification--troubleshooting)

---

## Build the Server

First, compile the server executable on your Raspberry Pi:

```bash
cd /home/pi/usbip_pro
bash build.sh
```

Or manually compile:

```bash
gcc -Wall -Wextra -O2 -o build/main code/main.c
```

Verify the build was successful:

```bash
ls -la build/main
./build/main --help
```

---

## Method 1: Systemd Service (Recommended)

Systemd is the standard init system on modern Raspberry Pi OS. This is the most reliable and manageable method.

### Step 1: Create a systemd service file

Create a new service file:

```bash
sudo nano /etc/systemd/system/usbip_server.service
```

Add the following content:

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

**Important:** Replace `/home/pi/usbip_pro` with your actual project path.

### Step 2: Reload systemd and enable the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable usbip_server.service
sudo systemctl start usbip_server.service
```

### Step 3: Verify the service is running

```bash
sudo systemctl status usbip_server.service
```

You should see:

```
● usbip_server.service - USB/IP Server
     Loaded: loaded (/etc/systemd/system/usbip_server.service; enabled; vendor preset: enabled)
     Active: active (running) since ...
```

### View logs

```bash
sudo journalctl -u usbip_server.service -f
```

### Stop/Restart the service

```bash
sudo systemctl stop usbip_server.service
sudo systemctl restart usbip_server.service
```

### Disable auto-start

```bash
sudo systemctl disable usbip_server.service
```

---

## Method 2: Cron Job

Use cron to run the server at startup.

### Step 1: Open crontab editor

```bash
sudo crontab -e
```

### Step 2: Add reboot command

Add this line at the end of the file:

```
@reboot /home/pi/usbip_pro/build/main > /var/log/usbip_server.log 2>&1 &
```

**Notes:**
- `@reboot` runs the command at startup
- `> /var/log/usbip_server.log 2>&1` redirects output to a log file
- `&` runs in the background

### Step 3: Save and verify

Press `Ctrl+X`, then `Y` to save.

### View logs

```bash
sudo tail -f /var/log/usbip_server.log
```

---

## Method 3: rc.local

The `rc.local` script runs at the end of the boot process.

### Step 1: Create or edit rc.local

```bash
sudo nano /etc/rc.local
```

### Step 2: Add the server command

Insert this line before the final `exit 0`:

```bash
/home/pi/usbip_pro/build/main > /var/log/usbip_server.log 2>&1 &
```

Example file structure:

```bash
#!/bin/sh -e
#
# rc.local
#

# Run USB/IP server
/home/pi/usbip_pro/build/main > /var/log/usbip_server.log 2>&1 &

exit 0
```

### Step 3: Make it executable

```bash
sudo chmod +x /etc/rc.local
```

### Step 4: Enable and start the service

```bash
sudo systemctl enable rc-local.service
sudo systemctl start rc-local.service
```

---

## Method 4: Init.d Script

For older Raspberry Pi OS versions without systemd.

### Step 1: Create an init script

```bash
sudo nano /etc/init.d/usbip_server
```

Add this content:

```bash
#!/bin/bash
### BEGIN INIT INFO
# Provides:          usbip_server
# Required-Start:    $network
# Required-Stop:     $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: USB/IP Server
# Description:       USB/IP TCP server for remote control
### END INIT INFO

DAEMON="/home/pi/usbip_pro/build/main"
PIDFILE="/var/run/usbip_server.pid"

case "$1" in
    start)
        echo "Starting USB/IP server..."
        start-stop-daemon --start --background --pidfile $PIDFILE --make-pidfile --exec $DAEMON
        echo "USB/IP server started."
        ;;
    stop)
        echo "Stopping USB/IP server..."
        start-stop-daemon --stop --pidfile $PIDFILE
        rm -f $PIDFILE
        echo "USB/IP server stopped."
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start
        ;;
    *)
        echo "Usage: /etc/init.d/usbip_server {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
```

### Step 2: Make it executable and register

```bash
sudo chmod +x /etc/init.d/usbip_server
sudo update-rc.d usbip_server defaults
```

### Step 3: Start the service

```bash
sudo service usbip_server start
```

### Check status

```bash
sudo service usbip_server status
```

---

## Verification & Troubleshooting

### Check if the server is running

```bash
ps aux | grep main
```

You should see the process running.

### Test the connection from another machine

From a Windows PC or another Linux machine:

```bash
telnet <raspberry_pi_ip> 5000
```

If connected, you can send commands:

```
list_usb
```

Press Ctrl+] and then type `quit` to exit telnet.

### View system logs

```bash
# For systemd method
sudo journalctl -u usbip_server.service -n 50

# For rc.local or cron methods
sudo tail -f /var/log/usbip_server.log
```

### Server not starting?

1. **Verify the executable exists:**
   ```bash
   ls -la /home/pi/usbip_pro/build/main
   ```

2. **Test manual execution:**
   ```bash
   sudo /home/pi/usbip_pro/build/main
   ```

3. **Check for permission issues:**
   ```bash
   sudo ls -la /etc/systemd/system/usbip_server.service
   ```

4. **Check for port conflicts:**
   ```bash
   sudo netstat -tuln | grep 5000
   ```

5. **View detailed error logs:**
   ```bash
   sudo systemctl status usbip_server.service -l
   sudo journalctl -u usbip_server.service -e
   ```

### Disable auto-start

**Systemd:**
```bash
sudo systemctl disable usbip_server.service
```

**Cron:**
```bash
sudo crontab -e
# Delete the @reboot line
```

**rc.local:**
```bash
sudo nano /etc/rc.local
# Remove or comment out the server line
```

---

## Quick Reference

| Method | Pros | Cons | Best For |
|--------|------|------|----------|
| **Systemd** | Modern, powerful, easy logging, standard | Requires modern OS | Most users |
| **Cron** | Simple, universal | Less control, timing | Quick setup |
| **rc.local** | Simple, direct | Less control | Older systems |
| **Init.d** | Works on older systems | Deprecated, verbose | Legacy systems |

---

## Summary

For **most Raspberry Pi users (Raspberry Pi OS)**, use **Method 1: Systemd Service**. It's the recommended approach for modern systems.

If you encounter issues, check the troubleshooting section above and ensure:
- ✅ The server executable was built successfully
- ✅ File paths are correct for your setup
- ✅ The Raspberry Pi user has permissions to run USB/IP commands (usually requires `sudo`)
- ✅ Port 5000 is available and not blocked by the firewall
