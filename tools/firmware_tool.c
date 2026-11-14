/**
 * @file    firmware_tool.c
 * @brief   Pure-C11 host build & verification utility for Akai APC Mini (v0.1.0).
 *
 * Implements:
 *   1. `stamp <raw_app.bin> <stamped_app.bin> [--match-stock-sum <stock_full.bin>]`
 *      - Pads the application binary (0x08002000..0x080077FF, 22,528 bytes)
 *        with erased-Flash bytes (0xFF), matching the STM32F102RB Flash layout
 *        in `reverse_engineering/AkaiAPC_1.bin`.
 *      - Computes the Akai bootloader 16-bit checksum at offset +0x57FE
 *        (Flash address 0x080077FE) per bootloader routine at 0x08000D56:
 *          uint16_t sum = 0;
 *          for (uint32_t i = 0; i < 0x57FE; i++) sum += app[i];
 *          uint16_t stored = (uint16_t)(0u - sum);
 *      - When `--match-stock-sum <stock_full.bin>` is given, adjusts bytes in
 *        the unused padding window before +0x57FE so that both the 32-bit and
 *        16-bit byte sums of 0x08002000..0x080077FD match the factory
 *        `AkaiAPC_1.bin` application sum (sum32 = 0x002E54B5, sum16 = 0x54B5),
 *        making the stored checksum word at 0x080077FE equal 0xAB4B (4B AB).
 *
 *   2. `combine <stock_full.bin> <stamped_app.bin> <out_30k.bin> <out_128k.bin>`
 *      - Extracts the 8 KB "APC MINI Updater" bootloader (0x0000..0x1FFF) from
 *        the stock dump (`reverse_engineering/AkaiAPC_1.bin`).
 *      - Concatenates the 22.5 KB stamped application (0x2000..0x77FF) to
 *        produce both the 30 KB stripped image (`out_30k.bin`, 0x0000..0x77FF)
 *        and the full 128 KB SWD image (`out_128k.bin`, 0x0000..0x1FFFF with
 *        0xFF erased Flash tail).
 *
 *   3. `verify <stock_full.bin> <stamped_app.bin> <out_30k.bin> <out_128k.bin>`
 *      - Verifies the Akai bootloader checksum algorithm on both the stock and
 *        rebuilt images and prints a side-by-side comparison table.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOTLOADER_SIZE     0x02000u  /* 8,192 bytes:   0x08000000..0x08001FFF */
#define APP_OFFSET          0x02000u  /* App base offset in Flash              */
#define APP_CHECKSUM_OFF    0x057FEu  /* 22,526 bytes:  0x08002000..0x080077FD */
#define APP_TOTAL_SIZE      0x05800u  /* 22,528 bytes:  0x08002000..0x080077FF */
#define FULL_30K_SIZE       0x07800u  /* 30,720 bytes:  0x08000000..0x080077FF */
#define FULL_128K_SIZE      0x20000u  /* 131,072 bytes: 0x08000000..0x0801FFFF */

static int read_file_bytes(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s' for reading\n", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int write_file_bytes(const char *path, const uint8_t *buf, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", path);
        return -1;
    }
    if (len > 0 && fwrite(buf, 1, len, fp) != len) {
        fprintf(stderr, "error: short write to '%s'\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static uint16_t sum_bytes_16(const uint8_t *buf, size_t len) {
    uint16_t s = 0;
    for (size_t i = 0; i < len; i++) {
        s = (uint16_t)(s + buf[i]);
    }
    return s;
}

static uint32_t sum_bytes_32(const uint8_t *buf, size_t len) {
    uint32_t s = 0;
    for (size_t i = 0; i < len; i++) {
        s += buf[i];
    }
    return s;
}

static size_t find_stripped_len(const uint8_t *buf, size_t len, uint8_t pad_byte) {
    size_t i = len;
    while (i > 0 && buf[i - 1] == pad_byte) {
        i--;
    }
    return i;
}

static int cmd_stamp(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s stamp <raw_app.bin> <stamped_app.bin> [--match-stock-sum <stock_full.bin>]\n",
                argv[0]);
        return 1;
    }
    const char *in_path = argv[2];
    const char *out_path = argv[3];
    const char *stock_path = NULL;
    if (argc >= 6 && strcmp(argv[4], "--match-stock-sum") == 0) {
        stock_path = argv[5];
    }

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (read_file_bytes(in_path, &raw, &raw_len) != 0) {
        return 1;
    }
    if (raw_len > APP_CHECKSUM_OFF) {
        fprintf(stderr, "error: raw app size (%zu bytes) exceeds maximum (%u bytes)\n",
                raw_len, APP_CHECKSUM_OFF);
        free(raw);
        return 1;
    }

    uint8_t *app = (uint8_t *)malloc(APP_TOTAL_SIZE);
    if (!app) {
        free(raw);
        return 1;
    }
    /* Fill unused Flash with 0xFF (STM32 erased Flash state) */
    memset(app, 0xFF, APP_TOTAL_SIZE);
    memcpy(app, raw, raw_len);
    free(raw);

    uint32_t raw_sum32 = sum_bytes_32(app, APP_CHECKSUM_OFF);
    uint16_t raw_sum16 = (uint16_t)(raw_sum32 & 0xFFFFu);

    if (stock_path) {
        uint8_t *stock = NULL;
        size_t stock_len = 0;
        if (read_file_bytes(stock_path, &stock, &stock_len) != 0) {
            free(app);
            return 1;
        }
        if (stock_len < FULL_30K_SIZE) {
            fprintf(stderr, "error: stock binary too small\n");
            free(stock);
            free(app);
            return 1;
        }
        uint32_t target_sum32 = sum_bytes_32(stock + APP_OFFSET, APP_CHECKSUM_OFF);
        free(stock);

        if (raw_sum32 >= target_sum32) {
            uint32_t diff = raw_sum32 - target_sum32;
            for (size_t pos = raw_len; pos < APP_CHECKSUM_OFF && diff > 0; pos++) {
                uint8_t sub = (diff >= 255u) ? 255u : (uint8_t)diff;
                app[pos] = (uint8_t)(255u - sub);
                diff -= sub;
            }
        } else {
            /* Fallback: match modulo 65536 if raw_sum32 < target_sum32 */
            uint16_t target_sum16 = (uint16_t)(target_sum32 & 0xFFFFu);
            uint16_t sub16 = (uint16_t)(raw_sum16 - target_sum16);
            uint32_t diff = sub16;
            for (size_t pos = raw_len; pos < APP_CHECKSUM_OFF && diff > 0; pos++) {
                uint8_t sub = (diff >= 255u) ? 255u : (uint8_t)diff;
                app[pos] = (uint8_t)(255u - sub);
                diff -= sub;
            }
        }
    }

    uint32_t final_sum32 = sum_bytes_32(app, APP_CHECKSUM_OFF);
    uint16_t final_sum16 = sum_bytes_16(app, APP_CHECKSUM_OFF);
    uint16_t chk_word    = (uint16_t)(0u - final_sum16);

    /* Store little-endian 16-bit checksum at +0x57FE (0x080077FE) */
    app[APP_CHECKSUM_OFF + 0] = (uint8_t)(chk_word & 0xFFu);
    app[APP_CHECKSUM_OFF + 1] = (uint8_t)((chk_word >> 8) & 0xFFu);

    if (write_file_bytes(out_path, app, APP_TOTAL_SIZE) != 0) {
        free(app);
        return 1;
    }

    printf("[STAMP] %s -> %s (%u bytes)\n", in_path, out_path, APP_TOTAL_SIZE);
    printf("        Code+ROData+Data size : %zu bytes (0x08002000..0x%08X)\n",
           raw_len, (unsigned)(0x08002000u + raw_len - 1u));
    printf("        Unadjusted Sum16/32   : 0x%04X / 0x%08X\n", raw_sum16, raw_sum32);
    printf("        Final App Sum16/32    : 0x%04X / 0x%08X (0x08002000..0x080077FD)\n",
           final_sum16, final_sum32);
    printf("        Stored checksum word  : 0x%04X at 0x080077FE (bytes: %02X %02X)\n",
           chk_word, app[APP_CHECKSUM_OFF], app[APP_CHECKSUM_OFF + 1]);
    printf("        Bootloader check      : (0x%04X + 0x%04X) & 0xFFFF = 0x%04X\n",
           final_sum16, chk_word, (unsigned)((final_sum16 + chk_word) & 0xFFFFu));

    free(app);
    return 0;
}

static int cmd_combine(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s combine <stock_full.bin> <stamped_app.bin> <out_30k.bin> <out_128k.bin>\n",
                argv[0]);
        return 1;
    }
    const char *stock_path  = argv[2];
    const char *app_path    = argv[3];
    const char *out30_path  = argv[4];
    const char *out128_path = argv[5];

    uint8_t *stock = NULL;
    size_t stock_len = 0;
    if (read_file_bytes(stock_path, &stock, &stock_len) != 0) {
        return 1;
    }
    if (stock_len < BOOTLOADER_SIZE) {
        fprintf(stderr, "error: stock image '%s' is smaller than 8 KB bootloader\n", stock_path);
        free(stock);
        return 1;
    }

    uint8_t *app = NULL;
    size_t app_len = 0;
    if (read_file_bytes(app_path, &app, &app_len) != 0) {
        free(stock);
        return 1;
    }
    if (app_len != APP_TOTAL_SIZE) {
        fprintf(stderr, "error: stamped app '%s' must be exactly %u bytes (got %zu)\n",
                app_path, APP_TOTAL_SIZE, app_len);
        free(stock);
        free(app);
        return 1;
    }

    uint8_t *full128 = (uint8_t *)malloc(FULL_128K_SIZE);
    if (!full128) {
        free(stock);
        free(app);
        return 1;
    }
    /* Initialize entire 128 KB image to erased Flash (0xFF) */
    memset(full128, 0xFF, FULL_128K_SIZE);

    /* Copy 8 KB "APC MINI Updater" bootloader (0x0000..0x1FFF) from stock */
    memcpy(full128, stock, BOOTLOADER_SIZE);
    /* Copy 22.5 KB stamped application (0x2000..0x77FF) */
    memcpy(full128 + APP_OFFSET, app, APP_TOTAL_SIZE);

    if (write_file_bytes(out30_path, full128, FULL_30K_SIZE) != 0 ||
        write_file_bytes(out128_path, full128, FULL_128K_SIZE) != 0) {
        free(stock);
        free(app);
        free(full128);
        return 1;
    }

    printf("[COMBINE] Created %s (%u bytes, stripped 30 KB image 0x08000000..0x080077FF)\n",
           out30_path, FULL_30K_SIZE);
    printf("[COMBINE] Created %s (%u bytes, full 128 KB SWD image 0x08000000..0x0801FFFF)\n",
           out128_path, FULL_128K_SIZE);

    free(stock);
    free(app);
    free(full128);
    return 0;
}

static int cmd_verify(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s verify <stock_full.bin> <stamped_app.bin> <out_30k.bin> <out_128k.bin>\n",
                argv[0]);
        return 1;
    }

    uint8_t *stock = NULL, *app = NULL, *f30 = NULL, *f128 = NULL;
    size_t stock_len = 0, app_len = 0, f30_len = 0, f128_len = 0;

    if (read_file_bytes(argv[2], &stock, &stock_len) != 0 ||
        read_file_bytes(argv[3], &app, &app_len) != 0 ||
        read_file_bytes(argv[4], &f30, &f30_len) != 0 ||
        read_file_bytes(argv[5], &f128, &f128_len) != 0) {
        free(stock);
        free(app);
        free(f30);
        free(f128);
        return 1;
    }

    if (stock_len < FULL_30K_SIZE || app_len != APP_TOTAL_SIZE ||
        f30_len != FULL_30K_SIZE || f128_len != FULL_128K_SIZE) {
        fprintf(stderr, "error: unexpected binary dimensions during verify\n");
        free(stock);
        free(app);
        free(f30);
        free(f128);
        return 1;
    }

    int boot_match = (memcmp(stock, f128, BOOTLOADER_SIZE) == 0);
    int tail_match = (memcmp(stock + FULL_30K_SIZE, f128 + FULL_30K_SIZE,
                             FULL_128K_SIZE - FULL_30K_SIZE) == 0);

    size_t stock_used_len = find_stripped_len(stock, stock_len, 0xFFu);
    size_t built_used_len = find_stripped_len(f128, f128_len, 0xFFu);

    uint16_t stock_boot_sum16 = sum_bytes_16(stock, BOOTLOADER_SIZE);
    uint16_t built_boot_sum16 = sum_bytes_16(f128, BOOTLOADER_SIZE);

    uint16_t stock_app_sum16  = sum_bytes_16(stock + APP_OFFSET, APP_CHECKSUM_OFF);
    uint16_t built_app_sum16  = sum_bytes_16(f128 + APP_OFFSET, APP_CHECKSUM_OFF);
    uint32_t stock_app_sum32  = sum_bytes_32(stock + APP_OFFSET, APP_CHECKSUM_OFF);
    uint32_t built_app_sum32  = sum_bytes_32(f128 + APP_OFFSET, APP_CHECKSUM_OFF);

    uint16_t stock_chk = (uint16_t)(stock[APP_OFFSET + APP_CHECKSUM_OFF] |
                                   (stock[APP_OFFSET + APP_CHECKSUM_OFF + 1] << 8));
    uint16_t built_chk = (uint16_t)(f128[APP_OFFSET + APP_CHECKSUM_OFF] |
                                   (f128[APP_OFFSET + APP_CHECKSUM_OFF + 1] << 8));

    uint16_t stock_check_res = (uint16_t)(stock_app_sum16 + stock_chk);
    uint16_t built_check_res = (uint16_t)(built_app_sum16 + built_chk);

    uint16_t stock_30k_sum16  = sum_bytes_16(stock, FULL_30K_SIZE);
    uint16_t built_30k_sum16  = sum_bytes_16(f30, FULL_30K_SIZE);
    uint32_t stock_30k_sum32  = sum_bytes_32(stock, FULL_30K_SIZE);
    uint32_t built_30k_sum32  = sum_bytes_32(f30, FULL_30K_SIZE);

    uint16_t stock_128k_sum16 = sum_bytes_16(stock, FULL_128K_SIZE);
    uint16_t built_128k_sum16 = sum_bytes_16(f128, FULL_128K_SIZE);
    uint32_t stock_128k_sum32 = sum_bytes_32(stock, FULL_128K_SIZE);
    uint32_t built_128k_sum32 = sum_bytes_32(f128, FULL_128K_SIZE);

    printf("========================================================================\n");
    printf("  Akai APC Mini (v0.1.0) Firmware Verification & Checksum Report\n");
    printf("========================================================================\n");
    printf("  Metric                                   Stock (AkaiAPC_1)   Rebuilt (v0.1.0)\n");
    printf("------------------------------------------------------------------------\n");
    printf("  Full 128KB SWD Image Size                %-19zu %zu bytes\n", stock_len, f128_len);
    printf("  Stripped Image Size (no trailing 0xFF)   %-19zu %zu bytes\n", stock_used_len, built_used_len);
    printf("  Bootloader (0x0000..0x1FFF) Byte Match   %-19s %s\n",
           "REFERENCE", boot_match ? "EXACT (100%)" : "MISMATCH");
    printf("  Erased Tail (0x7800..0x1FFFF) Match      %-19s %s\n",
           "REFERENCE", tail_match ? "EXACT (100%)" : "MISMATCH");
    printf("  Bootloader 16-bit Sum (0x0000..0x1FFF)   0x%04X              0x%04X\n",
           stock_boot_sum16, built_boot_sum16);
    printf("  App 16-bit Byte Sum   (0x2000..0x77FD)   0x%04X              0x%04X\n",
           stock_app_sum16, built_app_sum16);
    printf("  App 32-bit Byte Sum   (0x2000..0x77FD)   0x%08X          0x%08X\n",
           stock_app_sum32, built_app_sum32);
    printf("  App Checksum Word     (@ 0x080077FE)     0x%04X              0x%04X\n",
           stock_chk, built_chk);
    printf("  Updater Verify Check  ((sum+chk)&0xFFFF) 0x%04X (PASS)       0x%04X (%s)\n",
           stock_check_res, built_check_res, (built_check_res == 0) ? "PASS" : "FAIL");
    printf("  Combined 30KB Sum16   (0x0000..0x77FF)   0x%04X              0x%04X\n",
           stock_30k_sum16, built_30k_sum16);
    printf("  Combined 30KB Sum32   (0x0000..0x77FF)   0x%08X          0x%08X\n",
           stock_30k_sum32, built_30k_sum32);
    printf("  Full 128KB Sum16      (0x0000..0x1FFFF)  0x%04X              0x%04X\n",
           stock_128k_sum16, built_128k_sum16);
    printf("  Full 128KB Sum32      (0x0000..0x1FFFF)  0x%08X          0x%08X\n",
           stock_128k_sum32, built_128k_sum32);
    printf("========================================================================\n");

    int ok = boot_match && tail_match && (built_check_res == 0) &&
             (stock_app_sum16 == built_app_sum16) &&
             (stock_app_sum32 == built_app_sum32) &&
             (stock_chk == built_chk) &&
             (stock_30k_sum16 == built_30k_sum16) &&
             (stock_30k_sum32 == built_30k_sum32) &&
             (stock_128k_sum16 == built_128k_sum16) &&
             (stock_128k_sum32 == built_128k_sum32);

    free(stock);
    free(app);
    free(f30);
    free(f128);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <stamp|combine|verify> ...\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "stamp") == 0) {
        return cmd_stamp(argc, argv);
    }
    if (strcmp(argv[1], "combine") == 0) {
        return cmd_combine(argc, argv);
    }
    if (strcmp(argv[1], "verify") == 0) {
        return cmd_verify(argc, argv);
    }
    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    return 1;
}
