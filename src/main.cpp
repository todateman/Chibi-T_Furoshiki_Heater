#include <Arduino.h>
#include <Wire.h>
#include <M5DinMeter.h>
#include <M5_EXTIO2.h>
#include "M5UnitKmeterISO.h"

// エンコーダ
#include "driver/pcnt.h"
#define PULSE_PIN_A 41
#define PULSE_PIN_B 40

// M5_EXTIO2
M5_EXTIO2 extio;
extio_io_mode_t mode = DIGITAL_OUTPUT_MODE;

// 熱電対
M5UnitKmeterISO kmeter;

// 定数の定義
const int pumpPin = 2;    // M5Din Meter本体のポンプ出力用ピン
const int fanPin = 1;     // M5_EXTIO2の冷却ファン用ピン
const int heaterPin = 2;  // M5_EXTIO2のヒーター用ピン
const uint8_t sleeptime = 1;      // 熱電対のスリープ時間(sec)
const int PUMP_CHANNEL = 0;       // PWMのチャンネル
const int PUMP_BASE_FREQ = 1000;  // PWMの周波数
int8_t PUMP_SPEED = 50;   // 冷却水ポンプの速度 (0-100%の範囲)
int8_t HiTemp = 85;       // 上限温度(Celsius)
int8_t LoTemp = 75;       // 下限温度(Celsius)
uint8_t TargetTemp = (HiTemp + LoTemp ) / 2;  // 目標温度(Celsius)
volatile float NowTemperature = 0.0;   // 現在温度(Celsius)
int16_t oldPosition = -999;   // 更新前のエンコーダの値
bool cooling = false;     // 冷却中
bool heating = false;     // 加熱中
enum Mode {temp, pump, Hi, Lo, Mode_NUM};  // 画面遷移モード

// 温度の読み取り
void TempRead(void *pvParameters) {
  static uint8_t error_status;
  static unsigned long getTempTime;     // 温度を読み取った時刻
  while (1) {
    error_status = kmeter.getReadyStatus();
    if (millis() - getTempTime >= sleeptime * 1000) {  // センサーのスリープ時間以上経過したら
      if (error_status == 0) {
        NowTemperature = ((float)(kmeter.getCelsiusTempValue())) / 100;
        //Serial.printf("Celsius Temp: %.2fC\t", NowTemperature);
        //Serial.printf(
        //    "Chip Celsius Temp: %.2fC\r\n",
        //    ((float)(kmeter.getInternalCelsiusTempValue())) / 100);
      } else {
        Serial.printf("Error: %d", kmeter.getReadyStatus());
      }
      getTempTime = millis();
    }
    delay(1);
  }
}

void settingmode(enum Mode setmode, bool flash){
  static uint8_t PUMP_int8t; // ポンプ
  static int16_t newPosition;
  static float OldTemperature = 999.9;
  int8_t count = 0;

  DinMeter.Display.setTextColor(GREEN, BLACK);
  DinMeter.Display.setTextDatum(middle_center);
  DinMeter.Display.setTextFont(&fonts::Orbitron_Light_24);
  DinMeter.Display.setTextSize(1);

  // エンコーダ読み取り
  //newPosition = DinMeter.Encoder.read();
  pcnt_get_counter_value(PCNT_UNIT_0, &newPosition);
  if (newPosition != oldPosition) {
    DinMeter.Speaker.tone(8000, 20);
    if (newPosition < oldPosition) {  // 時計回り
      count++;
    }
    else {                            // 反時計回り
      count--;
    }
    //Serial.printf("%d\t%d\t%d\n", newPosition, oldPosition, count);
    oldPosition = newPosition;
  }

  if (NowTemperature != OldTemperature || flash) {
    DinMeter.Display.drawString(" current: " + String(NowTemperature) + ("C' "), DinMeter.Display.width() / 2, DinMeter.Display.height() / 4 * 1);
    OldTemperature = NowTemperature;
  }

  switch(setmode) {
    case temp:
      DinMeter.Display.drawString(" Target: " + String(TargetTemp) + ("C' "), DinMeter.Display.width() / 2, DinMeter.Display.height() / 4 * 2);
      break;

    case pump:
      if      (count > 0) {PUMP_SPEED += 5;}
      else if (count < 0) {PUMP_SPEED -= 5;}
      PUMP_SPEED = constrain(PUMP_SPEED, 0, 100);
      DinMeter.Display.drawString(" pump: " + String(PUMP_SPEED) + ("% "), DinMeter.Display.width() / 2, DinMeter.Display.height() / 4 * 2);
      PUMP_int8t = map(PUMP_SPEED, 0, 100, 0, 255);
      ledcWrite(PUMP_CHANNEL, PUMP_int8t);
      break;

    case Hi:
      if      (count > 0) {HiTemp++;}
      else if (count < 0) {HiTemp--;}
      HiTemp = constrain(HiTemp, 0, 100);
      DinMeter.Display.drawString(" HiTemp: " + String(HiTemp) + ("C' "), DinMeter.Display.width() / 2, DinMeter.Display.height() / 4 * 2);
      break;

    case Lo:
      if      (count > 0) {LoTemp++;}
      else if (count < 0) {LoTemp--;}
      LoTemp = constrain(LoTemp, 0, 100);
      DinMeter.Display.drawString(" LoTemp: " + String(LoTemp) + ("C' "), DinMeter.Display.width() / 2, DinMeter.Display.height() / 4 * 2);
      break;
  }

  if (cooling) {
    DinMeter.Display.setTextColor(BLACK, BLUE);
  }
  else {
    DinMeter.Display.setTextColor(GREEN, BLACK);
  }
  DinMeter.Display.drawString(" COOL ", DinMeter.Display.width() / 3 * 1 - 10, DinMeter.Display.height() / 4 * 3);
  if (heating) {
    DinMeter.Display.setTextColor(BLACK, ORANGE);
  }
  else {
    DinMeter.Display.setTextColor(GREEN, BLACK);
  }
  DinMeter.Display.drawString(" HEAT ", DinMeter.Display.width() / 3 * 2 + 10, DinMeter.Display.height() / 4 * 3);
}

void setup() {
  auto cfg = M5.config();
  //DinMeter.begin(cfg, true);
  DinMeter.begin(cfg, false);
  Serial.begin(115200);

  // PWMの初期化
  pinMode(pumpPin, OUTPUT);                   // PWM出力を行う端子を出力端子として設定
  ledcSetup(PUMP_CHANNEL, PUMP_BASE_FREQ, 8); // PWM出力波形の初期設定(チャンネル, 周波数 bit)
  ledcAttachPin(pumpPin, PUMP_CHANNEL);       // チャンネルに対する出力端子を設定

  DinMeter.Display.setRotation(1);
  DinMeter.Display.setTextColor(GREEN, BLACK);
  DinMeter.Display.setTextDatum(middle_center);
  DinMeter.Display.setTextFont(&fonts::Orbitron_Light_24);
  DinMeter.Display.setTextSize(1);

  // パルスカウンタの設定
  pcnt_config_t pcnt_config = {};
  pcnt_config.pulse_gpio_num  = PULSE_PIN_A;
  pcnt_config.ctrl_gpio_num   = PULSE_PIN_B;
  pcnt_config.lctrl_mode      = PCNT_MODE_REVERSE;
  pcnt_config.hctrl_mode      = PCNT_MODE_KEEP;
  pcnt_config.pos_mode        = PCNT_COUNT_INC;
  pcnt_config.neg_mode        = PCNT_COUNT_DEC;
  pcnt_config.counter_h_lim   = 32767;
  pcnt_config.counter_l_lim   = -32768;
  pcnt_config.unit            = PCNT_UNIT_0;
  pcnt_config.channel         = PCNT_CHANNEL_0;

  pcnt_unit_config(&pcnt_config);

  pcnt_counter_pause(PCNT_UNIT_0);
  pcnt_counter_clear(PCNT_UNIT_0);
  pcnt_counter_resume(PCNT_UNIT_0);

  // I2Cの初期化
  //Wire.begin((int)SDA, (int)SCL, 400000L);      // Wire.begin(21, 22, 400000L);

  // 熱電対の設定
  while (!kmeter.begin(&Wire, KMETER_DEFAULT_ADDR, (int)SDA, (int)SCL, 100000L)) {
    Serial.println("Unit KmeterISO not found");
  }

  // M5_EXTIO2の設定
  while (!extio.begin(&Wire, (int)SDA, (int)SCL, 0x45)) {
    Serial.println("extio Connect Error");
    delay(100);
  }
  // extio.setAllPinMode(DIGITAL_INPUT_MODE);
  // extio.setAllPinMode(DIGITAL_OUTPUT_MODE);
  // extio.setAllPinMode(ADC_INPUT_MODE);
  // extio.setAllPinMode(SERVO_CTL_MODE);
  // extio.setAllPinMode(RGB_LED_MODE);
  extio.setPinMode(fanPin, DIGITAL_OUTPUT_MODE);
  extio.setPinMode(heaterPin, DIGITAL_OUTPUT_MODE);

  // 冷却水ポンプを常時動作させる
  uint8_t PUMP_int8t = map(PUMP_SPEED, 0, 100, 0, 255);
  ledcWrite(PUMP_CHANNEL, PUMP_int8t);

  xTaskCreateUniversal(
    TempRead,             // 作成するタスク関数
    "TempRead",           // 表示用タスク名
    8192,                 // スタックメモリ量
    NULL,                 // 起動パラメータ
    1,                    // 優先度
    NULL,                 // タスクハンドル
    PRO_CPU_NUM           // 実行するコア
  );

}

void loop() {
  static int setmode;                   // 画面遷移モード
  TargetTemp = (HiTemp + LoTemp ) / 2;  // 目標温度(Celsius)
  bool flash = false;                   // 画面更新

  // ボタンの読み取り
  DinMeter.update();

  // 冷却ファンの制御
  Serial.print("Fan: ");
  if (NowTemperature > HiTemp) {
    if(!cooling){
      cooling = true;
      extio.setDigitalOutput(fanPin, HIGH);
    }
  } else if (NowTemperature < TargetTemp) {
    if(cooling){
      cooling = false;
      extio.setDigitalOutput(fanPin, LOW);
    }
  }
  if (cooling) {
    Serial.print("ON\t");
  }
  else {
    Serial.print("OFF\t");
  }

  // ヒーターの制御
  Serial.print("Heater: ");
  if (NowTemperature < LoTemp) {
    if(!heating){
      heating = true;
      extio.setDigitalOutput(heaterPin, HIGH);
    }
  } else if (NowTemperature > TargetTemp) {
    if(heating){
      heating = false;
      extio.setDigitalOutput(heaterPin, LOW);
    }
  }
  if (heating) {
    Serial.print("ON\t");
  }
  else {
    Serial.print("OFF\t");
  }

  Serial.println("");

  if (DinMeter.BtnA.wasPressed()) {
    DinMeter.Speaker.tone(8000, 20);
    setmode++;
    DinMeter.Display.clear();
    flash = true;
    if (setmode >= Mode_NUM){
      setmode = 0;
    }
  }
  if (DinMeter.BtnA.pressedFor(5000)) {
    //DinMeter.Encoder.write(100);
  }

  // 画面遷移
  settingmode(static_cast<Mode>(setmode), flash);

  //delay(100);
}