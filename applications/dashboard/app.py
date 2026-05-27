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
MQTT_TOPIC = "elevatorguard/dados"

SIMULAR = "--simular" in sys.argv

leituras = {
    "nivel_cm": 0.0,
    "umidade": 0.0,
    "status": "normal",
    "historico": []
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


def on_connect(client, userdata, flags, rc):
    print(f"MQTT conectado (rc={rc})")
    client.subscribe(MQTT_TOPIC)


def on_message(client, userdata, msg):
    try:
        dados = json.loads(msg.payload.decode())
        processar_dados(dados)
    except (json.JSONDecodeError, KeyError) as e:
        print(f"Erro ao processar mensagem MQTT: {e}")


def iniciar_mqtt():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()


def iniciar_simulacao():
    print("Modo simulacao ativo - gerando dados fictícios a cada 3s")
    while True:
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


if __name__ == "__main__":
    if SIMULAR:
        t = threading.Thread(target=iniciar_simulacao, daemon=True)
    else:
        t = threading.Thread(target=iniciar_mqtt, daemon=True)
    t.start()
    app.run(debug=True, host="0.0.0.0", port=5000)

