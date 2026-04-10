# RFC DCGP-1 – Datacenter Game Protocol
**Internet: Arquitectura y Protocolos – EAFIT 2026-1**

---

## Tabla de Contenidos
1. Resumen
2. Visión General del Protocolo
3. Arquitectura del Sistema
4. Especificación del Servicio
5. Formato de Mensajes
6. Reglas de Procedimiento
7. Máquina de Estados
8. Manejo de Excepciones
9. Ejemplos de Implementación
10. Justificación de Diseño

---

## 1. Resumen

DCGP (Datacenter Game Protocol) es un protocolo de capa de aplicación de tipo texto diseñado para coordinar interacciones en tiempo real entre múltiples clientes en un simulador de ciberseguridad multijugador. Dos roles participan: **ATTACKER** (explora el mapa y lanza ataques a recursos críticos) y **DEFENDER** (conoce la ubicación de los recursos y debe mitigar ataques).

El rol de cada usuario **no es declarado por el cliente**: lo determina exclusivamente el **Identity Server** externo, garantizando que no existen usuarios locales en el servidor de juego.

---

## 2. Visión General del Protocolo

| Atributo | Valor |
|---|---|
| Tipo | Cliente–Servidor |
| Transporte | **TCP** (SOCK_STREAM) |
| Codificación | Texto plano UTF-8 |
| Terminador de mensaje | `\r\n` |
| Puerto por defecto | 8080 |
| Concurrencia | 1 hilo por cliente en el servidor |

### Justificación del uso de TCP

Se eligió TCP (SOCK_STREAM) sobre UDP porque:

- Los **comandos de juego** (MOVE, ATTACK, MITIGATE) deben llegar sin pérdida ni reordenamiento. Una acción de mitigación perdida o llegada fuera de orden generaría un estado inconsistente.
- La **notificación de ataques** a defensores es crítica y no puede omitirse.
- El overhead de TCP es aceptable dado que solo se transmiten mensajes de control cortos.

---

## 3. Arquitectura del Sistema

```
──────────────────────────────────────────────────────────────────────
                         INTERNET / LAN

  ──────────────────  HTTP/JSON  ─────────────────────
  │  Web Browser   │────────────▶│  Web Server        │
  │  (Login/UI)    │◀────────────│  (Python :8082)    │
  ──────────────────             ─────────┬───────────
                                          │ HTTP/JSON
                                          ▼
                                 ─────────────────────
                                 │  Identity Server   │
                                 │  (Python :8081)    │
                                 ─────────────────────
                                          ▲
                                          │ HTTP POST /auth
  ──────────────────   DCGP/TCP  ─────────┴───────────
  │  Atacante      │────────────▶│                    │
  │  (Python)      │◀────────────│  Game Server       │
  ──────────────────             │  (C :8080)         │
  ──────────────────   DCGP/TCP  │                    │
  │  Defensor      │────────────▶│                    │
  │  (Java)        │◀────────────│                    │
  ──────────────────             ─────────────────────
──────────────────────────────────────────────────────────────────────
```

**Flujo de autenticación:**
1. El cliente envía `AUTH <username> <password>` al Game Server.
2. El Game Server realiza una petición `POST /auth` al Identity Server (HTTP/1.0, resolución DNS).
3. El Identity Server devuelve el rol del usuario (ATTACKER o DEFENDER).
4. El Game Server responde `OK AUTH <username> <role>` al cliente.

El sistema emplea **resolución de nombres DNS** en todos los componentes. Ningún servicio tiene direcciones IP codificadas; se utilizan nombres de host (variables de entorno `GAME_HOST`, `IDENTITY_HOST`, etc.).

---

## 4. Especificación del Servicio

### 4.1 Primitivas del servicio

| Comando | Origen | Descripción |
|---|---|---|
| `AUTH`     | Cliente  | Autenticar usuario (rol asignado por Identity Server) |
| `LIST`     | Cliente  | Listar partidas activas (no requiere AUTH) |
| `JOIN`     | Cliente  | Unirse a una sala existente o crear nueva |
| `START`    | Cliente  | Solicitar inicio de partida |
| `MOVE`     | Cliente  | Mover al jugador en el mapa |
| `SCAN`     | Atacante | Detectar recursos en radio de Manhattan 5 |
| `ATTACK`   | Atacante | Lanzar ataque a un recurso crítico |
| `MITIGATE` | Defensor | Mitigar un recurso bajo ataque |
| `STATUS`   | Cliente  | Consultar estado actual del jugador y sala |
| `QUIT`     | Cliente  | Cerrar sesión y desconectarse |

### 4.2 Eventos del servidor (push)

| Evento | Destinatarios | Descripción |
|---|---|---|
| `EVENT PLAYER_JOINED`  | Sala       | Un nuevo jugador se unió |
| `EVENT PLAYER_LEFT`    | Sala       | Un jugador se desconectó |
| `EVENT PLAYER_MOVED`   | Sala       | Un jugador cambió de posición |
| `EVENT GAME_STARTED`   | Sala       | La partida inició |
| `EVENT RESOURCE_FOUND` | Atacante   | El atacante pisó un recurso |
| `EVENT ATTACK_ALERT`   | Defensores | Un recurso fue atacado |
| `EVENT MITIGATED`      | Sala       | Un recurso fue mitigado |

---

## 5. Formato de Mensajes

### 5.1 Sintaxis general

```
MENSAJE ::= COMANDO [SP PARAM]* CRLF
COMANDO ::= 1*LETRA_MAYUSCULA
PARAM   ::= 1*CHAR_VISIBLE  (sin espacios, sin comillas dobles)
SP      ::= 0x20 (espacio)
CRLF    ::= 0x0D 0x0A (\r\n)
```

Los mensajes son texto plano. El terminador es `\r\n`.  
**Restricción:** Los parámetros no pueden contener espacios, `"` ni `\`.

### 5.2 Mensajes del cliente al servidor

```
AUTH <username> <password>
LIST
JOIN <room_id|NEW>
START
MOVE <dx> <dy>              dx,dy ∈ {-1, 0, 1}
SCAN
ATTACK <resource_name>
MITIGATE <resource_name>
STATUS
QUIT
```

> **Cambio respecto a versiones anteriores:** `AUTH` ya no incluye el rol.
> El rol lo determina el Identity Server a partir de las credenciales.

### 5.3 Respuestas del servidor (unicast)

```
WELCOME DCGP/1.0 EAFIT-2026

OK AUTH <username> <ATTACKER|DEFENDER>
OK JOIN <room_id> <x> <y> <map_w> <map_h>
OK MOVE <x> <y>
OK ATTACK
OK MITIGATE
OK BYE

ROOMS [<room_id>:<player_count>]*
SCAN_RESULT [<resource_name>:<manhattan_distance>]*
STATUS USER <username> ROLE <role> POS <x> <y> ROOM <room_id> STATE <state>
PLAYERS [<username>:<role>:<x>:<y>]*

ERR <code> <reason>
```

### 5.4 Códigos de error

| Código | Razón | Descripción |
|---|---|---|
| 400 | MISSING_PARAMS       | Faltan parámetros obligatorios |
| 400 | UNKNOWN_COMMAND      | Comando no reconocido |
| 400 | BAD_FORMAT           | Formato de mensaje incorrecto |
| 401 | INVALID_CREDENTIALS  | Credenciales inválidas (identity server) |
| 401 | INVALID_ROLE         | Rol retornado por identity server no reconocido |
| 403 | NOT_AUTHENTICATED    | Debe autenticarse primero |
| 403 | NOT_IN_GAME          | La partida no ha iniciado |
| 403 | NOT_ATTACKER         | Solo atacantes pueden ejecutar esto |
| 403 | NOT_DEFENDER         | Solo defensores pueden ejecutar esto |
| 404 | ROOM_NOT_FOUND       | Sala no encontrada |
| 404 | RESOURCE_NOT_FOUND   | Recurso no encontrado |
| 409 | ALREADY_STARTED      | La partida ya inició |
| 409 | ALREADY_ATTACKED     | El recurso ya fue atacado |
| 409 | NOT_AT_RESOURCE      | El jugador no está en la posición del recurso |
| 409 | NOT_ATTACKED         | El recurso no ha sido atacado aún |
| 428 | NEED_ATTACKER_AND_DEFENDER | Se requiere al menos 1 de cada rol |
| 503 | SERVER_FULL          | Sin capacidad para nuevos clientes |
| 503 | NO_ROOM_AVAILABLE    | Sin salas disponibles |

### 5.5 Eventos del servidor (broadcast/multicast)

```
EVENT PLAYER_JOINED <username> <ATTACKER|DEFENDER>
EVENT PLAYER_LEFT <username>
EVENT PLAYER_MOVED <username> <x> <y>
EVENT GAME_STARTED EXPLORE
EVENT GAME_STARTED RESOURCES <name:x:y> [<name:x:y>]*
EVENT RESOURCE_FOUND <resource_name> <x> <y>
EVENT ATTACK_ALERT <resource_name> <x> <y> BY <attacker_username>
EVENT MITIGATED <resource_name> BY <defender_username>
```

---

## 6. Reglas de Procedimiento

### 6.1 Flujo de conexión y autenticación

```
Cliente                         Servidor                   Identity Server
  |                                |                              |
  |──── TCP connect ───────────────▶|                              |
  |◀─── WELCOME DCGP/1.0 ──────────|                              |
  |                                |                              |
  |──── AUTH alice pass123 ────────▶|                              |
  |                                |──── POST /auth {alice,pass}──▶|
  |                                |◀─── 200 {"role":"ATTACKER"} ──|
  |◀─── OK AUTH alice ATTACKER ────|                              |
  |                                |                              |
  |──── LIST ──────────────────────▶|                              |
  |◀─── ROOMS 1001:2 ──────────────|                              |
  |                                |                              |
  |──── JOIN 1001 ─────────────────▶|                              |
  |◀─── OK JOIN 1001 0 10 40 20 ───|                              |
  |                    [broadcast a sala]                         |
  |◀─── EVENT PLAYER_JOINED alice ATTACKER  (otros clientes) ─────|
```

### 6.2 Inicio de partida

```
Cliente (cualquier)          Servidor
  |                             |
  |──── START ─────────────────▶|
  |     [valida: ≥1 ATT, ≥1 DEF, dentro del lock]
  |◀─── EVENT GAME_STARTED ─────|────────────────────▶ (toda la sala)
  |      (EXPLORE para att,      |  (RESOURCES para def)
  |       RESOURCES para def)   |
```

### 6.3 Movimiento y detección de recursos

```
Atacante                     Servidor
  |                             |
  |──── MOVE 1 0 ──────────────▶|  [valida límites]
  |◀─── OK MOVE 15 10 ──────────|
  |         [si hay recurso en (15,10)]
  |◀─── EVENT RESOURCE_FOUND DB-PRIMARY 15 10
  |──── ATTACK DB-PRIMARY ──────▶|
  |◀─── OK ATTACK ──────────────|
  |         [broadcast a defensores]
  |◀─── EVENT ATTACK_ALERT DB-PRIMARY 15 10 BY alice  (defensores)
```

### 6.4 Mitigación

```
Defensor                     Servidor
  |──── MOVE ... ──────────────▶|  [se mueve hasta (15,10)]
  |◀─── OK MOVE 15 10 ──────────|
  |──── MITIGATE DB-PRIMARY ────▶|  [verifica posición y estado]
  |◀─── OK MITIGATE ────────────|
  |         [broadcast a sala]
  |◀─── EVENT MITIGATED DB-PRIMARY BY bob  (toda la sala)
```

### 6.5 Desconexión

```
Cliente                      Servidor
  |──── QUIT ──────────────────▶|
  |◀─── OK BYE ─────────────────|
  |──── TCP close ──────────────▶|
  |         [broadcast a sala]
  |◀─── EVENT PLAYER_LEFT alice  (otros clientes)
```

---

## 7. Máquina de Estados del Cliente (vista del servidor)

```
          ─────────────────────────────────────────────────────────
          │                                                        │
          ▼                                                        │
    [CONNECTED]                                                    │
          │                                                        │
          │ AUTH user pass  →  OK AUTH (identity verifica)         │
          ▼                                                        │
   [AUTHENTICATED]                                                 │
          │                                                        │
          │ JOIN <room|NEW>  →  OK JOIN                            │
          ▼                                                        │
    [IN_LOBBY]                                                     │
          │                                                        │
          │ START (con ≥1 ATT y ≥1 DEF)  →  EVENT GAME_STARTED    │
          ▼                                                        │
    [IN_GAME]  ──── MOVE/SCAN/ATTACK/MITIGATE/STATUS ────▶ respuesta
          │                                                        │
          │ QUIT / desconexión TCP                                 │
          ─────────────────────────────────────────────────────────
```

---

## 8. Manejo de Excepciones

### 8.1 Conexión fallida
- `accept()` captura errores y continúa el bucle principal.
- Los clientes muestran el error y terminan limpiamente (excepción capturada).

### 8.2 Mensajes con formato incorrecto
- El servidor devuelve `ERR 400 BAD_FORMAT` o `ERR 400 MISSING_PARAMS` sin cerrar la conexión.
- Se registra en el archivo de log con IP y puerto del cliente.

### 8.3 Desconexión inesperada
- El hilo del cliente detecta `recv() == 0` o error en `recv()`.
- Se emite `EVENT PLAYER_LEFT` a los demás miembros de la sala.
- Si la sala queda vacía, se desactiva.

### 8.4 Comando fuera de estado
- `MOVE` antes de `START` → `ERR 403 NOT_IN_GAME`
- `ATTACK` por un DEFENDER → `ERR 403 NOT_ATTACKER`

### 8.5 Identity server no disponible
- El Game Server captura el fallo de conexión/DNS.
- Registra el error en el log y responde `ERR 401 INVALID_CREDENTIALS`.
- El servidor de juego **continúa operando** para otros clientes.

### 8.6 Resolución DNS fallida
- En el Game Server: `getaddrinfo` falla → se registra y se rechaza la autenticación.
- En los clientes Python/Java: se imprime el error y se termina limpiamente.
- En la web: se devuelve HTTP 503 al navegador.

---

## 9. Ejemplos de Implementación

### 9.1 Sesión completa (texto plano)

```
# Atacante se conecta
S→C: WELCOME DCGP/1.0 EAFIT-2026
C→S: AUTH alice pass123
      [Game Server llama POST /auth al identity server]
S→C: OK AUTH alice ATTACKER
C→S: JOIN NEW
S→C: OK JOIN 1001 0 10 40 20
C→S: START
S→C: ERR 428 NEED_ATTACKER_AND_DEFENDER

# Defensor se conecta en otra sesión
S→C: WELCOME DCGP/1.0 EAFIT-2026
C→S: AUTH bob pass456
S→C: OK AUTH bob DEFENDER
C→S: JOIN 1001
S→C: OK JOIN 1001 39 10 40 20

# Atacante inicia
C→S (alice): START
S→C (alice): EVENT GAME_STARTED EXPLORE
S→C (bob):   EVENT GAME_STARTED RESOURCES DB-PRIMARY:5:3 WEB-SERVER:15:15 AUTH-SVC:30:8 BACKUP-SRV:35:17

# Atacante explora
C→S (alice): MOVE 1 0
S→C (alice): OK MOVE 1 10
S→C (bob):   EVENT PLAYER_MOVED alice 1 10
C→S (alice): SCAN
S→C (alice): SCAN_RESULT  # nada a distancia ≤5 aún
...
# Alice llega a (5,3)
S→C (alice): OK MOVE 5 3
S→C (alice): EVENT RESOURCE_FOUND DB-PRIMARY 5 3
C→S (alice): ATTACK DB-PRIMARY
S→C (alice): OK ATTACK
S→C (bob):   EVENT ATTACK_ALERT DB-PRIMARY 5 3 BY alice

# Bob se desplaza y mitiga
C→S (bob): MOVE ...  # varios MOVE
S→C (bob): OK MOVE 5 3
C→S (bob): MITIGATE DB-PRIMARY
S→C (bob): OK MITIGATE
S→C (alice): EVENT MITIGATED DB-PRIMARY BY bob
S→C (bob):   EVENT MITIGATED DB-PRIMARY BY bob
```

### 9.2 Fragmento de código – cliente Python enviando AUTH

```python
import socket

host = "game.eafit.edu.co"  # nombre de dominio, nunca IP
port = 8080

s = socket.create_connection((host, port))
welcome = s.recv(256).decode()
# El rol lo determina el servidor consultando al identity server
s.sendall(b"AUTH alice pass123\r\n")
resp = s.recv(256).decode()
print(resp)  # OK AUTH alice ATTACKER
```

### 9.3 Diagrama de secuencia – ataque completo

```
Atacante       Game Server       Identity Server    Defensor
    |               |                  |                |
    |──AUTH─────────▶|                  |                |
    |               |──POST /auth──────▶|                |
    |               |◀─200 ATTACKER ───|                |
    |◀─OK AUTH──────|                  |                |
    |──JOIN NEW──────▶|                  |                |
    |◀─OK JOIN───────|                  |                |
    |               |                  |──AUTH──────────|
    |               |                  |◀─200 DEFENDER──|
    |               |◀─────────────────────────────AUTH─|
    |               |──OK AUTH──────────────────────────▶|
    |               |◀─────────────────────────JOIN 1001─|
    |               |──OK JOIN──────────────────────────▶|
    |──START─────────▶|                  |                |
    |◀─GAME_STARTED──|──GAME_STARTED─────────────────────▶|
    |  (EXPLORE)     |  (RESOURCES)      |                |
    |──MOVE …────────▶|                  |                |
    |◀─RESOURCE_FOUND|                  |                |
    |──ATTACK────────▶|                  |                |
    |◀─OK ATTACK─────|──ATTACK_ALERT─────────────────────▶|
    |               |◀─────────────────────────────MOVE──|
    |               |──OK MOVE──────────────────────────▶|
    |               |◀─────────────────────────MITIGATE──|
    |◀─MITIGATED ───|──OK MITIGATE──────────────────────▶|
```

---

## 10. Justificación de Diseño

### Protocolo de texto vs. binario
Se optó por **protocolo de texto** (requisito del proyecto) porque facilita la depuración con herramientas como `telnet` o `nc`, y elimina errores de alineación de bytes comunes en protocolos binarios.

### TCP vs. UDP
TCP garantiza entrega ordenada y confiable. Para este juego, donde perder un evento de ataque o mitigación rompe la consistencia del estado, la confiabilidad supera el costo de latencia. UDP sería apropiado si se transmitieran posiciones a alta frecuencia (ej. 60 Hz) donde algunos paquetes perdidos son tolerables.

### Concurrencia con hilos
Un hilo por cliente es simple de implementar y adecuado para la escala del proyecto (<64 clientes). Se usa `strtok_r` (reentrant) para parseo seguro en múltiples hilos. Para mayor escala se recomendaría I/O multiplexado (`select`/`epoll`).

### Separación de servicios
- **Identity Server** separado: cumple el requisito de no tener usuarios locales en el servidor de juego. El Game Server consulta al Identity Server vía HTTP cada vez que recibe un `AUTH`.
- **Web Server** separado: desacopla la presentación HTTP del protocolo DCGP.

### Sin IPs hardcodeadas
Todos los hosts se configuran vía variables de entorno (`GAME_HOST`, `IDENTITY_HOST`) y se resuelven con DNS en tiempo de ejecución usando `getaddrinfo` (C) y `gethostbyname` (Python/Java). Si la resolución falla, el error se captura y se registra sin terminar el proceso.

### Rol determinado por el servidor
El cliente **no declara su propio rol** en el comando `AUTH`. Esto evita que un cliente malintencionado asuma un rol incorrecto. El rol lo asigna exclusivamente el Identity Server en base a las credenciales.
