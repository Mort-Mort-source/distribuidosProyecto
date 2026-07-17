#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#define BROKER_PORT 65440
#define BUFFER_SIZE 8192

struct PeerInfo {
    std::string ip;
    int puerto;
    int socket_fd;  // socket para comunicación con el broker
};

std::map<std::string, PeerInfo> peers;  // key: "ip:puerto"
std::mutex mutex_peers;

// ------------------- Notificar a todos los peers -------------------
void notificar_peers(const std::string& comando, const std::string& ip, int puerto) {
    std::lock_guard<std::mutex> lock(mutex_peers);
    std::string msg = comando + " " + ip + " " + std::to_string(puerto) + "\n";
    for (auto& par : peers) {
        if (par.second.socket_fd != -1) {
            send(par.second.socket_fd, msg.c_str(), msg.size(), 0);
        }
    }
}

// ------------------- Atender un peer -------------------
void atender_peer(int peer_fd) {
    char buffer[BUFFER_SIZE];
    std::string peer_ip;
    int peer_puerto;
    bool registrado = false;

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

            std::string key = ip + ":" + std::to_string(puerto);
            {
                std::lock_guard<std::mutex> lock(mutex_peers);
                // Si ya existe, actualizar socket
                auto it = peers.find(key);
                if (it != peers.end()) {
                    close(it->second.socket_fd);
                }
                peers[key] = {ip, puerto, peer_fd};
            }
            peer_ip = ip;
            peer_puerto = puerto;
            registrado = true;

            // Enviar lista actual de peers al nuevo peer
            std::string lista = "PEERS_LIST\n";
            {
                std::lock_guard<std::mutex> lock(mutex_peers);
                for (const auto& par : peers) {
                    if (par.first != key) {
                        lista += par.second.ip + " " + std::to_string(par.second.puerto) + "\n";
                    }
                }
            }
            send(peer_fd, lista.c_str(), lista.size(), 0);

            // Notificar a todos los demás que un nuevo peer se unió
            notificar_peers("PEER_JOINED", ip, puerto);
            std::cout << "[Broker] Peer registrado: " << ip << ":" << puerto << std::endl;

        } else if (comando == "GET_PEERS") {
            // Devolver lista actual de peers
            std::string lista = "PEERS_LIST\n";
            {
                std::lock_guard<std::mutex> lock(mutex_peers);
                for (const auto& par : peers) {
                    lista += par.second.ip + " " + std::to_string(par.second.puerto) + "\n";
                }
            }
            send(peer_fd, lista.c_str(), lista.size(), 0);

        } else {
            send(peer_fd, "ERROR: Comando desconocido\n", 27, 0);
        }
    }

    // Peer desconectado
    if (registrado) {
        std::string key = peer_ip + ":" + std::to_string(peer_puerto);
        {
            std::lock_guard<std::mutex> lock(mutex_peers);
            peers.erase(key);
        }
        std::cout << "[Broker] Peer desconectado: " << peer_ip << ":" << peer_puerto << std::endl;
        notificar_peers("PEER_LEFT", peer_ip, peer_puerto);
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