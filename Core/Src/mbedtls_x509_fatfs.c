#include "mbedtls_x509_fatfs.h"

#include "ff.h"
#include <stdlib.h>
#include <string.h>

int monset_x509_crt_parse_fatfs_file(mbedtls_x509_crt *chain, const char *path)
{
    FIL f;
    FRESULT fr;
    UINT br = 0;

    fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) return -1;

    FSIZE_t sz = f_size(&f);
    if (sz == 0 || sz > (FSIZE_t) (1024 * 1024)) { /* защитный лимит 1MB */
        f_close(&f);
        return -1;
    }

    unsigned char *buf = (unsigned char*) malloc((size_t)sz + 1);
    if (!buf) {
        f_close(&f);
        return -1;
    }

    fr = f_read(&f, buf, (UINT)sz, &br);
    f_close(&f);

    if (fr != FR_OK || br != (UINT)sz) {
        free(buf);
        return -1;
    }

    /* Для PEM нужен завершающий '\0', для DER не мешает. */
    buf[sz] = '\0';

    int rc = mbedtls_x509_crt_parse(chain, buf, (size_t)sz + 1);

    /* mbedtls_x509_crt_parse копирует/парсит данные внутрь структуры, буфер можно освобождать. */
    free(buf);
    return rc;
}
