/*
 * VL53L0X ToF distance sensor — Pololu library init, fresh I2C each measurement.
 */
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "vl53l0x.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "VL53L0X";

#include "pins.h"
#define VL53_ADDR            0x29
#define I2C_FREQ_HZ          100000

/* ---- Pololu register / helpers --------------------------------------------------- */
enum { SYSRANGE_START=0x00, SYSTEM_SEQUENCE_CONFIG=0x01, SYSTEM_INTERRUPT_CONFIG_GPIO=0x0A,
       SYSTEM_INTERRUPT_CLEAR=0x0B, RESULT_INTERRUPT_STATUS=0x13, RESULT_RANGE_STATUS=0x14,
       PRE_RANGE_CONFIG_VCSEL_PERIOD=0x50, FINAL_RANGE_CONFIG_VCSEL_PERIOD=0x70,
       PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI=0x51, FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI=0x71,
       MSRC_CONFIG_TIMEOUT_MACROP=0x46, MSRC_CONFIG_CONTROL=0x60,
       FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT=0x44,
       VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV=0x89, GPIO_HV_MUX_ACTIVE_HIGH=0x84,
       IDENTIFICATION_MODEL_ID=0xC0,
       DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD=0x4E, DYNAMIC_SPAD_REF_EN_START_OFFSET=0x4F,
       GLOBAL_CONFIG_SPAD_ENABLES_REF_0=0xB0, GLOBAL_CONFIG_REF_EN_START_SELECT=0xB6 };
#define calcMacroPeriod(p) ((((uint32_t)2304*(p)*1655)+500)/1000)
#define dist_samples 10

static i2c_master_bus_handle_t  s_i2c_bus;
static i2c_master_dev_handle_t  s_dev;
static uint8_t s_stop;

/* ---------- I2C helpers ---------- */
static void w(uint8_t r, uint8_t v)  { uint8_t b[2]={r,v}; i2c_master_transmit(s_dev,b,2,50); }
static void w16(uint8_t r, uint16_t v){ uint8_t b[3]={r,v>>8,v}; i2c_master_transmit(s_dev,b,3,50); }
static uint8_t r8(uint8_t r)          { uint8_t v; i2c_master_transmit_receive(s_dev,&r,1,&v,1,50); return v; }
static uint16_t r16(uint8_t r)        { uint8_t b[2]; i2c_master_transmit_receive(s_dev,&r,1,b,2,50); return (b[0]<<8)|b[1]; }
static void rm(uint8_t r, uint8_t*d,size_t n){ i2c_master_transmit_receive(s_dev,&r,1,d,n,50); }
static void wm(uint8_t r,const uint8_t*s,size_t n){ uint8_t b[64];b[0]=r;for(size_t i=0;i<n;i++)b[i+1]=s[i]; i2c_master_transmit(s_dev,b,n+1,50); }

/* ---------- SPAD info ---------- */
static bool getSpadInfo(uint8_t *count, bool *aperture) {
    w(0x80,0x01); w(0xFF,0x01); w(0x00,0x00); w(0xFF,0x06); w(0x83,r8(0x83)|0x04);
    w(0xFF,0x07); w(0x81,0x01); w(0x80,0x01); w(0x94,0x6b); w(0x83,0x00);
    for(int t=100;r8(0x83)==0;){if(!--t)return false;vTaskDelay(pdMS_TO_TICKS(1));}
    w(0x83,0x01); uint8_t tmp=r8(0x92); *count=tmp&0x7f; *aperture=tmp>>7;
    w(0x81,0x00); w(0xFF,0x06); w(0x83,r8(0x83)&~0x04); w(0xFF,0x01); w(0x00,0x01);
    w(0xFF,0x00); w(0x80,0x00); return true;
}

/* ---------- timing helpers ---------- */
static uint16_t decT(uint16_t v){return (uint16_t)((v&0xFF)<<(uint16_t)((v&0xFF00)>>8))+1;}
static uint16_t encT(uint32_t m){if(!m)return 0;uint32_t l=m-1;uint16_t h=0;while((l&0xFFFFFF00)>0){l>>=1;h++;}return(h<<8)|(l&0xFF);}
static uint32_t m2u(uint16_t m,uint8_t p){return((m*calcMacroPeriod(p))+500)/1000;}
static uint32_t u2m(uint32_t u,uint8_t p){uint32_t mn=calcMacroPeriod(p);return(((u*1000)+(mn/2))/mn);}
static bool calib(uint8_t vhv) {
    w(SYSRANGE_START,0x01|vhv);
    for(int t=1000;(r8(RESULT_INTERRUPT_STATUS)&0x07)==0;){if(!--t)return false;vTaskDelay(pdMS_TO_TICKS(1));}
    w(SYSTEM_INTERRUPT_CLEAR,0x01); w(SYSRANGE_START,0x00); return true;
}
static uint8_t vcsel(uint8_t t){return((r8(t==0?PRE_RANGE_CONFIG_VCSEL_PERIOD:FINAL_RANGE_CONFIG_VCSEL_PERIOD)+1)<<1);}
static void setTimingBudget(uint32_t b) {
    const uint16_t So=1910,Eo=960,Mo=660,To=590,Do=690,Po=660,Fo=550;
    uint32_t u=So+Eo; uint8_t seq=r8(SYSTEM_SEQUENCE_CONFIG);
    bool tc=(seq>>4)&1,ds=(seq>>3)&1,ms=(seq>>2)&1,pr=(seq>>6)&1,fr=(seq>>7)&1;
    uint8_t pv=vcsel(0),fv=vcsel(1);
    uint16_t mm=r8(MSRC_CONFIG_TIMEOUT_MACROP)+1; uint32_t mu=m2u(mm,pv);
    uint16_t pm=decT(r16(PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI)); uint32_t pu=m2u(pm,pv);
    if(tc)u+=(mu+To);
    if(ds){u+=2*(mu+Do);}else if(ms){u+=(mu+Mo);}
    if(pr)u+=(pu+Po);
    if(fr){u+=Fo; if(u>b)return; uint32_t fm=u2m(b-u,fv);if(pr)fm+=pm;
           w16(FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,encT(fm));}
}

/* ---------- Pololu init sequence ---------- */
static bool vl53_init_sensor(void) {
    for(int i=0;i<5;i++){if(r8(IDENTIFICATION_MODEL_ID)==0xEE)break;vTaskDelay(pdMS_TO_TICKS(5));}
    if(r8(IDENTIFICATION_MODEL_ID)!=0xEE)return false;
    /* DataInit */
    w(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV,r8(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV)|0x01);
    w(0x88,0x00); w(0x80,0x01); w(0xFF,0x01); w(0x00,0x00);
    s_stop=r8(0x91); w(0x00,0x01); w(0xFF,0x00); w(0x80,0x00);
    w(MSRC_CONFIG_CONTROL,r8(MSRC_CONFIG_CONTROL)|0x12);
    w16(FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT,(uint16_t)(0.25f*(1<<7)));
    w(SYSTEM_SEQUENCE_CONFIG,0xFF);
    /* StaticInit */
    uint8_t sc; bool ap;
    if(!getSpadInfo(&sc,&ap))return false;
    uint8_t rm6[6]; rm(GLOBAL_CONFIG_SPAD_ENABLES_REF_0,rm6,6);
    w(0xFF,0x01); w(DYNAMIC_SPAD_REF_EN_START_OFFSET,0x00);
    w(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD,0x2C); w(0xFF,0x00);
    w(GLOBAL_CONFIG_REF_EN_START_SELECT,0xB4);
    uint8_t fs=ap?12:0,se=0;
    for(uint8_t i=0;i<48;i++){if(i<fs||se==sc)rm6[i/8]&=~(1<<(i%8));else if((rm6[i/8]>>(i%8))&1)se++;}
    wm(GLOBAL_CONFIG_SPAD_ENABLES_REF_0,rm6,6);
    /* Tuning */
    w(0xFF,0x01);w(0x00,0x00);w(0xFF,0x00);w(0x09,0x00);w(0x10,0x00);w(0x11,0x00);
    w(0x24,0x01);w(0x25,0xFF);w(0x75,0x00);w(0xFF,0x01);w(0x4E,0x2C);w(0x48,0x00);w(0x30,0x20);
    w(0xFF,0x00);w(0x30,0x09);w(0x54,0x00);w(0x31,0x04);w(0x32,0x03);w(0x40,0x83);
    w(0x46,0x25);w(0x60,0x00);w(0x27,0x00);w(0x50,0x06);w(0x51,0x00);w(0x52,0x96);
    w(0x56,0x08);w(0x57,0x30);w(0x61,0x00);w(0x62,0x00);w(0x64,0x00);w(0x65,0x00);w(0x66,0xA0);
    w(0xFF,0x01);w(0x22,0x32);w(0x47,0x14);w(0x49,0xFF);w(0x4A,0x00);w(0xFF,0x00);
    w(0x7A,0x0A);w(0x7B,0x00);w(0x78,0x21);w(0xFF,0x01);w(0x23,0x34);w(0x42,0x00);
    w(0x44,0xFF);w(0x45,0x26);w(0x46,0x05);w(0x40,0x40);w(0x0E,0x06);w(0x20,0x1A);w(0x43,0x40);
    w(0xFF,0x00);w(0x34,0x03);w(0x35,0x44);w(0xFF,0x01);w(0x31,0x04);w(0x4B,0x09);
    w(0x4C,0x05);w(0x4D,0x04);w(0xFF,0x00);w(0x44,0x00);w(0x45,0x20);w(0x47,0x08);
    w(0x48,0x28);w(0x67,0x00);w(0x70,0x04);w(0x71,0x01);w(0x72,0xFE);w(0x76,0x00);w(0x77,0x00);
    w(0xFF,0x01);w(0x0D,0x01);w(0xFF,0x00);w(0x80,0x01);w(0x01,0xF8);w(0xFF,0x01);
    w(0x8E,0x01);w(0x00,0x01);w(0xFF,0x00);w(0x80,0x00);
    /* interrupt config */
    w(SYSTEM_INTERRUPT_CONFIG_GPIO,0x04); w(GPIO_HV_MUX_ACTIVE_HIGH,r8(GPIO_HV_MUX_ACTIVE_HIGH)&~0x10);
    w(SYSTEM_INTERRUPT_CLEAR,0x01); w(SYSTEM_SEQUENCE_CONFIG,0xE8);
    /* timing budget */
    {uint32_t b=0;uint8_t seq=r8(SYSTEM_SEQUENCE_CONFIG);
     bool tc=(seq>>4)&1,ds=(seq>>3)&1,ms=(seq>>2)&1,pr=(seq>>6)&1,fr=(seq>>7)&1;
     uint8_t pv=vcsel(0),fv=vcsel(1);
     uint16_t mm=r8(MSRC_CONFIG_TIMEOUT_MACROP)+1;uint32_t mu=m2u(mm,pv);
     uint16_t pm=decT(r16(PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI));uint32_t pu=m2u(pm,pv);
     uint16_t fm=decT(r16(FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI));
     if(pr)fm-=pm;
     uint32_t fu=m2u(fm,fv);
     b=1910+960; if(tc)b+=mu+590;
     if(ds){b+=2*(mu+690);}else if(ms){b+=mu+660;}
     if(pr)b+=pu+660; if(fr)b+=fu+550;
     setTimingBudget(b);}
    /* calibrations */
    w(SYSTEM_SEQUENCE_CONFIG,0x01);if(!calib(0x40))return false;
    w(SYSTEM_SEQUENCE_CONFIG,0x02);if(!calib(0x00))return false;
    w(SYSTEM_SEQUENCE_CONFIG,0xE8);return true;
}

static void start_cont(void){w(0x80,0x01);w(0xFF,0x01);w(0x00,0x00);w(0x91,s_stop);w(0x00,0x01);w(0xFF,0x00);w(0x80,0x00);w(SYSRANGE_START,0x02);}

static uint16_t read_mm(void) {
    for(int t=500;(r8(RESULT_INTERRUPT_STATUS)&0x07)==0;){if(!--t)return 65535;vTaskDelay(pdMS_TO_TICKS(1));}
    uint16_t mm=r16(RESULT_RANGE_STATUS+10);w(SYSTEM_INTERRUPT_CLEAR,0x01);
    return(mm>2000||mm==0)?65535:mm;
}

/* ---- public API ------------------------------------------------------ */

bool vl53l0x_init(void) {
    gpio_config_t c={.pin_bit_mask=BIT64(PIN_VL53_XSHUT),.mode=GPIO_MODE_OUTPUT};
    gpio_config(&c); gpio_set_level(PIN_VL53_XSHUT,0);
    return true;
}

void vl53l0x_deinit(void) { gpio_set_level(PIN_VL53_XSHUT,0); }

float vl53l0x_measure(void) {
    /* teardown any previous I2C session */
    if(s_dev){i2c_master_bus_rm_device(s_dev);s_dev=NULL;}
    if(s_i2c_bus){i2c_del_master_bus(s_i2c_bus);s_i2c_bus=NULL;}

    /* fresh I2C bus each measurement — matching Arduino Wire.begin() */
    i2c_master_bus_config_t ic={.i2c_port=I2C_NUM_0,.sda_io_num=PIN_I2C_SDA,.scl_io_num=PIN_I2C_SCL,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags={.enable_internal_pullup=true}};
    ESP_ERROR_CHECK(i2c_new_master_bus(&ic,&s_i2c_bus));
    i2c_device_config_t dc={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=VL53_ADDR,.scl_speed_hz=I2C_FREQ_HZ};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus,&dc,&s_dev));

    /* power on + init — matching Arduino: digitalWrite(HIGH),delay(10),vl53.init() */
    gpio_set_level(PIN_VL53_XSHUT,1); vTaskDelay(pdMS_TO_TICKS(10));
    for(int a=0;a<2;a++){
        if(vl53_init_sensor())break;
        ESP_LOGW(TAG,"init attempt %d failed",a);
        gpio_set_level(PIN_VL53_XSHUT,0);vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_VL53_XSHUT,1);vTaskDelay(pdMS_TO_TICKS(10));
    }
    if(!s_stop){gpio_set_level(PIN_VL53_XSHUT,0);return 0.0f;} /* init failed */
    float r=0.0f;
    /* start continuous, read samples */
    start_cont();vTaskDelay(pdMS_TO_TICKS(20));read_mm(); /* discard */
    {int sum=0,mn=99999,mx=0,v=0;
     for(int i=0;i<dist_samples;i++){uint16_t mm=read_mm();if(mm==65535)continue;v++;sum+=mm;if((int)mm<mn)mn=mm;if((int)mm>mx)mx=mm;}
     if(v>=3)r=((float)(sum-mn-mx)/(v-2))/10.0f;else if(v==2)r=(float)sum/20.0f;else if(v==1)r=(float)sum/10.0f;}

    /* power off, tear down I2C */
    gpio_set_level(PIN_VL53_XSHUT,0);
    i2c_master_bus_rm_device(s_dev);s_dev=NULL;
    i2c_del_master_bus(s_i2c_bus);s_i2c_bus=NULL;
    return r;
}
