require('dotenv').config();
const http = require('http');
const mqtt = require('mqtt');
const sqlite3 = require('sqlite3').verbose();

// === CREDENCIAIS DO ADAFRUIT IO ===
const ADAFRUIT_USERNAME = process.env.ADAFRUIT_USERNAME;
const ADAFRUIT_KEY      = process.env.ADAFRUIT_KEY;
const FEED_LED          = `${ADAFRUIT_USERNAME}/feeds/led-status`;

// ==========================================
// 1. BANCO DE DADOS LOCAL (PARTE A)
// ==========================================
const db = new sqlite3.Database('./sensor_data.db');
db.serialize(() => {
    db.run(`CREATE TABLE IF NOT EXISTS historico_led (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        estado TEXT NOT NULL,
        data_hora DATETIME DEFAULT CURRENT_TIMESTAMP
    )`);
});

// ==========================================
// 2. CONEXÃO BROKER LOCAL (MOSQUITTO NA BBB)
// ==========================================
const localClient = mqtt.connect(process.env.LOCAL_BROKER);
const TOPICO_LED  = 'ifpb/projeto/led'; // Tópico único compartilhado

// ==========================================
// 3. CONEXÃO NUVEM (ADAFRUIT IO)
// ==========================================
const cloudClient = mqtt.connect(process.env.CLOUD_BROKER, {
    username: ADAFRUIT_USERNAME,
    password: ADAFRUIT_KEY,
    rejectUnauthorized: false
});

localClient.on('connect', () => {
    console.log('✔ Gateway conectado ao Mosquitto Local!');
    localClient.subscribe(TOPICO_LED);
});

// --- FLUXO 1: MENSAGEM DO BOTÃO FÍSICO (LOCAL -> NUVEM) ---
localClient.on('message', (topic, message) => {
    if (topic === TOPICO_LED) {
        const payload = message.toString();
        console.log(`[Barramento Local] Estado do LED recebido: ${payload}`);

        // A. Persistência local no SQLite (Parte A do Projeto)
        const stmt = db.prepare('INSERT INTO historico_led (estado) VALUES (?)');
        stmt.run(payload);
        stmt.finalize();

        // B. Traduz para binário antes de enviar para o Gráfico/Indicador da Nuvem
        const valorNuvem = payload.toUpperCase() === 'ON' ? "1" : "0";

        if (cloudClient.connected) {
            // Parte C: Envia com Retain ativo para guardar o estado na interface
            cloudClient.publish(FEED_LED, valorNuvem, { qos: 1, retain: true });
            console.log(`[Nuvem] Enviado para o feed led-status: ${valorNuvem}`);
        }
    }
});

// --- FLUXO 2: COMANDO DO BOTÃO VIRTUAL (NUVEM -> LOCAL) ---
cloudClient.on('connect', () => {
    console.log('✔ Gateway conectado à Nuvem Adafruit IO!');
    cloudClient.subscribe(FEED_LED);
});

cloudClient.on('message', (topic, message) => {
    if (topic === FEED_LED) {
        const comandoBruto = message.toString().toUpperCase();

        console.log(`[Adafruit IO] Clique recebido na Nuvem: ${comandoBruto}`);

        // O Gateway joga o comando em formato de texto simples ("ON" ou "OFF") no broker local.
        // O seu Node B (Atuador) escuta esse canal e muda o LED físico imediatamente.
        if (comandoBruto === 'LIGAR' || comandoBruto === 'ON' || comandoBruto === '1') {
            localClient.publish(TOPICO_LED, 'ON', { qos: 1, retain: true });
            console.log(`[Gateway -> Local] Injetado 'ON' no barramento local.`);
        } else if (comandoBruto === 'DESLIGAR' || comandoBruto === 'OFF' || comandoBruto === '0') {
            localClient.publish(TOPICO_LED, 'OFF', { qos: 1, retain: true });
            console.log(`[Gateway -> Local] Injetado 'OFF' no barramento local.`);
        }
    }
});

// ==========================================
// 4. API HTTP PARA O SEU FRONT-END PROPRIO (NOTHING DESIGN)
// ==========================================
const server = http.createServer((req, res) => {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
    res.setHeader('Content-Type', 'application/json');

    if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }

    // Rota GET: Retorna os logs do SQLite local para o painel minimalista
    if (req.url === '/api/historico' && req.method === 'GET') {
        db.all('SELECT * FROM historico_led ORDER BY id DESC LIMIT 20', [], (err, rows) => {
            if (err) { res.writeHead(500); res.end(JSON.stringify({ error: err.message })); return; }
            res.writeHead(200); res.end(JSON.stringify(rows));
        });
    } 
    // Rota POST: Permite que o botão do seu site próprio envie um comando de inversão
    else if (req.url === '/api/led/controle' && req.method === 'POST') {
        let body = '';
        req.on('data', chunk => { body += chunk.toString(); });
        req.on('end', () => {
            try {
                const acao = JSON.parse(body).acao.toUpperCase();
                const msgMqtt = acao === 'LIGAR' ? 'ON' : 'OFF';
                localClient.publish(TOPICO_LED, msgMqtt, { qos: 1, retain: true });
                res.writeHead(200); res.end(JSON.stringify({ success: true }));
            } catch (e) {
                res.writeHead(400); res.end();
            }
        });
    } else {
        res.writeHead(404); res.end();
    }
});

server.listen(process.env.API_PORT || 3000, () => {
    console.log('🚀 Gateway focado estritamente no LED rodando na porta 3000');
});