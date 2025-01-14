/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/param.h>

#include "flash_init.h"
#include "soc_flash_init.h"

#include "soc/io_mux_reg.h"
#include "esp_log.h"
#include "bootloader_flash_priv.h"
#include "bootloader_flash_config.h"
#include "esp_rom_efuse.h"
#include "esp_rom_gpio.h"
#include "flash_qio_mode.h"

#include "hal/cache_ll.h"
#include "hal/cache_hal.h"

#define FLASH_IO_MATRIX_DUMMY_40M       0
#define FLASH_IO_MATRIX_DUMMY_80M       0
#define FLASH_IO_DRIVE_GD_WITH_1V8PSRAM 3
#define FLASH_CS_SETUP_TIME             3
#define FLASH_CS_HOLD_TIME              3
#define FLASH_CS_HOLD_DELAY             2

#define TAG "flash_init"

extern esp_image_header_t bootloader_image_hdr;

void configure_spi_pins(int drv)
{
	const uint32_t spiconfig = esp_rom_efuse_get_flash_gpio_info();
	uint8_t wp_pin = esp_rom_efuse_get_flash_wp_gpio();
	uint8_t clk_gpio_num = SPI_CLK_GPIO_NUM;
	uint8_t q_gpio_num = SPI_Q_GPIO_NUM;
	uint8_t d_gpio_num = SPI_D_GPIO_NUM;
	uint8_t cs0_gpio_num = SPI_CS0_GPIO_NUM;
	uint8_t hd_gpio_num = SPI_HD_GPIO_NUM;
	uint8_t wp_gpio_num = SPI_WP_GPIO_NUM;

	if (spiconfig == 0) {

	} else {
		clk_gpio_num = spiconfig & 0x3f;
		q_gpio_num = (spiconfig >> 6) & 0x3f;
		d_gpio_num = (spiconfig >> 12) & 0x3f;
		cs0_gpio_num = (spiconfig >> 18) & 0x3f;
		hd_gpio_num = (spiconfig >> 24) & 0x3f;
		wp_gpio_num = wp_pin;
	}
	esp_rom_gpio_pad_set_drv(clk_gpio_num, drv);
	esp_rom_gpio_pad_set_drv(q_gpio_num, drv);
	esp_rom_gpio_pad_set_drv(d_gpio_num, drv);
	esp_rom_gpio_pad_set_drv(cs0_gpio_num, drv);
	if (hd_gpio_num <= MAX_PAD_GPIO_NUM) {
		esp_rom_gpio_pad_set_drv(hd_gpio_num, drv);
	}
	if (wp_gpio_num <= MAX_PAD_GPIO_NUM) {
		esp_rom_gpio_pad_set_drv(wp_gpio_num, drv);
	}
}

void flash_set_dummy_out(void)
{
	REG_SET_BIT(SPI_MEM_CTRL_REG(0), SPI_MEM_FDUMMY_OUT | SPI_MEM_D_POL | SPI_MEM_Q_POL);
	REG_SET_BIT(SPI_MEM_CTRL_REG(1), SPI_MEM_FDUMMY_OUT | SPI_MEM_D_POL | SPI_MEM_Q_POL);
}

void flash_dummy_config(const esp_image_header_t *pfhdr)
{
	configure_spi_pins(1);
	flash_set_dummy_out();
}

void flash_cs_timing_config(void)
{
	/* SPI0/1 share the cs_hold / cs_setup, cd_hold_time / cd_setup_time, cs_hold_delay
	 * registers for FLASH, so we only need to set SPI0 related registers here
	 */
	if (flash_is_octal_mode_enabled()) {

		SET_PERI_REG_MASK(SPI_MEM_USER_REG(0), SPI_MEM_CS_HOLD_M | SPI_MEM_CS_SETUP_M);
		SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_HOLD_TIME_V, FLASH_CS_HOLD_TIME,
				  SPI_MEM_CS_HOLD_TIME_S);
		SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_SETUP_TIME_V,
				  FLASH_CS_SETUP_TIME, SPI_MEM_CS_SETUP_TIME_S);
		/* CS high time */
		SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_HOLD_DELAY_V,
				  FLASH_CS_HOLD_DELAY, SPI_MEM_CS_HOLD_DELAY_S);
	} else {
		SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_HOLD_TIME_V, 0,
				  SPI_MEM_CS_HOLD_TIME_S);
		SET_PERI_REG_BITS(SPI_MEM_CTRL2_REG(0), SPI_MEM_CS_SETUP_TIME_V, 0,
				  SPI_MEM_CS_SETUP_TIME_S);
		SET_PERI_REG_MASK(SPI_MEM_USER_REG(0), SPI_MEM_CS_HOLD_M | SPI_MEM_CS_SETUP_M);
	}
}

static void init_flash_configure(void)
{
	flash_dummy_config(&bootloader_image_hdr);
	flash_cs_timing_config();
}

static void print_flash_info(const esp_image_header_t *bootloader_hdr)
{
	ESP_EARLY_LOGD(TAG, "magic %02x", bootloader_hdr->magic);
	ESP_EARLY_LOGD(TAG, "segments %02x", bootloader_hdr->segment_count);
	ESP_EARLY_LOGD(TAG, "spi_mode %02x", bootloader_hdr->spi_mode);
	ESP_EARLY_LOGD(TAG, "spi_speed %02x", bootloader_hdr->spi_speed);
	ESP_EARLY_LOGD(TAG, "spi_size %02x", bootloader_hdr->spi_size);

	const char *str;

	switch (bootloader_hdr->spi_speed) {
	case ESP_IMAGE_SPI_SPEED_DIV_2:
		str = "40MHz";
		break;
	case ESP_IMAGE_SPI_SPEED_DIV_3:
		str = "26.7MHz";
		break;
	case ESP_IMAGE_SPI_SPEED_DIV_4:
		str = "20MHz";
		break;
	case ESP_IMAGE_SPI_SPEED_DIV_1:
		str = "80MHz";
		break;
	default:
		str = "20MHz";
		break;
	}
	ESP_EARLY_LOGI(TAG, "Boot SPI Speed : %s", str);

	/* SPI mode could have been set to QIO during boot already,
	 * so test the SPI registers not the flash header
	 */
	uint32_t spi_ctrl = REG_READ(SPI_MEM_CTRL_REG(0));

	if (spi_ctrl & SPI_MEM_FREAD_QIO) {
		str = "QIO";
	} else if (spi_ctrl & SPI_MEM_FREAD_QUAD) {
		str = "QOUT";
	} else if (spi_ctrl & SPI_MEM_FREAD_DIO) {
		str = "DIO";
	} else if (spi_ctrl & SPI_MEM_FREAD_DUAL) {
		str = "DOUT";
	} else if (spi_ctrl & SPI_MEM_FASTRD_MODE) {
		str = "FAST READ";
	} else {
		str = "SLOW READ";
	}
	ESP_EARLY_LOGI(TAG, "SPI Mode       : %s", str);

	switch (bootloader_hdr->spi_size) {
	case ESP_IMAGE_FLASH_SIZE_1MB:
		str = "1MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_2MB:
		str = "2MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_4MB:
		str = "4MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_8MB:
		str = "8MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_16MB:
		str = "16MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_32MB:
		str = "32MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_64MB:
		str = "64MB";
		break;
	case ESP_IMAGE_FLASH_SIZE_128MB:
		str = "128MB";
		break;
	default:
		str = "2MB";
		break;
	}
	ESP_EARLY_LOGI(TAG, "SPI Flash Size : %s", str);
}

static void update_flash_config(const esp_image_header_t *bootloader_hdr)
{
	volatile uint32_t size;

	switch (bootloader_hdr->spi_size) {
	case ESP_IMAGE_FLASH_SIZE_1MB:
		size = 1;
		break;
	case ESP_IMAGE_FLASH_SIZE_2MB:
		size = 2;
		break;
	case ESP_IMAGE_FLASH_SIZE_4MB:
		size = 4;
		break;
	case ESP_IMAGE_FLASH_SIZE_8MB:
		size = 8;
		break;
	case ESP_IMAGE_FLASH_SIZE_16MB:
		size = 16;
		break;
	case ESP_IMAGE_FLASH_SIZE_32MB:
		size = 32;
		break;
	case ESP_IMAGE_FLASH_SIZE_64MB:
		size = 64;
		break;
	case ESP_IMAGE_FLASH_SIZE_128MB:
		size = 128;
		break;
	default:
		size = 2;
	}

	cache_hal_disable(CACHE_TYPE_ALL);
	/* Set flash chip size */
	esp_rom_spiflash_config_param(g_rom_flashchip.device_id, size * 0x100000, 0x10000, 0x1000,
				      0x100, 0xffff);
	cache_hal_enable(CACHE_TYPE_ALL);
}

int init_spi_flash(void)
{
	init_flash_configure();
#ifndef CONFIG_SPI_FLASH_ROM_DRIVER_PATCH
	const uint32_t spiconfig = esp_rom_efuse_get_flash_gpio_info();

	if (spiconfig != ESP_ROM_EFUSE_FLASH_DEFAULT_SPI &&
	    spiconfig != ESP_ROM_EFUSE_FLASH_DEFAULT_HSPI) {
		ESP_EARLY_LOGE(TAG, "SPI flash pins are overridden. Enable "
				    "CONFIG_SPI_FLASH_ROM_DRIVER_PATCH in menuconfig");
		return ESP_FAIL;
	}
#endif

#if CONFIG_SPI_FLASH_HPM_ENABLE
	/* Reset flash, clear volatile bits DC[0:1]. Make it work under default mode to boot. */
	bootloader_spi_flash_reset();
#endif

	bootloader_flash_unlock();

#if CONFIG_ESPTOOLPY_FLASHMODE_QIO || CONFIG_ESPTOOLPY_FLASHMODE_QOUT
	if (!flash_is_octal_mode_enabled()) {
		bootloader_enable_qio_mode();
	}
#endif

	print_flash_info(&bootloader_image_hdr);
	update_flash_config(&bootloader_image_hdr);
	/* ensure the flash is write-protected */
	bootloader_enable_wp();
	return ESP_OK;
}

void flash_update_id(void)
{
	esp_rom_spiflash_chip_t *chip = &rom_spiflash_legacy_data->chip;

	chip->device_id = bootloader_read_flash_id();
}
