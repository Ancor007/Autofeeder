// Тест веб-сервера ESP8266 в режиме точки доступа (Приложение 1 курсового проекта)
#include <ESP8266WiFi.h>

WiFiServer server(80);

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Создание точки доступа для тестирования
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AutoFeeder_Test", "12345678");

  Serial.println("=== Тест веб-сервера ===");
  Serial.println("Точка доступа создана");
  Serial.print("SSID: AutoFeeder_Test");
  Serial.println(" | Пароль: 12345678");
  Serial.print("IP адрес: ");
  Serial.println(WiFi.softAPIP());

  server.begin();
  Serial.println("Веб-сервер запущен на порту 80");
  Serial.println("Подключитесь к точке доступа и откройте в браузере: http://192.168.4.1");
  Serial.println("Ожидание подключений...\n");
}

void loop() {
  WiFiClient client = server.available();

  if(client) {
    Serial.println(">>> Новое подключение клиента");

    String request = client.readStringUntil('\r');
    Serial.print("Запрос: ");
    Serial.println(request);
    client.flush();

    // Отправка тестовой HTML страницы
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type:text/html; charset=utf-8");
    client.println();
    client.println("<!DOCTYPE html><html><head>");
    client.println("<meta charset='UTF-8'>");
    client.println("<style>body{font-family:Arial;text-align:center;padding:50px;background:#f0f0f0;}");
    client.println("h1{color:#28a745;}p{font-size:18px;}</style>");
    client.println("</head><body>");
    client.println("<h1> Тест веб-сервера успешен!</h1>");
    client.println("<p>Веб-сервер ESP8266 работает корректно.</p>");
    client.println("<p>Устройство: Wemos D1 Mini</p>");
    client.println("<p>IP адрес: 192.168.4.1</p>");
    client.println("</body></html>");

    delay(10);
    client.stop();
    Serial.println("<<< Клиент отключен\n");
  }
}
