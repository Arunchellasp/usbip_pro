# USB/IP Setup Guide (Linux Server ↔ Windows 11 Client)

This guide explains how to share a USB device from a Linux machine (e.g., Raspberry Pi) and access it from a Windows 11 system using USB/IP.

---

# Linux Side (USB/IP Server)

## 1. Install USB/IP

```bash
sudo apt update
sudo apt install usbip
```

## 2. Load Required Kernel Modules

```bash
sudo modprobe usbip_core
sudo modprobe usbip_host
```

## 3. Start the USB/IP Daemon

```bash
sudo usbipd -D
```

## 4. List Available USB Devices

```bash
usbip list -l
```

Example output:

```text
Local USB devices
=================

 - busid 1-1 (046d:0825)
   Logitech USB Camera
```

## 5. Export a USB Device

Bind the device so it can be shared over the network:

```bash
sudo usbip bind -b 1-1
```

Verify:

```bash
usbip list -l
```

---

# Windows 11 Side (USB/IP Client)

## 1. Install usbip-win

Download and install **usbip-win** from:

https://github.com/vadimgrn/usbip-win2

After installation, add the installation directory to the Windows **PATH** environment variable.

---

## 2. Verify VHCI Driver

Open Command Prompt and run:

```cmd
usbip port
```

If you receive:

```text
failed to open vhci driver
```

then the VHCI device has not been installed correctly.

Try:

```cmd
usbip install
```

or

```cmd
usbip.exe install
```

If this does not work, follow the manual installation procedure below.

---

## 3. Manually Install the VHCI Driver

### Open the Hardware Installation Wizard

Press:

```text
Win + R
```

Run:

```text
hdwwiz
```

### Install the Driver

1. Click **Next**
2. Select **Install the hardware that I manually select from a list**
3. Click **Next**
4. Select **Have Disk**
5. Browse to:

```text
C:\Users\arunp\OneDrive\Desktop\usbip-windows-client\resources\usbip
```

6. Select:

```text
usbip_vhci.inf
```

7. Install:

```text
USB/IP VHCI Root
```

After installation, verify:

```cmd
usbip port
```

The command should now work successfully.

---

# 4. Check Exported Devices from Windows

Run:

```cmd
usbip list -r <RaspberryPi_IP>
```

Example:

```cmd
usbip list -r 192.168.1.50
```

Example output:

```text
Exportable USB devices
======================

 - 192.168.1.50
      1-1: Logitech USB Camera
```

---

# 5. Attach a Remote USB Device

Attach the exported USB device:

```cmd
usbip attach -r 192.168.1.50 -b 1-1
```

Windows will now detect the device as if it were locally connected.

Examples:

- USB Camera → Appears in Device Manager
- USB-to-UART Adapter → Appears as a COM Port

---

# 6. Verify the Device in Windows

Open **Device Manager**.

You should see the attached device listed, for example:

```text
USB Device
```

or

```text
USB Serial Device (COM5)
```

---

# 7. Detach a USB Device

List attached USB/IP devices:

```cmd
usbip port
```

Example output:

```text
Imported USB devices
====================

Port 01: device in use at Full Speed (12Mbps)
         Future Technology Devices International, Ltd : FT232 Serial (UART) IC (0403:6001)
           -> usbip://192.168.0.15:3240/1-1.3
           -> remote bus/dev 001/004
```

Detach the device using the port number:

```cmd
usbip detach -p <port_number>
```

Example:

```cmd
usbip detach -p 1
```

---

# GUI Option

The usbip-win installation also includes a GUI application.

You can use this GUI to:

- Discover remote USB devices
- Attach devices
- Detach devices
- Monitor USB/IP connections

This provides an easier alternative to using command-line commands.

---

# Example Workflow

### Linux (Server)

```bash
sudo usbipd -D
usbip list -l
sudo usbip bind -b 1-1
```

### Windows (Client)

```cmd
usbip list -r 192.168.1.50
usbip attach -r 192.168.1.50 -b 1-1
```

### Later, Detach

```cmd
usbip port
usbip detach -p 1
```

---

# Notes

- Ensure TCP port **3240** is allowed through firewalls.
- The Linux server and Windows client must be on the same network or have network connectivity between them.
- Run commands with Administrator privileges on Windows when required.
- Rebinding may be required after reconnecting the USB device or rebooting the Linux server.