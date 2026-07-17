import socket
import threading
import tkinter as tk
from tkinter import ttk, messagebox
import os

PEER_HOST = "127.0.0.1"
PEER_PORT = 65442

class PeerFrontend:
    def __init__(self, root):
        self.root = root
        self.root.title("Peer P2P - Cliente de Video")
        self.root.geometry("850x650")

        self.socket = None
        self.receive_thread = None
        self.downloading = False
        self.buffer = ""
        self.lock = threading.Lock()

        self.peers_list = {}       # key -> {"ip": ip, "puerto": puerto}
        self.peer_states = {}      # key -> estado
        self.files_list = []
        self.progress_var = tk.IntVar()

        self.create_widgets()
        self.connect_to_peer()
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

    def create_widgets(self):
        # Marco superior: estado
        top_frame = tk.Frame(self.root)
        top_frame.pack(fill="x", padx=10, pady=5)
        self.status_label = tk.Label(top_frame, text="Conectando al peer backend...", fg="gray")
        self.status_label.pack(side="left")
        tk.Button(top_frame, text="Refrescar peers", command=self.refresh_peers).pack(side="right")

        # Lista de peers con estados
        list_frame = tk.LabelFrame(self.root, text="Peers disponibles (estado)")
        list_frame.pack(fill="both", expand=True, padx=10, pady=5)
        self.peer_listbox = tk.Listbox(list_frame, height=8)
        self.peer_listbox.pack(side="left", fill="both", expand=True)
        scrollbar1 = tk.Scrollbar(list_frame, orient="vertical", command=self.peer_listbox.yview)
        scrollbar1.pack(side="right", fill="y")
        self.peer_listbox.config(yscrollcommand=scrollbar1.set)
        self.peer_listbox.bind('<<ListboxSelect>>', self.on_peer_select)

        # Lista de archivos
        files_frame = tk.LabelFrame(self.root, text="Archivos del peer seleccionado")
        files_frame.pack(fill="both", expand=True, padx=10, pady=5)
        self.files_listbox = tk.Listbox(files_frame, height=8)
        self.files_listbox.pack(side="left", fill="both", expand=True)
        scrollbar2 = tk.Scrollbar(files_frame, orient="vertical", command=self.files_listbox.yview)
        scrollbar2.pack(side="right", fill="y")
        self.files_listbox.config(yscrollcommand=scrollbar2.set)
        tk.Button(files_frame, text="Listar archivos", command=self.list_files).pack(side="right", padx=5)

        # Descarga
        download_frame = tk.Frame(self.root)
        download_frame.pack(fill="x", padx=10, pady=5)
        tk.Label(download_frame, text="Archivo:").pack(side="left")
        self.filename_entry = tk.Entry(download_frame, width=30)
        self.filename_entry.pack(side="left", padx=5)
        self.download_btn = tk.Button(download_frame, text="Descargar", command=self.download_file)
        self.download_btn.pack(side="left", padx=5)

        # Barra de progreso
        self.progress_bar = ttk.Progressbar(self.root, variable=self.progress_var, maximum=100)
        self.progress_bar.pack(fill="x", padx=10, pady=5)
        self.progress_label = tk.Label(self.root, text="")
        self.progress_label.pack()

        # Estado de descarga
        self.download_status = tk.Label(self.root, text="Listo", fg="green")
        self.download_status.pack(pady=2)

    def connect_to_peer(self):
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect((PEER_HOST, PEER_PORT))
            self.receive_thread = threading.Thread(target=self.receive_messages, daemon=True)
            self.receive_thread.start()
            self.status_label.config(text="Conectado al peer backend", fg="green")
            self.refresh_peers()
        except Exception as e:
            self.status_label.config(text=f"Error: {e}", fg="red")
            messagebox.showerror("Error", f"No se pudo conectar al peer backend:\n{e}")

    def receive_messages(self):
        while True:
            try:
                data = self.socket.recv(4096).decode()
                if not data:
                    break
                with self.lock:
                    self.buffer += data
                    while "\n" in self.buffer:
                        line, self.buffer = self.buffer.split("\n", 1)
                        self.process_message(line)
            except Exception as e:
                print(f"Error en receive: {e}")
                break

    def process_message(self, msg):
        msg = msg.strip()
        if not msg:
            return

        if msg.startswith("SIZE "):
            size = int(msg.split()[1])
            self.downloading = True
            self.progress_var.set(0)
            self.progress_label.config(text=f"Iniciando descarga ({size} bytes)...")
            self.download_status.config(text="DESCARGANDO...", fg="orange")
            self.download_btn.config(state="disabled")

        elif msg.startswith("PROGRESS "):
            percent = int(msg.split()[1])
            self.progress_var.set(percent)
            self.progress_label.config(text=f"Descargando... {percent}%")
            self.download_status.config(text=f"DESCARGANDO... {percent}%", fg="orange")

        elif msg.startswith("DOWNLOAD_COMPLETE"):
            self.downloading = False
            self.download_btn.config(state="normal")
            self.progress_label.config(text="¡Descarga completada!")
            self.download_status.config(text="COMPLETADO ✅", fg="green")
            messagebox.showinfo("Éxito", "Archivo descargado en carpeta descargas/")
            # Resetear progreso después de un momento
            self.root.after(3000, lambda: self.download_status.config(text="Listo", fg="green"))

        elif msg.startswith("PEER_STATUS"):
            parts = msg.split()
            if len(parts) == 3:
                key = parts[1]
                estado = parts[2]
                self.peer_states[key] = estado
                self.update_peer_list()

        elif msg.startswith("PEERS_LIST"):
            self.peer_listbox.delete(0, tk.END)
            self.peers_list.clear()
            parts = msg.split()[1:]
            if not parts:
                self.peer_listbox.insert(tk.END, "No hay peers conectados")
            else:
                for part in parts:
                    if ':' in part:
                        ip, puerto = part.split(':')
                        key = f"{ip}:{puerto}"
                        self.peers_list[key] = {"ip": ip, "puerto": int(puerto)}
                        if key not in self.peer_states:
                            self.peer_states[key] = "IDLE"
                        self.peer_listbox.insert(tk.END, f"{key} ({self.peer_states.get(key, 'IDLE')})")
                # Forzar actualización de estados
                self.update_peer_list()

        elif msg.startswith("ERROR"):
            self.downloading = False
            self.download_btn.config(state="normal")
            self.progress_label.config(text="Error")
            self.download_status.config(text="ERROR", fg="red")
            messagebox.showerror("Error", msg)

        else:
            # Asumimos que es lista de archivos
            if not self.downloading:
                self.files_listbox.delete(0, tk.END)
                self.files_list.clear()
                for line in msg.splitlines():
                    if line.strip():
                        self.files_listbox.insert(tk.END, line)
                        self.files_list.append(line)

    def update_peer_list(self):
        """Actualiza el listbox de peers con los estados actuales"""
        self.peer_listbox.delete(0, tk.END)
        if not self.peers_list:
            self.peer_listbox.insert(tk.END, "No hay peers conectados")
        else:
            for key, info in self.peers_list.items():
                estado = self.peer_states.get(key, "IDLE")
                display_text = f"{key} ({estado})"
                self.peer_listbox.insert(tk.END, display_text)

    def send_command(self, cmd):
        try:
            self.socket.sendall((cmd + "\n").encode())
        except Exception as e:
            messagebox.showerror("Error", f"Perdida conexión con peer: {e}")

    def refresh_peers(self):
        self.send_command("GET_PEERS")

    def on_peer_select(self, event):
        selection = self.peer_listbox.curselection()
        if selection:
            key_raw = self.peer_listbox.get(selection[0])
            key = key_raw.split(" (")[0]  # extraer solo IP:puerto
            self.files_listbox.delete(0, tk.END)
            self.files_list.clear()

    def list_files(self):
        selection = self.peer_listbox.curselection()
        if not selection:
            messagebox.showwarning("Aviso", "Seleccione un peer primero")
            return
        key_raw = self.peer_listbox.get(selection[0])
        key = key_raw.split(" (")[0]
        peer = self.peers_list.get(key)
        if not peer:
            messagebox.showerror("Error", "Peer no encontrado")
            return
        self.send_command(f"LIST_FROM_PEER {peer['ip']} {peer['puerto']}")

    def download_file(self):
        selection = self.peer_listbox.curselection()
        if not selection:
            messagebox.showwarning("Aviso", "Seleccione un peer primero")
            return
        if self.downloading:
            return

        filename = self.filename_entry.get().strip()
        if not filename:
            file_sel = self.files_listbox.curselection()
            if file_sel:
                filename = self.files_listbox.get(file_sel[0])
            else:
                messagebox.showwarning("Aviso", "Seleccione o escriba un nombre de archivo")
                return

        key_raw = self.peer_listbox.get(selection[0])
        key = key_raw.split(" (")[0]
        peer = self.peers_list.get(key)
        if not peer:
            messagebox.showerror("Error", "Peer no encontrado")
            return
        self.send_command(f"DOWNLOAD_FROM_PEER {peer['ip']} {peer['puerto']} {filename}")

    def on_closing(self):
        if self.socket:
            self.socket.close()
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = PeerFrontend(root)
    root.mainloop()