/*
 * Copyright (c) 2021 Bestechnic (Shanghai) Co., Ltd. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "gpio_bes.h"
#include <stdlib.h>
#include "hal_iomux.h"
#include "gpio_if.h"
#include "device_resource_if.h"
#include "osal_irq.h"
#include "hdf_log.h"

#define HDF_LOG_TAG gpioDriver
static struct GpioCntlr gpioCntlr;
struct OemGpioIrqHandler {
    uint8_t port;
    GpioIrqFunc func;
    void *arg;
};

enum HAL_GPIO_PIN_T g_gpioPinReflectionMap[HAL_GPIO_PIN_LED_NUM] = {0};
static struct OemGpioIrqHandler g_oemGpioIrqHandler[HAL_GPIO_PIN_LED_NUM] = {0};
static struct HAL_GPIO_IRQ_CFG_T g_gpioIrqCfg[HAL_GPIO_PIN_LED_NUM] = {0};

static struct HAL_GPIO_IRQ_CFG_T HalGpioGetIrqConfig(enum HAL_GPIO_PIN_T pin)
{
    struct HAL_GPIO_IRQ_CFG_T irqCfg;

    irqCfg.irq_enable = g_gpioIrqCfg[pin].irq_enable;
    irqCfg.irq_debounce = g_gpioIrqCfg[pin].irq_debounce;
    irqCfg.irq_type = g_gpioIrqCfg[pin].irq_type;
    irqCfg.irq_polarity = g_gpioIrqCfg[pin].irq_polarity;

    return irqCfg;
}

static void OemGpioIrqHdl(enum HAL_GPIO_PIN_T pin)
{
    if (pin >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, pin);
        return;
    }
    for (size_t i = 0; i < HAL_GPIO_PIN_LED_NUM; i++) {
        if (pin == (enum HAL_GPIO_PIN_T)g_gpioPinReflectionMap[i]) {
            GpioCntlrIrqCallback(&gpioCntlr, i);
            return;
        }
    }
}

/* dispatch */
int32_t GpioDispatch(struct HdfDeviceIoClient *client, int cmdId, struct HdfSBuf *data, struct HdfSBuf *reply)
{
    if (client == NULL || client->device == NULL || data == NULL || reply == NULL) {
        HDF_LOGE("%s: client or client->device is NULL", __func__);
        return HDF_ERR_INVALID_PARAM;
    }
    return HDF_SUCCESS;
}

/* HdfDriverEntry method definitions */
static int32_t GpioDriverBind(struct HdfDeviceObject *device);
static int32_t GpioDriverInit(struct HdfDeviceObject *device);
static void GpioDriverRelease(struct HdfDeviceObject *device);

/* HdfDriverEntry definitions */
struct HdfDriverEntry g_GpioDriverEntry = {
    .moduleVersion = 1,
    .moduleName = "BES_GPIO_MODULE_HDF",
    .Bind = GpioDriverBind,
    .Init = GpioDriverInit,
    .Release = GpioDriverRelease,
};
HDF_INIT(g_GpioDriverEntry);

/* GpioMethod method definitions */
static int32_t GpioDevWrite(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t val);
static int32_t GpioDevRead(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t *val);
static int32_t GpioDevSetDir(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t dir);
static int32_t GpioDevGetDir(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t *dir);
static int32_t GpioDevSetIrq(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t mode, GpioIrqFunc func, void *arg);
static int32_t GpioDevUnSetIrq(struct GpioCntlr *cntlr, uint16_t gpio);
static int32_t GpioDevEnableIrq(struct GpioCntlr *cntlr, uint16_t gpio);
static int32_t GpioDevDisableIrq(struct GpioCntlr *cntlr, uint16_t gpio);
/* GpioMethod definitions */
struct GpioMethod g_GpioCntlrMethod = {
    .request = NULL,
    .release = NULL,
    .write = GpioDevWrite,
    .read = GpioDevRead,
    .setDir = GpioDevSetDir,
    .getDir = GpioDevGetDir,
    .toIrq = NULL,
    .setIrq = GpioDevSetIrq,
    .unsetIrq = GpioDevUnSetIrq,
    .enableIrq = GpioDevEnableIrq,
    .disableIrq = GpioDevDisableIrq,
};

static int InitGpioDevice(struct GpioDevice *device)
{
    struct HAL_IOMUX_PIN_FUNCTION_MAP gpioCfg;
    if (device == NULL) {
        HDF_LOGE("%s: device is NULL", __func__);
        return HDF_ERR_INVALID_PARAM;
    }
    gpioCfg.pin = device->port;
    gpioCfg.function = HAL_IOMUX_FUNC_AS_GPIO;
    gpioCfg.volt = HAL_IOMUX_PIN_VOLTAGE_VIO;

    if ((device->config == OUTPUT_PUSH_PULL) || (device->config == OUTPUT_OPEN_DRAIN_PULL_UP)
        || (device->config == INPUT_PULL_UP) || (device->config == IRQ_MODE)) {
        gpioCfg.pull_sel = HAL_IOMUX_PIN_PULLUP_ENABLE;
    } else if ((device->config == INPUT_PULL_DOWN)) {
        gpioCfg.pull_sel = HAL_IOMUX_PIN_PULLDOWN_ENABLE;
    } else {
        gpioCfg.pull_sel = HAL_IOMUX_PIN_NOPULL;
    }

    hal_iomux_init(&gpioCfg, 1);

    return HDF_SUCCESS;
}

static uint32_t GetGpioDeviceResource(
    struct GpioDevice *device, const struct DeviceResourceNode *resourceNode)
{
    uint32_t relPin;
    int32_t ret;
    struct GpioResource *resource = NULL;
    struct DeviceResourceIface *dri = NULL;
    if (device == NULL || resourceNode == NULL) {
        HDF_LOGE("%s: device is NULL", __func__);
        return HDF_ERR_INVALID_PARAM;
    }
    resource = &device->resource;
    if (resource == NULL) {
        HDF_LOGE("%s: resource is NULL", __func__);
        return HDF_ERR_INVALID_OBJECT;
    }
    dri = DeviceResourceGetIfaceInstance(HDF_CONFIG_SOURCE);
    if (dri == NULL || dri->GetUint32 == NULL) {
        HDF_LOGE("DeviceResourceIface is invalid");
        return HDF_ERR_INVALID_OBJECT;
    }

    if (dri->GetUint32(resourceNode, "pinNum", &resource->pinNum, 0) != HDF_SUCCESS) {
        HDF_LOGE("gpio config read pinNum fail");
        return HDF_FAILURE;
    }

    for (size_t i = 0; i < resource->pinNum; i++) {
        if (dri->GetUint32ArrayElem(resourceNode, "pin", i, &resource->pin, 0) != HDF_SUCCESS) {
            HDF_LOGE("gpio config read pin fail");
            return HDF_FAILURE;
        }

        if (dri->GetUint32ArrayElem(resourceNode, "realPin", i, &resource->realPin, 0) != HDF_SUCCESS) {
            HDF_LOGE("gpio config read realPin fail");
            return HDF_FAILURE;
        }

        if (dri->GetUint32ArrayElem(resourceNode, "config", i, &resource->config, 0) != HDF_SUCCESS) {
            HDF_LOGE("gpio config read config fail");
            return HDF_FAILURE;
        }

        relPin = resource->realPin / DECIMALNUM * OCTALNUM + resource->realPin % DECIMALNUM;
        g_gpioPinReflectionMap[resource->pin] = relPin;
        device->config = resource->config;
        resource->pin = relPin;
        device->port = relPin;

        ret = InitGpioDevice(device);
        if (ret != HDF_SUCCESS) {
            HDF_LOGE("InitGpioDevice FAIL\r\n");
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static int32_t AttachGpioDevice(struct GpioCntlr *gpioCntlr, struct HdfDeviceObject *device)
{
    int32_t ret;

    struct GpioDevice *gpioDevice = NULL;
    if (device == NULL || device->property == NULL) {
        HDF_LOGE("%s: property is NULL", __func__);
        return HDF_ERR_INVALID_PARAM;
    }

    gpioDevice = (struct GpioDevice *)OsalMemAlloc(sizeof(struct GpioDevice));
    if (gpioDevice == NULL) {
        HDF_LOGE("%s: OsalMemAlloc gpioDevice error", __func__);
        return HDF_ERR_MALLOC_FAIL;
    }

    ret = GetGpioDeviceResource(gpioDevice, device->property);
    if (ret != HDF_SUCCESS) {
        (void)OsalMemFree(gpioDevice);
        return HDF_FAILURE;
    }

    gpioCntlr->count = gpioDevice->resource.pinNum;

    return HDF_SUCCESS;
}

static int32_t GpioDriverInit(struct HdfDeviceObject *device)
{
    int32_t ret;
    struct GpioCntlr *gpioCntlr = NULL;

    if (device == NULL) {
        HDF_LOGE("%s: device is NULL", __func__);
        return HDF_ERR_INVALID_PARAM;
    }

    gpioCntlr = GpioCntlrFromDevice(device);
    if (gpioCntlr == NULL) {
        HDF_LOGE("GpioCntlrFromDevice fail\r\n");
        return HDF_DEV_ERR_NO_DEVICE_SERVICE;
    }

    ret = AttachGpioDevice(gpioCntlr, device); // GpioCntlr add GpioDevice to priv
    if (ret != HDF_SUCCESS) {
        HDF_LOGE("AttachGpioDevice fail\r\n");
        return HDF_DEV_ERR_ATTACHDEV_FAIL;
    }

    gpioCntlr->ops = &g_GpioCntlrMethod; // register callback
    ret = GpioCntlrAdd(gpioCntlr);
    if (ret != HDF_SUCCESS) {
        HDF_LOGE("GpioCntlrAdd fail %d\r\n", gpioCntlr->start);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t GpioDriverBind(struct HdfDeviceObject *device)
{
    if (device == NULL) {
        HDF_LOGE("Sample device object is null!");
        return HDF_ERR_INVALID_PARAM;
    }

    gpioCntlr.device.hdfDev = device;
    device->service = gpioCntlr.device.service;

    return HDF_SUCCESS;
}

static void GpioDriverRelease(struct HdfDeviceObject *device)
{
    struct GpioCntlr *gpioCntlr = NULL;

    if (device == NULL) {
        HDF_LOGE("%s: device is NULL", __func__);
        return;
    }

    gpioCntlr = GpioCntlrFromDevice(device);
    if (gpioCntlr == NULL) {
        HDF_LOGE("%s: host is NULL", __func__);
        return;
    }

    gpioCntlr->ops = NULL;
    OsalMemFree(gpioCntlr);
}

/* dev api */
static int32_t GpioDevWrite(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t val)
{
    (void)cntlr;
    uint16_t halGpio = g_gpioPinReflectionMap[gpio];
    if ((enum HAL_GPIO_PIN_T)halGpio >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, halGpio);
        return HDF_ERR_NOT_SUPPORT;
    }

    hal_gpio_pin_set_dir((enum HAL_GPIO_PIN_T)halGpio, HAL_GPIO_DIR_OUT, val);

    return HDF_SUCCESS;
}

static int32_t GpioDevRead(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t *val)
{
    (void)cntlr;
    uint16_t value;
    uint16_t halGpio = g_gpioPinReflectionMap[gpio];
    if ((enum HAL_GPIO_PIN_T)halGpio >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, halGpio);
        return HDF_ERR_NOT_SUPPORT;
    }

    value = (uint16_t)hal_gpio_pin_get_val((enum HAL_GPIO_PIN_T)halGpio);
    *val = value;

    return HDF_SUCCESS;
}

static int32_t GpioDevSetDir(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t dir)
{
    (void)cntlr;
    uint16_t halGpio = g_gpioPinReflectionMap[gpio];
    if ((enum HAL_GPIO_PIN_T)halGpio >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, halGpio);
        return HDF_ERR_NOT_SUPPORT;
    }

    hal_gpio_pin_set_dir((enum HAL_GPIO_PIN_T)halGpio, (enum HAL_GPIO_DIR_T)dir, 0);

    return HDF_SUCCESS;
}

static int32_t GpioDevGetDir(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t *dir)
{
    (void)cntlr;
    uint16_t value;
    uint16_t halGpio = g_gpioPinReflectionMap[gpio];
    if ((enum HAL_GPIO_PIN_T)halGpio >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, gpio);
        return HDF_ERR_NOT_SUPPORT;
    }

    value = (uint16_t)hal_gpio_pin_get_dir((enum HAL_GPIO_PIN_T)halGpio);
    *dir = value;

    return HDF_SUCCESS;
}

static int32_t GpioDevSetIrq(struct GpioCntlr *cntlr, uint16_t gpio, uint16_t mode, GpioIrqFunc func, void *arg)
{
    (void)cntlr;
    enum HAL_GPIO_PIN_T pin = (enum HAL_GPIO_PIN_T)g_gpioPinReflectionMap[gpio];
    if (pin >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, pin);
        return HDF_ERR_NOT_SUPPORT;
    }

    if ((mode == OSAL_IRQF_TRIGGER_RISING) || (mode == OSAL_IRQF_TRIGGER_FALLING)) {
        g_gpioIrqCfg[pin].irq_type = HAL_GPIO_IRQ_TYPE_EDGE_SENSITIVE;
    } else if ((mode == OSAL_IRQF_TRIGGER_HIGH) || (mode == OSAL_IRQF_TRIGGER_LOW)) {
        g_gpioIrqCfg[pin].irq_type = HAL_GPIO_IRQ_TYPE_LEVEL_SENSITIVE;
    } else {
        HDF_LOGE("%s %d, error mode:%d", __func__, __LINE__, mode);
        return HDF_ERR_NOT_SUPPORT;
    }

    g_oemGpioIrqHandler[pin].port = gpio;
    g_oemGpioIrqHandler[pin].func = func;
    g_oemGpioIrqHandler[pin].arg = arg;

    g_gpioIrqCfg[pin].irq_polarity = mode;

    return HDF_SUCCESS;
}

static int32_t GpioDevUnSetIrq(struct GpioCntlr *cntlr, uint16_t gpio)
{
    (void)cntlr;
    enum HAL_GPIO_PIN_T pin = (enum HAL_GPIO_PIN_T)g_gpioPinReflectionMap[gpio];
    if (pin >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, pin);
        return HDF_ERR_NOT_SUPPORT;
    }

    g_oemGpioIrqHandler[pin].func = NULL;
    g_oemGpioIrqHandler[pin].arg = NULL;

    return HDF_SUCCESS;
}

static int32_t GpioDevEnableIrq(struct GpioCntlr *cntlr, uint16_t gpio)
{
    (void)cntlr;
    struct HAL_GPIO_IRQ_CFG_T gpioCfg;
    uint16_t halGpio = (enum HAL_GPIO_PIN_T)g_gpioPinReflectionMap[gpio];
    if ((enum HAL_GPIO_PIN_T)halGpio >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, (enum HAL_GPIO_PIN_T)halGpio);
        return HDF_ERR_NOT_SUPPORT;
    }

    hal_gpio_pin_set_dir((enum HAL_GPIO_PIN_T)halGpio, HAL_GPIO_DIR_IN, 0);

    gpioCfg.irq_enable = true;
    gpioCfg.irq_debounce = true;
    gpioCfg.irq_polarity = g_gpioIrqCfg[(enum HAL_GPIO_PIN_T)halGpio].irq_polarity;
    gpioCfg.irq_handler = OemGpioIrqHdl;
    gpioCfg.irq_type = g_gpioIrqCfg[(enum HAL_GPIO_PIN_T)halGpio].irq_type;
    g_gpioIrqCfg[halGpio] = gpioCfg;

    hal_gpio_setup_irq((enum HAL_GPIO_PIN_T)halGpio, &gpioCfg);

    return HDF_SUCCESS;
}

static int32_t GpioDevDisableIrq(struct GpioCntlr *cntlr, uint16_t gpio)
{
    (void)cntlr;
    uint16_t halGpio = (enum HAL_GPIO_PIN_T)g_gpioPinReflectionMap[gpio];
    if ((enum HAL_GPIO_PIN_T)halGpio >= HAL_GPIO_PIN_LED_NUM) {
        HDF_LOGE("%s %d, error pin:%d", __func__, __LINE__, halGpio);
        return HDF_ERR_NOT_SUPPORT;
    }

    const struct HAL_GPIO_IRQ_CFG_T gpioCfg = {
        .irq_enable = false,
        .irq_debounce = false,
        .irq_polarity = HAL_GPIO_IRQ_POLARITY_LOW_FALLING,
        .irq_handler = NULL,
        .irq_type = HAL_GPIO_IRQ_TYPE_EDGE_SENSITIVE,
    };

    hal_gpio_setup_irq((enum HAL_GPIO_PIN_T)halGpio, &gpioCfg);

    return HDF_SUCCESS;
}