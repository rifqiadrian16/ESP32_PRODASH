import socket
import hashlib
import threading
import tkinter as tk
from tkinter import ttk, messagebox
import urllib.request
import ssl
import sys
import os

# ============================================================
# GANTI URL INI dengan URL GitHub Release kamu
# Format: https://github.com/<user>/<repo>/releases/latest/download/firmware.bin
# ============================================================

FIRMWARE_URL = "https://github.com/rifqiadrian16/livinaprodash-firmware/releases/latest/download/firmware.bin"

ESPOTA_UDP_PORT = 3232
OTA_COMMAND_FLASH = 0


def download_firmware(url, log_cb):
    log_cb("Mengunduh firmware terbaru dari server...")
    try:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        req = urllib.request.Request(url, headers={"User-Agent": "LivinaProOTA/1.0"})
        with urllib.request.urlopen(req, context=ctx, timeout=30) as r:
            data = r.read()
        log_cb(f"Firmware diunduh: {len(data):,} bytes")
        return data
    except urllib.error.HTTPError as e:
        raise Exception(f"Gagal unduh firmware (HTTP {e.code}). Cek koneksi internet.")
    except Exception as e:
        raise Exception(f"Gagal unduh firmware: {e}")


def upload_ota(ip, firmware_data, progress_cb, log_cb, done_cb):
    file_size = len(firmware_data)
    file_md5  = hashlib.md5(firmware_data).hexdigest()

    log_cb(f"Ukuran  : {file_size:,} bytes")
    log_cb(f"MD5     : {file_md5}")

    # -- Buka TCP server dulu (ESP32 yang akan konek ke kita) --
    try:
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("", 0))
        server.listen(1)
        server.settimeout(20)
        tcp_port = server.getsockname()[1]
    except Exception as e:
        done_cb(False, f"❌ Gagal buka port lokal: {e}\n   Coba matikan firewall sementara.")
        return

    # -- Kirim undangan via UDP ke ESP32 --
    invite = f"{OTA_COMMAND_FLASH} {tcp_port} {file_size} {file_md5}\n".encode()
    try:
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.sendto(invite, (ip, ESPOTA_UDP_PORT))
        udp.close()
        log_cb(f"Undangan OTA dikirim ke {ip}:{ESPOTA_UDP_PORT}")
        log_cb(f"Menunggu ESP32 menyambung ke port {tcp_port}...")
    except Exception as e:
        server.close()
        done_cb(False, f"❌ Gagal kirim UDP: {e}\n   Pastikan laptop & ESP32 di jaringan yang sama.")
        return

    # -- Tunggu ESP32 konek balik via TCP --
    try:
        conn, addr = server.accept()
        conn.settimeout(30)
        log_cb(f"ESP32 terhubung dari {addr[0]} ✓")
    except socket.timeout:
        server.close()
        done_cb(False,
                "❌ ESP32 tidak merespons (timeout 20 detik).\n\n"
                "Cek:\n"
                "  1. Modul sudah di Mode 0 (OTA Mode)?\n"
                "  2. IP address benar?\n"
                "  3. Laptop & ESP32 di hotspot yang SAMA?\n"
                "  4. Windows Firewall → izinkan app ini")
        return

    # -- Kirim firmware --
    try:
        sent = 0
        chunk_size = 1460
        log_cb("Mengirim firmware...")
        while sent < file_size:
            chunk = firmware_data[sent:sent + chunk_size]
            conn.sendall(chunk)
            sent += len(chunk)
            progress_cb(int(sent * 100 / file_size))

        log_cb("Firmware terkirim. Menunggu konfirmasi ESP32...")

        # Tunggu OK dari ESP32
        resp = b""
        try:
            resp = conn.recv(64)
        except Exception:
            pass  # Beberapa versi firmware langsung restart tanpa kirim OK

        conn.close()
        server.close()

        # ESP32 ArduinoOTA reply bisa "OK" atau langsung disconnect
        if b"OK" in resp or len(resp) == 0:
            done_cb(True, "✅ Update berhasil!\nESP32 sedang restart ke mode operasional.")
        elif b"ERROR" in resp:
            done_cb(False, f"❌ ESP32 menolak firmware: {resp.decode(errors='replace')}")
        else:
            done_cb(True, "✅ Update kemungkinan berhasil (ESP32 restart).")

    except Exception as e:
        done_cb(False, f"❌ Error saat kirim firmware: {e}")
    finally:
        try:
            conn.close()
        except Exception:
            pass
        try:
            server.close()
        except Exception:
            pass


def run_all(ip, log_cb, progress_cb, done_cb):
    try:
        fw = download_firmware(FIRMWARE_URL, log_cb)
        log_cb(f"\nMengirim ke ESP32 ({ip})...")
        upload_ota(ip, fw, progress_cb, log_cb, done_cb)
    except Exception as e:
        done_cb(False, str(e))


# ============================================================
# GUI
# ============================================================
class OTAApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("LivinaPro Firmware Updater")
        self.geometry("420x480")
        self.resizable(False, False)
        self.configure(bg="#f0f0f0", padx=24, pady=20)

        # Header
        tk.Label(self, text="LivinaPro Firmware Updater",
                 font=("Segoe UI", 15, "bold"), bg="#f0f0f0",
                 fg="#1a1a1a").pack(anchor="w", pady=(0, 4))
        tk.Label(self, text="Update firmware modul via WiFi — tidak perlu kabel",
                 font=("Segoe UI", 9), bg="#f0f0f0",
                 fg="#666").pack(anchor="w", pady=(0, 16))

        # IP input
        ip_frame = tk.Frame(self, bg="#f0f0f0")
        ip_frame.pack(fill="x", pady=(0, 4))
        tk.Label(ip_frame, text="IP Address ESP32:",
                 font=("Segoe UI", 10, "bold"), bg="#f0f0f0").pack(side="left")

        self.ip_var = tk.StringVar(value="192.168.43.")
        ip_entry = tk.Entry(ip_frame, textvariable=self.ip_var,
                            font=("Courier New", 12), width=16,
                            bd=1, relief="solid")
        ip_entry.pack(side="left", padx=(10, 0))

        # Hint
        hint = ("Cara cek IP:\n"
                "Android → Pengaturan → Hotspot & Tethering → Hotspot WiFi\n"
                "→ Perangkat Terhubung → lihat IP di samping nama \"Espressif\"")
        tk.Label(self, text=hint, font=("Segoe UI", 8), bg="#f0f0f0",
                 fg="#888", justify="left").pack(anchor="w", pady=(2, 14))

        # Progress bar
        tk.Label(self, text="Progress:", font=("Segoe UI", 9),
                 bg="#f0f0f0", fg="#555").pack(anchor="w")
        self.progress = ttk.Progressbar(self, length=370, mode="determinate")
        self.progress.pack(pady=(2, 12))

        # Log box
        tk.Label(self, text="Log:", font=("Segoe UI", 9),
                 bg="#f0f0f0", fg="#555").pack(anchor="w")
        log_frame = tk.Frame(self, bg="#f0f0f0")
        log_frame.pack(fill="x", pady=(2, 14))

        scrollbar = tk.Scrollbar(log_frame)
        scrollbar.pack(side="right", fill="y")

        self.log_box = tk.Text(log_frame, height=9,
                               font=("Courier New", 9),
                               state="disabled", bg="white",
                               fg="#222", relief="solid", bd=1,
                               yscrollcommand=scrollbar.set)
        self.log_box.pack(side="left", fill="x", expand=True)
        scrollbar.config(command=self.log_box.yview)

        # Button
        self.btn = tk.Button(self,
                             text="⚡  Update Firmware Sekarang",
                             command=self.start_update,
                             font=("Segoe UI", 11, "bold"),
                             bg="#1a6b3a", fg="white",
                             activebackground="#145530",
                             activeforeground="white",
                             relief="flat", pady=10,
                             cursor="hand2", bd=0)
        self.btn.pack(fill="x")

    # ----------------------------------------------------------
    def _log(self, msg):
        self.log_box.configure(state="normal")
        self.log_box.insert("end", msg + "\n")
        self.log_box.see("end")
        self.log_box.configure(state="disabled")

    def _set_progress(self, val):
        self.progress["value"] = val

    def start_update(self):
        ip = self.ip_var.get().strip()
        if not ip or ip.endswith("."):
            messagebox.showwarning("Perhatian",
                                   "Masukkan IP address ESP32 lengkap.\n"
                                   "Contoh: 192.168.43.105")
            return

        self.btn.configure(state="disabled", text="Sedang update, harap tunggu...")
        self.progress["value"] = 0
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")

        threading.Thread(
            target=run_all,
            args=(
                ip,
                lambda m: self.after(0, lambda msg=m: self._log(msg)),
                lambda v: self.after(0, lambda val=v: self._set_progress(val)),
                lambda ok, m: self.after(0, lambda o=ok, msg=m: self._finish(o, msg)),
            ),
            daemon=True
        ).start()

    def _finish(self, ok, msg):
        self._log(msg)
        self.btn.configure(state="normal", text="⚡  Update Firmware Sekarang")
        if ok:
            messagebox.showinfo("Berhasil!", msg)
        else:
            messagebox.showerror("Gagal", msg)


if __name__ == "__main__":
    app = OTAApp()
    app.mainloop()
