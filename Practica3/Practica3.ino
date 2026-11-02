#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ---------------------------------------------------------------------------
// 1) CREDENCIALES DE RED Y WEBSOCKET
// ---------------------------------------------------------------------------

const char* ssid     = "GWN571D04";
const char* password = "ESP32CUCEI$$";

WebSocketsServer webSocket = WebSocketsServer(81);

// ---------------------------------------------------------------------------
// 2) MOTORES (TB6612FNG)
// ---------------------------------------------------------------------------
#define IN1 11
#define IN2 12
#define IN3 13
#define IN4 14
#define PWM_A 15
#define PWM_B 16

#define PWM_FREQ 5000
#define PWM_RES  8

uint8_t velAvanzar = 170;
uint8_t velGiro    = 140;

struct ComandoMover {
  char accion[16];
  uint32_t ms;
};

QueueHandle_t colaMover;
TaskHandle_t hTareaMover = NULL;
char accionActual[16] = "parar";

// ---------------------------------------------------------------------------
// 3) ASIGNACIÓN DE PINES Y VARIABLES DE CONTROL
// ---------------------------------------------------------------------------
const int PIN_SERVO  = 8;    
const int TRIG_PIN   = 9;    
const int ECHO_PIN   = 10;   

Servo servo;
int anguloActual = 90;
const int pulsoMinimo = 500;
const int pulsoMaximo = 2400;

float distanciaActual = 100.0;
unsigned long ultimoMuestreoUS = 0;

// Distancia de seguridad para detener el avance
const float DISTANCIA_SEGURIDAD_CM = 25.0; 

// ---------------------------------------------------------------------------
// FUNCIONES DE CONTROL DE MOTORES
// ---------------------------------------------------------------------------
static inline void motores_init_pines() {
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  ledcAttach(PWM_A, PWM_FREQ, PWM_RES);
  ledcAttach(PWM_B, PWM_FREQ, PWM_RES);

  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  ledcWrite(PWM_A, 0); ledcWrite(PWM_B, 0);
}

static inline void set_velocidad(uint8_t vA, uint8_t vB) {
  ledcWrite(PWM_A, vA);
  ledcWrite(PWM_B, vB);
}

static inline void motores_parar_total() {
  set_velocidad(0, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

static inline void mover_avanzar() {
  set_velocidad(velAvanzar, velAvanzar);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

static inline void mover_retroceder() {
  set_velocidad(velAvanzar, velAvanzar);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

static inline void mover_izquierda() {
  set_velocidad(velGiro, velGiro);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

static inline void mover_derecha() {
  set_velocidad(velGiro, velGiro);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

// ---------------------------------------------------------------------------
// MEDICIÓN ULTRASÓNICA CORREGIDA (Timeout ampliado a 25000 us)
// ---------------------------------------------------------------------------
float medirDistancia() {
  float suma = 0;
  int lecturasValidas = 0;

  for (int i = 0; i < 3; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duracion = pulseIn(ECHO_PIN, HIGH, 25000);
    if (duracion > 0) {
      suma += (duracion * 0.0343) / 2.0;
      lecturasValidas++;
    }
    delayMicroseconds(500); // Pequeño intervalo entre pulsos para evitar ecos fantasma
  }

  if (lecturasValidas == 0) return 400.0;
  return suma / lecturasValidas;
}

// ---------------------------------------------------------------------------
// TAREA FREERTOS DE MOVIMIENTO
// ---------------------------------------------------------------------------
void tareaEjecutarMovimiento(void *pvParameters) {
  ComandoMover cmd;

  for (;;) {
    if (xQueueReceive(colaMover, &cmd, portMAX_DELAY) == pdTRUE) {
      strncpy(accionActual, cmd.accion, sizeof(accionActual) - 1);

      if (strcmp(cmd.accion, "avanzar") == 0) {
        // Confirmar que la lectura sea realmente menor a 25 cm y no un pico falso
        if (distanciaActual > 0.0 && distanciaActual <= DISTANCIA_SEGURIDAD_CM) {
          motores_parar_total();
          strcpy(accionActual, "parar");
        } else {
          mover_avanzar();
        }
      } else if (strcmp(cmd.accion, "retroceder") == 0) {
        mover_retroceder();
      } else if (strcmp(cmd.accion, "izquierda") == 0) {
        mover_izquierda();
      } else if (strcmp(cmd.accion, "derecha") == 0) {
        mover_derecha();
      } else {
        motores_parar_total();
      }

      if (cmd.ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cmd.ms));
        motores_parar_total();
        strcpy(accionActual, "parar");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// MANEJADOR WEBSOCKET
// ---------------------------------------------------------------------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      motores_parar_total();
      strcpy(accionActual, "parar");
      break;

    case WStype_CONNECTED: {
      JsonDocument docSaludo;
      docSaludo["tipo"] = "saludo";
      docSaludo["nombre_host"] = "ESP32-S3-Robot";
      
      String res;
      serializeJson(docSaludo, res);
      webSocket.sendTXT(num, res);
      break;
    }

    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, length);
      if (error) return;

      const char* tipo = doc["tipo"];
      if (!tipo) return;

      if (strcmp(tipo, "mover") == 0) {
        ComandoMover cmd;
        const char* accion = doc["accion"] | "parar";
        strncpy(cmd.accion, accion, sizeof(cmd.accion) - 1);
        cmd.accion[sizeof(cmd.accion) - 1] = '\0';
        cmd.ms = doc["ms"] | 0;

        xQueueReset(colaMover);
        xQueueSend(colaMover, &cmd, portMAX_DELAY);
      } 
      else if (strcmp(tipo, "servo") == 0) {
        int ang = doc["angulo"] | 90;
        anguloActual = constrain(ang, 0, 180);
        servo.write(anguloActual);
      }
      else if (strcmp(tipo, "leer_distancia") == 0) {
        distanciaActual = medirDistancia();
        JsonDocument docDist;
        docDist["tipo"] = "distancia_lectura";
        docDist["d"] = round(distanciaActual * 10.0) / 10.0;
        
        String res;
        serializeJson(docDist, res);
        webSocket.sendTXT(num, res);
      }
      else if (strcmp(tipo, "scan") == 0) {
        int ang = doc["angulo"] | 90;
        anguloActual = constrain(ang, 0, 180);
        servo.write(anguloActual);
        
        vTaskDelay(pdMS_TO_TICKS(150));
        distanciaActual = medirDistancia();

        JsonDocument docScan;
        docScan["tipo"] = "scan";
        docScan["angulo"] = anguloActual;
        docScan["d"] = round(distanciaActual * 10.0) / 10.0;
        
        String res;
        serializeJson(docScan, res);
        webSocket.sendTXT(num, res);
      }
      else if (strcmp(tipo, "latido") == 0) {
        JsonDocument docLatido;
        docLatido["tipo"] = "latido_ok";
        
        String res;
        serializeJson(docLatido, res);
        webSocket.sendTXT(num, res);
      }
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------
// SETUP Y LOOP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);
  servo.attach(PIN_SERVO, pulsoMinimo, pulsoMaximo);
  servo.write(anguloActual);

  motores_init_pines();

  colaMover = xQueueCreate(5, sizeof(ComandoMover));
  xTaskCreatePinnedToCore(tareaEjecutarMovimiento, "TareaMover", 2048, NULL, 1, &hTareaMover, 1);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
  }
  
  Serial.println("\nWiFi Conectado!");
  Serial.print("IP asignada: ");
  Serial.println(WiFi.localIP());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  webSocket.loop();

  unsigned long ahora = millis();

  // Muestreo del sensor ultrasónico (Cada 30 ms)
  if (ahora - ultimoMuestreoUS >= 30) {
    ultimoMuestreoUS = ahora;
    distanciaActual = medirDistancia();

    // Bloqueo inmediato de motores si intenta avanzar hacia un obstáculo a <= 25 cm
    if (strcmp(accionActual, "avanzar") == 0 && distanciaActual <= DISTANCIA_SEGURIDAD_CM) {
      motores_parar_total();
      strcpy(accionActual, "parar");
    }
  }
}
