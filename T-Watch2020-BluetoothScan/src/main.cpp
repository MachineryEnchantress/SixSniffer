/*
Copyright (c) 2019 lewis he
This is just a demonstration. Most of the functions are not implemented.
The main implementation is low-power standby.
The off-screen standby (not deep sleep) current is about 4mA.
Select standard motherboard and standard backplane for testing.
Created by Lewis he on October 10, 2019.
*/

// Please select the model you want to use in config.h
#include "config.h"
#include "TTGO.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include <soc/rtc.h>
#include "esp_wifi.h"
#include "esp_sleep.h"
#include <WiFi.h>
#include "gui.h"
#include "NimBLEDevice.h"
#include "SdFat.h"
#include "sdios.h"

#define G_EVENT_VBUS_PLUGIN _BV(0)
#define G_EVENT_VBUS_REMOVE _BV(1)
#define G_EVENT_CHARGE_DONE _BV(2)

#define G_EVENT_WIFI_SCAN_START _BV(3)
#define G_EVENT_WIFI_SCAN_DONE _BV(4)
#define G_EVENT_WIFI_CONNECTED _BV(5)
#define G_EVENT_WIFI_BEGIN _BV(6)
#define G_EVENT_WIFI_OFF _BV(7)

enum
{
  Q_EVENT_WIFI_SCAN_DONE,
  Q_EVENT_WIFI_CONNECT,
  Q_EVENT_BMA_INT,
  Q_EVENT_AXP_INT,
  Q_EVENT_BLESCAN_ADD_ONE,
};

#define DEFAULT_SCREEN_TIMEOUT 30 * 1000

#define WATCH_FLAG_SLEEP_MODE _BV(1)
#define WATCH_FLAG_SLEEP_EXIT _BV(2)
#define WATCH_FLAG_BMA_IRQ _BV(3)
#define WATCH_FLAG_AXP_IRQ _BV(4)

QueueHandle_t g_event_queue_handle = NULL;
QueueHandle_t g_ble_uuid_queue_handle = NULL;
EventGroupHandle_t g_event_group = NULL;
bool lenergy = false;
TTGOClass *ttgo;
NimBLEScan *pBLEScan;
int recordCount = 1;

void setupNetwork()
{
  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
               { xEventGroupClearBits(g_event_group, G_EVENT_WIFI_CONNECTED); },
               WiFiEvent_t::SYSTEM_EVENT_STA_DISCONNECTED);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
               {
        uint8_t data = Q_EVENT_WIFI_SCAN_DONE;
        xQueueSend(g_event_queue_handle, &data, portMAX_DELAY); },
               WiFiEvent_t::SYSTEM_EVENT_SCAN_DONE);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
               { xEventGroupSetBits(g_event_group, G_EVENT_WIFI_CONNECTED); },
               WiFiEvent_t::SYSTEM_EVENT_STA_CONNECTED);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
               { wifi_connect_status(true); },
               WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
}

typedef struct ble_address_t
{
  int rssi;
  uint8_t address[20];
};

//Record bluetooth information and send in queue
class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
  void onResult(NimBLEAdvertisedDevice *advertisedDevice)
  {
    Serial.printf("Advertised Device: %s \n", advertisedDevice->toString().c_str());
    ble_address_t address;
    memset(&address, '\0', sizeof(ble_address_t));
    memcpy(address.address, advertisedDevice->getAddress().toString().c_str(), advertisedDevice->getAddress().toString().length());
    address.rssi = advertisedDevice->getRSSI();
    xQueueSend(g_ble_uuid_queue_handle, &address, 0);
  }
};

void setupBleScan()
{
  NimBLEDevice::setScanFilterMode(CONFIG_BTDM_SCAN_DUPL_TYPE_DEVICE);
  NimBLEDevice::setScanDuplicateCacheSize(200);
  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan(); // create new scan
  // Set the callback for when devices are discovered, no duplicates.
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
  pBLEScan->setActiveScan(true); // Set active scanning, this will get more data from the advertiser.
  pBLEScan->setInterval(97);     // How often the scan occurs / switches channels; in milliseconds,
  pBLEScan->setWindow(37);       // How long to scan during the interval; in milliseconds.
  pBLEScan->setMaxResults(0);    // do not store the scan results, use callback only.
}

void low_energy()
{
  if (ttgo->bl->isOn())
  {
    ttgo->closeBL();
    ttgo->stopLvglTick();
    ttgo->bma->enableStepCountInterrupt(false);
    ttgo->displaySleep();
    if (!WiFi.isConnected())
    {
      lenergy = true;
      WiFi.mode(WIFI_OFF);
      Serial.println("ENTER IN LIGHT SLEEEP MODE");
      gpio_wakeup_enable((gpio_num_t)AXP202_INT, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)BMA423_INT1, GPIO_INTR_HIGH_LEVEL);
      esp_sleep_enable_gpio_wakeup();
      esp_light_sleep_start();
    }
  }
  else
  {
    ttgo->startLvglTick();
    ttgo->displayWakeup();
    ttgo->rtc->syncToSystem();
    updateStepCounter(ttgo->bma->getCounter());
    updateBatteryLevel();
    updateBatteryIcon(LV_ICON_CALCULATION);
    lv_disp_trig_activity(NULL);
    ttgo->openBL();
    ttgo->bma->enableStepCountInterrupt();
  }
}

void SD_Init(void)
{
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI))
  {
    Serial.println("SDCard MOUNT FAIL");
  }
  else
  {
    uint32_t cardSize = SD.cardSize() / (1024 * 1024);
    String str = "SDCard Size: " + String(cardSize) + "MB";
    Serial.println(str);

    if (!SD.exists("/bledata"))
    {
      Serial.println("/bledata creat");
      SD.mkdir("/bledata");
    }
    File fs = SD.open(RESULTS_PATH, FILE_WRITE);
    fs.close();
    fs = SD.open(GOLIST_PATH, FILE_WRITE);
    fs.close();
    fs = SD.open(GOLIST_PATH, FILE_WRITE);
    fs.close();

    if (SD.exists(RESULTS_PATH) && SD.exists(GOLIST_PATH) && SD.exists(GOLIST_PATH))
      Serial.println(".txt Creating a successful");
    else
      Serial.println(".txt Create a failure");
  }
}

void setup()
{
  Serial.begin(115200);

  // Create a program that allows the required message objects and group flags
  g_event_queue_handle = xQueueCreate(20, sizeof(uint8_t));
  g_ble_uuid_queue_handle = xQueueCreate(50, sizeof(ble_address_t));
  g_event_group = xEventGroupCreate();

  ttgo = TTGOClass::getWatch();

  // Initialize TWatch
  ttgo->begin();

  // Turn on the IRQ used
  ttgo->power->adc1Enable(AXP202_BATT_VOL_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1, AXP202_ON);
  ttgo->power->enableIRQ(AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_CHARGING_FINISHED_IRQ, AXP202_ON);
  ttgo->power->clearIRQ();

  // Initialize lvgl
  ttgo->lvgl_begin();

  // Enable BMA423 interrupt ，
  // The default interrupt configuration,
  // you need to set the acceleration parameters, please refer to the BMA423_Accel example
  ttgo->bma->attachInterrupt();

  // Connection interrupted to the specified pin
  pinMode(BMA423_INT1, INPUT);
  attachInterrupt(
      BMA423_INT1, []
      {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        uint8_t data = Q_EVENT_BMA_INT;
        xQueueSendFromISR(g_event_queue_handle, &data, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR ();
        } },
      RISING);

  // Connection interrupted to the specified pin
  pinMode(AXP202_INT, INPUT);
  attachInterrupt(
      AXP202_INT, []
      {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        uint8_t data = Q_EVENT_AXP_INT;
        xQueueSendFromISR(g_event_queue_handle, &data, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR ();
        } },
      FALLING);

  // Check if the RTC clock matches, if not, use compile time
  ttgo->rtc->check();

  // Synchronize time to system time
  ttgo->rtc->syncToSystem();

#ifdef LILYGO_WATCH_HAS_BUTTON

  /*
      ttgo->button->setClickHandler([]() {
          Serial.println("Button2 Pressed");
      });
  */

  // Set the user button long press to restart
  ttgo->button->setLongClickHandler([]()
                                    {
        Serial.println("Pressed Restart Button,Restart now ...");
        delay(1000);
        esp_restart(); });
#endif

  // Setting up the network
  setupNetwork();
  setupBleScan();
  // Execute your own GUI interface
  setupGui();
  SD_Init();
  // Clear lvgl counter
  lv_disp_trig_activity(NULL);

#ifdef LILYGO_WATCH_HAS_BUTTON
  // In lvgl we call the button processing regularly
  lv_task_create([](lv_task_t *args)
                 { ttgo->button->loop(); },
                 30, 1, nullptr);
#endif

  // When the initialization is complete, turn on the backlight
  ttgo->openBL();
}

void loop()
{
  bool rlst;
  uint8_t data;
  ble_address_t DeviceData;

//Receive queue data and add to list and text
  if (xQueueReceive(g_ble_uuid_queue_handle, &DeviceData, 0) == pdPASS)
  {
    char str[50];
    File file = SD.open(RESULTS_PATH, FILE_WRITE);
    if (file)
    {
      file.seek(file.size());
      file.printf("Address:%s\r\n", DeviceData.address);
      file.flush();
    }
    else
    {
      Serial.printf("Error : The %s text cannot be opened\n", RESULTS_PATH);
    }
    file.close();
    sprintf(str, "num:%d  Address:%s  Rssi:%d\n", recordCount, DeviceData.address, DeviceData.rssi);
    ble_list_add(str);
    recordCount++;
  }

  if (xQueueReceive(g_event_queue_handle, &data, 5 / portTICK_RATE_MS) == pdPASS)
  {
    switch (data)
    {
    case Q_EVENT_BMA_INT:
      do
      {
        rlst = ttgo->bma->readInterrupt();
      } while (!rlst);
      //! setp counter
      if (ttgo->bma->isStepCounter())
      {
        updateStepCounter(ttgo->bma->getCounter());
      }
      break;
    case Q_EVENT_AXP_INT:
      ttgo->power->readIRQ();
      if (ttgo->power->isVbusPlugInIRQ())
      {
        updateBatteryIcon(LV_ICON_CHARGE);
      }
      if (ttgo->power->isVbusRemoveIRQ())
      {
        updateBatteryIcon(LV_ICON_CALCULATION);
      }
      if (ttgo->power->isChargingDoneIRQ())
      {
        updateBatteryIcon(LV_ICON_CALCULATION);
      }
      if (ttgo->power->isPEKShortPressIRQ())
      {
        ttgo->power->clearIRQ();
        low_energy();
        return;
      }
      ttgo->power->clearIRQ();
      break;
    case Q_EVENT_WIFI_SCAN_DONE:
    {
      int16_t len = WiFi.scanComplete();
      for (int i = 0; i < len; ++i)
      {
        wifi_list_add(WiFi.SSID(i).c_str());
      }
      break;
    }
    default:
      break;
    }
  }

  if (lv_disp_get_inactive_time(NULL) < DEFAULT_SCREEN_TIMEOUT)
  {
    lv_task_handler();
  }
  else
  {
    low_energy();
  }
}
