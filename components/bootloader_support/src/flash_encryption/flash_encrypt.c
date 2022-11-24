/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <strings.h>
#include <string.h>
#include "bootloader_flash_priv.h"
#include "bootloader_random.h"
#include "bootloader_common.h"
#include "esp_image_format.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_partitions.h"
#include "esp_secure_boot.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "hal/wdt_hal.h"

#ifdef CONFIG_SECURE_FLASH_ENC_ENABLED

#if CONFIG_IDF_TARGET_ESP32
#define CRYPT_CNT ESP_EFUSE_FLASH_CRYPT_CNT
#define WR_DIS_CRYPT_CNT ESP_EFUSE_WR_DIS_FLASH_CRYPT_CNT
#else
#define CRYPT_CNT ESP_EFUSE_SPI_BOOT_CRYPT_CNT
#define WR_DIS_CRYPT_CNT ESP_EFUSE_WR_DIS_SPI_BOOT_CRYPT_CNT
#endif

/* This file implements FLASH ENCRYPTION related APIs to perform
 * various operations such as programming necessary flash encryption
 * eFuses, detect whether flash encryption is enabled (by reading eFuse)
 * and if required encrypt the partitions in flash memory
 */

static const char *TAG = "flash_encrypt";

/* Static functions for stages of flash encryption */
static esp_err_t initialise_flash_encryption(void);
static esp_err_t encrypt_flash_contents(size_t flash_crypt_cnt, bool flash_crypt_wr_dis) __attribute__((unused));
static esp_err_t encrypt_bootloader(void);
static esp_err_t encrypt_and_load_partition_table(esp_partition_info_t *partition_table, int *num_partitions);
static esp_err_t encrypt_partition(int index, const esp_partition_info_t *partition);

esp_err_t esp_flash_encrypt_check_and_update(void)
{
    size_t flash_crypt_cnt = 0;
    esp_efuse_read_field_cnt(CRYPT_CNT, &flash_crypt_cnt);
    bool flash_crypt_wr_dis = esp_efuse_read_field_bit(WR_DIS_CRYPT_CNT);

    ESP_LOGV(TAG, "CRYPT_CNT %d, write protection %d", flash_crypt_cnt, flash_crypt_wr_dis);

    if (flash_crypt_cnt % 2 == 1) {
        /* Flash is already encrypted */
        int left = (CRYPT_CNT[0]->bit_count - flash_crypt_cnt) / 2;
        if (flash_crypt_wr_dis) {
            left = 0; /* can't update FLASH_CRYPT_CNT, no more flashes */
        }
        ESP_LOGI(TAG, "flash encryption is enabled (%d plaintext flashes left)", left);
        return ESP_OK;
    } else {
#ifndef CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED
        /* Flash is not encrypted, so encrypt it! */
        return encrypt_flash_contents(flash_crypt_cnt, flash_crypt_wr_dis);
#else
        ESP_LOGE(TAG, "flash encryption is not enabled, and SECURE_FLASH_REQUIRE_ALREADY_ENABLED "
                      "is set, refusing to boot.");
        return ESP_ERR_INVALID_STATE;
#endif // CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED
    }
}

static esp_err_t check_and_generate_encryption_keys(void)
{
    size_t key_size = 32;
#ifdef CONFIG_IDF_TARGET_ESP32
    enum { BLOCKS_NEEDED = 1 };
    esp_efuse_purpose_t purposes[BLOCKS_NEEDED] = {
        ESP_EFUSE_KEY_PURPOSE_FLASH_ENCRYPTION,
    };
    esp_efuse_coding_scheme_t coding_scheme = esp_efuse_get_coding_scheme(EFUSE_BLK_ENCRYPT_FLASH);
    if (coding_scheme != EFUSE_CODING_SCHEME_NONE && coding_scheme != EFUSE_CODING_SCHEME_3_4) {
        ESP_LOGE(TAG, "Unknown/unsupported CODING_SCHEME value 0x%x", coding_scheme);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (coding_scheme == EFUSE_CODING_SCHEME_3_4) {
        key_size = 24;
    }
#else
#ifdef CONFIG_SECURE_FLASH_ENCRYPTION_AES256
    enum { BLOCKS_NEEDED = 2 };
    esp_efuse_purpose_t purposes[BLOCKS_NEEDED] = {
        ESP_EFUSE_KEY_PURPOSE_XTS_AES_256_KEY_1,
        ESP_EFUSE_KEY_PURPOSE_XTS_AES_256_KEY_2,
    };
    if (esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_XTS_AES_128_KEY, NULL)) {
        ESP_LOGE(TAG, "XTS_AES_128_KEY is already in use, XTS_AES_256_KEY_1/2 can not be used");
        return ESP_ERR_INVALID_STATE;
    }
#else
    enum { BLOCKS_NEEDED = 1 };
    esp_efuse_purpose_t purposes[BLOCKS_NEEDED] = {
        ESP_EFUSE_KEY_PURPOSE_XTS_AES_128_KEY,
    };
#endif // CONFIG_SECURE_FLASH_ENCRYPTION_AES256
#endif // CONFIG_IDF_TARGET_ESP32

    /* Initialize all efuse block entries to invalid (max) value */
    esp_efuse_block_t blocks[BLOCKS_NEEDED] = {[0 ... BLOCKS_NEEDED-1] = EFUSE_BLK_KEY_MAX};
    bool has_key = true;
    for (unsigned i = 0; i < BLOCKS_NEEDED; i++) {
        bool tmp_has_key = esp_efuse_find_purpose(purposes[i], &blocks[i]);
        if (tmp_has_key) { // For ESP32: esp_efuse_find_purpose() always returns True, need to check whether the key block is used or not.
            tmp_has_key &= !esp_efuse_key_block_unused(blocks[i]);
        }
        if (i == 1 && tmp_has_key != has_key) {
            ESP_LOGE(TAG, "Invalid efuse key blocks: Both AES-256 key blocks must be set.");
            return ESP_ERR_INVALID_STATE;
        }
        has_key &= tmp_has_key;
    }

    if (!has_key) {
        /* Generate key */
        uint8_t keys[BLOCKS_NEEDED][32] = { 0 };
        ESP_LOGI(TAG, "Generating new flash encryption key...");
        for (unsigned i = 0; i < BLOCKS_NEEDED; ++i) {
            bootloader_fill_random(keys[i], key_size);
        }
        ESP_LOGD(TAG, "Key generation complete");

        esp_err_t err = esp_efuse_write_keys(purposes, keys, BLOCKS_NEEDED);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NOT_ENOUGH_UNUSED_KEY_BLOCKS) {
                ESP_LOGE(TAG, "Not enough free efuse key blocks (need %d) to continue", BLOCKS_NEEDED);
            } else {
                ESP_LOGE(TAG, "Failed to write efuse block with purpose (err=0x%x). Can't continue.", err);
            }
            return err;
        }
    } else {
        for (unsigned i = 0; i < BLOCKS_NEEDED; i++) {
            if (!esp_efuse_get_key_dis_write(blocks[i])
                || !esp_efuse_get_key_dis_read(blocks[i])
                || !esp_efuse_get_keypurpose_dis_write(blocks[i])) { // For ESP32: no keypurpose, it returns always True.
                ESP_LOGE(TAG, "Invalid key state, check read&write protection for key and keypurpose(if exists)");
                return ESP_ERR_INVALID_STATE;
            }
        }
        ESP_LOGI(TAG, "Using pre-loaded flash encryption key in efuse");
    }
    return ESP_OK;
}

static esp_err_t initialise_flash_encryption(void)
{
    esp_efuse_batch_write_begin(); /* Batch all efuse writes at the end of this function */

    /* Before first flash encryption pass, need to initialise key & crypto config */
    esp_err_t err = check_and_generate_encryption_keys();
    if (err != ESP_OK) {
        esp_efuse_batch_write_cancel();
        return err;
    }

    err = esp_flash_encryption_enable_secure_features();
    if (err != ESP_OK) {
        esp_efuse_batch_write_cancel();
        return err;
    }

    err = esp_efuse_batch_write_commit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error programming security eFuses (err=0x%x).", err);
        return err;
    }

    return ESP_OK;
}

/* Encrypt all flash data that should be encrypted */
static esp_err_t encrypt_flash_contents(size_t flash_crypt_cnt, bool flash_crypt_wr_dis)
{
    esp_err_t err;
    esp_partition_info_t partition_table[ESP_PARTITION_TABLE_MAX_ENTRIES];
    int num_partitions;

    /* If all flash_crypt_cnt bits are burned or write-disabled, the
       device can't re-encrypt itself. */
    if (flash_crypt_wr_dis || flash_crypt_cnt == CRYPT_CNT[0]->bit_count) {
        ESP_LOGE(TAG, "Cannot re-encrypt data CRYPT_CNT %d write disabled %d", flash_crypt_cnt, flash_crypt_wr_dis);
        return ESP_FAIL;
    }

    if (flash_crypt_cnt == 0) {
        /* Very first flash of encrypted data: generate keys, etc. */
        err = initialise_flash_encryption();
        if (err != ESP_OK) {
            return err;
        }
    }

    err = encrypt_bootloader();
    if (err != ESP_OK) {
        return err;
    }

    err = encrypt_and_load_partition_table(partition_table, &num_partitions);
    if (err != ESP_OK) {
        return err;
    }

    /* Now iterate the just-loaded partition table, looking for entries to encrypt
     */

    /* Go through each partition and encrypt if necessary */
    for (int i = 0; i < num_partitions; i++) {
        err = encrypt_partition(i, &partition_table[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGD(TAG, "All flash regions checked for encryption pass");

#ifdef CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
    // Go straight to max, permanently enabled
    ESP_LOGI(TAG, "Setting CRYPT_CNT for permanent encryption");
    size_t new_flash_crypt_cnt = CRYPT_CNT[0]->bit_count - flash_crypt_cnt;
#else
    /* Set least significant 0-bit in flash_crypt_cnt */
    size_t new_flash_crypt_cnt = 1;
#endif
    ESP_LOGD(TAG, "CRYPT_CNT %d -> %d", flash_crypt_cnt, new_flash_crypt_cnt);
    err = esp_efuse_write_field_cnt(CRYPT_CNT, new_flash_crypt_cnt);

    ESP_LOGI(TAG, "Flash encryption completed");

    return err;
}

static esp_err_t encrypt_bootloader(void)
{
    esp_err_t err;
    uint32_t image_length;
    /* Check for plaintext bootloader (verification will fail if it's already encrypted) */
    if (esp_image_verify_bootloader(&image_length) == ESP_OK) {
        ESP_LOGD(TAG, "bootloader is plaintext. Encrypting...");

#if CONFIG_SECURE_BOOT_V2_ENABLED
        /* The image length obtained from esp_image_verify_bootloader includes the sector boundary padding and the signature block lengths */
        if (ESP_BOOTLOADER_OFFSET + image_length > ESP_PARTITION_TABLE_OFFSET) {
            ESP_LOGE(TAG, "Bootloader is too large to fit Secure Boot V2 signature sector and partition table (configured offset 0x%x)", ESP_PARTITION_TABLE_OFFSET);
            return ESP_ERR_INVALID_SIZE;
        }
#endif // CONFIG_SECURE_BOOT_V2_ENABLED

        err = esp_flash_encrypt_region(ESP_BOOTLOADER_OFFSET, ESP_PARTITION_TABLE_OFFSET);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to encrypt bootloader in place: 0x%x", err);
            return err;
        }

#ifdef CONFIG_SECURE_BOOT_V1_ENABLED
        /* If secure boot is enabled and bootloader was plaintext, also
         * need to encrypt secure boot IV+digest.
         */
        ESP_LOGD(TAG, "Encrypting secure bootloader IV & digest...");
        err = esp_flash_encrypt_region(FLASH_OFFS_SECURE_BOOT_IV_DIGEST, FLASH_SECTOR_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to encrypt bootloader IV & digest in place: 0x%x", err);
            return err;
        }
#endif
        ESP_LOGI(TAG, "bootloader encrypted successfully");
    } else {
        ESP_LOGW(TAG, "no valid bootloader was found");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t encrypt_and_load_partition_table(esp_partition_info_t *partition_table, int *num_partitions)
{
    esp_err_t err;
    /* Check for plaintext partition table */
    err = bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET, partition_table, ESP_PARTITION_TABLE_MAX_LEN, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read partition table data");
        return err;
    }
    if (esp_partition_table_verify(partition_table, false, num_partitions) == ESP_OK) {
        ESP_LOGD(TAG, "partition table is plaintext. Encrypting...");
        esp_err_t err = esp_flash_encrypt_region(ESP_PARTITION_TABLE_OFFSET,
                                                 FLASH_SECTOR_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to encrypt partition table in place. %x", err);
            return err;
        }
    } else {
        ESP_LOGE(TAG, "Failed to read partition table data - not plaintext?");
        return ESP_ERR_INVALID_STATE;
    }

    /* Valid partition table loaded */
    ESP_LOGI(TAG, "partition table encrypted and loaded successfully");
    return ESP_OK;
}


static esp_err_t encrypt_partition(int index, const esp_partition_info_t *partition)
{
    esp_err_t err;
    bool should_encrypt = (partition->flags & PART_FLAG_ENCRYPTED);
    uint32_t version;

    if (partition->type == PART_TYPE_APP) {
        if (partition->subtype == PART_SUBTYPE_FACTORY)
        {
            should_encrypt = true;
        }
        else if ((partition->subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG)
        {
            if ((partition->subtype & PART_SUBTYPE_OTA_MASK) < 2)
            {
                err = bootloader_flash_read(partition->pos.offset + 0xFFC0 + 0x20, &version, 4, false);
                if ((err != ESP_OK ) || 
                        ((version != 0xFFFFFFFF) && ((version >> 8) >= 30)))
                {
                    should_encrypt = true;
                }
            }
        }
#ifndef NDEBUG
        // Want to retain and encrypt all app partitions in production
        should_encrypt = true;
#endif

        // Delete if we're not encrypting
        if (!should_encrypt)
        {
            bootloader_flash_erase_range(partition->pos.offset, partition->pos.size);
        }

#if 0
        /* check if the partition holds a valid unencrypted app */
        esp_image_metadata_t data_ignored;
        err = esp_image_verify(ESP_IMAGE_VERIFY,
                               &partition->pos,
                               &data_ignored);
        should_encrypt = (err == ESP_OK);
#endif
    } else if ((partition->type == PART_TYPE_DATA && partition->subtype == PART_SUBTYPE_DATA_OTA)
                || (partition->type == PART_TYPE_DATA && partition->subtype == PART_SUBTYPE_DATA_NVS_KEYS)) {
        /* check if we have ota data partition and the partition should be encrypted unconditionally */
        should_encrypt = true;
    } else if ((partition->type == PART_TYPE_DATA) && (memcmp(partition->label, "datapack", 8) == 0)) {
#ifdef NDEBUG
        should_encrypt = false;
        bootloader_flash_erase_range(partition->pos.offset, partition->pos.size);
#else
        // Retain and encrypt in production
        should_encrypt = true;
#endif
    } else if (partition->type == PART_TYPE_DATA && (memcmp(partition->label, "staging", 7) == 0)) {
        should_encrypt = false;
        bootloader_flash_erase_range(partition->pos.offset, partition->pos.size);
    }

    if (!should_encrypt) {
        return ESP_OK;
    } else {
        /* should_encrypt */
        ESP_LOGI(TAG, "Encrypting partition %d at offset 0x%x (length 0x%x)...", index, partition->pos.offset, partition->pos.size);

        err = esp_flash_encrypt_region(partition->pos.offset, partition->pos.size);
        ESP_LOGI(TAG, "Done encrypting");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to encrypt partition %d", index);
        }
        return err;
    }
}


esp_err_t esp_flash_encrypt_region(uint32_t src_addr, size_t data_length)
{
    esp_err_t err;
    uint32_t buf[FLASH_SECTOR_SIZE / sizeof(uint32_t)];

    if (src_addr % FLASH_SECTOR_SIZE != 0) {
        ESP_LOGE(TAG, "esp_flash_encrypt_region bad src_addr 0x%x", src_addr);
        return ESP_FAIL;
    }

    wdt_hal_context_t rtc_wdt_ctx = {.inst = WDT_RWDT, .rwdt_dev = &RTCCNTL};
    for (size_t i = 0; i < data_length; i += FLASH_SECTOR_SIZE) {
        wdt_hal_write_protect_disable(&rtc_wdt_ctx);
        wdt_hal_feed(&rtc_wdt_ctx);
        wdt_hal_write_protect_enable(&rtc_wdt_ctx);
        uint32_t sec_start = i + src_addr;
        err = bootloader_flash_read(sec_start, buf, FLASH_SECTOR_SIZE, false);
        if (err != ESP_OK) {
            goto flash_failed;
        }
        err = bootloader_flash_erase_sector(sec_start / FLASH_SECTOR_SIZE);
        if (err != ESP_OK) {
            goto flash_failed;
        }
        err = bootloader_flash_write(sec_start, buf, FLASH_SECTOR_SIZE, true);
        if (err != ESP_OK) {
            goto flash_failed;
        }
    }
    return ESP_OK;

flash_failed:
    ESP_LOGE(TAG, "flash operation failed: 0x%x", err);
    return err;
}

#endif // CONFIG_SECURE_FLASH_ENC_ENABLED
