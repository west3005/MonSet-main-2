#pragma once

#include "mbedtls/x509_crt.h"

/* Возвращает 0 при успехе, иначе код ошибки mbedTLS (<0) или -1 при ошибке FatFs/памяти. */
int monset_x509_crt_parse_fatfs_file(mbedtls_x509_crt *chain, const char *path);
