import os
import sqlite3
import json
from flask import Flask, jsonify, request
from flask_cors import CORS
from dotenv import load_dotenv
import paho.mqtt.client as mqtt

load_dotenv()

# === CREDENCIAIS DO ADAFRUIT IO ===
ADAFRUIT_USERNAME = os.getenv('ADAFRUIT_USERNAME')
ADAFRUIT_KEY      = os.getenv('ADAFRUIT_KEY')
FEED_LED          = f"{ADAFRUIT_USERNAME}/feeds/led-status"

TOPICO_LED        = 'ifpb/projeto/led'
CLOUD_HOST        = os.getenv('CLOUD_BROKER_WS_HOST', 'io.adafruit.com')
CLOUD_PORT        = int(os.getenv('CLOUD_BROKER_WS_PORT', '443'))
LOCAL_BROKER_URL  = os.getenv('LOCAL_BROKER', 'mqtt://localhost:1883')
API_PORT          = int(os.getenv('API_PORT', '3000'))

# ==========================================
# 1. BANCO DE DADOS LOCAL (PARTE A)
# ==========================================
def init_db():
    conn = sqlite3.connect('./sensor_data.db')
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS historico_led (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            estado TEXT NOT NULL,
            data_hora DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    conn.commit()
    conn.close()

init_db()

# ==========================================
# 2. CONFIGURAÇÃO MQTT (LOCAL E NUVEM)
# ==========================================

# --- Callbacks do Broker Local (Mosquitto na BBB) ---
def on_local_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print('✔ Gateway conectado ao Mosquitto Local!')
        client.subscribe(TOPICO_LED)
    else:
        print(f'❌ Falha na conexão local. Código: {rc}')

def on_local_message(client, userdata, msg):
    if msg.topic == TOPICO_LED:
        payload = msg.payload.decode('utf-8')
        print(f'[Barramento Local] Estado do LED recebido: ${payload}')

        # A. Persistência local no SQLite
        conn = sqlite3.connect('./sensor_data.db')
        cursor = conn.cursor()
        cursor.execute('INSERT INTO historico_led (estado) VALUES (?)', (payload,))
        conn.commit()
        conn.close()

        # B. Traduz para binário antes de enviar para a Nuvem
        valor_nuvem = "1" if payload.upper() == 'ON' else "0"

        # C. Envia com Retain ativo para a Nuvem
        if cloud_client.is_connected():
            cloud_client.publish(FEED_LED, valor_nuvem, qos=1, retain=True)
            print(f'[Nuvem] Enviado para o feed led-status: {valor_nuvem}')
        else:
            print('[Nuvem] Aguardando conexão para enviar dados...')

# --- Callbacks do Broker Nuvem (Adafruit IO) ---
def on_cloud_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print('✔ Gateway conectado à Nuvem Adafruit IO via WebSockets (Porta 443)!')
        client.subscribe(FEED_LED)
    else:
        print(f'❌ Falha na conexão Nuvem. Código: {rc}')

def on_cloud_message(client, userdata, msg):
    if msg.topic == FEED_LED:
        comando_bruto = msg.payload.decode('utf-8').upper()
        print(f'[Adafruit IO] Clique recebido na Nuvem: {comando_bruto}')

        if comando_bruto in ['LIGAR', 'ON', '1']:
            local_client.publish(TOPICO_LED, 'ON', qos=1, retain=True)
            print("[Gateway -> Local] Injetado 'ON' no barramento local.")
        elif comando_bruto in ['DESLIGAR', 'OFF', '0']:
            local_client.publish(TOPICO_LED, 'OFF', qos=1, retain=True)
            print("[Gateway -> Local] Injetado 'OFF' no barramento local.")

# --- Inicializando Cliente Local (Mosquitto) ---
local_client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
local_client.on_connect = on_local_connect
local_client.on_message = on_local_message

# --- Inicializando Cliente Nuvem (Adafruit via WebSockets para a BBB) ---
cloud_client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2, 
    transport="websockets"  # Força o uso de WebSockets (Bypass de Firewalls)
)
cloud_client.username_pw_set(ADAFRUIT_USERNAME, ADAFRUIT_KEY)
cloud_client.tls_set()  # Mantém a conexão segura SSL/TLS
cloud_client.tls_insecure_set(True) 
cloud_client.on_connect = on_cloud_connect
cloud_client.on_message = on_cloud_message

# Conectando ao Mosquitto Interno da BBB
local_host = LOCAL_BROKER_URL.replace('mqtt://', '').split(':')[0]
local_client.connect(local_host, 1883, 60)
local_client.loop_start()

# Conectando à Nuvem de forma segura (Se a rede falhar, o script NÃO morre)
try:
    print('🔄 Tentando conectar ao Adafruit IO...')
    # io.adafruit.com na porta 443 usando WebSockets
    cloud_client.connect(CLOUD_HOST, CLOUD_PORT, 60)
    cloud_client.loop_start()
except Exception as e:
    print(f'⚠️ Erro inicial de rede com a Nuvem: {e}')
    print('▶ O Gateway continuará rodando localmente. O paho-mqtt tentará reconectar automaticamente assim que a internet voltar.')
    # Inicia o loop mesmo com erro para permitir a reconexão automática em background
    cloud_client.loop_start()


# ==========================================
# 3. API HTTP COM FLASK (NOTHING DESIGN)
# ==========================================
app = Flask(__name__)
CORS(app) 

# Rota GET: Retorna os logs do SQLite local
@app.route('/api/historico', methods=['GET'])
def get_historico():
    try:
        conn = sqlite3.connect('./sensor_data.db')
        conn.row_factory = sqlite3.Row 
        cursor = conn.cursor()
        cursor.execute('SELECT * FROM historico_led ORDER BY id DESC LIMIT 20')
        rows = cursor.fetchall()
        conn.close()
        
        resultado = [dict(row) for row in rows]
        return jsonify(resultado), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500

# Rota POST: Controle manual pelo front-end
@app.route('/api/led/controle', methods=['POST'])
def post_controle():
    try:
        dados = request.get_json()
        acao = dados.get('acao', '').upper()
        
        msg_mqtt = 'ON' if acao == 'LIGAR' else 'OFF'
        local_client.publish(TOPICO_LED, msg_mqtt, qos=1, retain=True)
        
        return jsonify({"success": True}), 200
    except Exception as e:
        return jsonify({"error": "Requisição inválida"}), 400


if __name__ == '__main__':
    print('🚀 Gateway focado estritamente no LED rodando na porta 3000')
    app.run(host='0.0.0.0', port=API_PORT, threaded=True)
