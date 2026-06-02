# Projeto IoT — Controle de LED via MQTT

**Autores:** Luís Felipe Nonato e Victor Medeiros  
**Instituição:** IFPB — Instituto Federal de Educação, Ciência e Tecnologia da Paraíba  
**Disciplina:** Sistemas Embarcados / IoT

---

## Visão Geral

Sistema IoT distribuído para controle de LED via botão físico, com sincronização em nuvem e histórico de estados. O sistema integra dois nós ESP32 (botão e atuador), um gateway local (BeagleBone Black) e a plataforma de nuvem Adafruit IO, todos comunicando-se via protocolo MQTT.

```
┌─────────────────────────────────────────────────────────────────┐
│                        ARQUITETURA GERAL                        │
│                                                                 │
│  [Botão GPIO 4]                              [LED GPIO 13]      │
│       │                                            ▲            │
│  ┌────▼────┐    MQTT (WiFi)    ┌──────────┐  MQTT  │            │
│  │  Nó A   │ ─────────────── ▶│ Mosquitto│───────▶│  Nó B     │
│  │ mqtt_btn│    "ON"/"OFF"    │ (Gateway)│        │  atuador  │
│  └─────────┘                  └────┬─────┘        └────────────┘
│                                    │                            │
│                              ┌─────▼──────┐                    │
│                              │ main.js /  │                    │
│                              │ getway.py  │                    │
│                              └──┬──────┬──┘                    │
│                           SQLite│      │TLS/MQTT               │
│                         ┌───────▼┐  ┌──▼──────────┐           │
│                         │ Banco  │  │ Adafruit IO │           │
│                         │ Local  │  │   (Nuvem)   │           │
│                         └────────┘  └─────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

---

## Estrutura do Repositório

```
projeto3/
├── mqtt_btn/          # Nó A — Botão (ESP32 Publisher)
│   ├── main/
│   │   └── mqtt_new_btn.c
│   ├── CMakeLists.txt
│   └── sdkconfig
│
├── atuador/           # Nó B — Atuador (ESP32 Subscriber)
│   ├── main/
│   │   └── mqtt_new.c
│   ├── CMakeLists.txt
│   └── sdkconfig
│
└── backend/           # Gateway (BeagleBone Black / PC)
    ├── main.js        # Implementação Node.js (principal)
    ├── getway.py      # Implementação Python (alternativa)
    ├── package.json
    └── sensor_data.db # Banco SQLite (gerado em runtime)
```

---

## Componentes do Sistema

### Nó A — mqtt_btn (Botão / Publisher)

**Hardware:** ESP32 + botão físico no GPIO 4 (pull-up interno)

O nó A monitora um botão físico e publica comandos MQTT cada vez que ele é pressionado. A cada pressionamento, o estado do LED é alternado (toggle) e enviado ao broker.

**Comportamento:**
- Polling a cada 20 ms no GPIO 4
- Debounce de 250 ms por software
- Publica `"ON"` ou `"OFF"` no tópico `ifpb/projeto/led`
- Envia keep-alive a cada 30 s no tópico `ifpb/projeto/status` com o tempo de uptime

**Tarefas FreeRTOS:**

| Tarefa | Stack | Prioridade | Função |
|---|---|---|---|
| `button_task` | 3072 bytes | 10 | Leitura do GPIO e publicação |
| `keep_alive_task` | 3072 bytes | 5 | Status periódico |

---

### Nó B — atuador (LED / Subscriber)

**Hardware:** ESP32 + LED no GPIO 13 (resistor de 220 Ω)

O nó B assina o tópico MQTT e acende ou apaga o LED conforme os comandos recebidos.

**Comportamento:**
- Assina `ifpb/projeto/led` após conectar ao broker
- Interpreta `"ON"` → GPIO 13 HIGH | `"OFF"` → GPIO 13 LOW
- Envia keep-alive a cada 30 s: `"Node B (Atuador): ONLINE"`

**Tarefas FreeRTOS:**

| Tarefa | Stack | Prioridade | Função |
|---|---|---|---|
| `keep_alive_task` | 2048 bytes | 5 | Status periódico |

---

### Gateway — backend/

O gateway é o cérebro do sistema: recebe eventos do broker local, persiste no banco de dados e sincroniza com a nuvem. Há duas implementações equivalentes.

#### main.js (Node.js) — Implementação Principal

Conecta-se ao broker Mosquitto local e ao Adafruit IO via TLS, armazena o histórico em SQLite e expõe uma API REST.

**Fluxo de dados:**

```
Nó A publica "ON"/"OFF"
       ↓
Mosquitto (localhost:1883)
       ↓
main.js recebe
  ├─→ Salva em SQLite (tabela historico_led)
  ├─→ Publica "1"/"0" no Adafruit IO (feed: led-status)
  └─→ Disponível via API REST GET /api/historico
       ↑
POST /api/led/controle
ou Adafruit IO → main.js → Mosquitto → Nó B
```

**API REST (porta 3000):**

| Método | Endpoint | Descrição |
|---|---|---|
| `GET` | `/api/historico` | Últimos 20 estados do LED (JSON) |
| `POST` | `/api/led/controle` | Controla o LED remotamente |

Exemplo de corpo para `POST /api/led/controle`:
```json
{ "acao": "LIGAR" }
```
ou
```json
{ "acao": "DESLIGAR" }
```

#### getway.py (Python) — Implementação Alternativa

Mesma funcionalidade do `main.js`, com Flask no lugar do HTTP nativo e conexão WebSocket para o Adafruit IO (útil para redes com restrição de portas TLS). Continua operando localmente mesmo se a conexão com a nuvem cair.

---

## Tópicos MQTT

| Tópico | Publisher | Subscriber | Payload |
|---|---|---|---|
| `ifpb/projeto/led` | Nó A | Nó B, Gateway | `"ON"` / `"OFF"` |
| `ifpb/projeto/status` | Nó A, Nó B | — | Mensagens de status |

**QoS:** 1 (at-least-once)  
**Retain:** Habilitado nas mensagens de status

---

## Banco de Dados SQLite

Arquivo: `backend/sensor_data.db`

**Tabela `historico_led`:**

| Coluna | Tipo | Descrição |
|---|---|---|
| `id` | INTEGER PK | Identificador único |
| `estado` | TEXT | `"ON"` ou `"OFF"` |
| `data_hora` | TIMESTAMP | Data e hora do evento |

---

## Configuração de Rede

| Parâmetro | Valor |
|---|---|
| WiFi SSID | `MERCUSYS_7E02` |
| Broker local | `mqtt://192.168.1.101` |
| Adafruit IO | `mqtts://io.adafruit.com:8883` |
| API REST | `http://<gateway>:3000` |

> Para adaptar a outro ambiente, altere as constantes de rede no topo de cada arquivo `.c` e no `main.js` / `getway.py`.

---

## Tecnologias Utilizadas

**Embarcado:**
- ESP32 (Xtensa 32-bit dual-core)
- ESP-IDF 6.0.0
- FreeRTOS
- CMake + Ninja
- Espressif MQTT Client

**Gateway:**
- Node.js com bibliotecas `mqtt` e `sqlite3`
- Python 3 com `paho-mqtt`, `flask` e `flask-cors`

**Nuvem:**
- Adafruit IO (MQTT over TLS)

**Protocolos:**
- WiFi 802.11 b/g/n (WPA2-PSK)
- MQTT 3.1.1
- HTTP/REST com JSON
- TLS/SSL

---

## Como Compilar e Gravar (ESP32)

### Pré-requisitos
- ESP-IDF 6.0.0 instalado e configurado no PATH
- Cabo USB conectado ao ESP32

### Nó A (mqtt_btn)

```bash
cd mqtt_btn
idf.py build
idf.py -p COM<N> flash monitor
```

### Nó B (atuador)

```bash
cd atuador
idf.py build
idf.py -p COM<N> flash monitor
```

> Substitua `COM<N>` pela porta serial correta (ex.: `COM3` no Windows ou `/dev/ttyUSB0` no Linux).

---

## Como Executar o Gateway

### Node.js (principal)

```bash
cd backend
npm install
node main.js
```

### Python (alternativo)

```bash
cd backend
pip install paho-mqtt flask flask-cors
python getway.py
```

O broker Mosquitto deve estar rodando no mesmo host que o gateway:

```bash
# Instalar Mosquitto (Ubuntu/Debian)
sudo apt install mosquitto mosquitto-clients
sudo systemctl start mosquitto
```

---

## Fluxo Completo de Operação

1. **Inicialização:** Ambos os ESP32 conectam-se ao WiFi e ao broker MQTT.
2. **Nó B** assina o tópico `ifpb/projeto/led`.
3. **Usuário pressiona o botão** → Nó A detecta borda de descida no GPIO 4.
4. **Nó A publica** `"ON"` ou `"OFF"` no tópico `ifpb/projeto/led`.
5. **Nó B recebe** o comando e acende/apaga o LED no GPIO 13.
6. **Gateway** intercepta a mensagem e:
   - Persiste o evento no SQLite com timestamp.
   - Publica `"1"` ou `"0"` no feed do Adafruit IO.
7. **Dashboard remoto** (Adafruit IO) reflete o estado atual.
8. **Controle remoto:** Uma ação na nuvem ou na API REST chega ao gateway, que republica no broker local para o Nó B.

---

## Diagrama de Sequência

```
Usuário   Nó A (ESP32)    Mosquitto     Gateway      Nó B (ESP32)   Adafruit IO
   │            │              │             │              │               │
   │─pressiona─▶│              │             │              │               │
   │            │──publica────▶│             │              │               │
   │            │         "ON"/"OFF"         │              │               │
   │            │              │──entrega───▶│              │               │
   │            │              │             │──republica──▶│               │
   │            │              │             │         acende/apaga LED     │
   │            │              │             │──publica "1"/"0"────────────▶│
   │            │              │             │──salva SQLite│               │
```

---

## Recursos de Resiliência

- Reconexão automática ao WiFi nos dois nós ESP32
- MQTT bloqueante: inicialização do cliente só ocorre após WiFi conectado
- Flag retain no MQTT preserva o último estado após reinicialização do broker
- Gateway Python continua operando localmente mesmo sem conexão com a nuvem
- Debounce de 250 ms evita múltiplos disparos por pressionamento
