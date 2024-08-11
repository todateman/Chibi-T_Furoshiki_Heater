#include <Arduino.h>
#include <Wire.h>
#include <M5DinMeter.h>
// #include <M5_EXTIO2.h>
#include "M5UnitKmeterISO.h"

//EEPROM
#include <EEPROM.h>
int addr = 0;       // EEPROMのスタートアドレス
#define SIZE 32     // EEPROMのサイズ

// BLE Peripheral
#include <BLEDevice.h>
#include <BLE2902.h>

// BLE サービスとキャラクタリスティックのUUIDを定義 https://www.uuidgenerator.net/version4
#define SERVICE_UUID "7c44181A-c1a4-4635-a119-b490ed272552"
#define CHARACTERISTIC_UUID "7c442A00-c1a4-4635-a119-b490ed272552"
#define NOTIFY_CHARACTERISTIC_UUID "7c442A6E-c1a4-4635-a119-b490ed272552"

// キャラクタリスティックの初期データ
std::string initialData = "Engine Temp";
BLECharacteristic *pCharacteristic;       // Read/Write
BLECharacteristic *pNotifyCharacteristic; // Notify

// エンコーダ
#include "driver/pcnt.h"
#define PULSE_PIN_A 41
#define PULSE_PIN_B 40

// M5_EXTIO2
// M5_EXTIO2 extio;
// extio_io_mode_t mode = DIGITAL_OUTPUT_MODE;

// 熱電対
M5UnitKmeterISO kmeter;

// 定数の定義
const int pumpPin = 2;    // M5Din Meter本体のポンプ出力用ピン
// const int fanPin = 1;     // M5_EXTIO2の冷却ファン用ピン
// const int heaterPin = 2;  // M5_EXTIO2のヒーター用ピン
const uint8_t sleeptime = 1;      // 熱電対のスリープ時間(sec)
const int PUMP_CHANNEL = 0;       // PWMのチャンネル
const int PUMP_BASE_FREQ = 1000;  // PWMの周波数
int8_t PUMP_SPEED = 50;   // 保温水ポンプの速度 (0-100%の範囲)
int8_t HiTemp = 85;       // 上限温度(Celsius)
int8_t LoTemp = 75;       // 下限温度(Celsius)
uint8_t TargetTemp = (HiTemp + LoTemp ) / 2;  // 目標温度(Celsius)
volatile float NowTemperature = 0.0;   // 現在温度(Celsius)
int16_t oldPosition = -999;   // 更新前のエンコーダの値
bool cooling = false;         // 冷却中
bool heating = false;         // 加熱中
bool BLEPeripheral = true;    // BLE Peripheral有効/無効
enum Mode {temp, pump, Hi, Lo, Mode_NUM};  // 画面遷移モード
bool save = false;            // EEPROMに保存
unsigned long saveTime = 0;   // EEPROMに保存した時刻

// BLE Serverのコールバックで接続に対する処理を行う https://qiita.com/IRumA/items/00fc746892570f8d1c38
class ServerCallbacks : public BLEServerCallbacks {
  // 接続時に呼び出される
  void onConnect(BLEServer* pServer) {
    // 接続されたらAdvertisingを停止する
    BLEDevice::stopAdvertising();
  }

  // 切断された時に呼び出される
  void onDisconnect(BLEServer* pServer) {
    // 再接続のためにもう一度Advertisingする
    BLEDevice::startAdvertising();
  }
};

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

        std::string newData = String(NowTemperature).c_str();   //BLE Peripheral用にデータを格納
        //pCharacteristic->setValue(newData);                     //BLE PeripheralのReadコマンドでデータを送信
        pNotifyCharacteristic->setValue(newData);               //BLE PeripheralのNotifyコマンドでデータを送信
        pNotifyCharacteristic->notify();
      } else {
        Serial.printf("Error: %d", kmeter.getReadyStatus());
      }
      getTempTime = millis();
    }
    delay(1);
  }
}

// 画面遷移
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

  // 温度が更新された場合
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

  // EEPROMの初期化
  EEPROM.begin(SIZE);

  // ボタンを押したまま起動した場合は初期値をEEPROMに復元する
  if (DinMeter.BtnA.isPressed()) {
    EEPROM.put(0, PUMP_SPEED);                  // ポンプ速度をEEPROMに書き込み
    EEPROM.put(sizeof(PUMP_SPEED), HiTemp);     // 上限温度をEEPROMに書き込み
    EEPROM.put(sizeof(HiTemp), LoTemp);         // 下限温度をEEPROMに書き込み
    Serial.println("Settings saved to EEPROM.");
    if (EEPROM.commit()) {
      Serial.println("EEPROM successfully committed");
    } else {
      Serial.println("ERROR! EEPROM commit failed");
    }
  }
  EEPROM.get(addr, PUMP_SPEED);                                     // ポンプ速度をEEPROMから読み取り
  EEPROM.get(addr + sizeof(PUMP_SPEED), HiTemp);                    // 上限温度をEEPROMから読み取り
  EEPROM.get(addr + sizeof(PUMP_SPEED) + sizeof(HiTemp), LoTemp);   // 下限温度をEEPROMから読み取り

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
  /*
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
  */

  if(BLEPeripheral){
    // BLEデバイスの初期化
    BLEDevice::init("M5Din Furoshiki Heater");  
    // BLEサーバの作成
    BLEServer *pServer = BLEDevice::createServer();
    // 再接続時のコールバック処理の作成
    pServer->setCallbacks(new ServerCallbacks());
    // BLEサービスの作成
    BLEService *pService = pServer->createService(SERVICE_UUID);
    // BLEキャラクタリスティックの作成
    pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE
                    );
    // キャラクタリスティックに初期データを設定
    pCharacteristic->setValue(initialData);
    pCharacteristic->addDescriptor(new BLE2902());
    // 権限を最小にするためにNotify用のCharacteristicはReadWrite用とは別に定義
    pNotifyCharacteristic = pService->createCharacteristic(
                            NOTIFY_CHARACTERISTIC_UUID,
                            BLECharacteristic::PROPERTY_NOTIFY);
    pNotifyCharacteristic->addDescriptor(new BLE2902());
    // サービスの開始
    pService->start();
    // アドバタイジングの開始
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // Functions that help with iPhone connections issue
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.println("Characteristic defined! Now you can read it in your phone!");
  }

  // エンジン温度取得タスクを生成
  xTaskCreateUniversal(
    TempRead,             // 作成するタスク関数
    "TempRead",           // 表示用タスク名
    8192,                 // スタックメモリ量
    NULL,                 // 起動パラメータ
    2,                    // 優先度
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
    }
  } else if (NowTemperature < TargetTemp) {
    if(cooling){
      cooling = false;
    }
  }
  if (cooling) {
    Serial.print("ON\t");
    // extio.setDigitalOutput(fanPin, HIGH);
    PUMP_SPEED = 0;                           // 保温水ポンプを止める
  }
  else {
    Serial.print("OFF\t");
    // extio.setDigitalOutput(fanPin, LOW);
  }

  // ヒーターの制御
  Serial.print("Heater: ");
  if (NowTemperature < LoTemp) {
    if(!heating){
      heating = true;
    }
  } else if (NowTemperature > TargetTemp) {
    if(heating){
      heating = false;
    }
  }
  if (heating) {
    Serial.print("ON\t");
    // extio.setDigitalOutput(heaterPin, HIGH);
  }
  else {
    Serial.print("OFF\t");
    // extio.setDigitalOutput(heaterPin, LOW);
  }

  Serial.println("");

  // 保温水ポンプを動作させる
  uint8_t PUMP_int8t = map(PUMP_SPEED, 0, 100, 0, 255);
  ledcWrite(PUMP_CHANNEL, PUMP_int8t);

  // 画面遷移のためのモード切替
  if (DinMeter.BtnA.wasPressed()) {
    DinMeter.Speaker.tone(8000, 20);
    setmode++;
    DinMeter.Display.clear(TFT_BLACK);
    flash = true;
    if (setmode >= Mode_NUM){
      setmode = 0;
    }
  }

  // EEPROMに設定値を書き込み
  if (!save && DinMeter.BtnA.pressedFor(2000)) {
    EEPROM.put(addr, PUMP_SPEED);                                     // ポンプ速度をEEPROMに書き込み
    EEPROM.put(addr + sizeof(PUMP_SPEED), HiTemp);                    // 上限温度をEEPROMに書き込み
    EEPROM.put(addr + sizeof(PUMP_SPEED) + sizeof(HiTemp), LoTemp);   // 下限温度をEEPROMに書き込み
    Serial.println("Settings saved to EEPROM.");
    if (EEPROM.commit()) {
      Serial.println("EEPROM successfully committed");
      DinMeter.Display.clear(TFT_RED);                                // 背景色を赤に設定
    } else {
      Serial.println("ERROR! EEPROM commit failed");
    }
    save = true;
    saveTime = millis();
  }

  if (save && millis() - saveTime > 2000) {                           // EEPROMに保存後2000msecを過ぎたら
    DinMeter.Display.clear(TFT_BLACK);
    save = false;
  }

  // 画面遷移
  settingmode(static_cast<Mode>(setmode), flash);

  //delay(100);
}