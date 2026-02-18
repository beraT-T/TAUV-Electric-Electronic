import socket
import tkinter as tk
from tkinter import font

# --- Yapılandırma ---
STM32_IP = "192.168.1.20"
STM32_PORT = 5000
MY_IP = "0.0.0.0" 
MY_PORT = 5000

class AUVControlApp:
    def __init__(self, root):
        self.root = root
        self.root.title("AUV Güç Kontrol") 
        self.root.geometry("350x500")
        self.root.configure(bg="#121212")

        # UDP Soket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((MY_IP, MY_PORT))
        self.sock.settimeout(0.8)

        # Cihaz Durumları
        self.trafo_on = False
        self.lazer_on = False
        self.led_on = False

        # Komut ID Takibi
        self.cmd_id = 0

        self.setup_ui()

    def setup_ui(self):
        # Başlık Fontu
        title_font = font.Font(family="Helvetica", size=18, weight="bold")
        btn_font = font.Font(family="Helvetica", size=14, weight="bold")

        tk.Label(self.root, text="AUV GÜÇ SİSTEMİ", font=title_font, fg="#00d1b2", bg="#121212").pack(pady=20)

        # --- TRAFO BÖLÜMÜ ---
        tk.Label(self.root, text="TRAFO KONTROL (PB9)", fg="white", bg="#121212").pack()
        self.btn_trafo = tk.Label(self.root, text="KAPALI", font=btn_font, bg="#c0392b", fg="white", 
                                  width=15, height=2, cursor="hand2")
        self.btn_trafo.pack(pady=10)
        self.btn_trafo.bind("<Button-1>", lambda e: self.toggle_trafo())

        # --- LAZER BÖLÜMÜ ---
        tk.Label(self.root, text="LAZER KONTROL (PB8)", fg="white", bg="#121212").pack()
        self.btn_lazer = tk.Label(self.root, text="KAPALI", font=btn_font, bg="#c0392b", fg="white", 
                                  width=15, height=2, cursor="hand2")
        self.btn_lazer.pack(pady=10)
        self.btn_lazer.bind("<Button-1>", lambda e: self.toggle_lazer())

        # --- LED BÖLÜMÜ ---
        tk.Label(self.root, text="LED PARLAKLIK (PB7)", fg="white", bg="#121212").pack(pady=(10,0))
        self.led_slider = tk.Scale(self.root, from_=0, to=100, orient="horizontal", 
                                   bg="#121212", fg="white", highlightthickness=0, length=200,
                                   command=self.on_slider_move)
        self.led_slider.pack()
        
        self.btn_led = tk.Label(self.root, text="KAPALI", font=btn_font, bg="#c0392b", fg="white", 
                               width=15, height=2, cursor="hand2")
        self.btn_led.pack(pady=10)
        self.btn_led.bind("<Button-1>", lambda e: self.toggle_led())

        # Durum Mesajı
        self.status_label = tk.Label(self.root, text="Sistem Hazır", fg="#888", bg="#121212")
        self.status_label.pack(side="bottom", pady=20)

    # --- MANTIK FONKSİYONLARI ---

    def toggle_trafo(self):
        if not self.trafo_on:
            if self.send_udp("T1"):
                self.trafo_on = True
                self.btn_trafo.config(text="AÇIK", bg="#27ae60")
        else:
            if self.send_udp("T0"):
                self.trafo_on = False
                self.btn_trafo.config(text="KAPALI", bg="#c0392b")

    def toggle_lazer(self):
        if not self.lazer_on:
            if self.send_udp("L1"):
                self.lazer_on = True
                self.btn_lazer.config(text="AÇIK", bg="#27ae60")
        else:
            if self.send_udp("L0"):
                self.lazer_on = False
                self.btn_lazer.config(text="KAPALI", bg="#c0392b")

    def toggle_led(self):
        val = self.led_slider.get()
        if not self.led_on:
            if self.send_udp(f"LED-{val}"):
                self.led_on = True
                self.btn_led.config(text=f"AÇIK (%{val})", bg="#27ae60")
        else:
            if self.send_udp("LED-0"):
                self.led_on = False
                self.btn_led.config(text="KAPALI", bg="#c0392b")

    def on_slider_move(self, val):
        # Eğer LED zaten açıksa, kaydırıcı hareket ettikçe güncelle
        if self.led_on:
            self.send_udp(f"LED-{val}")
            self.btn_led.config(text=f"AÇIK (%{val})")

    def send_udp(self, cmd):
        try:
            # Komut ID'yi artır ve komuta ekle
            self.cmd_id = (self.cmd_id + 1) % 1000  # 0-999 arası döngü
            cmd_with_id = f"{self.cmd_id}:{cmd}"
            
            self.sock.sendto(cmd_with_id.encode(), (STM32_IP, STM32_PORT))
            data, addr = self.sock.recvfrom(1024)
            resp = data.decode().strip()
            
            # ACK'den ID'yi parse et ve kontrol et
            if ":" in resp:
                resp_id_str, resp_msg = resp.split(":", 1)
                try:
                    resp_id = int(resp_id_str)
                    if resp_id == self.cmd_id:
                        self.status_label.config(text=f"[ID:{resp_id}] {resp_msg}", fg="#00d1b2")
                        return True
                    else:
                        self.status_label.config(text=f"ID Uyumsuz! Beklenen:{self.cmd_id}, Gelen:{resp_id}", fg="#ff3860")
                        return False
                except ValueError:
                    self.status_label.config(text=f"Geçersiz ID formatı: {resp}", fg="#ff3860")
                    return False
            else:
                # Eski format desteği (geriye uyumluluk)
                self.status_label.config(text=f"Eski format: {resp}", fg="#ffaa00")
                return True
        except socket.timeout:
            self.status_label.config(text=f"HATA: STM32 Yanıt Vermedi! [ID:{self.cmd_id}]", fg="#ff3860")
            return False
        except Exception as e:
            self.status_label.config(text=f"Sistem Hatası: {e}", fg="#ff3860")
            return False

if __name__ == "__main__":
    root = tk.Tk()
    app = AUVControlApp(root)
    root.mainloop()