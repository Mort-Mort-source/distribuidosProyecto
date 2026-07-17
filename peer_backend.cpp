#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <fstream>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <chrono>

#define BROKER_PORT 65440
#define P2P_PORT 65441          // Puerto base, se puede especificar
#define LOCAL_PORT 65442        // Para comunicación con frontend
#define BUFFER_SIZE 8192

namespace fs = std::filesystem;

// ------------------- Estructuras -------------------
struct Peer {
    std::string ip;
    int puerto;
};

std::map<std::string, Peer> lista_peers;  // key: "ip:puerto"
std::mutex mutex_peers;

int broker_fd = -1;
int p2p_server_fd = -1;
int local_server_fd = -1;
std::string mi_ip;
int mi_puerto_p2p;
std::string directorio_videos = "videos/";

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
        std::string ip;
        int puerto;
        while (iss >> ip >> puerto) {
            std::string key = ip + ":" + std::to_string(puerto);
            lista_peers[key] = {ip, puerto};
        }
        std::cout << "[Peer] Lista de peers actualizada: " << lista_peers.size() << " peers" << std::endl;
    }
    else if (comando == "PEER_JOINED") {
        std::string ip;
        int puerto;
        iss >> ip >> puerto;
        std::string key = ip + ":" + std::to_string(puerto);
        {
            std::lock_guard<std::mutex> lock(mutex_peers);
            lista_peers[key] = {ip, puerto};
        }
        std::cout << "[Peer] Nuevo peer: " << ip << ":" << puerto << std::endl;
    }
    else if (comando == "PEER_LEFT") {
        std::string ip;
        int puerto;
        iss >> ip >> puerto;
        std::string key = ip + ":" + std::to_string(puerto);
        {
            std::lock_guard<std::mutex> lock(mutex_peers);
            lista_peers.erase(key);
        }
        std::cout << "[Peer] Peer desconectado: " << ip << ":" << puerto << std::endl;
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

    // Enviar registro
    std::string reg = "REGISTER " + mi_ip + " " + std::to_string(mi_puerto_p2p) + "\n";
    send(broker_fd, reg.c_str(), reg.size(), 0);

    // Iniciar hilo para escuchar broker
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

void manejar_descarga(int client_fd, const std::string& filename) {
    std::string path = directorio_videos + "/" + filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        send(client_fd, "ERROR: Archivo no encontrado\n", 30, 0);
        return;
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string sizeMsg = "SIZE " + std::to_string(size) + "\n";
    send(client_fd, sizeMsg.c_str(), sizeMsg.size(), 0);

    char buffer[BUFFER_SIZE];
    while (!file.eof()) {
        file.read(buffer, BUFFER_SIZE);
        std::streamsize bytesRead = file.gcount();
        if (bytesRead > 0) {
            send(client_fd, buffer, bytesRead, 0);
        }
    }
    file.close();
    std::cout << "[Peer P2P] Enviado: " << filename << " (" << size << " bytes)" << std::endl;
}

void atener_peer_p2p(int client_fd) {
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
            manejar_descarga(client_fd, filename);
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
        std::thread(atener_peer_p2p, client_fd).detach();
    }
}

// ------------------- Funciones para el frontend (Python) -------------------
void enviar_al_frontend(int frontend_fd, const std::string& msg) {
    send(frontend_fd, (msg + "\n").c_str(), msg.size() + 1, 0);
}

void manejar_comando_frontend(int frontend_fd, const std::string& cmd) {
    std::istringstream iss(cmd);
    std::string comando;
    iss >> comando;

    if (comando == "GET_PEERS") {
        std::lock_guard<std::mutex> lock(mutex_peers);
        std::string respuesta = "PEERS_LIST\n";
        for (const auto& par : lista_peers) {
            respuesta += par.second.ip + " " + std::to_string(par.second.puerto) + "\n";
        }
        send(frontend_fd, respuesta.c_str(), respuesta.size(), 0);
    }
    else if (comando == "LIST_FROM_PEER") {
        std::string ip;
        int puerto;
        iss >> ip >> puerto;
        // Conectar al peer y pedir lista
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(puerto);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            send(frontend_fd, "ERROR: No se pudo conectar al peer\n", 35, 0);
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
        send(frontend_fd, respuesta.c_str(), respuesta.size(), 0);
        close(sock);
    }
    else if (comando == "DOWNLOAD_FROM_PEER") {
        std::string ip, filename;
        int puerto;
        iss >> ip >> puerto >> filename;
        // Conectar al peer y descargar
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(puerto);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            send(frontend_fd, "ERROR: No se pudo conectar al peer\n", 35, 0);
            return;
        }
        std::string cmd = "DOWNLOAD " + filename + "\n";
        send(sock, cmd.c_str(), cmd.size(), 0);
        // Recibir SIZE
        char buffer[BUFFER_SIZE];
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            send(frontend_fd, "ERROR: No hay respuesta\n", 24, 0);
            close(sock);
            return;
        }
        std::string respuesta(buffer);
        if (respuesta.rfind("SIZE ", 0) != 0) {
            send(frontend_fd, respuesta.c_str(), respuesta.size(), 0);
            close(sock);
            return;
        }
        size_t total_size = std::stoull(respuesta.substr(5));
        send(frontend_fd, ("SIZE " + std::to_string(total_size) + "\n").c_str(), 0, 0);

        // Crear archivo
        fs::create_directory("descargas");
        std::string path = "descargas/" + filename;
        std::ofstream outfile(path, std::ios::binary);
        size_t recibidos = 0;
        while (recibidos < total_size) {
            memset(buffer, 0, BUFFER_SIZE);
            int chunk = recv(sock, buffer, BUFFER_SIZE, 0);
            if (chunk <= 0) break;
            outfile.write(buffer, chunk);
            recibidos += chunk;
            int progress = (int)((recibidos * 100) / total_size);
            if (progress % 10 == 0) {
                send(frontend_fd, ("PROGRESS " + std::to_string(progress) + "\n").c_str(), 0, 0);
            }
        }
        outfile.close();
        if (recibidos == total_size) {
            send(frontend_fd, "DOWNLOAD_COMPLETE\n", 19, 0);
            std::cout << "[Peer] Descarga completada: " << filename << std::endl;
        } else {
            send(frontend_fd, "ERROR: Descarga incompleta\n", 27, 0);
        }
        close(sock);
    }
    else {
        send(frontend_fd, "ERROR: Comando desconocido\n", 27, 0);
    }
}

void atender_frontend(int frontend_fd) {
    char buffer[BUFFER_SIZE];
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(frontend_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;
        std::string cmd(buffer);
        manejar_comando_frontend(frontend_fd, cmd);
    }
    close(frontend_fd);
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

// ------------------- Main -------------------
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

    // Crear directorio de videos si no existe
    fs::create_directory(directorio_videos);

    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     Peer P2P - Nodo de Video                          ║" << std::endl;
    std::cout << "║     IP: " << mi_ip << "  P2P Puerto: " << mi_puerto_p2p << "                        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    // Conectar al broker
    if (!conectar_broker(broker_ip)) {
        std::cerr << "Error: No se pudo conectar al broker" << std::endl;
        return 1;
    }

    // Iniciar servidor P2P en un hilo
    std::thread p2p_thread(iniciar_servidor_p2p);

    // Iniciar servidor local para frontend
    std::thread local_thread(iniciar_servidor_local);

    // Mantener el programa corriendo
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    close(p2p_server_fd);
    close(local_server_fd);
    return 0;
}