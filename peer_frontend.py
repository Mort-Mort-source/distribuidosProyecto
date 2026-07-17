import socket
import threading
import tkinter as tk
from tkinter import ttk, messagebox
import os
import sys

PEER_HOST = "127.0.0.1"
PEER_PORT = 65442

class PeerFrontend:
    def __init__(self, root):
        self.root = root
        self.root.title("Peer P2P - Cliente de Video")
        self.root.geometry("800x600")

        self.socket = None
        self.receive_thread = None
        self.downloading = False
        self.buffer = ""
        self.lock = threading.Lock()

        self.peers_list = {}  # ip:puerto -> nombre
        self.files_list = []  # archivos del peer seleccionado

        self.progress_var = tk.IntVar()

        self.create_widgets()
        self.connect_to_peer()
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

    def create_widgets(self):
        # Marco superior
        top_frame = tk.Frame(self.root)
        top_frame.pack(fill="x", padx=10, pady=5)
        self.status_label = tk.Label(top_frame, text="Conectando al peer backend...", fg="gray")
        self.status_label.pack(side="left")
        tk.Button(top_frame, text="Refrescar peers", command=self.refresh_peers).pack(side="right")

        # Lista de peers
        list_frame = tk.LabelFrame(self.root, text="Peers disponibles")
        list_frame.pack(fill="both", expand=True, padx=10, pady=5)
        self.peer_listbox = tk.Listbox(list_frame, height=8)
        self.peer_listbox.pack(side="left", fill="both", expand=True)
        scrollbar1 = tk.Scrollbar(list_frame, orient="vertical", command=self.peer_listbox.yview)
        scrollbar1.pack(side="right", fill="y")
        self.peer_listbox.config(yscrollcommand=scrollbar1.set)
        self.peer_listbox.bind('<<ListboxSelect>>', self.on_peer_select)

        # Lista de archivos del peer seleccionado
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
            sys.exit(1)

    def receive_messages(self):
        while True:
            try:
                data = self.socket.recv(4096).decode()
                if not data:
                    break
                print(f"[DEBUG] Recibido: {data!r}")  # ← LÍNEA DE DEPURACIÓN
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

        print(f"[DEBUG] Procesando: {msg}")  # ← LÍNEA DE DEPURACIÓN

        # Manejo de mensajes de descarga
        if msg.startswith("SIZE "):
            size = int(msg.split()[1])
            self.downloading = True
            self.progress_var.set(0)
            self.progress_label.config(text=f"Iniciando descarga ({size} bytes)...")
            self.download_btn.config(state="disabled")
            return

        elif msg.startswith("PROGRESS "):
            percent = int(msg.split()[1])
            self.progress_var.set(percent)
            self.progress_label.config(text=f"Descargando... {percent}%")
            return

        elif msg.startswith("DOWNLOAD_COMPLETE"):
            self.downloading = False
            self.download_btn.config(state="normal")
            self.progress_label.config(text="¡Descarga completada!")
            messagebox.showinfo("Éxito", "Archivo descargado en carpeta descargas/")
            return

        elif msg.startswith("ERROR"):
            self.downloading = False
            self.download_btn.config(state="normal")
            self.progress_label.config(text="Error")
            messagebox.showerror("Error", msg)
            return

        # ========== MANEJO DE LISTA DE PEERS ==========
        if msg.startswith("PEERS_LIST"):
            # Limpiar listbox y diccionario
            self.peer_listbox.delete(0, tk.END)
            self.peers_list.clear()

            # Obtener la lista de peers (después de "PEERS_LIST")
            parts = msg.split()[1:]  # omitir "PEERS_LIST"
            if not parts:
                self.peer_listbox.insert(tk.END, "No hay peers conectados")
                self.status_label.config(text="No hay peers disponibles", fg="orange")
                return

            # Insertar cada peer en el listbox
            for part in parts:
                if ':' in part:
                    ip, puerto = part.split(':')
                    key = f"{ip}:{puerto}"
                    self.peers_list[key] = {"ip": ip, "puerto": int(puerto)}
                    self.peer_listbox.insert(tk.END, key)

            self.status_label.config(text=f"Peers actualizados: {len(self.peers_list)} conectados", fg="blue")
            return

        # ========== LISTA DE ARCHIVOS (de un peer) ==========
        else:
            # Si estamos descargando, no procesamos archivos
            if self.downloading:
                return

            # Limpiar listbox de archivos
            self.files_listbox.delete(0, tk.END)
            self.files_list.clear()

            # Insertar cada línea como un archivo
            for line in msg.splitlines():
                if line.strip():
                    self.files_listbox.insert(tk.END, line)
                    self.files_list.append(line)

    def send_command(self, cmd):
        try:
            self.socket.sendall((cmd + "\n").encode())
            print(f"[DEBUG] Enviado: {cmd}")
        except Exception as e:
            messagebox.showerror("Error", f"Perdida conexión con peer: {e}")

    def refresh_peers(self):
        """Solicitar lista de peers al backend"""
        self.send_command("GET_PEERS")

    def on_peer_select(self, event):
        """Cuando se selecciona un peer, limpiar lista de archivos"""
        selection = self.peer_listbox.curselection()
        if selection:
            self.files_listbox.delete(0, tk.END)
            self.files_list.clear()

    def list_files(self):
        """Listar archivos del peer seleccionado"""
        selection = self.peer_listbox.curselection()
        if not selection:
            messagebox.showwarning("Aviso", "Seleccione un peer primero")
            return
        key = self.peer_listbox.get(selection[0])
        if key not in self.peers_list:
            messagebox.showerror("Error", "Peer no encontrado")
            return
        peer = self.peers_list[key]
        self.send_command(f"LIST_FROM_PEER {peer['ip']} {peer['puerto']}")

    def download_file(self):
        """Descargar archivo del peer seleccionado"""
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

        key = self.peer_listbox.get(selection[0])
        if key not in self.peers_list:
            messagebox.showerror("Error", "Peer no encontrado")
            return
        peer = self.peers_list[key]
        self.send_command(f"DOWNLOAD_FROM_PEER {peer['ip']} {peer['puerto']} {filename}")

    def on_closing(self):
        if self.socket:
            self.socket.close()
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = PeerFrontend(root)
    root.mainloop()