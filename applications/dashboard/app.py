from flask import Flask, render_template, jsonify
import paho.mqtt.client as mqtt
import threading
import json
import time
import random
import sys

app = Flask(__name__)

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC_DADOS = "elevatorguard/dados"
MQTT_TOPIC_PERF = "elevatorguard/performance"

SIMULAR = "--simular" in sys.argv

leituras = {
    "nivel_cm": 0.0,
    "umidade": 0.0,
    "status": "normal",
    "historico": []
}

performance = {
    "tempo_real": [],
    "estresse": []
}


def classificar_status(nivel, umidade):
    if nivel >= 6.0 and umidade >= 80:
        return "critico"
    if nivel >= 3.5 and umidade >= 70:
        return "atencao"
    return "normal"


def processar_dados(dados):
    leituras["nivel_cm"] = dados["nivel_cm"]
    leituras["umidade"] = dados["umidade"]
    leituras["status"] = dados["status"]
    leituras["historico"].append({
        "nivel_cm": dados["nivel_cm"],
        "umidade": dados["umidade"],
        "timestamp": time.strftime("%H:%M:%S")
    })
    if len(leituras["historico"]) > 50:
        leituras["historico"] = leituras["historico"][-50:]


def processar_performance(dados):
    if "N" in dados:
        performance["estresse"].append({
            "N": dados["N"],
            "linear_us": dados["linear_us"],
            "circular_us": dados["circular_us"],
            "heap_linear": dados.get("heap_linear", 0),
            "heap_circular": dados.get("heap_circular", 0),
            "speedup": dados.get("speedup", 0),
            "timestamp": time.strftime("%H:%M:%S")
        })
    else:
        performance["tempo_real"].append({
            "linear_us": dados["linear_us"],
            "circular_us": dados["circular_us"],
            "heap_livre": dados.get("heap_livre", 0),
            "buffer_count": dados.get("buffer_count", 0),
            "timestamp": time.strftime("%H:%M:%S")
        })
        if len(performance["tempo_real"]) > 100:
            performance["tempo_real"] = performance["tempo_real"][-100:]


def on_connect(client, userdata, flags, rc):
    print(f"MQTT conectado (rc={rc})")
    client.subscribe(MQTT_TOPIC_DADOS)
    client.subscribe(MQTT_TOPIC_PERF)


def on_message(client, userdata, msg):
    try:
        dados = json.loads(msg.payload.decode())
        if msg.topic == MQTT_TOPIC_DADOS:
            processar_dados(dados)
        elif msg.topic == MQTT_TOPIC_PERF:
            processar_performance(dados)
    except (json.JSONDecodeError, KeyError) as e:
        print(f"Erro ao processar mensagem MQTT: {e}")


def iniciar_mqtt():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()


def simular_performance():
    """Simula dados de performance para testar o dashboard sem ESP32"""
    n_values = [100, 5000, 20000]
    for n in n_values:
        linear = int(n * 0.8 + random.uniform(0, n * 0.2))
        circular = int(n * 0.01 + random.uniform(0, 50))
        processar_performance({
            "N": n,
            "linear_us": linear,
            "circular_us": circular,
            "heap_linear": 280000 - n * 2,
            "heap_circular": 280000,
            "speedup": round(linear / max(circular, 1), 2)
        })
    time.sleep(1)

    while True:
        linear_us = random.randint(3, 15)
        circular_us = random.randint(1, 4)
        processar_performance({
            "linear_us": linear_us,
            "circular_us": circular_us,
            "heap_livre": random.randint(270000, 290000),
            "buffer_count": random.randint(1, 100)
        })

        nivel = round(random.uniform(0.0, 8.0), 1)
        umidade = round(random.uniform(40.0, 95.0), 1)
        status = classificar_status(nivel, umidade)
        processar_dados({"nivel_cm": nivel, "umidade": umidade, "status": status})
        time.sleep(3)


@app.route("/")
def index():
    return render_template("index.html", leituras=leituras)


@app.route("/dados")
def dados():
    return render_template("partials/dados.html", leituras=leituras)


@app.route("/api/historico")
def historico():
    return jsonify(leituras["historico"])


@app.route("/api/performance")
def perf_tempo_real():
    return jsonify(performance["tempo_real"])


@app.route("/api/estresse")
def perf_estresse():
    return jsonify(performance["estresse"])


if __name__ == "__main__":
    if SIMULAR:
        t = threading.Thread(target=simular_performance, daemon=True)
    else:
        t = threading.Thread(target=iniciar_mqtt, daemon=True)
    t.start()
    app.run(debug=False, host="0.0.0.0", port=5000)
