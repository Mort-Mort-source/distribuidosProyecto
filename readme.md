# Instructivo de uso

## Paso 1 levantar el broker

compilar y ejecutar el broker con el comando 

``` shell
g++ -std=c++17 -pthread broker.cpp -o broker
./broker
```
## Paso 2  compilar el backend
```shell
g++ -std=c++17 -pthread peer_backend.cpp -o peer
```
## Paso 3 ejecutar en terminal

``` shell
./peer 172.18.53.240 172.18.53.240 65441 videos/
```
donde la primera ip es la ip del broker y la segunda es la del peer.


## Paso 4 levantar frontend


levantar frontend con 
```shell
python3 peer_frontend.py
```

