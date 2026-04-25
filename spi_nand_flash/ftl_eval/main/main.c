/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "reporter/reporter.h"
#include "runner/runner.h"

static const char *TAG = "ftl_eval";

typedef struct {
    int argc;
    char **argv;
    bool owns_argv;
} process_args_t;

static char *dup_string(const char *src)
{
    size_t len;
    char *copy;

    if (src == NULL) {
        return NULL;
    }

    len = strlen(src) + 1;
    copy = malloc(len);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, src, len);
    return copy;
}

static void free_process_args(process_args_t *args)
{
    if (args == NULL || !args->owns_argv || args->argv == NULL) {
        return;
    }

    for (int i = 0; i < args->argc; i++) {
        free(args->argv[i]);
    }
    free(args->argv);
    args->argv = NULL;
    args->argc = 0;
    args->owns_argv = false;
}

static esp_err_t get_process_args(process_args_t *args)
{
#ifdef __APPLE__
    ESP_LOGD(TAG, "Using _NSGetArgc/_NSGetArgv for process arguments");
    args->argc = *_NSGetArgc();
    args->argv = *_NSGetArgv();
    args->owns_argv = false;
    return ESP_OK;
#elif defined(__linux__)
    FILE *f;
    long size;
    char *buffer = NULL;
    char **argv = NULL;
    int argc = 0;

    f = fopen("/proc/self/cmdline", "rb");
    if (f == NULL) {
        return ESP_FAIL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    buffer = malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (fread(buffer, 1, (size_t)size, f) != (size_t)size) {
        free(buffer);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    buffer[size] = '\0';

    for (long i = 0; i < size; ) {
        size_t len = strlen(&buffer[i]);
        argc++;
        i += (long)len + 1L;
    }

    argv = calloc((size_t)argc + 1u, sizeof(char *));
    if (argv == NULL) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    for (long i = 0, index = 0; i < size; index++) {
        size_t len = strlen(&buffer[i]);
        argv[index] = dup_string(&buffer[i]);
        if (argv[index] == NULL) {
            for (long j = 0; j < index; j++) {
                free(argv[j]);
            }
            free(argv);
            free(buffer);
            return ESP_ERR_NO_MEM;
        }
        i += (long)len + 1L;
    }

    free(buffer);
    args->argc = argc;
    args->argv = argv;
    args->owns_argv = true;
    return ESP_OK;
#else
    (void)args;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t parse_cli_args(const process_args_t *args, const char **config_path, const char **output_path)
{
    ESP_RETURN_ON_FALSE(args != NULL, ESP_ERR_INVALID_ARG, TAG, "args must not be NULL");
    ESP_RETURN_ON_FALSE(config_path != NULL, ESP_ERR_INVALID_ARG, TAG, "config_path must not be NULL");
    ESP_RETURN_ON_FALSE(output_path != NULL, ESP_ERR_INVALID_ARG, TAG, "output_path must not be NULL");

    *config_path = NULL;
    *output_path = NULL;

    for (int i = 1; i < args->argc; i++) {
        if (strcmp(args->argv[i], "--config") == 0) {
            ESP_RETURN_ON_FALSE(i + 1 < args->argc, ESP_ERR_INVALID_ARG, TAG,
                                "missing value for --config");
            *config_path = args->argv[++i];
        } else if (strcmp(args->argv[i], "--output") == 0) {
            ESP_RETURN_ON_FALSE(i + 1 < args->argc, ESP_ERR_INVALID_ARG, TAG,
                                "missing value for --output");
            *output_path = args->argv[++i];
        } else {
            ESP_LOGW(TAG, "Ignoring unknown argument: %s", args->argv[i]);
        }
    }

    ESP_RETURN_ON_FALSE(*config_path != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "--config <path> is required");
    return ESP_OK;
}

static esp_err_t read_text_file(const char *path, char **contents)
{
    FILE *file;
    long size;
    char *buffer;

    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path must not be NULL");
    ESP_RETURN_ON_FALSE(contents != NULL, ESP_ERR_INVALID_ARG, TAG, "contents must not be NULL");

    file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    buffer = malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);
    buffer[size] = '\0';
    *contents = buffer;
    return ESP_OK;
}

void app_main(void)
{
    process_args_t args = { 0 };
    const char *config_path = NULL;
    const char *output_override = NULL;
    char *config_json = NULL;
    sweep_config_t *cfg = NULL;
    reporter_json_ctx_t reporter_ctx;
    reporter_t *reporter = reporter_json_get();
    esp_err_t ret;

    ret = get_process_args(&args);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to obtain process arguments");
        exit(1);
    }

    ret = parse_cli_args(&args, &config_path, &output_override);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Usage: ftl_eval --config sweep.json [--output report.json]");
        free_process_args(&args);
        exit(1);
    }

    ret = read_text_file(config_path, &config_json);
    if (ret != ESP_OK) {
        free_process_args(&args);
        exit(1);
    }

    cfg = app_config_parse(config_json);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Failed to parse config: %s", config_path);
        free(config_json);
        free_process_args(&args);
        exit(1);
    }

    if (output_override != NULL) {
        char *new_output = dup_string(output_override);
        if (new_output == NULL) {
            ESP_LOGE(TAG, "Failed to copy output override");
            app_config_free(cfg);
            free(config_json);
            free_process_args(&args);
            exit(1);
        }

        free(cfg->output);
        cfg->output = new_output;
    }

    reporter_json_init_ctx(&reporter_ctx, cfg);

    ret = reporter->open(&reporter_ctx, cfg->output);
    if (ret == ESP_OK) {
        esp_err_t close_ret;

        ret = run_suite(cfg, reporter, &reporter_ctx);
        close_ret = reporter->close(&reporter_ctx);
        if (ret == ESP_OK && close_ret != ESP_OK) {
            ret = close_ret;
        }
    }

    app_config_free(cfg);
    free(config_json);
    free_process_args(&args);
    exit((ret == ESP_OK) ? 0 : 1);
}
