#!/usr/bin/env python3
"""
Cliente Atacante – DCGP
Lenguaje: Python + pygame
Rol: ATTACKER – explora el mapa, encuentra recursos y lanza ataques.

Dependencias: pip install pygame requests
Uso: python3 client_attacker.py
"""

import pygame
import socket
import threading
import queue
import sys
import os
import time

# ── Resolución DNS (sin IPs hardcodeadas) ──────────────────────────────────
def resolve(hostname: str) -> str:
    try:
        return socket.gethostbyname(hostname)
    except socket.gaierror as e:
        print(f"[DNS ERROR] No se pudo resolver '{hostname}': {e}")
        return None

GAME_HOST = os.environ.get("GAME_HOST", "localhost")
GAME_PORT = int(os.environ.get("GAME_PORT", "8080"))

# ── Colores y constantes de UI ──────────────────────────────────────────────
W, H         = 900, 620
MAP_COLS     = 40
MAP_ROWS     = 20
CELL         = 16
MAP_X_OFF    = 20
MAP_Y_OFF    = 80
PANEL_X      = MAP_X_OFF + MAP_COLS * CELL + 20

BLACK   = (8,   12,  16)
DARK    = (13,  21,  32)
BORDER  = (26,  58,  74)
ACCENT  = (0,   229, 255)
WARN    = (255, 68,  68)
OK      = (0,   255, 136)
TEXT    = (200, 216, 232)
DIM     = (60,  100, 120)
PLAYER  = (0,   229, 255)
ENEMY   = (255, 68,  68)
RESOURCE= (255, 200, 0)

# ── Estado del juego ─────────────────────────────────────────────────────────
class GameState:
    def __init__(self):
        self.x = 0; self.y = 0
        self.map_w = MAP_COLS; self.map_h = MAP_ROWS
        self.room_id    = -1
        self.username   = ""
        self.role       = ""           # asignado por el servidor
        self.connected  = False
        self.in_game    = False
        self.auth_error = ""           # error si el rol no es ATTACKER
        self.messages   = []
        self.players    = {}           # username -> (x, y, role)
        self.resources  = []
        self.found_resource = None

state = GameState()
recv_queue = queue.Queue()
sock = None

# ── Comunicación con el servidor ────────────────────────────────────────────
def recv_thread(s):
    buf = ""
    while True:
        try:
            data = s.recv(4096)
            if not data:
                recv_queue.put("__DISCONNECT__")
                break
            buf += data.decode(errors="ignore")
            while "\r\n" in buf or "\n" in buf:
                sep = "\r\n" if "\r\n" in buf else "\n"
                line, buf = buf.split(sep, 1)
                line = line.strip()
                if line:
                    recv_queue.put(line)
        except Exception as e:
            recv_queue.put(f"__ERROR__ {e}")
            break

def send_cmd(cmd: str):
    global sock
    if sock:
        try:
            sock.sendall((cmd + "\r\n").encode())
            state.messages.append(f"> {cmd}")
        except Exception:
            pass

# ── Procesamiento de mensajes ────────────────────────────────────────────────
def process_msg(msg: str):
    state.messages.append(f"< {msg}")
    if len(state.messages) > 40:
        state.messages = state.messages[-40:]

    parts = msg.split()
    if not parts:
        return

    if msg == "__DISCONNECT__":
        state.connected = False; state.in_game = False; return
    if msg.startswith("__ERROR__"):
        return

    # OK AUTH username role  ← rol asignado por el servidor vía identity server
    if parts[0] == "OK" and len(parts) >= 4 and parts[1] == "AUTH":
        state.username = parts[2]
        state.role     = parts[3]
        if state.role != "ATTACKER":
            state.auth_error = (
                f"Tu rol es '{state.role}'. "
                "Este cliente es solo para ATACANTES. Usa DefenderClient."
            )

    # ERR <code> <reason>
    elif parts[0] == "ERR":
        pass  # ya se muestra en el log

    # OK MOVE x y
    elif parts[0] == "OK" and len(parts) >= 4 and parts[1] == "MOVE":
        state.x = int(parts[2]); state.y = int(parts[3])

    # EVENT GAME_STARTED EXPLORE
    elif parts[0] == "EVENT" and parts[1] == "GAME_STARTED":
        state.in_game = True

    # EVENT PLAYER_MOVED name x y
    elif parts[0] == "EVENT" and parts[1] == "PLAYER_MOVED" and len(parts) >= 5:
        state.players[parts[2]] = (int(parts[3]), int(parts[4]), "?")

    # EVENT PLAYER_JOINED name role
    elif parts[0] == "EVENT" and parts[1] == "PLAYER_JOINED" and len(parts) >= 4:
        state.players[parts[2]] = (0, 0, parts[3])

    # EVENT PLAYER_LEFT name
    elif parts[0] == "EVENT" and parts[1] == "PLAYER_LEFT" and len(parts) >= 3:
        state.players.pop(parts[2], None)

    # EVENT RESOURCE_FOUND name x y
    elif parts[0] == "EVENT" and parts[1] == "RESOURCE_FOUND" and len(parts) >= 5:
        state.found_resource = parts[2]

    # OK JOIN room_id x y w h
    elif parts[0] == "OK" and len(parts) >= 7 and parts[1] == "JOIN":
        state.room_id = int(parts[2])
        state.x = int(parts[3]); state.y = int(parts[4])
        state.map_w = int(parts[5]); state.map_h = int(parts[6])

# ── Dibujo ────────────────────────────────────────────────────────────────────
def draw_map(screen, font_small):
    pygame.draw.rect(screen, DARK,
        (MAP_X_OFF-2, MAP_Y_OFF-2,
         MAP_COLS*CELL+4, MAP_ROWS*CELL+4), 1)
    for col in range(MAP_COLS):
        for row in range(MAP_ROWS):
            rx = MAP_X_OFF + col*CELL
            ry = MAP_Y_OFF + row*CELL
            pygame.draw.rect(screen, (10, 18, 26), (rx, ry, CELL-1, CELL-1))
    for uname, (px, py, role) in state.players.items():
        if uname == state.username: continue
        rx = MAP_X_OFF + px*CELL
        ry = MAP_Y_OFF + py*CELL
        color = ENEMY if role == "ATTACKER" else OK
        pygame.draw.rect(screen, color, (rx+2, ry+2, CELL-4, CELL-4))
    rx = MAP_X_OFF + state.x*CELL
    ry = MAP_Y_OFF + state.y*CELL
    pygame.draw.rect(screen, PLAYER, (rx+1, ry+1, CELL-2, CELL-2))
    label = font_small.render("A", True, BLACK)
    screen.blit(label, (rx+3, ry+2))

def draw_panel(screen, font, font_small):
    px = PANEL_X
    py = MAP_Y_OFF
    pw = W - PANEL_X - 10  # noqa: F841

    def txt(s, x, y, c=TEXT, f=None):
        screen.blit((f or font_small).render(s, True, c), (x, y))

    txt("ATACANTE", px, py, ACCENT, font)
    py += 28
    txt(f"Usuario: {state.username}", px, py); py += 18
    txt(f"Posición: ({state.x}, {state.y})", px, py); py += 18
    txt(f"Sala: {state.room_id}", px, py); py += 18
    txt(f"Estado: {'EN JUEGO' if state.in_game else 'LOBBY'}", px, py,
        OK if state.in_game else DIM); py += 28

    txt("─── CONTROLES ───", px, py, BORDER); py += 16
    txt("WASD / Flechas → mover", px, py, DIM); py += 14
    txt("F → SCAN (radar)", px, py, DIM); py += 14
    txt("SPACE → ATTACK recurso", px, py, DIM); py += 28

    if state.auth_error:
        for i, line in enumerate(state.auth_error.split(". ")):
            txt(line, px, py + i*14, WARN)
        py += 42

    if state.found_resource:
        txt("! RECURSO ENCONTRADO:", px, py, RESOURCE); py += 16
        txt(f"  {state.found_resource}", px, py, RESOURCE)
        txt("  [SPACE] para atacar", px, py+14, DIM); py += 36

    txt("─── LOG ───", px, py, BORDER); py += 16
    for line in state.messages[-12:]:
        color = WARN   if line.startswith("< ERR")   else \
                OK     if line.startswith("< OK")    else \
                ACCENT if line.startswith("< EVENT") else DIM
        if len(line) > 38: line = line[:36] + "…"
        screen.blit(font_small.render(line, True, color), (px, py))
        py += 13


# ── Login overlay ─────────────────────────────────────────────────────────────
class LoginScreen:
    def __init__(self):
        self.username = ""
        self.password = ""
        self.room_id  = ""        # vacío = NEW
        self.active   = "username"
        self.error    = ""
        self.done     = False

    def handle(self, event):
        if event.type == pygame.KEYDOWN:
            if event.key == pygame.K_TAB:
                order = ["username", "password", "room_id"]
                idx = order.index(self.active)
                self.active = order[(idx + 1) % len(order)]
            elif event.key == pygame.K_RETURN:
                order = ["username", "password", "room_id"]
                idx = order.index(self.active)
                if idx < len(order) - 1:
                    self.active = order[idx + 1]
                else:
                    if self.username and self.password:
                        self.done = True
                    else:
                        self.error = "Ingresa usuario y contraseña"
            elif event.key == pygame.K_BACKSPACE:
                if self.active == "username":   self.username = self.username[:-1]
                elif self.active == "password": self.password = self.password[:-1]
                else:                           self.room_id  = self.room_id[:-1]
            else:
                ch = event.unicode
                if ch.isprintable() and ' ' not in ch:
                    if self.active == "username":   self.username += ch
                    elif self.active == "password": self.password += ch
                    else:                           self.room_id  += ch

    def draw(self, screen, font, font_big):
        screen.fill(BLACK)
        W2, H2 = W//2, H//2
        t = font_big.render("DCGP – CLIENTE ATACANTE", True, ACCENT)
        screen.blit(t, (W2 - t.get_width()//2, 100))

        box_y = H2 - 80
        fields = [
            ("Usuario:",          "username", self.username),
            ("Contraseña:",       "password", self.password),
            ("Sala (vacío=NEW):", "room_id",  self.room_id),
        ]
        for label, key, field in fields:
            active = self.active == key
            col = ACCENT if active else BORDER
            screen.blit(font.render(label, True, TEXT), (W2-160, box_y)); box_y += 22
            pygame.draw.rect(screen, DARK, (W2-160, box_y, 320, 32))
            pygame.draw.rect(screen, col,  (W2-160, box_y, 320, 32), 1)
            val = field if key != "password" else "*" * len(field)
            screen.blit(font.render(val, True, TEXT), (W2-154, box_y+6))
            box_y += 48

        hint = font.render("TAB: sig. campo | ENTER: confirmar | Sin espacios", True, DIM)
        screen.blit(hint, (W2 - hint.get_width()//2, box_y+10))

        if self.error:
            e = font.render(self.error, True, WARN)
            screen.blit(e, (W2 - e.get_width()//2, box_y+40))

        pygame.display.flip()


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    global sock

    pygame.init()
    screen = pygame.display.set_mode((W, H))
    pygame.display.set_caption("DCGP – Atacante")
    clock = pygame.time.Clock()

    font_big   = pygame.font.SysFont("Courier New", 22, bold=True)
    font       = pygame.font.SysFont("Courier New", 14)
    font_small = pygame.font.SysFont("Courier New", 12)

    # ── Login ──
    login = LoginScreen()
    while not login.done:
        for event in pygame.event.get():
            if event.type == pygame.QUIT: pygame.quit(); sys.exit()
            login.handle(event)
        login.draw(screen, font, font_big)

    state.username = login.username
    password       = login.password

    # ── Conectar al servidor de juego (DNS, no IP) ──
    ip = resolve(GAME_HOST)
    if not ip:
        print(f"No se pudo resolver el host '{GAME_HOST}'.")
        sys.exit(1)

    try:
        sock = socket.create_connection((GAME_HOST, GAME_PORT), timeout=5)
        sock.settimeout(None)  # modo bloqueante tras conectar
    except Exception as e:
        print(f"No se pudo conectar: {e}")
        sys.exit(1)

    state.connected = True
    threading.Thread(target=recv_thread, args=(sock,), daemon=True).start()

    time.sleep(0.3)  # esperar WELCOME

    # Autenticar — el servidor verifica con el identity server y retorna el rol
    send_cmd(f"AUTH {state.username} {password}")
    time.sleep(0.3)

    # Si el rol es incorrecto se mostrará en pantalla; seguir esperando
    # Si la auth fue OK y el rol es ATTACKER, pedir sala al usuario
    if not state.auth_error:
        room_input = login.room_id.strip() if login.room_id.strip() else "NEW"
        send_cmd(f"JOIN {room_input}")
        time.sleep(0.2)

    # ── Game loop ──
    move_cooldown = 0
    running = True

    while running:
        clock.tick(30)

        while not recv_queue.empty():
            process_msg(recv_queue.get_nowait())

        # Si se detectó error de rol, unirse solo después de que el usuario lo vea
        if state.auth_error and state.room_id == -1:
            pass  # mostrar error en pantalla, no unirse

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_RETURN and not state.in_game and not state.auth_error:
                    send_cmd("START")
                elif event.key == pygame.K_f and state.in_game:
                    send_cmd("SCAN")
                elif event.key == pygame.K_SPACE and state.found_resource and state.in_game:
                    send_cmd(f"ATTACK {state.found_resource}")
                    state.found_resource = None

        if state.in_game and move_cooldown <= 0 and not state.auth_error:
            keys = pygame.key.get_pressed()
            dx = dy = 0
            if keys[pygame.K_LEFT]  or keys[pygame.K_a]: dx = -1
            if keys[pygame.K_RIGHT] or keys[pygame.K_d]: dx = 1
            if keys[pygame.K_UP]    or keys[pygame.K_w]: dy = -1
            if keys[pygame.K_DOWN]  or keys[pygame.K_s]: dy = 1
            if dx or dy:
                send_cmd(f"MOVE {dx} {dy}")
                move_cooldown = 8
        if move_cooldown > 0: move_cooldown -= 1

        # ── Dibujar ──
        screen.fill(BLACK)
        header = font_big.render(
            f"DCGP  ATACANTE  {'● EN JUEGO' if state.in_game else '○ LOBBY'}",
            True, ACCENT)
        screen.blit(header, (MAP_X_OFF, 20))

        if not state.in_game and not state.auth_error:
            hint = font.render("Esperando jugadores… [ENTER] para iniciar partida", True, DIM)
            screen.blit(hint, (MAP_X_OFF, 50))

        draw_map(screen, font_small)
        draw_panel(screen, font, font_small)

        pos_text = font_small.render(
            f"X:{state.x:02d}  Y:{state.y:02d}  |  Sala #{state.room_id}", True, DIM)
        screen.blit(pos_text, (MAP_X_OFF, MAP_Y_OFF + MAP_ROWS*CELL + 8))

        pygame.display.flip()

    send_cmd("QUIT")
    pygame.quit()

if __name__ == "__main__":
    main()
