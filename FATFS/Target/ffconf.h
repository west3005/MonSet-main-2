/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ffconf.h
  * @brief   FatFs configuration file
  ******************************************************************************
  */

/* USER CODE END Header */

#ifndef _FFCONF
#define _FFCONF 68300	/* Revision ID */

/*-----------------------------------------------------------------------------/
/ Additional user header to include (if needed)
/-----------------------------------------------------------------------------*/
/* USER CODE BEGIN Includes */
// НЕ включаем здесь user_diskio.h — это приведет к циклической зависимости
// Все необходимые заголовки включаются в diskio.c и fatfs.c
/* USER CODE END Includes */

/*-----------------------------------------------------------------------------/
/ Function Configurations
/-----------------------------------------------------------------------------*/
#define _FS_READONLY         0      /* 0:Read/Write or 1:Read only */
#define _FS_MINIMIZE         0      /* 0 to 3 */
#define _USE_STRFUNC         2      /* 0:Disable or 1-2:Enable */
#define _USE_FIND            0
#define _USE_MKFS            1      /* 1: Enable f_mkfs */
#define _USE_FASTSEEK        1
#define _USE_EXPAND          0
#define _USE_CHMOD           0
#define _USE_LABEL           0
#define _USE_FORWARD         0
#define _USE_STRFUNC_OPT     0

/*-----------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/-----------------------------------------------------------------------------*/
#define _CODE_PAGE           866     /* Russian (OEM) */
#define _USE_LFN             2       /* 0 to 3 */
#define _MAX_LFN             255     /* Maximum LFN length to handle (12 to 255) */
#define _LFN_UNICODE         0       /* 0:ANSI/OEM or 1:Unicode */
#define _STRF_ENCODE         3       /* 0:ANSI/OEM, 1:UTF-16LE, 2:UTF-16BE, 3:UTF-8 */
#define _FS_RPATH            0       /* 0 to 2 */

/*-----------------------------------------------------------------------------/
/ Drive/Volume Configurations
/-----------------------------------------------------------------------------*/
#define _VOLUMES             1       /* Только USER драйвер */
#define _STR_VOLUME_ID       0
#define _MULTI_PARTITION     0
#define _MIN_SS              512
#define _MAX_SS              512
#define _USE_TRIM             0
#define _FS_NOFSINFO        0

/*-----------------------------------------------------------------------------/
/ System Configurations
/-----------------------------------------------------------------------------*/
#define _FS_TINY             0
#define _FS_EXFAT           0       /* 0:Disable or 1:Enable */
#define _FS_NORTC            1
#define _NORTC_MON          1
#define _NORTC_MDAY         1
#define _NORTC_YEAR         2020
#define _FS_LOCK            2       /* 0:Disable or >=1:Enable */
#define _FS_REENTRANT       0
#define _USE_MUTEX          0
#define _FS_TIMEOUT         1000

#endif /* _FFCONF */
