/*
 * DCGP - Datacenter Game Protocol Server
 * Servidor del juego multijugador - Internet: Arquitectura y Protocolos
 * EAFIT 2026-1
 *
 * Uso: ./server <puerto> <archivoDeLogs>
 * Vars de entorno opcionales: IDENTITY_HOST, IDENTITY_PORT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <signal.h>

/* ===================== CONSTANTES ===================== */
#define MAX_CLIENTS     64
#define MAX_ROOMS       16
#define BUF_SIZE        4096
#define MSG_SIZE        512
#define NAME_SIZE       64
#define MAP_WIDTH       40
#define MAP_HEIGHT      20
#define MAX_RESOURCES   4

/* Tipo de socket: SOCK_STREAM (TCP) — fiabilidad y orden garantizados */
#define SOCKET_TYPE     SOCK_STREAM

/* ===================== ESTRUCTURAS ===================== */

typedef enum {
    ROLE_NONE = 0,
    ROLE_ATTACKER,
    ROLE_DEFENDER
} PlayerRole;

typedef enum {
    STATE_CONNECTED = 0,
    STATE_AUTHENTICATED,
    STATE_IN_LOBBY,
    STATE_IN_GAME
} PlayerState;

typedef struct {
    int    x;
    int    y;
    int    attacked;
    int    mitigated;
    char   name[NAME_SIZE];
} Resource;

typedef struct {
    int          fd;
    int          id;
    char         username[NAME_SIZE];
    PlayerRole   role;
    PlayerState  state;
    int          x;
    int          y;
    int          room_id;
    pthread_t    thread;
    struct sockaddr_in addr;
} Client;

typedef struct {
    int       id;
    int       active;
    int       started;
    Client   *clients[MAX_CLIENTS];
    int       client_count;
    Resource  resources[MAX_RESOURCES];
    int       resource_count;
    pthread_mutex_t lock;
} Room;

/* ===================== GLOBALES ===================== */
static Client  g_clients[MAX_CLIENTS];
static Room    g_rooms[MAX_ROOMS];
static int     g_client_count  = 0;
static FILE   *g_log_file      = NULL;
static int     g_next_client_id = 1;
static int     g_next_room_id   = 1000;

/* Identidad */
static char g_identity_host[256] = "localhost";
static int  g_identity_port      = 8081;

static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rooms_lock   = PTHREAD_MUTEX_INITIALIZER;

/* ===================== LOGGING ===================== */
static void log_event(const char *client_ip, int client_port,
                      const char *direction, const char *msg)
{
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    pthread_mutex_lock(&g_log_lock);
    printf("[%s] [%s:%d] [%s] %s\n",
           ts, client_ip ? client_ip : "SERVER", client_port, direction, msg);
    fflush(stdout);
    if (g_log_file) {
        fprintf(g_log_file, "[%s] [%s:%d] [%s] %s\n",
                ts, client_ip ? client_ip : "SERVER", client_port, direction, msg);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_lock);
}

/* ===================== UTILIDADES ===================== */
static void send_to_client(Client *c, const char *msg)
{
    if (!c || c->fd < 0) return;
    char buf[BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", msg);
    if (send(c->fd, buf, n, 0) < 0) {
        perror("send");
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &c->addr.sin_addr, ip, sizeof(ip));
    log_event(ip, ntohs(c->addr.sin_port), "SEND", msg);
}

/*
 * broadcast_room — copia lista de clientes bajo lock, luego envía fuera del lock.
 * Evita deadlocks y condiciones de carrera con la lista de clientes.
 */
static void broadcast_room(Room *room, const char *msg, int exclude_id)
{
    Client *snap[MAX_CLIENTS];
    int cnt = 0;

    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->client_count; i++) {
        Client *cl = room->clients[i];
        if (cl && cl->fd >= 0 && cl->id != exclude_id)
            snap[cnt++] = cl;
    }
    pthread_mutex_unlock(&room->lock);

    for (int i = 0; i < cnt; i++)
        send_to_client(snap[i], msg);
}

/*
 * find_or_create_room — thread-safe con g_rooms_lock global.
 * room_id == -1  →  crea sala nueva.
 */
static Room *find_or_create_room(int room_id)
{
    Room *result = NULL;
    pthread_mutex_lock(&g_rooms_lock);

    for (int i = 0; i < MAX_ROOMS; i++) {
        if (g_rooms[i].active && g_rooms[i].id == room_id) {
            result = &g_rooms[i];
            goto done;
        }
    }
    if (room_id == -1) {
        for (int i = 0; i < MAX_ROOMS; i++) {
            if (!g_rooms[i].active) {
                Room *r = &g_rooms[i];
                memset(r, 0, sizeof(Room));
                r->active       = 1;
                r->started      = 0;
                r->client_count = 0;
                r->resource_count = 0;
                r->id = g_next_room_id++;
                pthread_mutex_init(&r->lock, NULL);

                /* Recursos críticos fijos */
                int rx[] = {5, 15, 30, 35};
                int ry[] = {3, 15,  8, 17};
                const char *rnames[] = {"DB-PRIMARY","WEB-SERVER","AUTH-SVC","BACKUP-SRV"};
                for (int k = 0; k < MAX_RESOURCES; k++) {
                    r->resources[k].x        = rx[k];
                    r->resources[k].y        = ry[k];
                    r->resources[k].attacked  = 0;
                    r->resources[k].mitigated = 0;
                    strncpy(r->resources[k].name, rnames[k], NAME_SIZE - 1);
                }
                r->resource_count = MAX_RESOURCES;
                result = r;
                goto done;
            }
        }
    }
done:
    pthread_mutex_unlock(&g_rooms_lock);
    return result;
}

/* ===================== VERIFICACIÓN CON IDENTITY SERVER ===================== */
/*
 * Realiza una petición HTTP POST /auth al identity server.
 * Devuelve 1 si las credenciales son válidas y escribe el rol en role_out.
 * Usa resolución DNS (getaddrinfo), sin IPs hardcodeadas.
 */
static int verify_identity(const char *username, const char *password,
                            char *role_out, int role_out_size)
{
    /* Sanidad: no permitir comillas ni barras en username/password */
    for (const char *p = username; *p; p++)
        if (*p == '"' || *p == '\\') return 0;
    for (const char *p = password; *p; p++)
        if (*p == '"' || *p == '\\') return 0;

    /* Resolución DNS del identity server */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", g_identity_port);

    int gai = getaddrinfo(g_identity_host, port_str, &hints, &res);
    if (gai != 0) {
        char err[MSG_SIZE];
        snprintf(err, sizeof(err), "Identity DNS failed for '%s': %s",
                 g_identity_host, gai_strerror(gai));
        log_event(NULL, 0, "ERROR", err);
        return 0;
    }

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return 0; }

    /* Timeout de 5 s para evitar bloqueos largos */
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(fd);
        log_event(NULL, 0, "ERROR", "Cannot connect to identity server");
        return 0;
    }
    freeaddrinfo(res);

    /* Construir cuerpo JSON y petición HTTP/1.0 */
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"username\":\"%s\",\"password\":\"%s\"}", username, password);

    char request[1024];
    snprintf(request, sizeof(request),
        "POST /auth HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        g_identity_host, g_identity_port, body_len, body);

    if (send(fd, request, strlen(request), 0) < 0) {
        close(fd);
        return 0;
    }

    /* Leer respuesta completa */
    char response[4096] = {0};
    int total = 0, n;
    while (total < (int)sizeof(response) - 1 &&
           (n = recv(fd, response + total,
                     sizeof(response) - total - 1, 0)) > 0)
        total += n;
    close(fd);
    response[total] = '\0';

    /* Verificar 200 OK */
    if (!strstr(response, "200 OK") && !strstr(response, "200 ok"))
        return 0;

    /* Buscar cuerpo JSON (tras la línea vacía \r\n\r\n) */
    char *json = strstr(response, "\r\n\r\n");
    if (!json) return 0;
    json += 4;

    /* Parsear "role":"VALUE" de forma simple */
    char *rkey = strstr(json, "\"role\"");
    if (!rkey) return 0;
    char *colon = strchr(rkey, ':');
    if (!colon) return 0;
    char *q1 = strchr(colon, '"');
    if (!q1) return 0;
    q1++;
    char *q2 = strchr(q1, '"');
    if (!q2) return 0;

    int len = (int)(q2 - q1);
    if (len <= 0 || len >= role_out_size) return 0;
    strncpy(role_out, q1, len);
    role_out[len] = '\0';
    return 1;
}

/* ===================== PARSEO DE COMANDOS ===================== */
/*
 * FIX: usa strtok_r (reentrant) para evitar corrupción del estado global
 * de strtok cuando se llama desde client_thread que también usa strtok_r.
 */
static int parse_message(char *raw, char *cmd,
                         char params[][MSG_SIZE], int max_params)
{
    /* Eliminar \r\n */
    char *end = raw + strlen(raw) - 1;
    while (end >= raw && (*end == '\r' || *end == '\n')) *end-- = '\0';
    if (strlen(raw) == 0) return -1;

    char buf[BUF_SIZE];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, " ", &saveptr);
    if (!token) return -1;
    strncpy(cmd, token, MSG_SIZE - 1);
    cmd[MSG_SIZE - 1] = '\0';

    int count = 0;
    while ((token = strtok_r(NULL, " ", &saveptr)) && count < max_params) {
        strncpy(params[count], token, MSG_SIZE - 1);
        params[count][MSG_SIZE - 1] = '\0';
        count++;
    }
    return count;
}

/* ===================== LÓGICA DEL PROTOCOLO ===================== */

/*
 * AUTH <username> <password>
 * El rol lo determina el identity server; el cliente no lo declara.
 */
static void handle_auth(Client *c, char params[][MSG_SIZE], int nparams)
{
    if (nparams < 2) {
        send_to_client(c, "ERR 400 MISSING_PARAMS");
        return;
    }
    char role[NAME_SIZE] = {0};
    if (!verify_identity(params[0], params[1], role, sizeof(role))) {
        send_to_client(c, "ERR 401 INVALID_CREDENTIALS");
        return;
    }
    strncpy(c->username, params[0], NAME_SIZE - 1);
    if (strcmp(role, "ATTACKER") == 0) {
        c->role = ROLE_ATTACKER;
    } else if (strcmp(role, "DEFENDER") == 0) {
        c->role = ROLE_DEFENDER;
    } else {
        send_to_client(c, "ERR 401 INVALID_ROLE");
        return;
    }
    c->state = STATE_AUTHENTICATED;
    char resp[MSG_SIZE];
    snprintf(resp, sizeof(resp), "OK AUTH %s %s", c->username, role);
    send_to_client(c, resp);
}

/* LIST */
static void handle_list(Client *c)
{
    char resp[BUF_SIZE];
    int  found = 0;
    strncpy(resp, "ROOMS", sizeof(resp) - 1);

    pthread_mutex_lock(&g_rooms_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (g_rooms[i].active && !g_rooms[i].started) {
            char entry[128];
            snprintf(entry, sizeof(entry), " %d:%d",
                     g_rooms[i].id, g_rooms[i].client_count);
            strncat(resp, entry, sizeof(resp) - strlen(resp) - 1);
            found++;
        }
    }
    pthread_mutex_unlock(&g_rooms_lock);

    if (!found) strncat(resp, " EMPTY", sizeof(resp) - strlen(resp) - 1);
    send_to_client(c, resp);
}

/* JOIN <room_id|NEW> */
static void handle_join(Client *c, char params[][MSG_SIZE], int nparams)
{
    if (c->state < STATE_AUTHENTICATED) {
        send_to_client(c, "ERR 403 NOT_AUTHENTICATED");
        return;
    }
    Room *room = NULL;
    if (nparams >= 1 && strcmp(params[0], "NEW") == 0) {
        room = find_or_create_room(-1);
        if (!room) { send_to_client(c, "ERR 503 NO_ROOM_AVAILABLE"); return; }
    } else if (nparams >= 1) {
        int rid = atoi(params[0]);
        room = find_or_create_room(rid);
        if (!room) { send_to_client(c, "ERR 404 ROOM_NOT_FOUND"); return; }
    } else {
        send_to_client(c, "ERR 400 MISSING_PARAMS");
        return;
    }

    pthread_mutex_lock(&room->lock);
    if (room->client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&room->lock);
        send_to_client(c, "ERR 503 ROOM_FULL");
        return;
    }
    c->x = (c->role == ROLE_ATTACKER) ? 0 : MAP_WIDTH - 1;
    c->y = MAP_HEIGHT / 2;
    c->room_id = room->id;
    c->state = STATE_IN_LOBBY;
    room->clients[room->client_count++] = c;
    pthread_mutex_unlock(&room->lock);

    char resp[MSG_SIZE];
    snprintf(resp, sizeof(resp), "OK JOIN %d %d %d %d %d",
             room->id, c->x, c->y, MAP_WIDTH, MAP_HEIGHT);
    send_to_client(c, resp);

    char notif[MSG_SIZE];
    snprintf(notif, sizeof(notif), "EVENT PLAYER_JOINED %s %s",
             c->username,
             c->role == ROLE_ATTACKER ? "ATTACKER" : "DEFENDER");
    broadcast_room(room, notif, c->id);
}

/* START */
static void handle_start(Client *c)
{
    Room *room = NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (g_rooms[i].active && g_rooms[i].id == c->room_id) {
            room = &g_rooms[i]; break;
        }
    }
    if (!room) { send_to_client(c, "ERR 404 ROOM_NOT_FOUND"); return; }

    pthread_mutex_lock(&room->lock);

    /* FIX: verificar started DENTRO del lock para evitar TOCTOU */
    if (room->started) {
        pthread_mutex_unlock(&room->lock);
        send_to_client(c, "ERR 409 ALREADY_STARTED");
        return;
    }

    int att = 0, def = 0;
    for (int i = 0; i < room->client_count; i++) {
        if (room->clients[i]->role == ROLE_ATTACKER) att++;
        if (room->clients[i]->role == ROLE_DEFENDER) def++;
    }
    if (att < 1 || def < 1) {
        pthread_mutex_unlock(&room->lock);
        send_to_client(c, "ERR 428 NEED_ATTACKER_AND_DEFENDER");
        return;
    }

    room->started = 1;
    for (int i = 0; i < room->client_count; i++)
        room->clients[i]->state = STATE_IN_GAME;

    /* Construir mensajes */
    char def_msg[BUF_SIZE];
    snprintf(def_msg, sizeof(def_msg), "EVENT GAME_STARTED RESOURCES");
    for (int r = 0; r < room->resource_count; r++) {
        char entry[128];
        snprintf(entry, sizeof(entry), " %s:%d:%d",
                 room->resources[r].name,
                 room->resources[r].x, room->resources[r].y);
        strncat(def_msg, entry, sizeof(def_msg) - strlen(def_msg) - 1);
    }
    const char *att_msg = "EVENT GAME_STARTED EXPLORE";

    /* FIX: copiar listas bajo lock, enviar fuera del lock */
    Client *attackers[MAX_CLIENTS], *defenders[MAX_CLIENTS];
    int na = 0, nd = 0;
    for (int i = 0; i < room->client_count; i++) {
        if (room->clients[i]->role == ROLE_DEFENDER)
            defenders[nd++] = room->clients[i];
        else
            attackers[na++] = room->clients[i];
    }
    pthread_mutex_unlock(&room->lock);

    for (int i = 0; i < nd; i++) send_to_client(defenders[i], def_msg);
    for (int i = 0; i < na; i++) send_to_client(attackers[i], att_msg);
}

/* MOVE <dx> <dy> */
static void handle_move(Client *c, char params[][MSG_SIZE], int nparams)
{
    if (c->state != STATE_IN_GAME) {
        send_to_client(c, "ERR 403 NOT_IN_GAME"); return;
    }
    if (nparams < 2) {
        send_to_client(c, "ERR 400 MISSING_PARAMS"); return;
    }

    int dx = atoi(params[0]);
    int dy = atoi(params[1]);
    if (dx >  1) dx =  1; else if (dx < -1) dx = -1;
    if (dy >  1) dy =  1; else if (dy < -1) dy = -1;

    int nx = c->x + dx;
    int ny = c->y + dy;
    if (nx < 0) nx = 0;
    if (nx >= MAP_WIDTH)  nx = MAP_WIDTH  - 1;
    if (ny < 0) ny = 0;
    if (ny >= MAP_HEIGHT) ny = MAP_HEIGHT - 1;
    c->x = nx; c->y = ny;

    char resp[MSG_SIZE];
    snprintf(resp, sizeof(resp), "OK MOVE %d %d", c->x, c->y);
    send_to_client(c, resp);

    Room *room = NULL;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (g_rooms[i].id == c->room_id) { room = &g_rooms[i]; break; }

    if (room) {
        char notif[MSG_SIZE];
        snprintf(notif, sizeof(notif), "EVENT PLAYER_MOVED %s %d %d",
                 c->username, c->x, c->y);
        broadcast_room(room, notif, c->id);

        /* Verificar colisión con recurso (solo atacantes) */
        if (c->role == ROLE_ATTACKER) {
            pthread_mutex_lock(&room->lock);
            for (int r = 0; r < room->resource_count; r++) {
                Resource *res = &room->resources[r];
                if (!res->attacked &&
                    res->x == c->x && res->y == c->y) {
                    char found[MSG_SIZE];
                    snprintf(found, sizeof(found),
                             "EVENT RESOURCE_FOUND %s %d %d",
                             res->name, res->x, res->y);
                    pthread_mutex_unlock(&room->lock);
                    send_to_client(c, found);
                    return;
                }
            }
            pthread_mutex_unlock(&room->lock);
        }
    }
}

/* ATTACK <resource_name> */
static void handle_attack(Client *c, char params[][MSG_SIZE], int nparams)
{
    if (c->role != ROLE_ATTACKER) {
        send_to_client(c, "ERR 403 NOT_ATTACKER"); return;
    }
    if (c->state != STATE_IN_GAME) {
        send_to_client(c, "ERR 403 NOT_IN_GAME"); return;
    }
    if (nparams < 1) {
        send_to_client(c, "ERR 400 MISSING_PARAMS"); return;
    }

    Room *room = NULL;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (g_rooms[i].id == c->room_id) { room = &g_rooms[i]; break; }
    if (!room) { send_to_client(c, "ERR 404 ROOM_NOT_FOUND"); return; }

    pthread_mutex_lock(&room->lock);
    for (int r = 0; r < room->resource_count; r++) {
        Resource *res = &room->resources[r];
        if (strcmp(res->name, params[0]) == 0) {
            if (res->x != c->x || res->y != c->y) {
                pthread_mutex_unlock(&room->lock);
                send_to_client(c, "ERR 409 NOT_AT_RESOURCE");
                return;
            }
            if (res->attacked) {
                pthread_mutex_unlock(&room->lock);
                send_to_client(c, "ERR 409 ALREADY_ATTACKED");
                return;
            }
            res->attacked = 1;

            char alert[MSG_SIZE];
            snprintf(alert, sizeof(alert),
                     "EVENT ATTACK_ALERT %s %d %d BY %s",
                     res->name, res->x, res->y, c->username);

            /* FIX: copiar defensores bajo lock, enviar fuera */
            Client *defs[MAX_CLIENTS];
            int nd = 0;
            for (int i = 0; i < room->client_count; i++) {
                if (room->clients[i]->role == ROLE_DEFENDER)
                    defs[nd++] = room->clients[i];
            }
            pthread_mutex_unlock(&room->lock);

            send_to_client(c, "OK ATTACK");
            for (int i = 0; i < nd; i++)
                send_to_client(defs[i], alert);
            return;
        }
    }
    pthread_mutex_unlock(&room->lock);
    send_to_client(c, "ERR 404 RESOURCE_NOT_FOUND");
}

/* MITIGATE <resource_name> */
static void handle_mitigate(Client *c, char params[][MSG_SIZE], int nparams)
{
    if (c->role != ROLE_DEFENDER) {
        send_to_client(c, "ERR 403 NOT_DEFENDER"); return;
    }
    if (c->state != STATE_IN_GAME) {
        send_to_client(c, "ERR 403 NOT_IN_GAME"); return;
    }
    if (nparams < 1) {
        send_to_client(c, "ERR 400 MISSING_PARAMS"); return;
    }

    Room *room = NULL;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (g_rooms[i].id == c->room_id) { room = &g_rooms[i]; break; }
    if (!room) { send_to_client(c, "ERR 404 ROOM_NOT_FOUND"); return; }

    pthread_mutex_lock(&room->lock);
    for (int r = 0; r < room->resource_count; r++) {
        Resource *res = &room->resources[r];
        if (strcmp(res->name, params[0]) == 0) {
            if (res->x != c->x || res->y != c->y) {
                pthread_mutex_unlock(&room->lock);
                send_to_client(c, "ERR 409 NOT_AT_RESOURCE");
                return;
            }
            if (!res->attacked) {
                pthread_mutex_unlock(&room->lock);
                send_to_client(c, "ERR 409 NOT_ATTACKED");
                return;
            }
            /* Resetear estado del recurso: tras mitigar vuelve a ser atacable */
            res->attacked  = 0;
            res->mitigated = 0;

            char notif[MSG_SIZE];
            snprintf(notif, sizeof(notif),
                     "EVENT MITIGATED %s BY %s", res->name, c->username);

            /* Copiar todos los clientes para broadcast */
            Client *all[MAX_CLIENTS];
            int cnt = 0;
            for (int i = 0; i < room->client_count; i++)
                all[cnt++] = room->clients[i];
            pthread_mutex_unlock(&room->lock);

            send_to_client(c, "OK MITIGATE");
            for (int i = 0; i < cnt; i++)
                if (all[i]->id != c->id)
                    send_to_client(all[i], notif);
            return;
        }
    }
    pthread_mutex_unlock(&room->lock);
    send_to_client(c, "ERR 404 RESOURCE_NOT_FOUND");
}

/* SCAN */
static void handle_scan(Client *c)
{
    if (c->role != ROLE_ATTACKER) {
        send_to_client(c, "ERR 403 NOT_ATTACKER"); return;
    }
    if (c->state != STATE_IN_GAME) {
        send_to_client(c, "ERR 403 NOT_IN_GAME"); return;
    }

    Room *room = NULL;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (g_rooms[i].id == c->room_id) { room = &g_rooms[i]; break; }
    if (!room) { send_to_client(c, "ERR 404 ROOM_NOT_FOUND"); return; }

    char resp[BUF_SIZE];
    strncpy(resp, "SCAN_RESULT", sizeof(resp) - 1);

    pthread_mutex_lock(&room->lock);
    for (int r = 0; r < room->resource_count; r++) {
        Resource *res = &room->resources[r];
        int dist = abs(res->x - c->x) + abs(res->y - c->y);
        if (dist <= 5) {
            char hint[64];
            snprintf(hint, sizeof(hint), " %s:%d", res->name, dist);
            strncat(resp, hint, sizeof(resp) - strlen(resp) - 1);
        }
    }
    pthread_mutex_unlock(&room->lock);
    send_to_client(c, resp);
}

/* STATUS */
static void handle_status(Client *c)
{
    char resp[BUF_SIZE];
    snprintf(resp, sizeof(resp),
             "STATUS USER %s ROLE %s POS %d %d ROOM %d STATE %s",
             c->username,
             c->role == ROLE_ATTACKER ? "ATTACKER" : "DEFENDER",
             c->x, c->y, c->room_id,
             c->state == STATE_IN_GAME  ? "IN_GAME" :
             c->state == STATE_IN_LOBBY ? "LOBBY"   : "AUTH");
    send_to_client(c, resp);

    for (int i = 0; i < MAX_ROOMS; i++) {
        if (g_rooms[i].id == c->room_id) {
            char players[BUF_SIZE] = "PLAYERS";
            pthread_mutex_lock(&g_rooms[i].lock);
            for (int j = 0; j < g_rooms[i].client_count; j++) {
                char p[128];
                snprintf(p, sizeof(p), " %s:%s:%d:%d",
                         g_rooms[i].clients[j]->username,
                         g_rooms[i].clients[j]->role == ROLE_ATTACKER ? "ATT" : "DEF",
                         g_rooms[i].clients[j]->x,
                         g_rooms[i].clients[j]->y);
                strncat(players, p, sizeof(players) - strlen(players) - 1);
            }
            pthread_mutex_unlock(&g_rooms[i].lock);
            send_to_client(c, players);
            break;
        }
    }
}

/* QUIT */
static void handle_quit(Client *c) { send_to_client(c, "OK BYE"); }

/* ===================== HILO POR CLIENTE ===================== */
static void remove_client_from_room(Client *c)
{
    for (int i = 0; i < MAX_ROOMS; i++) {
        Room *r = &g_rooms[i];
        if (!r->active || r->id != c->room_id) continue;
        pthread_mutex_lock(&r->lock);
        for (int j = 0; j < r->client_count; j++) {
            if (r->clients[j] == c) {
                r->clients[j] = r->clients[r->client_count - 1];
                r->client_count--;
                break;
            }
        }
        if (r->client_count == 0) r->active = 0;
        pthread_mutex_unlock(&r->lock);
        break;
    }
}

static void *client_thread(void *arg)
{
    Client *c = (Client *)arg;
    char buf[BUF_SIZE];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &c->addr.sin_addr, ip, sizeof(ip));
    int port = ntohs(c->addr.sin_port);

    char conn_msg[MSG_SIZE];
    snprintf(conn_msg, sizeof(conn_msg),
             "New connection from %s:%d (id=%d)", ip, port, c->id);
    log_event(ip, port, "CONNECT", conn_msg);
    send_to_client(c, "WELCOME DCGP/1.0 EAFIT-2026");

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(c->fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n == 0) log_event(ip, port, "DISCONNECT", "Client disconnected");
            else        log_event(ip, port, "ERROR", strerror(errno));
            break;
        }
        buf[n] = '\0';

        /*
         * FIX: strtok_r (reentrant) en el bucle externo.
         * Evita que parse_message (que también usa strtok_r internamente)
         * corrompa el estado del tokenizador del bucle exterior.
         */
        char *saveptr_outer = NULL;
        char *line = strtok_r(buf, "\n", &saveptr_outer);
        while (line) {
            /* Limpiar \r final */
            int llen = strlen(line);
            if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';
            if (strlen(line) == 0) {
                line = strtok_r(NULL, "\n", &saveptr_outer);
                continue;
            }

            log_event(ip, port, "RECV", line);

            char cmd[MSG_SIZE] = {0};
            char params[8][MSG_SIZE];
            memset(params, 0, sizeof(params));
            int np = parse_message(line, cmd, params, 8);
            if (np < 0) {
                line = strtok_r(NULL, "\n", &saveptr_outer);
                continue;
            }

            if      (strcmp(cmd, "AUTH")     == 0) handle_auth(c, params, np);
            else if (strcmp(cmd, "LIST")     == 0) handle_list(c);
            else if (strcmp(cmd, "JOIN")     == 0) handle_join(c, params, np);
            else if (strcmp(cmd, "START")    == 0) handle_start(c);
            else if (strcmp(cmd, "MOVE")     == 0) handle_move(c, params, np);
            else if (strcmp(cmd, "ATTACK")   == 0) handle_attack(c, params, np);
            else if (strcmp(cmd, "MITIGATE") == 0) handle_mitigate(c, params, np);
            else if (strcmp(cmd, "SCAN")     == 0) handle_scan(c);
            else if (strcmp(cmd, "STATUS")   == 0) handle_status(c);
            else if (strcmp(cmd, "QUIT")     == 0) {
                handle_quit(c);
                goto disconnect;
            } else {
                send_to_client(c, "ERR 400 UNKNOWN_COMMAND");
            }

            line = strtok_r(NULL, "\n", &saveptr_outer);
        }
    }

disconnect:
    remove_client_from_room(c);

    /* Notificar desconexión a la sala */
    if (c->room_id > 0) {
        char notif[MSG_SIZE];
        snprintf(notif, sizeof(notif), "EVENT PLAYER_LEFT %s", c->username);
        for (int i = 0; i < MAX_ROOMS; i++) {
            if (g_rooms[i].id == c->room_id) {
                broadcast_room(&g_rooms[i], notif, c->id);
                break;
            }
        }
    }

    close(c->fd);
    pthread_mutex_lock(&g_clients_lock);
    c->fd    = -1;
    c->state = STATE_CONNECTED;
    g_client_count--;
    pthread_mutex_unlock(&g_clients_lock);
    return NULL;
}

/* ===================== MAIN ===================== */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <puerto> <archivoDeLogs>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto inválido: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    g_log_file = fopen(argv[2], "a");
    if (!g_log_file) {
        fprintf(stderr, "No se pudo abrir archivo de logs: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    /* Leer configuración del identity server desde entorno (sin IPs hardcodeadas) */
    const char *id_host = getenv("IDENTITY_HOST");
    const char *id_port = getenv("IDENTITY_PORT");
    if (id_host) strncpy(g_identity_host, id_host, sizeof(g_identity_host) - 1);
    if (id_port) g_identity_port = atoi(id_port);

    signal(SIGPIPE, SIG_IGN);

    memset(g_clients, 0, sizeof(g_clients));
    memset(g_rooms,   0, sizeof(g_rooms));
    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    /* Crear socket TCP (Berkeley Sockets API) */
    int server_fd = socket(AF_INET, SOCKET_TYPE, 0);
    if (server_fd < 0) { perror("socket"); return EXIT_FAILURE; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); return EXIT_FAILURE;
    }
    if (listen(server_fd, 16) < 0) {
        perror("listen"); close(server_fd); return EXIT_FAILURE;
    }

    char startup[MSG_SIZE];
    snprintf(startup, sizeof(startup),
             "DCGP Server en puerto %d. Log: %s. Identity: %s:%d",
             port, argv[2], g_identity_host, g_identity_port);
    log_event(NULL, 0, "STARTUP", startup);

    /* Bucle principal de aceptación */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&g_clients_lock);
        if (g_client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_clients_lock);
            const char *msg = "ERR 503 SERVER_FULL\r\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
            continue;
        }

        Client *c = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd == -1) { c = &g_clients[i]; break; }
        }
        if (!c) {
            pthread_mutex_unlock(&g_clients_lock);
            close(client_fd);
            continue;
        }

        c->fd      = client_fd;
        c->id      = g_next_client_id++;
        c->state   = STATE_CONNECTED;
        c->role    = ROLE_NONE;
        c->room_id = -1;
        c->x = 0; c->y = 0;
        memset(c->username, 0, sizeof(c->username));
        memcpy(&c->addr, &client_addr, sizeof(client_addr));
        g_client_count++;
        pthread_mutex_unlock(&g_clients_lock);

        if (pthread_create(&c->thread, NULL, client_thread, c) != 0) {
            perror("pthread_create");
            close(c->fd);
            c->fd = -1;
            pthread_mutex_lock(&g_clients_lock);
            g_client_count--;
            pthread_mutex_unlock(&g_clients_lock);
        } else {
            pthread_detach(c->thread);
        }
    }

    fclose(g_log_file);
    close(server_fd);
    return EXIT_SUCCESS;
}
