/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Скетч для передачи показаний с беспроводных датчиков Oregon Scientific на сервис «Народный мониторинг» (narodmon.ru)
//с помощью Arduino-совместимых плат на основе ESP8266 (Wemos D1, NodeMCU).

//Для подключения необходимы:
//- Сам датчик Oregon Scientific THN132N, THGN132N, THGN123 и т.п.,
//- Плата микроконтроллера на основе ESP8266 (Wemos D1 или NodeMCU),
//- Приёмник OOK 433Мгц (Питание 3В, подключается к D7 платы микроконтроллера),
//- WiFi подключение к Интернет
//- Arduino IDE с установленной поддержкой ESP8266-совместимых устройств и библиотекой Oregon_NR
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <Oregon_NR.h>
#include <ESP8266WiFi.h>

#define TEST_MODE        1              //Режим отладки (данные на narodmon.ru не отсылаются)
#define USE_TYPE_PREFIX  0              //Добавлять префикс типа датчика к номеру канала (1: T301, H301 / 0: T1, H1)

//Кол-во датчиков различных типов, используемых в системе.
//Для экономии памяти можно обнулить неиспользуемые типы датчиков
#define NOF_132     3     //THGN132
#define NOF_500     1     //THGN500
#define NOF_968     1     //BTHR968
#define NOF_129     5     //BTHGN129
#define NOF_318     5     //RTGN318
#define NOF_800     9     //THGR810
#define NOF_THP     8     //THP

#define SEND_INTERVAL 300000            //Интервал отсылки данных на сервер, мс
#define CONNECT_TIMEOUT 10000           //Время ожидания  соединения, мс
#define DISCONNECT_TIMEOUT 10000        //Время ожидания отсоединения, мс
#define WIFI_MAX_ATTEMPTS 30            //Кол-во неудачных попыток подключения к WiFi до перезагрузки модуля (~5 мин при CONNECT_TIMEOUT=10с)

#define mac       "#FF:FF:FF:FF:FF:FF"  //МАС-адрес на narodmon.ru
#define ssid      "ASUS"                //Параметры входа в WiFi
#define password  "asus"

//Анемометр
#define WIND_CORRECTION 0     //Коррекция севера на флюгере в градусах (используется при невозможности сориентировать датчик строго на север)
#define NO_WINDDIR      4     //Кол-во циклов передачи, необходимое для накопления данных о направлении ветра

//Антизалипание влажности при морозе.
//При сильном минусе показания влажности у Oregon часто застывают на одном значении, и narodmon
//присылает уведомление "показания не менялись". Если температура ниже COLD_TEMP_THRESHOLD,
//к влажности добавляется случайная добавка в пределах ±COLD_HUMIDITY_JITTER (%).
//Величина меньше паспортной погрешности датчика (~5%), на интерпретацию данных не влияет.
#define COLD_TEMP_THRESHOLD   -10.0
#define COLD_HUMIDITY_JITTER  0.5

#define  N_OF_THP_SENSORS NOF_132 + NOF_500 + NOF_968 + NOF_129 + NOF_318 + NOF_800 + NOF_THP
//****************************************************************************************

Oregon_NR oregon(13, 13, 2, true); // Приёмник 433Мгц подключён к D7 (GPIO13), Светодиод на D2 подтянут к +пит.

//****************************************************************************************
//Структура для хранения полученных данных от термогигрометров:
struct BTHGN_sensor
{
  byte  number_of_receiving = 0;  //сколько пакетов получено в процессе сбора данных
  unsigned long rcv_time = 7000000;// времена прихода последних пакетов
  byte  chnl;                     //канал передачи
  word  type;                     //Тип датчика
  float temperature;              //Температура
  float humidity;                 //Влажность.
  float pressure;                 //Давление в мм.рт.ст.
  bool  battery;                  //Флаг батареи
  float voltage;                  //Напряжение батареи
};

BTHGN_sensor t_sensor[N_OF_THP_SENSORS];
//****************************************************************************************
//Структура для хранения полученных данных от анемометра:
struct WGR800_sensor
{
  byte  number_of_receiving = 0;  //сколько пакетов получено в процессе сбора данных
  byte  number_of_dir_receiving = 0;  //сколько пакетов получено в процессе сбора данных
  unsigned long rcv_time = 7000000;// времена прихода последних пакетов

  float midspeed;                 //Средняя скорость ветра
  float maxspeed;                 //Порывы ветра
  float direction_x;              // Направление ветра
  float direction_y;
  byte dir_cycle = 0;             //Кол-во циклов накопления данных
  float dysp_wind_dir = -1;
  bool  battery;                 //Флаг батареи
};

WGR800_sensor wind_sensor;

//****************************************************************************************
//Структура для хранения УФ-индекса:
struct UVN800_sensor
{
  byte  number_of_receiving = 0;  //сколько пакетов получено в процессе сбора данных
  unsigned long rcv_time = 7000000;// времена прихода последних пакетов

  float  index;                     //УФ-индекс
  bool battery;                    //Флаг батареи
};

UVN800_sensor uv_sensor;
//****************************************************************************************
//Структура для хранения полученных данных от счётчика осадков:
struct PCR800_sensor
{
  byte  number_of_receiving = 0;  //сколько пакетов получено в процессе сбора данных
  unsigned long rcv_time = 7000000;// времена прихода последних пакетов

  float  rate;                    //Интенсивность осадков
  float  counter;                 //счётчик осадков
  bool battery;                   //Флаг батареи
};

PCR800_sensor rain_sensor;
//****************************************************************************************

#define BLUE_LED 2      //Индикация подключения к WiFi
#define GREEN_LED 14    //Индикатор успешной доставки пакета на народмон
#define RED_LED 255     //Индикатор ошибки доставки пакета на народмон


//Параметры соединения с narodmon:
//IPAddress nardomon_server(94,19,113,221);
char nardomon_server[] = "narodmon.ru";
int port=8283;
WiFiClient client; //Клиент narodmon


const unsigned long postingInterval = SEND_INTERVAL;
unsigned long lastConnectionTime = 0;
boolean lastConnected = false;
unsigned long cur_mark;


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//SETUP//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void setup()
{


  pinMode(BLUE_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  /////////////////////////////////////////////////////
  //Запуск Serial-ов

  Serial.begin(115200);
  Serial.println();
  Serial.println();

  //Инициализация ГПСЧ для джиттера влажности при морозе
  randomSeed(micros());

  if (TEST_MODE) Serial.println(F("TEST MODE"));


  /////////////////////////////////////////////////////
  //Запуск Wifi


  wifi_connect();
  /////////////////////////////////////////////////////


  digitalWrite(BLUE_LED, HIGH);
  if (test_narodmon_connection()){
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, LOW);


  }
  else {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, HIGH);

  }


  //Включение прослушивания радиоканала
  oregon.start();

  //Скрытие логов от Oregon_NR (блоки с сырыми данными: BEFORE и AFTER)
  oregon.receiver_dump = 0;
}
//////////////////////////////////////////////////////////////////////
//LOOP//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

void loop()
{
  //////////////////////////////////////////////////////////////////////
  //Защита от подвисаний/////////////////////////////////////////
  //////////////////////////////////////////////////////////////////////
  if  (micros() > 0xFFF00000) while ( micros() < 0xFFF00000); //Висим секунду до переполнения
  if  (millis() > 0xFFFFFC0F) while ( millis() < 0xFFFFFC0F); //Висим секунду до переполнения


  //////////////////////////////////////////////////////////////////////
  //Проверка полученных данных,/////////////////////////////////////////
  //////////////////////////////////////////////////////////////////////
  bool is_a_data_to_send = false;
  for (int i = 0; i < N_OF_THP_SENSORS; i++){
    if (t_sensor[i].number_of_receiving) is_a_data_to_send = 1;                 // Есть ли данные для отправки?
  }
   if (wind_sensor.number_of_receiving) is_a_data_to_send = 1;                 // Есть ли данные для отправки?
   if (rain_sensor.number_of_receiving) is_a_data_to_send = 1;                 // Есть ли данные для отправки?
   if (uv_sensor.number_of_receiving) is_a_data_to_send = 1;                 // Есть ли данные для отправки?
  //////////////////////////////////////////////////////////////////////
  //Отправка данных на narodmon.ru/////////////////////////////////////
  //////////////////////////////////////////////////////////////////////

  if (millis() - lastConnectionTime > postingInterval && is_a_data_to_send)  {

    if (is_a_data_to_send)
    {
    //Обязательно отключить прослушивание канала
    oregon.stop();


    digitalWrite(BLUE_LED, HIGH);
    if (send_data()){
      digitalWrite(GREEN_LED, HIGH);
      digitalWrite(RED_LED, LOW);

    }
    else {
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(RED_LED, HIGH);

    }

    oregon.start();
    }
    else Serial.println(F("No data to send"));
  }


  //////////////////////////////////////////////////////////////////////
  //Захват пакета,//////////////////////////////////////////////
  //////////////////////////////////////////////////////////////////////
  oregon.capture(0);
  //
  //Захваченные данные годны до следующего вызова capture

  //////////////////////////////////////////////////////////////////////
  //Обработка полученного пакета//////////////////////////////////////////////
  //Обработка полученного пакета//////////////////////////////////////////////
  if (oregon.captured)
  {
    yield();
    //Вывод информации в Serial
    Serial.print ((float) millis() / 1000, 1); //Время
    Serial.print (F("s\t\t"));
    //Версия протокола
    if (oregon.ver == 2) Serial.print(F("  "));
    if (oregon.ver == 3) Serial.print(F("3 "));

    //Информация о восстановлении пакета
    if (oregon.restore_sign & 0x01) Serial.print(F("s")); //восстановлены одиночные такты
    else  Serial.print(F(" "));
    if (oregon.restore_sign & 0x02) Serial.print(F("d")); //восстановлены двойные такты
    else  Serial.print(F(" "));
    if (oregon.restore_sign & 0x04) Serial.print(F("p ")); //исправлена ошибка при распознавании версии пакета
    else  Serial.print(F("  "));
    if (oregon.restore_sign & 0x08) Serial.print(F("r ")); //собран из двух пакетов (для режима сборки в v.2)
    else  Serial.print(F("  "));

    //Вывод полученного пакета.
    for (int q = 0;q < oregon.packet_length; q++)
      if (oregon.valid_p[q] == 0x0F) Serial.print(oregon.packet[q], HEX);
      else Serial.print(F(" "));

    //Время обработки пакета
    Serial.print(F("  "));
    Serial.print(oregon.work_time);
    Serial.print(F("ms "));

    if ((oregon.sens_type == THGN132 ||
    (oregon.sens_type & 0x0FFF) == RTGN318 ||
    (oregon.sens_type & 0x0FFF) == RTHN318 ||
    oregon.sens_type == THGR810 ||
    oregon.sens_type == THN132 ||
    oregon.sens_type == THN800 ||
    oregon.sens_type == BTHGN129 ||
    oregon.sens_type == BTHR968 ||
    oregon.sens_type == THGN500) && oregon.crc_c)
    {
      Serial.print(F("\t"));
      if (oregon.sens_type == THGN132) Serial.print(F("THGN132N"));
      if (oregon.sens_type == THGN500) Serial.print(F("THGN500 "));
      if (oregon.sens_type == THGR810) Serial.print(F("THGR810 "));
      if ((oregon.sens_type & 0x0FFF) == RTGN318) Serial.print(F("RTGN318 "));
      if ((oregon.sens_type & 0x0FFF) == RTHN318) Serial.print(F("RTHN318 "));
      if (oregon.sens_type == THN132 ) Serial.print(F("THN132N "));
      if (oregon.sens_type == THN800 ) Serial.print(F("THN800  "));
      if (oregon.sens_type == BTHGN129 ) Serial.print(F("BTHGN129"));
      if (oregon.sens_type == BTHR968 ) Serial.print(F("BTHR968 "));

      if (oregon.sens_type != BTHR968 && oregon.sens_type != THGN500)
      {
        Serial.print(F(" CHNL: "));
        Serial.print(oregon.sens_chnl);
      }
      else Serial.print(F("        "));
      Serial.print(F(" BAT: "));
      if (oregon.sens_battery) Serial.print(F("F ")); else Serial.print(F("e "));
      Serial.print(F("ID: "));
      Serial.print(oregon.sens_id, HEX);

      if (oregon.sens_tmp >= 0 && oregon.sens_tmp < 10) Serial.print(F(" TMP:  "));
      if (oregon.sens_tmp < 0 && oregon.sens_tmp >-10) Serial.print(F(" TMP: "));
      if (oregon.sens_tmp <= -10) Serial.print(F(" TMP:"));
      if (oregon.sens_tmp >= 10) Serial.print(F(" TMP: "));
      Serial.print(oregon.sens_tmp, 1);
      Serial.print(F("C "));
      if (oregon.sens_type == THGN132 ||
          oregon.sens_type == THGR810 ||
          oregon.sens_type == BTHGN129 ||
          oregon.sens_type == BTHR968 ||
          (oregon.sens_type & 0x0FFF) == RTGN318 ||
          oregon.sens_type == THGN500 ) {
        Serial.print(F("HUM: "));
        Serial.print(oregon.sens_hmdty, 0);
        Serial.print(F("%"));
      }
      else Serial.print(F("        "));

      if (oregon.sens_type == BTHGN129 ||  oregon.sens_type == BTHR968)
      {
      Serial.print(F(" PRESS: "));
      Serial.print(oregon.get_pressure(), 1);
      Serial.print(F("Hgmm "));
      }

      if (oregon.sens_type == THGN132 && oregon.sens_chnl > NOF_132) {Serial.println(); return;}
      if (oregon.sens_type == THGN500 && NOF_500 == 0) {Serial.println(); return;}
      if (oregon.sens_type == BTHR968 && NOF_968 == 0) {Serial.println(); return;}
      if (oregon.sens_type == THGR810 && oregon.sens_chnl > NOF_800) {Serial.println(); return;}
      if (oregon.sens_type == BTHGN129 && oregon.sens_chnl > NOF_129) {Serial.println(); return;}
      if ((oregon.sens_type & 0x0FFF) == RTGN318 && oregon.sens_chnl > NOF_318) {Serial.println(); return;}

      byte _chnl = oregon.sens_chnl - 1;

      if (oregon.sens_type == THGN500) _chnl = NOF_132;
      if (oregon.sens_type == BTHR968 ) _chnl = NOF_132 + NOF_500;
      if (oregon.sens_type == BTHGN129 ) _chnl = NOF_132 + NOF_500 + NOF_968;
      if (oregon.sens_type == THGR810 || oregon.sens_type == THN800) _chnl = NOF_132 + NOF_500 + NOF_968 + NOF_129;
      if ((oregon.sens_type & 0x0FFF) == RTGN318 || (oregon.sens_type & 0x0FFF) == RTHN318) _chnl  = NOF_132 + NOF_500 + NOF_968 + NOF_129 + NOF_800;

      t_sensor[ _chnl].chnl = oregon.sens_chnl;
      t_sensor[ _chnl].number_of_receiving++;
      t_sensor[ _chnl].type = oregon.sens_type;
      t_sensor[ _chnl].battery = oregon.sens_battery;
      t_sensor[ _chnl].pressure = t_sensor[ _chnl].pressure * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.get_pressure() / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].temperature = t_sensor[ _chnl].temperature * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.sens_tmp / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].humidity = t_sensor[ _chnl].humidity * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.sens_hmdty / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].rcv_time = millis();
    }

    if (oregon.sens_type == PCR800 && oregon.crc_c)
    {
      Serial.print(F("\tPCR800  "));
      Serial.print(F("        "));
      Serial.print(F(" BAT: "));
      if (oregon.sens_battery) Serial.print(F("F ")); else Serial.print(F("e "));
      Serial.print(F(" ID: "));
      Serial.print(oregon.sens_id, HEX);
      Serial.print(F("   TOTAL: "));
      Serial.print(oregon.get_total_rain(), 1);
      Serial.print(F("mm  RATE: "));
      Serial.print(oregon.get_rain_rate(), 1);
      Serial.print(F("mm/h"));
      rain_sensor.number_of_receiving++;
      rain_sensor.battery = oregon.sens_battery;
      rain_sensor.rate = oregon.get_rain_rate();
      rain_sensor.counter = oregon.get_total_rain();
      rain_sensor.rcv_time = millis();
    }

  if (oregon.sens_type == WGR800 && oregon.crc_c){
      Serial.print(F("\tWGR800  "));
      Serial.print(F("        "));
      Serial.print(F(" BAT: "));
      if (oregon.sens_battery) Serial.print(F("F ")); else Serial.print(F("e "));
      Serial.print(F("ID: "));
      Serial.print(oregon.sens_id, HEX);

      Serial.print(F(" AVG: "));
      Serial.print(oregon.sens_avg_ws, 1);
      Serial.print(F("m/s  MAX: "));
      Serial.print(oregon.sens_max_ws, 1);
      Serial.print(F("m/s  DIR: ")); //N = 0, E = 4, S = 8, W = 12
      switch (oregon.sens_wdir)
      {
      case 0: Serial.print(F("N")); break;
      case 1: Serial.print(F("NNE")); break;
      case 2: Serial.print(F("NE")); break;
      case 3: Serial.print(F("NEE")); break;
      case 4: Serial.print(F("E")); break;
      case 5: Serial.print(F("SEE")); break;
      case 6: Serial.print(F("SE")); break;
      case 7: Serial.print(F("SSE")); break;
      case 8: Serial.print(F("S")); break;
      case 9: Serial.print(F("SSW")); break;
      case 10: Serial.print(F("SW")); break;
      case 11: Serial.print(F("SWW")); break;
      case 12: Serial.print(F("W")); break;
      case 13: Serial.print(F("NWW")); break;
      case 14: Serial.print(F("NW")); break;
      case 15: Serial.print(F("NNW")); break;
      }

      wind_sensor.battery = oregon.sens_battery;
      wind_sensor.number_of_receiving++;
      wind_sensor.number_of_dir_receiving++;

      //Средняя скорость
      wind_sensor.midspeed = wind_sensor.midspeed * (((float)wind_sensor.number_of_receiving - 1) / (float)wind_sensor.number_of_receiving) + oregon.sens_avg_ws / wind_sensor.number_of_receiving;

      //Порывы
      if (oregon.sens_max_ws > wind_sensor.maxspeed || wind_sensor.number_of_receiving == 1) wind_sensor.maxspeed = oregon.sens_max_ws;

      //Направление
      //Вычисляется вектор - его направление и модуль.
      if (wind_sensor.number_of_dir_receiving == 1 && (wind_sensor.direction_x != 0 || wind_sensor.direction_y != 0))
      {
        float wdiv = sqrt((wind_sensor.direction_x * wind_sensor.direction_x) + (wind_sensor.direction_y * wind_sensor.direction_y));
        wind_sensor.direction_x /= wdiv;
        wind_sensor.direction_y /= wdiv;
      }

      //Единичные векторы для 16 румбов (индекс = oregon.sens_wdir, 0=N, 4=E, 8=S, 12=W)
      static const float wind_dir_vec[16][2] = {
        { 1.00f,  0.00f}, { 0.92f, -0.38f}, { 0.71f, -0.71f}, { 0.38f, -0.92f},
        { 0.00f, -1.00f}, {-0.38f, -0.92f}, {-0.71f, -0.71f}, {-0.92f, -0.38f},
        {-1.00f,  0.00f}, {-0.92f,  0.38f}, {-0.71f,  0.71f}, {-0.38f,  0.92f},
        { 0.00f,  1.00f}, { 0.38f,  0.92f}, { 0.71f,  0.71f}, { 0.92f,  0.38f}
      };
      if (oregon.sens_wdir < 16) {
        wind_sensor.direction_x += wind_dir_vec[oregon.sens_wdir][0];
        wind_sensor.direction_y += wind_dir_vec[oregon.sens_wdir][1];
      }
      wind_sensor.rcv_time = millis();
    }

    if (oregon.sens_type == UVN800 && oregon.crc_c)
    {
      Serial.print(F("\tUVN800  "));
      Serial.print(F("        "));
      Serial.print(F(" BAT: "));
      if (oregon.sens_battery) Serial.print(F("F ")); else Serial.print(F("e "));
      Serial.print(F("ID: "));
      Serial.print(oregon.sens_id, HEX);

      Serial.print(F(" UV IDX: "));
      Serial.print(oregon.UV_index);

      uv_sensor.number_of_receiving++;
      uv_sensor.battery = oregon.sens_battery;
      uv_sensor.index = uv_sensor.index * ((float)(uv_sensor.number_of_receiving - 1) / (float)uv_sensor.number_of_receiving) + oregon.UV_index / uv_sensor.number_of_receiving;
      uv_sensor.rcv_time = millis();
    }

    if (oregon.sens_type == RFCLOCK && oregon.crc_c){
      Serial.print(F("\tRF CLOCK"));
      Serial.print(F(" CHNL: "));
      Serial.print(oregon.sens_chnl);
      Serial.print(F(" BAT: "));
      if (oregon.sens_battery) Serial.print(F("F ")); else Serial.print(F("e "));
      Serial.print(F("ID: "));
      Serial.print(oregon.sens_id, HEX);
      Serial.print(F(" TIME: "));
      Serial.print(oregon.packet[6] & 0x0F, HEX);
      Serial.print((oregon.packet[6] & 0xF0) >> 4, HEX);
      Serial.print(':');
      Serial.print(oregon.packet[5] & 0x0F, HEX);
      Serial.print((oregon.packet[5] & 0xF0) >> 4, HEX);
      Serial.print(':');
      Serial.print(oregon.packet[4] & 0x0F, HEX);
      Serial.print((oregon.packet[4] & 0xF0) >> 4, HEX);
      Serial.print(F(" DATE: "));
      Serial.print(oregon.packet[7] & 0x0F, HEX);
      Serial.print((oregon.packet[7] & 0xF0) >> 4, HEX);
      Serial.print('.');
      if ((oregon.packet[8] & 0x0F) == 1 || (oregon.packet[8] & 0x0F) == 3)   Serial.print('1');
      else Serial.print('0');
      Serial.print((oregon.packet[8] & 0xF0) >> 4, HEX);
      Serial.print('.');
      Serial.print(oregon.packet[9] & 0x0F, HEX);
      Serial.print((oregon.packet[9] & 0xF0) >> 4, HEX);

    }

    if (oregon.sens_type == PCR800 && oregon.crc_c){
      Serial.print(F("\tPCR800  "));
      Serial.print(F("        "));
      Serial.print(F(" BAT: "));
      if (oregon.sens_battery) Serial.print(F("F ")); else Serial.print(F("e "));
      Serial.print(F(" ID: "));
      Serial.print(oregon.sens_id, HEX);
      Serial.print(F("   TOTAL: "));
      Serial.print(oregon.get_total_rain(), 1);
      Serial.print(F("mm  RATE: "));
      Serial.print(oregon.get_rain_rate(), 1);
      Serial.print(F("mm/h"));

    }

#if ADD_SENS_SUPPORT == 1
      if ((oregon.sens_type & 0xFF00) == THP && oregon.crc_c) {
      Serial.print(F("\tTHP     "));
      Serial.print(F(" CHNL: "));
      Serial.print(oregon.sens_chnl);
      Serial.print(F(" BAT: "));
      Serial.print(oregon.sens_voltage, 2);
      Serial.print(F("V"));
      if (oregon.sens_tmp > 0 && oregon.sens_tmp < 10) Serial.print(F(" TMP:  "));
      if (oregon.sens_tmp < 0 && oregon.sens_tmp > -10) Serial.print(F(" TMP: "));
      if (oregon.sens_tmp <= -10) Serial.print(F(" TMP:"));
      if (oregon.sens_tmp >= 10) Serial.print(F(" TMP: "));
      Serial.print(oregon.sens_tmp, 1);
      Serial.print(F("C "));
      Serial.print(F("HUM: "));
      Serial.print(oregon.sens_hmdty, 1);
      Serial.print(F("% "));
      Serial.print(F("PRESS: "));
      Serial.print(oregon.sens_pressure, 1);
      Serial.print(F("Hgmm"));
      yield();

      if (oregon.sens_chnl > NOF_THP - 1) {Serial.println(); return;}

      byte _chnl = oregon.sens_chnl  + NOF_132 + NOF_500 + NOF_968 + NOF_129 + NOF_800 + NOF_318;
      t_sensor[ _chnl].chnl = oregon.sens_chnl + 1;
      t_sensor[ _chnl].number_of_receiving++;
      t_sensor[ _chnl].type = oregon.sens_type;
      t_sensor[ _chnl].pressure = t_sensor[ _chnl].pressure * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.sens_pressure / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].temperature = t_sensor[ _chnl].temperature * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.sens_tmp / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].humidity = t_sensor[ _chnl].humidity * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.sens_hmdty / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].voltage = t_sensor[ _chnl].voltage * ((float)(t_sensor[ _chnl].number_of_receiving - 1) / (float)t_sensor[ _chnl].number_of_receiving) + oregon.sens_voltage / t_sensor[ _chnl].number_of_receiving;
      t_sensor[ _chnl].rcv_time = millis();

    }
#endif
    Serial.println();
  }
  yield();
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//***************************************************************************************************************************************
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void wifi_connect() {

  Serial.println();
  Serial.print(F("Connecting to "));
  Serial.print(ssid);
  unsigned long cur_mark = millis();
  bool blink = 0;
  byte attempts = 0;
  //WiFi.config(ip, gateway, subnet);
  //Только режим клиента — гасим встроенную точку доступа ESP8266
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  do {
      while (WiFi.status() != WL_CONNECTED) {
      if (blink) {
        digitalWrite(BLUE_LED, LOW);

      }
      else {
        digitalWrite(BLUE_LED, HIGH);

      }
      blink = !blink;
      delay(500);
      Serial.print(F("."));
      //Подключаемся слишком долго. Переподключаемся....
      if ((millis() - cur_mark) > CONNECT_TIMEOUT){
        attempts++;
        if (attempts >= WIFI_MAX_ATTEMPTS) {
          Serial.println();
          Serial.println(F("WiFi unreachable, rebooting..."));
          delay(1000);
          ESP.restart();
        }
        blink = 0;
        digitalWrite(BLUE_LED, HIGH);
        WiFi.disconnect();
        delay(3000);
        cur_mark = millis();
        WiFi.begin(ssid, password);
      }
    }
  } while (WiFi.status() != WL_CONNECTED);

  Serial.println();
  Serial.println(F("WiFi connected"));
  Serial.println(WiFi.localIP());

}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool test_narodmon_connection() {
  if (TEST_MODE) return true;
  // Проверяем только TCP-соединение, не отправляя данных протокола:
  // отправка "##" без MAC и данных вызывала бы NODATA_ERROR на сервере.
  if (client.connect(nardomon_server, port)) {
    Serial.println(F("narodmon.ru is attainable"));
    client.stop();
    return 1;
  }
  else {
    Serial.println(F("connection to narodmon.ru failed"));
    return 0;
  }
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool send_data() {

  wind_sensor.dir_cycle++;   //Накапливаем циклы для расчёта направления ветра

  //Сначала собираем полезную нагрузку. Если данных от датчиков нет —
  //не подключаемся к серверу и ничего не отправляем (иначе сервер вернёт NODATA_ERROR).
  String payload = buildOregonData();
  if (payload.length() == 0)
  {
    Serial.println(F("No sensor data — skip transmission"));
    lastConnectionTime = millis();
    reset_sensor_counters();
    return false;
  }

  //Если соединения с сервером нет, то переподключаемся
  if (WiFi.status() != WL_CONNECTED) wifi_connect();

  bool what_return = false;
  bool is_connect = true;
  if (!TEST_MODE) is_connect = client.connect(nardomon_server, port);

  if (is_connect) {
    //Отправляем MAC-адрес
    Serial.println(' ');
    String s = mac;
    Serial.println(s);
    if (!TEST_MODE) client.print(s + "\n");
    //Отправляем данные Oregon
    Serial.print(payload);
    if (!TEST_MODE) client.print(payload);

    //Завершаем передачу
    if (!TEST_MODE) client.println(F("##"));
    Serial.println(F("##"));
    //Ждём отключения клиента (сервер сам закрывает соединение после "##")
    cur_mark = millis();
    if (!TEST_MODE)
    {
      while (client.connected())
      {
        yield();
        if (millis() - cur_mark > DISCONNECT_TIMEOUT) break;
      }
    }

    Serial.println(' ');
    if (!TEST_MODE) client.stop();
    what_return = true;
  }
  else {
    Serial.println(F("connection to narodmon.ru failed"));
    if (!TEST_MODE) client.stop();
  }
  lastConnectionTime = millis();

  //Обнуляем флаги полученных данных
  reset_sensor_counters();

  return what_return;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//Сброс счётчиков принятых пакетов после цикла отправки
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void reset_sensor_counters()
{
  for (int i = 0; i < N_OF_THP_SENSORS; i++)
    t_sensor[i].number_of_receiving = 0;

  if (wind_sensor.dir_cycle >= NO_WINDDIR)
  {
    wind_sensor.number_of_dir_receiving = 0;
    wind_sensor.dir_cycle = 0;
  }

  rain_sensor.number_of_receiving = 0;
  wind_sensor.number_of_receiving = 0;
  uv_sensor.number_of_receiving = 0;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
String buildOregonData()
{
  String s = "", pref;
  //Резервируем буфер сразу — чтобы не дробить кучу множеством realloc при +=
  s.reserve(512);

  for (byte i = 0; i < N_OF_THP_SENSORS; i++)
  {
    if (t_sensor[i].number_of_receiving > 0)
    {
      pref = "";
      if (t_sensor[i].type == BTHGN129) pref = "20";
      if (t_sensor[i].type == THGN132 ||t_sensor[i].type == THN132) pref = "30";
      if ((t_sensor[i].type & 0x0FFF) == RTGN318 || (t_sensor[i].type & 0x0FFF) == RTHN318) pref = "40";
      if (t_sensor[i].type == THGN500) pref = "50";
      if ((t_sensor[i].type & 0xFF00) == THP) pref = "70";
      if (t_sensor[i].type == THGR810 ||t_sensor[i].type == THN800) pref = "80";
      if (t_sensor[i].type == BTHR968) pref = "90";

      //Если тип неизвестен — пропускаем сенсор, не льём мусор на сервер
      if (pref.length() == 0) continue;

      s += "#T";
      #if USE_TYPE_PREFIX
      s += pref;
      #endif
      s += t_sensor[i].chnl;
      s += "#";
      s += t_sensor[i].temperature;
      s += "\n";

      if (t_sensor[i].humidity > 0 && t_sensor[i].humidity <= 100 &&
      (t_sensor[i].type == THGN132 ||
       t_sensor[i].type == THGN500 ||
       t_sensor[i].type == THGR810 ||
       (t_sensor[i].type & 0x0FFF) == RTGN318 ||
       t_sensor[i].type == BTHGN129 ||
      #if ADD_SENS_SUPPORT == 1
       (t_sensor[i].type & 0xFF00) == THP ||
      #endif
       t_sensor[i].type == BTHR968))
      {
        float humidity_to_send = t_sensor[i].humidity;
        //При морозе показания влажности у Oregon часто залипают. Добавляем
        //небольшой случайный сдвиг, чтобы narodmon не присылал NODATA-уведомления.
        if (t_sensor[i].temperature < COLD_TEMP_THRESHOLD)
        {
          float jitter = ((float)random(-100, 101) / 100.0f) * COLD_HUMIDITY_JITTER;
          humidity_to_send += jitter;
          if (humidity_to_send < 0)   humidity_to_send = 0;
          if (humidity_to_send > 100) humidity_to_send = 100;
        }

        s += "#H";
        #if USE_TYPE_PREFIX
        s += pref;
        #endif
        s += t_sensor[i].chnl;
        s += "#";
        s += humidity_to_send;
        s += "\n";
      }

      if ((t_sensor[i].type == BTHGN129  ||
      #if ADD_SENS_SUPPORT == 1
      (t_sensor[i].type & 0xFF00) == THP  ||
      #endif
      t_sensor[i].type == BTHR968))
      {
        s += "#P";
        #if USE_TYPE_PREFIX
        s += pref;
        #endif
        s += t_sensor[i].chnl;
        s += "#";
        s += t_sensor[i].pressure;
        s += "\n";
      }
      #if ADD_SENS_SUPPORT == 1
      if ((t_sensor[i].type & 0xFF00) == THP)
      {
        s += "#V";
        #if USE_TYPE_PREFIX
        s += pref;
        #endif
        s += t_sensor[i].chnl;
        s += "#";
        s += t_sensor[i].voltage;
        s += "\n";
      }
    #endif
    }
  }
  //Отправляем данные WGR800
  if (wind_sensor.number_of_receiving > 0)
  {
    s += "#WSMID#";
    s += wind_sensor.midspeed;
    s += '\n';
    s += "#WSMAX#";
    s += wind_sensor.maxspeed;
    s += '\n';
  }

  if (wind_sensor.number_of_dir_receiving > 0 && wind_sensor.dir_cycle >= NO_WINDDIR)
  {
    s += "#DIR#";
    s += calc_wind_direction(&wind_sensor);
    s += '\n';
  }


  //Отправляем данные PCR800
  if (rain_sensor.number_of_receiving > 0)
  {
    s += "#RAIN#";
    s += rain_sensor.counter;
    s += '\n';
  }

  //Отправляем данные UVN800
  if (uv_sensor.number_of_receiving > 0)
  {
    s += "#UV#";
    s += uv_sensor.index;
    s += '\n';
  }

  return s;
}
////////////////////////////////////////////////////////////////////////////////////////
// Расчёт направления ветра
////////////////////////////////////////////////////////////////////////////////////////
float calc_wind_direction(WGR800_sensor* wdata)
{
 if (wdata->direction_x == 0) wdata->direction_x = 0.01;
 float otn = abs(wdata->direction_y / wdata->direction_x);
 float angle = (asin(otn / sqrt(1 + otn * otn))) * 180 / 3.14;

 //Определяем направление
 if (wdata->direction_x > 0 && wdata->direction_y < 0) otn = angle;
 if (wdata->direction_x < 0 && wdata->direction_y < 0) otn = 180 - angle;
 if (wdata->direction_x < 0 && wdata->direction_y >= 0) otn = 180 + angle;
 if (wdata->direction_x > 0 && wdata->direction_y >= 0) otn = 360 - angle;

 angle = otn + WIND_CORRECTION; // Если маркер флюгера направлен не на север
 if (angle >= 360) angle -= 360;

 return angle;
}

////////////////////////////////////////////////////////////////////////////////////////
// ЗАМЕНА DELAY, которая работает и не приводит к вылету...
////////////////////////////////////////////////////////////////////////////////////////
void wait_timer(int del){
  unsigned long tm_marker = millis();
  while (millis() - tm_marker < del) yield();
  return;

}
