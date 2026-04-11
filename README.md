# DCGP – Datacenter Game Protocol
## Internet: Arquitectura y Protocolos – EAFIT 2026-1

---

## Estructura del proyecto

```
dcgp/
├── server.c              # Servidor de juego (C, Berkeley Sockets)
├── Makefile              # Compilación del servidor
├── identity_server.py    # Servicio de identidad (Python HTTP)
├── web_server.py         # Interfaz web (Python HTTP)
├── client_attacker.py    # Cliente Atacante (Python + pygame)
├── DefenderClient.java   # Cliente Defensor (Java + Swing)
└── RFC_DCGP.md           # Especificación del protocolo
```

---

## Requisitos

### Servidor C (Linux / GCP Compute Engine)
```bash
sudo apt install gcc make
```

### Python (clientes y servicios auxiliares)
```bash
pip install pygame requests
```

### Java (cliente defensor)
```bash
sudo apt install default-jdk   # Linux
# o instalar JDK desde https://adoptium.net en Windows
```

---

## Compilación y ejecución

### 1. Servicio de identidad (Python) — iniciar primero
```bash
python3 identity_server.py 8081
```

### 2. Servidor de juego (C)
```bash
make
./server 8080 server.log

# Con identity server en otro host:
export IDENTITY_HOST=mi-servidor.ejemplo.com
export IDENTITY_PORT=8081
./server 8080 server.log
```

### 3. Servidor web (Python) — opcional
```bash
export IDENTITY_HOST=localhost
export IDENTITY_PORT=8081
export GAME_HOST=localhost
export GAME_PORT=8080
python3 web_server.py 8082
# Abrir http://localhost:8082 en el navegador
```

### 4. Cliente Atacante (Python)
```bash
export GAME_HOST=localhost
export GAME_PORT=8080
python3 client_attacker.py
# Ingresar usuario y contraseña (ej. alice / pass123)
```

### 5. Cliente Defensor (Java)
```bash
javac DefenderClient.java
java DefenderClient localhost 8080
# Ingresar usuario y contraseña (ej. bob / pass456)
```

---

## Usuarios de prueba (Identity Server)

| Usuario | Contraseña | Rol       |
|---------|-----------|-----------|
| alice   | pass123   | ATTACKER  |
| bob     | pass456   | DEFENDER  |
| charlie | pass789   | ATTACKER  |
| diana   | pass000   | DEFENDER  |
| eve     | eve2026   | ATTACKER  |
| frank   | frank26   | DEFENDER  |

> **Nota:** Las contraseñas no pueden contener espacios (protocolo delimitado por espacios).

---

## Flujo de prueba rápida con telnet

```bash
# Terminal 1 — identity server
python3 identity_server.py 8081

# Terminal 2 — game server
./server 8080 test.log

# Terminal 3 — cliente atacante manual
telnet localhost 8080
AUTH alice pass123
JOIN NEW
# (En terminal 4 conectar un defensor)
START
MOVE 1 0
SCAN
ATTACK DB-PRIMARY   # solo si estás en (5,3)

# Terminal 4 — cliente defensor manual
telnet localhost 8080
AUTH bob pass456
JOIN 1001
# (alice inicia con START)
MOVE -1 0   # moverse hacia el recurso
MITIGATE DB-PRIMARY
```

---

## Despliegue en GCP (Compute Engine)

1. Crear una VM en Compute Engine (Debian 12 o Ubuntu 22.04, e2-small o superior):
   ```bash
   gcloud compute instances create dcgp-server \
       --zone=us-central1-a \
       --machine-type=e2-small \
       --image-family=debian-12 \
       --image-project=debian-cloud \
       --tags=dcgp
   ```
2. Abrir los puertos del servicio en el firewall:
   ```bash
   gcloud compute firewall-rules create dcgp-allow \
       --allow=tcp:8080,tcp:8081,tcp:8082 \
       --target-tags=dcgp
   ```
3. Copiar los archivos a la VM (o clonar el repo directamente dentro):
   ```bash
   gcloud compute scp --recurse ./ dcgp-server:~/dcgp --zone=us-central1-a
   ```
4. Conectarse, instalar dependencias y compilar:
   ```bash
   gcloud compute ssh dcgp-server --zone=us-central1-a
   sudo apt update && sudo apt install -y gcc make python3-pip default-jdk
   pip3 install requests --break-system-packages
   cd ~/dcgp
   make
   ```
5. Levantar los tres servicios (en sesiones `tmux` o terminales separadas):
   ```bash
   python3 identity_server.py 8081 &
   ./server 8080 ~/dcgp.log
   python3 web_server.py 8082 &
   ```
6. En los clientes locales, configurar el hostname (no la IP). GCP no asigna DNS público por defecto, así que puedes usar `nip.io` para resolver por nombre la IP externa de la VM:
   ```bash
   export GAME_HOST=34-123-45-67.nip.io   # ejemplo: la IP externa con guiones
   export GAME_PORT=8080
   python3 client_attacker.py
   ```

---

## Arquitectura de red

```
Internet
  │
  ├── :8082  Web Server   (login + lista de salas)
  ├── :8081  Identity Server (autenticación, servicio externo)
  └── :8080  Game Server  (protocolo DCGP, C + Berkeley Sockets)
```

El Game Server consulta al Identity Server en cada autenticación vía HTTP.  
Los clientes Python y Java se conectan al puerto 8080 resolviendo el hostname vía DNS.

---

## Notas de diseño

- **TCP** elegido sobre UDP: los eventos de juego (ataques, mitigaciones) deben llegar sin pérdida.
- **Sin IPs hardcodeadas**: todos los hosts se resuelven con DNS en tiempo de ejecución.
- **Rol no declarado por el cliente**: el rol lo asigna el Identity Server, no el cliente.
- **strtok_r**: el servidor usa `strtok_r` (reentrant) para parseo seguro en múltiples hilos.
- **Locks correctos**: `broadcast_room` copia la lista de clientes bajo lock y envía fuera del lock para evitar deadlocks.
- **Logging**: todas las peticiones y respuestas se registran en consola y archivo con timestamp, IP y puerto del cliente.
