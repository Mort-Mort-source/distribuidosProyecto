#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BROKER_PORT 65440
#define BUFFER_SIZE 8192

struct PeerInfo {
    std::string ip;
    int puerto;
    int socket_fd;
};

std::map<std::string, PeerInfo> peers;  // key: "ip:puerto"
std::mutex mutex_peers;

// ------------------- Enviar lista completa a todos los peers -------------------
void broadcast_peers_list() {
    std::lock_guard<std::mutex> lock(mutex_peers);
    std::string lista = "PEERS_LIST";
    for (const auto& par : peers) {
        lista += " " + par.second.ip + ":" + std::to_string(par.second.puerto);
    }
    lista += "\n";
    
    std::vector<std::string> to_remove;
    for (auto& par : peers) {
        int fd = par.second.socket_fd;
        if (fd != -1) {
            int sent = send(fd, lista.c_str(), lista.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                to_remove.push_back(par.first);  // marcar para eliminar
            }
        }
    }
    // Eliminar peers con socket roto
    for (const auto& key : to_remove) {
        peers.erase(key);
        std::cout << "[Broker] Peer eliminado por socket roto: " << key << std::endl;
    }
}

// ------------------- Atender un peer -------------------
void atender_peer(int peer_fd) {
    char buffer[BUFFER_SIZE];
    std::string peer_key;

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(peer_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;

        std::string cmd(buffer);
        std::istringstream iss(cmd);
        std::string comando;
        iss >> comando;

        if (comando == "REGISTER") {
            std::string ip;
            int puerto;
            iss >> ip >> puerto;
            if (ip.empty() || puerto == 0) {
                send(peer_fd, "ERROR: IP y puerto requeridos\n", 30, 0);
                continue;
            }

            peer_key = ip + ":" + std::to_string(puerto);
            {
                std::lock_guard<std::mutex> lock(mutex_peers);
                // Si ya existe, actualizar socket
                auto it = peers.find(peer_key);
                if (it != peers.end()) {
                    close(it->second.socket_fd);
                }
                peers[peer_key] = {ip, puerto, peer_fd};
            }
            std::cout << "[Broker] Peer registrado: " << ip << ":" << puerto << std::endl;

            // Enviar lista completa a TODOS los peers
            broadcast_peers_list();

        } else if (comando == "GET_PEERS") {
            // Devolver lista completa al peer que la solicita
            std::lock_guard<std::mutex> lock(mutex_peers);
            std::string lista = "PEERS_LIST";
            for (const auto& par : peers) {
                lista += " " + par.second.ip + ":" + std::to_string(par.second.puerto);
            }
            lista += "\n";
            send(peer_fd, lista.c_str(), lista.size(), 0);

        } else {
            send(peer_fd, "ERROR: Comando desconocido\n", 27, 0);
        }
    }

    // Peer desconectado
    if (!peer_key.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_peers);
            peers.erase(peer_key);
        }
        std::cout << "[Broker] Peer desconectado: " << peer_key << std::endl;
        // Notificar a todos los demás con la nueva lista
        broadcast_peers_list();
    }
    close(peer_fd);
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(BROKER_PORT);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 10);

    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     Broker P2P - Coordinador de Peers                 ║" << std::endl;
    std::cout << "║     Escuchando en puerto " << BROKER_PORT << "                         ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    while (true) {
        client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        std::thread(atender_peer, client_fd).detach();
    }
    close(server_fd);
    return 0;
}