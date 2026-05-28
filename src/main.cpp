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

const char* WIFI_SSID = "NET_602.2G";
const char* WIFI_PASS = "409369048";
const char* MQTT_BROKER = "192.168.0.44";
const int MQTT_PORT = 1883;
const char* MQTT_TOPIC_DADOS = "elevatorguard/dados";
const char* MQTT_TOPIC_PERF = "elevatorguard/performance";

WiFiClient espClient;
PubSubClient mqtt(espClient);
DHT dht(DHT_PIN, DHT_TYPE);

// ============================================================
// VERTENTE 2: Buffer Circular (Eficiente - O(1))
// ============================================================
#define BUFFER_SIZE 1000

struct Amostra {
    float nivel_cm;
    float umidade;
    unsigned long timestamp;
};

struct RingBuffer {
    Amostra dados[BUFFER_SIZE];
    int head;
    int tail;
    int count;

    void init() {
        head = 0;
        tail = 0;
        count = 0;
    }

    void push(Amostra a) {
        dados[head] = a;
        head = (head + 1) % BUFFER_SIZE;
        if (count < BUFFER_SIZE) {
            count++;
        } else {
            tail = (tail + 1) % BUFFER_SIZE;
        }
    }

    Amostra pop() {
        Amostra a = dados[tail];
        tail = (tail + 1) % BUFFER_SIZE;
        count--;
        return a;
    }

    bool isEmpty() {
        return count == 0;
    }

    bool isFull() {
        return count == BUFFER_SIZE;
    }
};

RingBuffer bufferCircular;

// ============================================================
// VERTENTE 1: Deslocamento de Array (Ineficiente - O(n))
// ============================================================
Amostra arrayLinear[BUFFER_SIZE];
int arrayCount = 0;

void pushLinear(Amostra a) {
    if (arrayCount >= BUFFER_SIZE) {
        for (int i = 0; i < BUFFER_SIZE - 1; i++) {
            arrayLinear[i] = arrayLinear[i + 1];
        }
        arrayLinear[BUFFER_SIZE - 1] = a;
    } else {
        arrayLinear[arrayCount] = a;
        arrayCount++;
    }
}

// ============================================================
// Conexao WiFi e MQTT
// ============================================================
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

// ============================================================
// Sensores e LED
// ============================================================
int medirNivel() {
    return analogRead(WATER_SENSOR_PIN);
}

void setLed(bool r, bool g, bool b) {
    digitalWrite(LED_R, r);
    digitalWrite(LED_G, g);
    digitalWrite(LED_B, b);
}

const char* classificarStatus(float nivel_cm, float umidade) {
    if (nivel_cm >= 7.0 && umidade >= 80) return "critico";
    if (nivel_cm >= 4.0 && umidade >= 70) return "atencao";
    return "normal";
}

// ============================================================
// Teste de Estresse - compara as duas vertentes com N insercoes
// ============================================================
void testeEstresse(int N) {
    Serial.printf("\n=== TESTE DE ESTRESSE: N = %d ===\n", N);

    Amostra amostraFake = {5.0, 65.0, micros()};

    // --- Vertente 1: Deslocamento Linear ---
    arrayCount = 0;
    unsigned long startLinear = micros();
    for (int i = 0; i < N; i++) {
        amostraFake.timestamp = micros();
        pushLinear(amostraFake);
    }
    unsigned long duracaoLinear = micros() - startLinear;
    uint32_t heapDepoisLinear = ESP.getFreeHeap();

    // --- Vertente 2: Buffer Circular ---
    bufferCircular.init();
    unsigned long startCircular = micros();
    for (int i = 0; i < N; i++) {
        amostraFake.timestamp = micros();
        bufferCircular.push(amostraFake);
    }
    unsigned long duracaoCircular = micros() - startCircular;
    uint32_t heapDepoisCircular = ESP.getFreeHeap();

    // --- Resultados ---
    Serial.printf("Vertente 1 (Linear):   %lu us | Heap: %u bytes\n", duracaoLinear, heapDepoisLinear);
    Serial.printf("Vertente 2 (Circular): %lu us | Heap: %u bytes\n", duracaoCircular, heapDepoisCircular);
    Serial.printf("Speedup: %.2fx mais rapido\n", (float)duracaoLinear / (float)duracaoCircular);

    // Publica comparativo via MQTT (com retain pra dashboard nao perder)
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"N\":%d,\"linear_us\":%lu,\"circular_us\":%lu,\"heap_linear\":%u,\"heap_circular\":%u,\"speedup\":%.2f}",
        N, duracaoLinear, duracaoCircular, heapDepoisLinear, heapDepoisCircular,
        (float)duracaoLinear / (float)duracaoCircular);
    mqtt.publish(MQTT_TOPIC_PERF, payload, true);
}

// ============================================================
// Variaveis de instrumentacao
// ============================================================
unsigned long latenciaV1 = 0;
unsigned long latenciaV2 = 0;
#define INSERCOES_POR_CICLO 500

// ============================================================
// VERTENTE 1: Envio Sincrono (bloqueia amostragem)
// Le sensor -> desloca array N vezes -> envia MQTT -> so entao libera
// ============================================================
void vertente1_sincrono() {
    unsigned long inicioTotal = micros();

    // Leitura do sensor
    int nivelRaw = medirNivel();
    float umidade = dht.readHumidity();
    float nivel_cm = (nivelRaw / 4095.0) * 10.0;

    Amostra novaAmostra = {nivel_cm, umidade, micros()};

    // Insercao com deslocamento O(n) - multiplas insercoes pra evidenciar custo
    unsigned long t1 = micros();
    for (int i = 0; i < INSERCOES_POR_CICLO; i++) {
        novaAmostra.timestamp = micros();
        pushLinear(novaAmostra);
    }
    unsigned long latenciaInsercao = micros() - t1;

    // Envio MQTT SINCRONO - bloqueia ate completar
    char payload[200];
    snprintf(payload, sizeof(payload),
        "{\"vertente\":1,\"nivel_cm\":%.1f,\"umidade\":%.1f,\"status\":\"%s\"}",
        nivel_cm, umidade, classificarStatus(nivel_cm, umidade));
    mqtt.publish(MQTT_TOPIC_DADOS, payload);

    // Tempo total inclui leitura + insercao + envio (tudo bloqueante)
    unsigned long tempoTotal = micros() - inicioTotal;

    Serial.printf("[V1-SINCRONO] %d insercoes: %lu us | Total: %lu us\n",
        INSERCOES_POR_CICLO, latenciaInsercao, tempoTotal);

    latenciaV1 = latenciaInsercao;
}

// ============================================================
// VERTENTE 2: Produtor-Consumidor (buffer absorve latencia)
// Produtor: le sensor -> push no ring buffer O(1) -> retorna imediatamente
// Consumidor: em outro momento, consome do buffer e envia MQTT
// ============================================================
void vertente2_produtor() {
    // PRODUTOR: le sensor e insere no buffer - NAO envia MQTT aqui
    int nivelRaw = medirNivel();
    float umidade = dht.readHumidity();
    float nivel_cm = (nivelRaw / 4095.0) * 10.0;

    Amostra novaAmostra = {nivel_cm, umidade, micros()};

    // Mesma quantidade de insercoes pra comparacao justa
    unsigned long t2 = micros();
    for (int i = 0; i < INSERCOES_POR_CICLO; i++) {
        novaAmostra.timestamp = micros();
        bufferCircular.push(novaAmostra);
    }
    latenciaV2 = micros() - t2;

    Serial.printf("[V2-PRODUTOR] %d insercoes O(1): %lu us | Buffer: %d/%d\n",
        INSERCOES_POR_CICLO, latenciaV2, bufferCircular.count, BUFFER_SIZE);
}

void vertente2_consumidor() {
    // CONSUMIDOR: envia dados acumulados no buffer sem bloquear o produtor
    if (bufferCircular.isEmpty()) return;

    // Envia o lote de amostras disponiveis (ate 10 por ciclo pra nao travar)
    int enviados = 0;
    while (!bufferCircular.isEmpty() && enviados < 10) {
        Amostra a = bufferCircular.pop();
        char payload[200];
        snprintf(payload, sizeof(payload),
            "{\"vertente\":2,\"nivel_cm\":%.1f,\"umidade\":%.1f,\"status\":\"%s\"}",
            a.nivel_cm, a.umidade, classificarStatus(a.nivel_cm, a.umidade));
        mqtt.publish(MQTT_TOPIC_DADOS, payload);
        enviados++;
    }

    Serial.printf("[V2-CONSUMIDOR] Enviou %d amostras | Restam: %d\n",
        enviados, bufferCircular.count);
}

// ============================================================
// Setup e Loop
// ============================================================
void setup() {
    Serial.begin(115200);
    dht.begin();

    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);

    conectarWiFi();
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);

    bufferCircular.init();

    Serial.println("ElevatorGuard - Analise de Algoritmos");
    Serial.println("Buffer Circular vs Deslocamento Linear");
    Serial.println("======================================");

    // Testes de estresse ao iniciar (diferentes escalas de N)
    conectarMQTT();
    mqtt.loop();
    testeEstresse(100);
    mqtt.loop();
    testeEstresse(5000);
    mqtt.loop();
    testeEstresse(20000);
    mqtt.loop();

    Serial.println("\n=== Iniciando monitoramento continuo ===\n");
}

unsigned long ultimaLeitura = 0;
unsigned long ultimoEnvioConsumidor = 0;
unsigned long ultimoEstresse = 0;
const unsigned long INTERVALO_LEITURA = 3000;
const unsigned long INTERVALO_CONSUMIDOR = 5000;
const unsigned long INTERVALO_ESTRESSE = 30000;

void loop() {
    if (!mqtt.connected()) conectarMQTT();
    mqtt.loop();

    unsigned long agora = millis();

    // --- Amostragem a cada 3s: ambas vertentes leem o sensor ---
    if (agora - ultimaLeitura >= INTERVALO_LEITURA) {
        ultimaLeitura = agora;

        // Vertente 1: leitura + insercao + envio MQTT (tudo bloqueante)
        vertente1_sincrono();

        // Vertente 2: so o PRODUTOR roda aqui (push no buffer, sem envio)
        vertente2_produtor();

        // LED de status baseado na ultima leitura
        int nivelRaw = analogRead(WATER_SENSOR_PIN);
        float nivel_cm = (nivelRaw / 4095.0) * 10.0;
        float umidade = dht.readHumidity();
        const char* status = classificarStatus(nivel_cm, umidade);
        if (strcmp(status, "critico") == 0) {
            setLed(1, 1, 0);
        } else if (strcmp(status, "atencao") == 0) {
            setLed(0, 1, 1);
        } else {
            setLed(0, 1, 0);
        }

        // Publica metricas de performance comparativas
        char payloadPerf[200];
        snprintf(payloadPerf, sizeof(payloadPerf),
            "{\"linear_us\":%lu,\"circular_us\":%lu,\"heap_livre\":%u,\"buffer_count\":%d}",
            latenciaV1, latenciaV2, ESP.getFreeHeap(), bufferCircular.count);
        mqtt.publish(MQTT_TOPIC_PERF, payloadPerf);
    }

    // --- Consumidor da Vertente 2: roda em intervalo SEPARADO ---
    if (agora - ultimoEnvioConsumidor >= INTERVALO_CONSUMIDOR) {
        ultimoEnvioConsumidor = agora;
        vertente2_consumidor();
    }

    // --- Teste de estresse periodico (a cada 30s) ---
    if (agora - ultimoEstresse >= INTERVALO_ESTRESSE) {
        ultimoEstresse = agora;
        Serial.println("\n--- Executando testes de estresse periodicos ---");
        testeEstresse(100);
        mqtt.loop();
        testeEstresse(5000);
        mqtt.loop();
        testeEstresse(20000);
        mqtt.loop();
    }
}