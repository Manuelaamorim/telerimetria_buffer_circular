#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

#define WATER_SENSOR_PIN 33
#define DHT_PIN 4
#define DHT_TYPE DHT11

#define LED_R 25
#define LED_G 26
#define LED_B 27

const char* WIFI_SSID = "Iphone Manuela Amorim";
const char* WIFI_PASS = "Manuela11";
const char* MQTT_BROKER = "172.20.10.8"; 
const int MQTT_PORT = 1883;
const char* MQTT_TOPIC = "elevatorguard/dados";

WiFiClient espClient;
PubSubClient mqtt(espClient);
DHT dht(DHT_PIN, DHT_TYPE);

void conectarWiFi() {
    Serial.print("Conectando ao WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Conectado! IP: ");
    Serial.println(WiFi.localIP());
}

void conectarMQTT() {
    while (!mqtt.connected()) {
        Serial.print("Conectando ao MQTT...");
        if (mqtt.connect("elevatorguard-esp32")) {
            Serial.println("conectado!");
        } else {
            Serial.print("falhou (rc=");
            Serial.print(mqtt.state());
            Serial.println("). Tentando novamente em 3s...");
            delay(3000);
        }
    }
}

int medirNivel() {
    return analogRead(WATER_SENSOR_PIN);
}

void setLed(bool r, bool g, bool b) {
    digitalWrite(LED_R, r);
    digitalWrite(LED_G, g);
    digitalWrite(LED_B, b);
}

const char* classificarStatus(int nivel, float umidade) {
    if (nivel >= 1200 && umidade >= 80) return "critico";
    if (nivel >= 700 && umidade >= 70) return "atencao";
    return "normal";
}

void setup() {
    Serial.begin(115200);
    dht.begin();

    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);

    conectarWiFi();
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);

    Serial.println("Elevator Guard - Sensores iniciados");
}

void loop() {
    if (!mqtt.connected()) conectarMQTT();
    mqtt.loop();

    int nivel = medirNivel();
    float umidade = dht.readHumidity();
    const char* status = classificarStatus(nivel, umidade);

    if (strcmp(status, "critico") == 0) {
        setLed(1, 1, 0);
    } else if (strcmp(status, "atencao") == 0) {
        setLed(0, 1, 1);
    } else {
        setLed(0, 1, 0);
    }

    // Converte nivel analogico (0-4095) para cm (0-10)
    float nivel_cm = (nivel / 4095.0) * 10.0;

    char payload[128];
    snprintf(payload, sizeof(payload),
        "{\"nivel_cm\":%.1f,\"umidade\":%.1f,\"status\":\"%s\"}",
        nivel_cm, umidade, status);

    mqtt.publish(MQTT_TOPIC, payload);

    Serial.printf("Publicado: %s\n", payload);
    delay(3000);
}
