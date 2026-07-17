#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <fstream>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>

#define BROKER_PORT 65440
#define LOCAL_PORT 65442
#define BUFFER_SIZE 8192

namespace fs = std::filesystem;

struct Peer {
    std::string ip;
    int puerto;
    std::string estado;  // "IDLE", "UPLOADING", "DOWNLOADING"
};

std::map<std::string, Peer> lista_peers;
std::mutex mutex_peers;

int broker_fd = -1;
int p2p_server_fd = -1;
int local_server_fd = -1;
int frontend_fd = -1;  // socket del frontend actual (solo uno)
std::string mi_ip;
int mi_puerto_p2p;
std::string directorio_videos = "videos/";

// ------------------- Enviar al frontend -------------------
void enviar_frontend(const std::string& msg) {
    if (frontend_fd != -1) {
        send(frontend_fd, (msg + "\n").c_str(), msg.size() + 1, 0);
    }
}

// ------------------- Notificar estado de un peer -------------------
void notificar_estado(const std::string& key, const std::string& estado) {
    enviar_frontend("PEER_STATUS " + key + " " + estado);
}

// ------------------- Comunicación con Broker -------------------
void enviar_a_broker(const std::string& cmd) {
    if (broker_fd != -1) {
        send(broker_fd, (cmd + "\n").c_str(), cmd.size() + 1, 0);
    }
}

void procesar_mensaje_broker(const std::string& msg) {
    std::istringstream iss(msg);
    std::string comando;
    iss >> comando;

    if (comando == "PEERS_LIST") {
        std::lock_guard<std::mutex> lock(mutex_peers);
        lista_peers.clear();
        std::string peer_entry;
        while (iss >> peer_entry) {
            size_t colon = peer_entry.find(':');
            if (colon != std::string::npos) {
                std::string ip = peer_entry.substr(0, colon);
                int puerto = std::stoi(peer_entry.substr(colon + 1));
                std::string key = ip + ":" + std::to_string(puerto);
                if (ip != mi_ip || puerto != mi_puerto_p2p) {  // no incluirse a sí mismo
                    lista_peers[key] = {ip, puerto, "IDLE"};
                }
            }
        }
        std::cout << "[Peer] Lista de peers actualizada: " << lista_peers.size() << " peers" << std::endl;
        // Notificar al frontend la lista actualizada
        std::string lista = "PEERS_LIST";
        for (const auto& par : lista_peers) {
            lista += " " + par.second.ip + ":" + std::to_string(par.second.puerto);
        }
        enviar_frontend(lista);
    }
}

void escuchar_broker() {
    char buffer[BUFFER_SIZE];
    std::string buffer_str;
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(broker_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            std::cout << "[Peer] Conexión con broker perdida" << std::endl;
            close(broker_fd);
            broker_fd = -1;
            break;
        }
        buffer_str += buffer;
        while (buffer_str.find('\n') != std::string::npos) {
            size_t pos = buffer_str.find('\n');
            std::string line = buffer_str.substr(0, pos);
            buffer_str.erase(0, pos + 1);
            procesar_mensaje_broker(line);
        }
    }
}

bool conectar_broker(const std::string& broker_ip) {
    broker_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (broker_fd < 0) return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BROKER_PORT);
    inet_pton(AF_INET, broker_ip.c_str(), &addr.sin_addr);

    if (connect(broker_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(broker_fd);
        broker_fd = -1;
        return false;
    }

    std::string reg = "REGISTER " + mi_ip + " " + std::to_string(mi_puerto_p2p) + "\n";
    send(broker_fd, reg.c_str(), reg.size(), 0);

    std::thread(escuchar_broker).detach();
    return true;
}

// ------------------- Servidor P2P (atender otros peers) -------------------
std::string obtener_lista_archivos() {
    std::string lista;
    for (const auto& entry : fs::directory_iterator(directorio_videos)) {
        if (entry.is_regular_file()) {
            lista += entry.path().filename().string() + "\n";
        }
    }
    if (lista.empty()) lista = "No hay archivos disponibles\n";
    return lista;
}

void manejar_descarga(int client_fd, const std::string& filename, const std::string& peer_key) {
    // Notificar que este peer está subiendo
    notificar_estado(peer_key, "UPLOADING");
    
    std::string path = directorio_videos + "/" + filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        send(client_fd, "ERROR: Archivo no encontrado\n", 30, 0);
        notificar_estado(peer_key, "IDLE");
        return;
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string sizeMsg = "SIZE " + std::to_string(size) + "\n";
    send(client_fd, sizeMsg.c_str(), sizeMsg.size(), 0);

    char buffer[BUFFER_SIZE];
    size_t enviados = 0;
    int last_progress = -1;
    while (!file.eof()) {
        file.read(buffer, BUFFER_SIZE);
        std::streamsize bytesRead = file.gcount();
        if (bytesRead > 0) {
            send(client_fd, buffer, bytesRead, 0);
            enviados += bytesRead;
            int progress = (int)((enviados * 100) / size);
            if (progress != last_progress && progress % 10 == 0) {
                std::cout << "[Peer P2P] Subiendo " << filename << ": " << progress << "%" << std::endl;
                last_progress = progress;
            }
        }
    }
    file.close();
    std::cout << "[Peer P2P] Enviado: " << filename << " (" << size << " bytes)" << std::endl;
    
    // Volver a IDLE
    notificar_estado(peer_key, "IDLE");
}

void atender_peer_p2p(int client_fd) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_fd, (struct sockaddr*)&addr, &addr_len);
    std::string peer_ip = inet_ntoa(addr.sin_addr);
    int peer_puerto = ntohs(addr.sin_port);
    std::string peer_key = peer_ip + ":" + std::to_string(peer_puerto);
    
    char buffer[BUFFER_SIZE];
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;

        std::string cmd(buffer);
        if (cmd.rfind("LIST", 0) == 0) {
            std::string lista = obtener_lista_archivos();
            send(client_fd, lista.c_str(), lista.size(), 0);
        } else if (cmd.rfind("DOWNLOAD ", 0) == 0) {
            std::string filename = cmd.substr(9);
            filename.erase(filename.find_last_not_of("\n\r") + 1);
            manejar_descarga(client_fd, filename, peer_key);
        } else {
            send(client_fd, "ERROR: Comando desconocido\n", 27, 0);
        }
    }
    close(client_fd);
}

void iniciar_servidor_p2p() {
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    p2p_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(p2p_server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(mi_puerto_p2p);

    bind(p2p_server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(p2p_server_fd, 5);

    std::cout << "[Peer P2P] Servidor escuchando en puerto " << mi_puerto_p2p << std::endl;

    while (true) {
        client_fd = accept(p2p_server_fd, (struct sockaddr*)&address, &addrlen);
        std::thread(atender_peer_p2p, client_fd).detach();
    }
}

// ------------------- Funciones para el frontend (Python) -------------------
void manejar_comando_frontend(int fd, const std::string& cmd) {
    std::istringstream iss(cmd);
    std::string comando;
    iss >> comando;

    if (comando == "GET_PEERS") {
        std::lock_guard<std::mutex> lock(mutex_peers);
        std::string respuesta = "PEERS_LIST";
        for (const auto& par : lista_peers) {
            respuesta += " " + par.second.ip + ":" + std::to_string(par.second.puerto);
        }
        send(fd, (respuesta + "\n").c_str(), respuesta.size() + 1, 0);
        // También enviar estados actuales
        for (const auto& par : lista_peers) {
            if (par.second.estado != "IDLE") {
                send(fd, ("PEER_STATUS " + par.first + " " + par.second.estado + "\n").c_str(), 0, 0);
            }
        }
    }
    else if (comando == "LIST_FROM_PEER") {
        std::string ip;
        int puerto;
        iss >> ip >> puerto;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(puerto);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            send(fd, "ERROR: No se pudo conectar al peer\n", 35, 0);
            return;
        }
        send(sock, "LIST\n", 5, 0);
        char buffer[BUFFER_SIZE];
        std::string respuesta;
        while (true) {
            memset(buffer, 0, BUFFER_SIZE);
            int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
            if (bytes <= 0) break;
            respuesta += buffer;
            if (respuesta.find('\n') != std::string::npos) break;
        }
        send(fd, (respuesta + "\n").c_str(), respuesta.size() + 1, 0);
        close(sock);
    }
    else if (comando == "DOWNLOAD_FROM_PEER") {
        std::string ip, filename;
        int puerto;
        iss >> ip >> puerto >> filename;
        
        // Notificar que este peer está descargando
        std::string mi_key = mi_ip + ":" + std::to_string(mi_puerto_p2p);
        notificar_estado(mi_key, "DOWNLOADING");
        
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(puerto);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            send(fd, "ERROR: No se pudo conectar al peer\n", 35, 0);
            notificar_estado(mi_key, "IDLE");
            return;
        }
        std::string cmd_download = "DOWNLOAD " + filename + "\n";
        send(sock, cmd_download.c_str(), cmd_download.size(), 0);
        
        char buffer[BUFFER_SIZE];
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            send(fd, "ERROR: No hay respuesta\n", 24, 0);
            close(sock);
            notificar_estado(mi_key, "IDLE");
            return;
        }
        std::string respuesta(buffer);
        if (respuesta.rfind("SIZE ", 0) != 0) {
            send(fd, respuesta.c_str(), respuesta.size(), 0);
            close(sock);
            notificar_estado(mi_key, "IDLE");
            return;
        }
        size_t total_size = std::stoull(respuesta.substr(5));
        send(fd, ("SIZE " + std::to_string(total_size) + "\n").c_str(), 0, 0);

        fs::create_directory("descargas");
        std::string path = "descargas/" + filename;
        std::ofstream outfile(path, std::ios::binary);
        size_t recibidos = 0;
        int last_progress = -1;
        while (recibidos < total_size) {
            memset(buffer, 0, BUFFER_SIZE);
            int chunk = recv(sock, buffer, BUFFER_SIZE, 0);
            if (chunk <= 0) break;
            outfile.write(buffer, chunk);
            recibidos += chunk;
            int progress = (int)((recibidos * 100) / total_size);
            if (progress != last_progress && progress % 5 == 0) {
                send(fd, ("PROGRESS " + std::to_string(progress) + "\n").c_str(), 0, 0);
                std::cout << "[Peer] Descargando " << filename << ": " << progress << "%" << std::endl;
                last_progress = progress;
            }
        }
        outfile.close();
        if (recibidos == total_size) {
            send(fd, "DOWNLOAD_COMPLETE\n", 19, 0);
            std::cout << "[Peer] ✅ Descarga completada: " << filename << std::endl;
        } else {
            send(fd, "ERROR: Descarga incompleta\n", 27, 0);
        }
        close(sock);
        notificar_estado(mi_key, "IDLE");
    }
    else {
        send(fd, "ERROR: Comando desconocido\n", 27, 0);
    }
}

void atender_frontend(int fd) {
    frontend_fd = fd;
    char buffer[BUFFER_SIZE];
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;
        std::string cmd(buffer);
        manejar_comando_frontend(fd, cmd);
    }
    frontend_fd = -1;
    close(fd);
}

void iniciar_servidor_local() {
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    local_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(local_server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons(LOCAL_PORT);

    bind(local_server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(local_server_fd, 5);

    std::cout << "[Peer] Servidor local para frontend en puerto " << LOCAL_PORT << std::endl;

    while (true) {
        client_fd = accept(local_server_fd, (struct sockaddr*)&address, &addrlen);
        std::thread(atender_frontend, client_fd).detach();
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Uso: " << argv[0] << " <broker_ip> <mi_ip> <puerto_p2p> [directorio_videos]" << std::endl;
        return 1;
    }

    std::string broker_ip = argv[1];
    mi_ip = argv[2];
    mi_puerto_p2p = std::stoi(argv[3]);
    if (argc >= 5) {
        directorio_videos = argv[4];
    }

    fs::create_directory(directorio_videos);

    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     Peer P2P - Nodo de Video                          ║" << std::endl;
    std::cout << "║     IP: " << mi_ip << "  P2P Puerto: " << mi_puerto_p2p << "                        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    if (!conectar_broker(broker_ip)) {
        std::cerr << "Error: No se pudo conectar al broker" << std::endl;
        return 1;
    }

    std::thread p2p_thread(iniciar_servidor_p2p);
    std::thread local_thread(iniciar_servidor_local);

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    close(p2p_server_fd);
    close(local_server_fd);
    return 0;
}