# DCGP – Datacenter Game Protocol
## Internet: Arquitectura y Protocolos – EAFIT 2026-1

---

## Estructura del proyecto

```
dcgp/
├── server.c              # Servidor de juego (C, Berkeley Sockets TCP)
├── Makefile              # Compilación del servidor
├── identity_server.py    # Servicio de identidad (Python HTTP/JSON)
├── web_server.py         # Interfaz web (Python HTTP)
├── client_attacker.py    # Cliente Atacante (Python + pygame)
├── DefenderClient.java   # Cliente Defensor (Java + Swing)
└── RFC_DCGP.md           # Especificación completa del protocolo
```

---

## Arquitectura general

```
                         INTERNET / LAN

  ─────────────────  HTTP/JSON  ─────────────────────
  │  Web Browser  │────────────▶│  Web Server        │
  │  (Login/UI)   │◀────────────│  (Python :8082)    │
  ─────────────────             ─────────┬───────────
                                         │ HTTP/JSON
                                         ▼
                                ─────────────────────
                                │  Identity Server   │
                                │  (Python :8081)    │
                                ─────────────────────
                                         ▲
                                         │ HTTP POST /auth
  ─────────────────  DCGP/TCP  ──────────┴──────────
  │ Atacante      │───────────▶│                    │
  │ (Python)      │◀───────────│  Game Server       │
  ─────────────────            │  (C :8080)         │
  ─────────────────  DCGP/TCP  │                    │
  │ Defensor      │───────────▶│                    │
  │ (Java)        │◀───────────│                    │
  ─────────────────            ─────────────────────
```

El **Game Server** es el único componente que habla con el **Identity Server**; los clientes nunca lo contactan directamente. El rol (ATTACKER / DEFENDER) lo asigna el Identity Server, no el cliente.

---

## Flujo de conexión y autenticación

```
Cliente                  Game Server              Identity Server
  │                          │                          │
  │──── TCP connect :8080 ──▶│                          │
  │                          │                          │
  │──── AUTH alice pass123 ─▶│                          │
  │                          │──── POST /auth ─────────▶│
  │                          │     {"username":"alice",  │
  │                          │      "password":"pass123"}│
  │                          │◀─── {"role":"ATTACKER"} ──│
  │◀─── OK AUTH alice ATTACKER│                          │
  │                          │                          │
  │──── JOIN NEW ───────────▶│  (crea sala 1001)        │
  │◀─── OK JOIN 1001 0 10 40 20                         │
  │     (sala, x, y, ancho, alto)                       │
  │                          │                          │
  │──── START ──────────────▶│  (requiere ≥1 atacante   │
  │                          │   + ≥1 defensor en sala) │
  │◀─── EVENT GAME_STARTED EXPLORE          (atacante)  │
  │  o  EVENT GAME_STARTED RESOURCES DB-PRIMARY:5:3 ... (defensor)
```

### Posiciones iniciales en el mapa (40×20)

| Rol      | Posición inicial |
|----------|-----------------|
| ATTACKER | (0, 10)         |
| DEFENDER | (39, 10)        |

---

## Referencia de mensajes

### Comandos del cliente → servidor

| Comando                  | Descripción                                        | Rol requerido |
|--------------------------|----------------------------------------------------|---------------|
| `AUTH <user> <pass>`     | Autenticarse. El servidor valida con el Identity Server | Cualquiera |
| `JOIN <NEW\|room_id>`    | Unirse a sala nueva o existente                    | Autenticado   |
| `START`                  | Iniciar partida (necesita ≥1 de cada rol en sala)  | Cualquiera    |
| `MOVE <dx> <dy>`         | Moverse ±1 en X o Y                                | Cualquiera    |
| `SCAN`                   | Detectar recursos cercanos (radio ~3 celdas)       | ATTACKER      |
| `ATTACK <recurso>`       | Atacar un recurso (debes estar en su posición)     | ATTACKER      |
| `MITIGATE <recurso>`     | Mitigar un recurso atacado (debes estar en su posición) | DEFENDER |

### Respuestas del servidor → cliente

| Respuesta                                        | Significado                                    |
|--------------------------------------------------|------------------------------------------------|
| `OK AUTH <user> <role>`                          | Autenticación exitosa; rol asignado            |
| `OK JOIN <room_id> <x> <y> <w> <h>`             | Unido a sala; posición y dimensiones del mapa  |
| `OK MOVE <x> <y>`                               | Movimiento aceptado; nueva posición            |
| `OK ATTACK`                                      | Ataque registrado; defensores notificados      |
| `OK MITIGATE`                                    | Mitigación exitosa; recurso reseteable         |
| `SCAN_RESULT <recurso:x:y> ...`                 | Recursos encontrados en rango de escaneo       |
| `ERR <código> <motivo>`                          | Error (ver códigos abajo)                      |

### Eventos broadcast (servidor → todos en sala)

| Evento                                           | Quién lo recibe    | Cuándo                                |
|--------------------------------------------------|--------------------|---------------------------------------|
| `EVENT GAME_STARTED EXPLORE`                     | ATTACKER           | Al iniciar partida                    |
| `EVENT GAME_STARTED RESOURCES <res:x:y> ...`     | DEFENDER           | Al iniciar partida (con coordenadas)  |
| `EVENT PLAYER_JOINED <user> <rol>`               | Todos en sala      | Cuando alguien se une                 |
| `EVENT PLAYER_MOVED <user> <x> <y>`              | Todos en sala      | Tras cada movimiento exitoso          |
| `EVENT PLAYER_LEFT <user>`                       | Todos en sala      | Cuando un cliente se desconecta       |
| `EVENT ATTACK_ALERT <res> <x> <y> BY <attacker>` | Solo DEFENDER(s)   | Cuando un recurso es atacado          |
| `EVENT MITIGATED <res> BY <defender>`            | Todos en sala      | Cuando un recurso es mitigado         |

### Recursos del juego

| Nombre       | Descripción                   |
|--------------|-------------------------------|
| DB-PRIMARY   | Base de datos principal       |
| WEB-SERVER   | Servidor web                  |
| AUTH-SVC     | Servicio de autenticación     |
| BACKUP-SRV   | Servidor de respaldo          |

Las posiciones de los recursos son **aleatorias cada partida** y solo el DEFENDER las conoce al inicio.

### Reglas clave de ATTACK y MITIGATE

- `ATTACK <recurso>`: el atacante **debe estar en la misma celda** que el recurso (`x == res.x && y == res.y`). Si el recurso ya está atacado → `ERR 409 ALREADY_ATTACKED`.
- `MITIGATE <recurso>`: el defensor **debe estar en la misma celda** que el recurso **y** el recurso debe estar en estado atacado. Tras la mitigación, el recurso vuelve a estado normal (puede ser atacado de nuevo).

### Códigos de error

| Código | Motivo                  |
|--------|-------------------------|
| 400    | Parámetros faltantes    |
| 401    | Credenciales inválidas  |
| 403    | Permiso denegado / rol incorrecto |
| 404    | Sala o recurso no encontrado |
| 409    | Conflicto de estado (ya atacado, no en posición, etc.) |
| 428    | Faltan jugadores para iniciar |

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
# Abrir http://34.58.226.96.nip.io:8082 en el navegador
```

### 4. Cliente Atacante (Python)
```bash
export GAME_HOST=34.58.226.96.nip.io
export GAME_PORT=8080
python3 client_attacker.py
# Ingresar usuario y contraseña cuando lo pida (ej. alice / pass123)
```

### 5. Cliente Defensor (Java)
```bash
javac DefenderClient.java
java DefenderClient 34.58.226.96.nip.io 8080
# Se abre ventana de login; ingresar credenciales (ej. bob / pass456)
# El cliente envía AUTH y JOIN NEW automáticamente al conectarse
```

> **Nota:** el cliente Java acepta el host también como variable de entorno `GAME_HOST` / `GAME_PORT` si no se pasan argumentos.

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

> Las contraseñas no pueden contener espacios (protocolo delimitado por espacios). Las contraseñas se almacenan como SHA-256 en el Identity Server.

---

## Flujo de prueba rápida con telnet

```bash
# Terminal 1 — identity server
python3 identity_server.py 8081

# Terminal 2 — game server
./server 8080 test.log

# Terminal 3 — cliente atacante manual
telnet 34.58.226.96.nip.io 8080
AUTH alice pass123
# Respuesta: OK AUTH alice ATTACKER
JOIN NEW
# Respuesta: OK JOIN 1001 0 10 40 20
# (conectar defensor en terminal 4 antes de START)

# Terminal 4 — cliente defensor manual
telnet 34.58.226.96.nip.io 8080
AUTH bob pass456
# Respuesta: OK AUTH bob DEFENDER
JOIN 1001
# Respuesta: OK JOIN 1001 39 10 40 20

# Volver a terminal 3 — iniciar partida
START
# Atacante recibe: EVENT GAME_STARTED EXPLORE
# Defensor recibe: EVENT GAME_STARTED RESOURCES DB-PRIMARY:5:3 WEB-SERVER:12:7 ...

MOVE 1 0       # avanzar hacia la derecha
SCAN           # buscar recursos cercanos
ATTACK DB-PRIMARY   # solo si estás en (5,3)

# Terminal 4 — defensor navega hasta el recurso atacado
MOVE -1 0
MITIGATE DB-PRIMARY   # solo si estás en (5,3) y DB-PRIMARY fue atacado
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
6. En los clientes locales, configurar el hostname vía `nip.io` (resuelve IP por DNS sin configuración extra). Para esta entrega la VM tiene IP `34.58.226.96`:
   ```bash
   export GAME_HOST=34.58.226.96.nip.io
   export GAME_PORT=8080
   python3 client_attacker.py
   ```
   Defensor Java:
   ```bash
   java DefenderClient 34.58.226.96.nip.io 8080
   ```
   Web (navegador):
   ```
   http://34.58.226.96.nip.io:8082
   ```
   En la propia VM, exportar el hostname del identity server antes de levantar el game server:
   ```bash
   export IDENTITY_HOST=34.58.226.96.nip.io
   export IDENTITY_PORT=8081
   ./server 8080 server.log
   ```

---

## Notas de diseño

- **TCP sobre UDP**: los eventos de juego (ataques, mitigaciones) deben llegar sin pérdida ni reordenamiento. Una acción de mitigación perdida generaría estado inconsistente.
- **Sin IPs hardcodeadas**: todos los hosts se resuelven con DNS en tiempo de ejecución (`getaddrinfo` en C, `InetAddress.getByName` en Java, `socket.gethostbyname` en Python).
- **Rol asignado por el servidor**: el cliente nunca declara su propio rol; lo determina el Identity Server vía `POST /auth`.
- **1 hilo por cliente**: el Game Server lanza un `pthread` por conexión aceptada. El acceso a salas y clientes está protegido con `pthread_mutex_t`.
- **`strtok_r`**: el servidor usa la versión reentrant para parseo seguro en contexto multihilo.
- **Broadcast sin deadlock**: `broadcast_room` copia la lista de clientes bajo lock y envía fuera del lock.
- **Logging**: todas las peticiones y respuestas se registran en consola y archivo con timestamp, IP y puerto del cliente.
- **Mitigación reseteable**: tras un `MITIGATE` exitoso el recurso vuelve a estado normal, permitiendo que sea atacado nuevamente en la misma partida.
