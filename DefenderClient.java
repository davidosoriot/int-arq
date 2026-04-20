import javax.swing.*;
import javax.swing.border.*;
import java.awt.*;
import java.awt.event.*;
import java.io.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.List;

/**
 * Cliente Defensor – DCGP
 * Lenguaje: Java + Swing
 * Rol: DEFENDER – conoce la ubicación de los recursos, se mueve y mitiga ataques.
 *
 * Compilar: javac DefenderClient.java
 * Ejecutar: java DefenderClient [host] [puerto]
 */
public class DefenderClient {

    /* ── Constantes ────────────────────────────────────────────────── */
    static final int MAP_COLS  = 40;
    static final int MAP_ROWS  = 20;
    static final int CELL      = 18;
    static final int MAP_X_OFF = 20;
    static final int MAP_Y_OFF = 80;

    static final Color BG         = new Color(8,   12,  16);
    static final Color SURFACE    = new Color(13,  21,  32);
    static final Color BORDER_C   = new Color(26,  58,  74);
    static final Color ACCENT     = new Color(0,   229, 255);
    static final Color WARN       = new Color(255,  68,  68);
    static final Color OK_C       = new Color(0,   255, 136);
    static final Color TEXT_C     = new Color(200, 216, 232);
    static final Color DIM_C      = new Color(60,  100, 120);
    static final Color RESOURCE_C = new Color(255, 200,   0);

    /* ── Estado ────────────────────────────────────────────────────── */
    static volatile int     playerX   = MAP_COLS - 1;
    static volatile int     playerY   = MAP_ROWS / 2;
    static volatile int     roomId    = -1;
    static volatile boolean inGame    = false;
    static volatile String  username  = "";
    static volatile String  userRole  = "";   // rol asignado por el servidor

    static final Map<String, int[]> otherPlayers = new ConcurrentHashMap<>();
    // resources: name -> [x, y, attacked, mitigated]
    static final Map<String, int[]> resources    = new ConcurrentHashMap<>();
    static final List<String>       alertedRes   = new CopyOnWriteArrayList<>();
    static final List<String>       logMessages  = new CopyOnWriteArrayList<>();

    static PrintWriter     out;
    static BufferedReader  in;
    static Socket          socket;

    static GamePanel       gamePanel;
    static JTextArea       logArea;
    static JLabel          statusLabel;

    /* ──────────────────────────────────────────────────────────────────
     *  Resolución DNS (sin IPs hardcodeadas)
     * ──────────────────────────────────────────────────────────────── */
    static InetAddress resolveHost(String hostname) {
        try {
            return InetAddress.getByName(hostname);
        } catch (UnknownHostException e) {
            System.err.println("[DNS ERROR] No se pudo resolver: "
                               + hostname + " - " + e.getMessage());
            return null;
        }
    }

    /* ──────────────────────────────────────────────────────────────────
     *  Networking
     * ──────────────────────────────────────────────────────────────── */
    static void send(String cmd) {
        if (out != null) {
            out.print(cmd + "\r\n");
            out.flush();
            addLog("> " + cmd);
        }
    }

    static void addLog(String msg) {
        logMessages.add(msg);
        if (logMessages.size() > 60) logMessages.remove(0);
        SwingUtilities.invokeLater(() -> {
            if (logArea == null) return;
            StringBuilder sb = new StringBuilder();
            for (String l : logMessages) sb.append(l).append("\n");
            logArea.setText(sb.toString());
            logArea.setCaretPosition(logArea.getDocument().getLength());
        });
    }

    static void processMessage(String msg) {
        addLog("< " + msg);
        String[] parts = msg.trim().split("\\s+");
        if (parts.length == 0) return;

        // OK AUTH username role  ← rol asignado por el servidor
        if (parts[0].equals("OK") && parts.length >= 4 && parts[1].equals("AUTH")) {
            userRole = parts[3];
            if (!userRole.equals("DEFENDER")) {
                String errMsg = "Tu rol es '" + userRole + "'.\n"
                              + "Este cliente es solo para DEFENSORES.\n"
                              + "Usa client_attacker.py para atacantes.";
                SwingUtilities.invokeLater(() ->
                    JOptionPane.showMessageDialog(gamePanel, errMsg,
                        "Rol incorrecto", JOptionPane.ERROR_MESSAGE));
            }
        }
        // OK JOIN room_id x y w h
        else if (parts[0].equals("OK") && parts.length >= 7 && parts[1].equals("JOIN")) {
            roomId  = Integer.parseInt(parts[2]);
            playerX = Integer.parseInt(parts[3]);
            playerY = Integer.parseInt(parts[4]);
            updateStatus("Sala #" + roomId + " – Lobby");
        }
        // OK MOVE x y
        else if (parts[0].equals("OK") && parts.length >= 4 && parts[1].equals("MOVE")) {
            playerX = Integer.parseInt(parts[2]);
            playerY = Integer.parseInt(parts[3]);
        }
        // EVENT GAME_STARTED RESOURCES name:x:y ...
        else if (parts[0].equals("EVENT") && parts[1].equals("GAME_STARTED")) {
            inGame = true;
            if (parts.length > 3 && parts[2].equals("RESOURCES")) {
                for (int i = 3; i < parts.length; i++) {
                    String[] r = parts[i].split(":");
                    if (r.length >= 3) {
                        resources.put(r[0], new int[]{
                            Integer.parseInt(r[1]), Integer.parseInt(r[2]), 0, 0
                        });
                    }
                }
            }
            updateStatus("EN JUEGO – Sala #" + roomId);
            if (gamePanel != null) SwingUtilities.invokeLater(() -> gamePanel.requestFocusInWindow());
        }
        // EVENT ATTACK_ALERT resource x y BY attacker
        // partes: [0]=EVENT [1]=ATTACK_ALERT [2]=resource [3]=x [4]=y [5]=BY [6]=attacker
        // FIX: verificar parts.length >= 7 (antes era >= 6, causaba ArrayIndexOutOfBoundsException)
        else if (parts[0].equals("EVENT") && parts[1].equals("ATTACK_ALERT")
                 && parts.length >= 7) {
            String rname = parts[2];
            alertedRes.add(rname);
            int[] res = resources.get(rname);
            if (res != null) res[2] = 1;
            String attacker = parts[6];
            showAlert("⚠ ATAQUE: " + rname
                      + " en (" + parts[3] + "," + parts[4] + ")"
                      + " por " + attacker);
        }
        // OK MITIGATE  ← respuesta al defensor que ejecutó la mitigación
        else if (parts[0].equals("OK") && parts.length >= 2 && parts[1].equals("MITIGATE")) {
            for (int[] r : resources.values()) { if (r[2] == 1 && r[3] == 0) { r[2] = 0; r[3] = 0; } }
            updateStatus("✔ Mitigación exitosa");
        }
        // EVENT MITIGATED resource BY defender
        else if (parts[0].equals("EVENT") && parts[1].equals("MITIGATED")
                 && parts.length >= 4) {
            String rname = parts[2];
            int[] res = resources.get(rname);
            if (res != null) { res[2] = 0; res[3] = 0; }
            alertedRes.remove(rname);
            updateStatus("✔ Mitigado: " + rname);
        }
        // EVENT PLAYER_MOVED name x y
        else if (parts[0].equals("EVENT") && parts[1].equals("PLAYER_MOVED")
                 && parts.length >= 5) {
            otherPlayers.put(parts[2], new int[]{
                Integer.parseInt(parts[3]), Integer.parseInt(parts[4])
            });
        }
        // EVENT PLAYER_LEFT name
        else if (parts[0].equals("EVENT") && parts[1].equals("PLAYER_LEFT")
                 && parts.length >= 3) {
            otherPlayers.remove(parts[2]);
        }

        if (gamePanel != null) gamePanel.repaint();
    }

    static void showAlert(String message) {
        SwingUtilities.invokeLater(() ->
            JOptionPane.showMessageDialog(gamePanel, message,
                "¡ALERTA DE ATAQUE!", JOptionPane.WARNING_MESSAGE));
    }

    static void updateStatus(String s) {
        SwingUtilities.invokeLater(() -> {
            if (statusLabel != null) statusLabel.setText(s);
        });
    }

    /* ──────────────────────────────────────────────────────────────────
     *  Panel del mapa
     * ──────────────────────────────────────────────────────────────── */
    static class GamePanel extends JPanel {
        Font monoFont  = new Font("Courier New", Font.BOLD, 10);
        Font labelFont = new Font("Courier New", Font.PLAIN,  9);

        GamePanel() {
            setBackground(BG);
            setPreferredSize(new Dimension(MAP_COLS*CELL + 4, MAP_ROWS*CELL + 4));
            setFocusable(true);

            addKeyListener(new KeyAdapter() {
                final Set<Integer> pressed = new HashSet<>();

                @Override public void keyPressed(KeyEvent e) {
                    pressed.add(e.getKeyCode());
                    move();
                }

                private void move() {
                    if (!inGame) return;
                    int dx = 0, dy = 0;
                    if (pressed.contains(KeyEvent.VK_LEFT)  || pressed.contains(KeyEvent.VK_A)) dx = -1;
                    if (pressed.contains(KeyEvent.VK_RIGHT) || pressed.contains(KeyEvent.VK_D)) dx = 1;
                    if (pressed.contains(KeyEvent.VK_UP)    || pressed.contains(KeyEvent.VK_W)) dy = -1;
                    if (pressed.contains(KeyEvent.VK_DOWN)  || pressed.contains(KeyEvent.VK_S)) dy = 1;
                    if (dx != 0 || dy != 0) send("MOVE " + dx + " " + dy);
                    // Mitigar con M
                    if (pressed.contains(KeyEvent.VK_M)) {
                        for (Map.Entry<String, int[]> e2 : resources.entrySet()) {
                            int[] r = e2.getValue();
                            if (r[0] == playerX && r[1] == playerY
                                    && r[2] == 1 && r[3] == 0) {
                                send("MITIGATE " + e2.getKey());
                            }
                        }
                    }
                }

                @Override public void keyReleased(KeyEvent e) {
                    pressed.remove(e.getKeyCode());
                }
            });
        }

        @Override protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g;
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING,
                                RenderingHints.VALUE_ANTIALIAS_ON);

            // Grid
            for (int col = 0; col < MAP_COLS; col++) {
                for (int row = 0; row < MAP_ROWS; row++) {
                    int rx = col * CELL + 2;
                    int ry = row * CELL + 2;
                    g2.setColor(SURFACE);
                    g2.fillRect(rx, ry, CELL-1, CELL-1);
                }
            }

            // Recursos críticos
            g2.setFont(labelFont);
            for (Map.Entry<String, int[]> entry : resources.entrySet()) {
                int[] r = entry.getValue();
                int rx = r[0] * CELL + 2;
                int ry = r[1] * CELL + 2;
                Color c;
                if      (r[3] == 1) c = DIM_C;       // mitigado
                else if (r[2] == 1) c = WARN;         // atacado
                else                c = RESOURCE_C;   // normal
                g2.setColor(c);
                g2.fillRect(rx+1, ry+1, CELL-3, CELL-3);
                g2.setColor(BG);
                g2.drawString("R", rx+4, ry+11);
            }

            // Otros jugadores (atacantes)
            for (Map.Entry<String, int[]> entry : otherPlayers.entrySet()) {
                int[] pos = entry.getValue();
                int rx = pos[0] * CELL + 2;
                int ry = pos[1] * CELL + 2;
                g2.setColor(WARN);
                g2.fillOval(rx+2, ry+2, CELL-5, CELL-5);
            }

            // Jugador local (defensor)
            int px = playerX * CELL + 2;
            int py = playerY * CELL + 2;
            g2.setColor(OK_C);
            g2.fillRect(px+1, py+1, CELL-3, CELL-3);
            g2.setColor(BG);
            g2.setFont(monoFont);
            g2.drawString("D", px+4, py+11);

            // Borde del mapa
            g2.setColor(BORDER_C);
            g2.drawRect(1, 1, MAP_COLS*CELL+1, MAP_ROWS*CELL+1);
        }
    }

    /* ──────────────────────────────────────────────────────────────────
     *  Login Dialog
     * ──────────────────────────────────────────────────────────────── */
    static String[] showLoginDialog() {
        JPanel panel = new JPanel(new GridLayout(2, 2, 8, 8));
        panel.setBackground(SURFACE);
        JLabel uLabel = new JLabel("Usuario:");
        JLabel pLabel = new JLabel("Contraseña:");
        uLabel.setForeground(TEXT_C); pLabel.setForeground(TEXT_C);
        JTextField    uField = new JTextField(15);
        JPasswordField pField = new JPasswordField(15);
        uField.setBackground(BG); uField.setForeground(TEXT_C);
        pField.setBackground(BG); pField.setForeground(TEXT_C);
        panel.add(uLabel); panel.add(uField);
        panel.add(pLabel); panel.add(pField);

        int result = JOptionPane.showConfirmDialog(null, panel,
            "DCGP – Autenticación Defensor",
            JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE);
        if (result != JOptionPane.OK_OPTION) System.exit(0);
        return new String[]{ uField.getText().trim(),
                             new String(pField.getPassword()) };
    }

    /* ──────────────────────────────────────────────────────────────────
     *  UI principal
     * ──────────────────────────────────────────────────────────────── */
    static void buildUI(JFrame frame) {
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.getContentPane().setBackground(BG);
        frame.setLayout(new BorderLayout(8, 8));

        // Header
        JPanel header = new JPanel(new FlowLayout(FlowLayout.LEFT, 10, 6));
        header.setBackground(BG);
        JLabel title = new JLabel("DCGP – DEFENSOR");
        title.setFont(new Font("Courier New", Font.BOLD, 18));
        title.setForeground(OK_C);
        statusLabel = new JLabel("Conectando...");
        statusLabel.setFont(new Font("Courier New", Font.PLAIN, 12));
        statusLabel.setForeground(DIM_C);
        header.add(title); header.add(statusLabel);
        frame.add(header, BorderLayout.NORTH);

        // Mapa
        gamePanel = new GamePanel();
        JPanel mapWrapper = new JPanel(new FlowLayout(FlowLayout.LEFT, MAP_X_OFF, MAP_Y_OFF));
        mapWrapper.setBackground(BG);
        mapWrapper.add(gamePanel);
        frame.add(mapWrapper, BorderLayout.CENTER);

        // Panel derecho
        JPanel right = new JPanel();
        right.setLayout(new BoxLayout(right, BoxLayout.Y_AXIS));
        right.setBackground(SURFACE);
        right.setPreferredSize(new Dimension(220, 0));
        right.setBorder(BorderFactory.createEmptyBorder(10, 10, 10, 10));

        JLabel ctrlLabel = new JLabel("CONTROLES");
        ctrlLabel.setFont(new Font("Courier New", Font.BOLD, 11));
        ctrlLabel.setForeground(ACCENT);
        right.add(ctrlLabel); right.add(Box.createVerticalStrut(6));

        for (String s : new String[]{
            "WASD / Flechas → mover",
            "M → MITIGAR recurso",
            "ENTER → iniciar partida",
            "",
            "Leyenda:",
            "■ Amarillo = recurso",
            "■ Rojo = ATACADO",
            "■ Gris = mitigado",
            "● Rojo = atacante",
            "■ Verde = tú (defensor)"
        }) {
            JLabel l = new JLabel(s);
            l.setFont(new Font("Courier New", Font.PLAIN, 10));
            l.setForeground(s.isEmpty() ? DIM_C : TEXT_C);
            right.add(l);
        }

        right.add(Box.createVerticalStrut(16));
        JLabel logLabel = new JLabel("LOG");
        logLabel.setFont(new Font("Courier New", Font.BOLD, 11));
        logLabel.setForeground(ACCENT);
        right.add(logLabel);

        logArea = new JTextArea();
        logArea.setBackground(BG); logArea.setForeground(DIM_C);
        logArea.setFont(new Font("Courier New", Font.PLAIN, 9));
        logArea.setEditable(false); logArea.setLineWrap(true);
        JScrollPane scroll = new JScrollPane(logArea);
        scroll.setPreferredSize(new Dimension(200, 300));
        scroll.setBorder(BorderFactory.createLineBorder(BORDER_C));
        right.add(scroll);

        right.add(Box.createVerticalStrut(10));
        JButton startBtn = makeBtn("INICIAR PARTIDA");
        startBtn.addActionListener(e -> send("START"));
        right.add(startBtn);

        frame.add(right, BorderLayout.EAST);

        // Teclas globales
        frame.addKeyListener(new KeyAdapter() {
            @Override public void keyPressed(KeyEvent e) {
                if (e.getKeyCode() == KeyEvent.VK_ENTER && !inGame) send("START");
                gamePanel.dispatchEvent(e);
            }
        });

        frame.pack();
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);
        gamePanel.requestFocusInWindow();
    }

    static JButton makeBtn(String text) {
        JButton btn = new JButton(text);
        btn.setFont(new Font("Courier New", Font.BOLD, 11));
        btn.setForeground(OK_C);
        btn.setBackground(SURFACE);
        btn.setBorder(BorderFactory.createLineBorder(OK_C));
        btn.setFocusPainted(false);
        btn.setMaximumSize(new Dimension(200, 32));
        return btn;
    }

    /* ──────────────────────────────────────────────────────────────────
     *  Main
     * ──────────────────────────────────────────────────────────────── */
    public static void main(String[] args) throws Exception {
        String host = args.length > 0 ? args[0]
            : System.getenv().getOrDefault("GAME_HOST", "localhost");
        int port = args.length > 1 ? Integer.parseInt(args[1])
            : Integer.parseInt(System.getenv().getOrDefault("GAME_PORT", "8080"));

        // Login
        String[] creds = showLoginDialog();
        username        = creds[0];
        String password = creds[1];

        if (username.isEmpty() || password.isEmpty()) {
            JOptionPane.showMessageDialog(null, "Usuario y contraseña requeridos.",
                "Error", JOptionPane.ERROR_MESSAGE);
            System.exit(1);
        }

        // Resolver nombre del host (sin IP hardcodeada)
        InetAddress addr = resolveHost(host);
        if (addr == null) {
            JOptionPane.showMessageDialog(null,
                "No se pudo resolver el host: " + host,
                "Error DNS", JOptionPane.ERROR_MESSAGE);
            System.exit(1);
        }

        // Conectar
        try {
            socket = new Socket(host, port);
        } catch (IOException e) {
            JOptionPane.showMessageDialog(null,
                "No se pudo conectar: " + e.getMessage(),
                "Error de conexión", JOptionPane.ERROR_MESSAGE);
            System.exit(1);
        }

        out = new PrintWriter(new BufferedWriter(
                  new OutputStreamWriter(socket.getOutputStream())));
        in  = new BufferedReader(new InputStreamReader(socket.getInputStream()));

        // UI
        JFrame frame = new JFrame("DCGP – Defensor");
        frame.getContentPane().setBackground(BG);
        SwingUtilities.invokeAndWait(() -> buildUI(frame));

        // Leer mensajes en hilo separado
        Thread readerThread = new Thread(() -> {
            try {
                String line;
                while ((line = in.readLine()) != null) {
                    processMessage(line);
                }
            } catch (IOException e) {
                addLog("! Desconectado: " + e.getMessage());
                updateStatus("DESCONECTADO");
            }
        });
        readerThread.setDaemon(true);
        readerThread.start();

        // Autenticar — el servidor verifica con identity server y devuelve el rol
        Thread.sleep(300);
        send("AUTH " + username + " " + password);
        Thread.sleep(300);
        send("JOIN NEW");
    }
}
