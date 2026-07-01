#define BLYNK_TEMPLATE_ID "TMPL2bHv78OAk"
#define BLYNK_TEMPLATE_NAME "ESP32 WIFI"
#define BLYNK_PRINT Serial

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <BlynkSimpleEsp32.h>
#include <esp_eap_client.h>
#include "secrets.h"

#define RGB_COMMON_ANODE false

#define PIN_LED1 2
#define PIN_LED2 15
#define PIN_RGB_R 17
#define PIN_RGB_G 16
#define PIN_RGB_B 4
#define PIN_DHT 18
#define PIN_BTN_SCREEN 26
#define PIN_BTN_RESET 27
#define PIN_SW_BLOCK 34
#define PIN_SW_LED1 32
#define PIN_SW_LED2 35
#define PIN_SW_UNIT 33

#define VP_TEMP_C V0
#define VP_TEMP_F V1
#define VP_HUM V2
#define VP_RSSI V3
#define VP_TMIN V4
#define VP_TMAX V5
#define VP_HMIN V6
#define VP_HMAX V7
#define VP_LED1 V8
#define VP_LED2 V9
#define VP_R V10
#define VP_G V11
#define VP_B V12
#define VP_RESET V13
#define VP_NEXT V14
#define VP_BLOCK V15
#define VP_UNIT V16

#define MS_DHT_READ 2000
#define MS_SEND 5000
#define MS_LCD_AUTO 3000
#define MS_HIST_SLOT 60000
#define MS_DEBOUNCE 40
#define MS_WIFI_TIMEOUT 15000
#define MS_WIFI_RETRY 5000

DHT dht(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);
BlynkTimer blynk_timer;

float temp_c = NAN, temp_f = NAN, humidity = NAN;
float temp_min = NAN, temp_max = NAN;
float hum_min = NAN, hum_max = NAN;

#define HIST_SIZE 60
float hist_temp[HIST_SIZE], hist_hum[HIST_SIZE];
uint8_t hist_count = 0;
float acc_temp = 0, acc_hum = 0;
uint32_t acc_n = 0;

bool led1 = false, led2 = false;
uint8_t rgb_r = 0, rgb_g = 0, rgb_b = 0;

bool sw_block = false, sw_led1 = false, sw_led2 = false, sw_unit = false;
bool sw_led1_last = false, sw_led2_last = false;
bool sw_block_last = false, sw_unit_last = false;

uint8_t screen = 0;
#define N_SCREENS 5

enum NetState
{
    NET_INIT,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_RECONNECTING
};
NetState net_state = NET_INIT;
uint32_t net_state_t = 0;

uint32_t t_dht = 0, t_hist = 0, t_lcd = 0;

bool btn_screen_state = false, btn_screen_last = false;
bool btn_reset_state = false, btn_reset_last = false;
unsigned long btn_screen_t = 0, btn_reset_t = 0;

void go_to_screen(uint8_t s);
void render_lcd();

bool btn_screen_pressed()
{
    bool raw = digitalRead(PIN_BTN_SCREEN);
    unsigned long now = millis();
    if (raw != btn_screen_last)
    {
        btn_screen_last = raw;
        btn_screen_t = now;
    }
    if ((now - btn_screen_t) > MS_DEBOUNCE && raw != btn_screen_state)
    {
        btn_screen_state = raw;
        if (btn_screen_state)
            return true;
    }
    return false;
}

bool btn_reset_pressed()
{
    bool raw = digitalRead(PIN_BTN_RESET);
    unsigned long now = millis();
    if (raw != btn_reset_last)
    {
        btn_reset_last = raw;
        btn_reset_t = now;
    }
    if ((now - btn_reset_t) > MS_DEBOUNCE && raw != btn_reset_state)
    {
        btn_reset_state = raw;
        if (btn_reset_state)
            return true;
    }
    return false;
}

void apply_led1() { digitalWrite(PIN_LED1, led1 ? HIGH : LOW); }
void apply_led2() { digitalWrite(PIN_LED2, led2 ? HIGH : LOW); }

void apply_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_r = r;
    rgb_g = g;
    rgb_b = b;
    ledcWrite(PIN_RGB_R, RGB_COMMON_ANODE ? 255 - r : r);
    ledcWrite(PIN_RGB_G, RGB_COMMON_ANODE ? 255 - g : g);
    ledcWrite(PIN_RGB_B, RGB_COMMON_ANODE ? 255 - b : b);
}

void reset_minmax()
{
    temp_min = temp_max = temp_c;
    hum_min = hum_max = humidity;
}

void read_sensor()
{
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h))
        return;
    temp_c = t;
    temp_f = t * 9.0 / 5.0 + 32.0;
    humidity = h;
    if (isnan(temp_min))
        reset_minmax();
    if (temp_c < temp_min)
        temp_min = temp_c;
    if (temp_c > temp_max)
        temp_max = temp_c;
    if (humidity < hum_min)
        hum_min = humidity;
    if (humidity > hum_max)
        hum_max = humidity;
    acc_temp += temp_c;
    acc_hum += humidity;
    acc_n++;
}

void push_history()
{
    if (acc_n == 0)
        return;
    float mt = acc_temp / acc_n, mh = acc_hum / acc_n;
    acc_temp = 0;
    acc_hum = 0;
    acc_n = 0;
    if (hist_count < HIST_SIZE)
    {
        hist_temp[hist_count] = mt;
        hist_hum[hist_count] = mh;
        hist_count++;
    }
    else
    {
        memmove(hist_temp, hist_temp + 1, (HIST_SIZE - 1) * sizeof(float));
        memmove(hist_hum, hist_hum + 1, (HIST_SIZE - 1) * sizeof(float));
        hist_temp[HIST_SIZE - 1] = mt;
        hist_hum[HIST_SIZE - 1] = mh;
    }
}

void sync_leds()
{
    if (!Blynk.connected())
        return;
    Blynk.virtualWrite(VP_LED1, led1 ? 1 : 0);
    Blynk.virtualWrite(VP_LED2, led2 ? 1 : 0);
}

void sync_switches()
{
    if (!Blynk.connected())
        return;
    Blynk.virtualWrite(VP_BLOCK, sw_block ? 1 : 0);
    Blynk.virtualWrite(VP_UNIT, sw_unit ? 1 : 0);
}

void send_telemetry()
{
    if (!Blynk.connected())
        return;
    Blynk.virtualWrite(VP_TEMP_C, isnan(temp_c) ? 0 : temp_c);
    Blynk.virtualWrite(VP_TEMP_F, isnan(temp_f) ? 0 : temp_f);
    Blynk.virtualWrite(VP_HUM, isnan(humidity) ? 0 : humidity);
    Blynk.virtualWrite(VP_TMIN, isnan(temp_min) ? 0 : temp_min);
    Blynk.virtualWrite(VP_TMAX, isnan(temp_max) ? 0 : temp_max);
    Blynk.virtualWrite(VP_HMIN, isnan(hum_min) ? 0 : hum_min);
    Blynk.virtualWrite(VP_HMAX, isnan(hum_max) ? 0 : hum_max);
    Blynk.virtualWrite(VP_RSSI, WiFi.RSSI());
    if (screen == 4)
        render_lcd();
}

BLYNK_CONNECTED()
{
    Blynk.syncAll();
    sync_leds();
    sync_switches();
}

BLYNK_WRITE(VP_LED1)
{
    if (sw_block)
    {
        sync_leds();
        return;
    }
    led1 = param.asInt();
    apply_led1();
}

BLYNK_WRITE(VP_LED2)
{
    if (sw_block)
    {
        sync_leds();
        return;
    }
    led2 = param.asInt();
    apply_led2();
}

BLYNK_WRITE(VP_R) { apply_rgb((uint8_t)param.asInt(), rgb_g, rgb_b); }
BLYNK_WRITE(VP_G) { apply_rgb(rgb_r, (uint8_t)param.asInt(), rgb_b); }
BLYNK_WRITE(VP_B) { apply_rgb(rgb_r, rgb_g, (uint8_t)param.asInt()); }

BLYNK_WRITE(VP_RESET)
{
    if (param.asInt() == 1)
    {
        reset_minmax();
        render_lcd();
    }
}

BLYNK_WRITE(VP_NEXT)
{
    if (param.asInt() == 1)
        go_to_screen(screen + 1);
}

const char *net_state_str()
{
    switch (net_state)
    {
    case NET_INIT:
        return "Iniciando";
    case NET_CONNECTING:
        return "Conectando";
    case NET_CONNECTED:
        return Blynk.connected() ? "Blynk: OK" : "WiFi OK";
    case NET_RECONNECTING:
        return "Reconectando";
    }
    return "?";
}

void render_lcd()
{
    lcd.clear();
    float tc = isnan(temp_c) ? 0 : temp_c;
    float tf = isnan(temp_f) ? 0 : temp_f;
    float uh = isnan(humidity) ? 0 : humidity;

    switch (screen)
    {
    case 0:
        lcd.setCursor(0, 0);
        lcd.print("Temp: ");
        lcd.print(tc, 1);
        lcd.print((char)223);
        lcd.print("C");
        lcd.setCursor(0, 1);
        lcd.print("Hum:  ");
        lcd.print(uh, 1);
        lcd.print("%");
        break;
    case 1:
        lcd.setCursor(0, 0);
        lcd.print("Temp: ");
        lcd.print(tf, 1);
        lcd.print((char)223);
        lcd.print("F");
        lcd.setCursor(0, 1);
        lcd.print("Hum:  ");
        lcd.print(uh, 1);
        lcd.print("%");
        break;
    case 2:
        lcd.setCursor(0, 0);
        lcd.print("Tmin: ");
        lcd.print(isnan(temp_min) ? 0 : temp_min, 1);
        lcd.print((char)223);
        lcd.print("C");
        lcd.setCursor(0, 1);
        lcd.print("Tmax: ");
        lcd.print(isnan(temp_max) ? 0 : temp_max, 1);
        lcd.print((char)223);
        lcd.print("C");
        break;
    case 3:
        lcd.setCursor(0, 0);
        lcd.print("Hmin: ");
        lcd.print(isnan(hum_min) ? 0 : hum_min, 1);
        lcd.print("%");
        lcd.setCursor(0, 1);
        lcd.print("Hmax: ");
        lcd.print(isnan(hum_max) ? 0 : hum_max, 1);
        lcd.print("%");
        break;
    case 4:
        lcd.setCursor(0, 0);
        lcd.print(net_state_str());
        lcd.setCursor(0, 1);
        if (net_state == NET_CONNECTED)
        {
            lcd.print("RSSI: ");
            lcd.print(WiFi.RSSI());
            lcd.print("dBm");
        }
        else
            lcd.print("Aguardando...");
        break;
    }
}

void go_to_screen(uint8_t s)
{
    screen = s % N_SCREENS;
    t_lcd = millis();
    render_lcd();
    if (Blynk.connected())
        Blynk.virtualWrite(VP_NEXT, screen);
}

void read_switches()
{
    bool nb = digitalRead(PIN_SW_BLOCK);
    bool n1 = digitalRead(PIN_SW_LED1);
    bool n2 = digitalRead(PIN_SW_LED2);
    bool nu = digitalRead(PIN_SW_UNIT);

    bool old_block = sw_block, old_unit = sw_unit;
    bool old_led1 = sw_led1_last, old_led2 = sw_led2_last;

    sw_block = nb;
    sw_unit = nu;
    sw_led1 = n1;
    sw_led2 = n2;

    if (nb != old_block || nu != old_unit)
        sync_switches();

    if (n1 != old_led1)
    {
        sw_led1_last = n1;
        led1 = n1;
        apply_led1();
        sync_leds();
    }
    if (n2 != old_led2)
    {
        sw_led2_last = n2;
        led2 = n2;
        apply_led2();
        sync_leds();
    }
}

void begin_wifi()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_eap_client_set_identity((uint8_t *)SECRET_EAP_IDENTITY, strlen(SECRET_EAP_IDENTITY));
    esp_eap_client_set_username((uint8_t *)SECRET_EAP_IDENTITY, strlen(SECRET_EAP_IDENTITY));
    esp_eap_client_set_password((uint8_t *)SECRET_EAP_PASSWORD, strlen(SECRET_EAP_PASSWORD));
    esp_wifi_sta_enterprise_enable();
    WiFi.begin("eduroam");
}

void handle_net()
{
    uint32_t now = millis();
    switch (net_state)
    {
    case NET_INIT:
        begin_wifi();
        net_state = NET_CONNECTING;
        net_state_t = now;
        break;
    case NET_CONNECTING:
        if (WiFi.status() == WL_CONNECTED)
        {
            net_state = NET_CONNECTED;
        }
        else if (now - net_state_t > MS_WIFI_TIMEOUT)
        {
            net_state = NET_RECONNECTING;
            net_state_t = now;
        }
        break;
    case NET_CONNECTED:
        if (WiFi.status() != WL_CONNECTED)
        {
            net_state = NET_RECONNECTING;
            net_state_t = now;
        }
        break;
    case NET_RECONNECTING:
        if (now - net_state_t > MS_WIFI_RETRY)
        {
            net_state = NET_INIT;
        }
        break;
    }
}

void setup()
{
    Serial.begin(115200);

    pinMode(PIN_LED1, OUTPUT);
    pinMode(PIN_LED2, OUTPUT);
    digitalWrite(PIN_LED1, LOW);
    digitalWrite(PIN_LED2, LOW);

    ledcAttach(PIN_RGB_R, 5000, 8);
    ledcAttach(PIN_RGB_G, 5000, 8);
    ledcAttach(PIN_RGB_B, 5000, 8);
    apply_rgb(0, 0, 0);

    pinMode(PIN_BTN_SCREEN, INPUT);
    pinMode(PIN_BTN_RESET, INPUT);
    pinMode(PIN_SW_BLOCK, INPUT);
    pinMode(PIN_SW_LED1, INPUT);
    pinMode(PIN_SW_LED2, INPUT);
    pinMode(PIN_SW_UNIT, INPUT);

    Wire.begin();
    Wire.setClock(100000);
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ESP32 WiFi TEMP");
    lcd.setCursor(0, 1);
    lcd.print("Iniciando...");

    dht.begin();

    Blynk.config(BLYNK_AUTH_TOKEN);
    net_state = NET_INIT;

    blynk_timer.setInterval(MS_SEND, send_telemetry);

    delay(1500);
    read_sensor();
    if (!isnan(temp_c))
        reset_minmax();

    sw_led1 = sw_led1_last = digitalRead(PIN_SW_LED1);
    sw_led2 = sw_led2_last = digitalRead(PIN_SW_LED2);
    sw_block = sw_block_last = digitalRead(PIN_SW_BLOCK);
    sw_unit = sw_unit_last = digitalRead(PIN_SW_UNIT);
    led1 = sw_led1;
    led2 = sw_led2;
    apply_led1();
    apply_led2();

    uint32_t now = millis();
    t_dht = t_hist = t_lcd = now;

    lcd.clear();
    go_to_screen(0);
}

void loop()
{
    uint32_t now = millis();

    handle_net();

    if (net_state == NET_CONNECTED)
    {
        Blynk.run();
    }

    blynk_timer.run();

    if (btn_screen_pressed())
        go_to_screen(screen + 1);
    if (btn_reset_pressed())
    {
        reset_minmax();
        render_lcd();
    }

    read_switches();

    if (now - t_dht >= MS_DHT_READ)
    {
        t_dht = now;
        read_sensor();
        if (screen <= 3)
            render_lcd();
    }

    if (now - t_hist >= MS_HIST_SLOT)
    {
        t_hist = now;
        push_history();
    }

    if (now - t_lcd >= MS_LCD_AUTO)
    {
        t_lcd = now;
        go_to_screen(screen + 1);
    }
}