#pragma once

// clang-format off
#define VERSION_MAJOR               0
#define VERSION_MINOR               7
#define VERSION_REVISION            4

#define STRINGIFY_(s)               #s
#define STRINGIFY(s)                STRINGIFY_(s)

#define VER_FILE_DESCRIPTION_STR    "Anno 1800 Mod Loader"
#define VER_FILE_VERSION            VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION
#define VER_FILE_VERSION_STR        STRINGIFY(VERSION_MAJOR) "." STRINGIFY(VERSION_MINOR) "." STRINGIFY(VERSION_REVISION)

#define VER_PRODUCTNAME_STR         "anno1800-mod-loader"
#define VER_PRODUCT_VERSION         VER_FILE_VERSION
#define VER_PRODUCT_VERSION_STR     VER_FILE_VERSION_STR
#define VER_ORIGINAL_FILENAME_STR   VER_PRODUCTNAME_STR ".exe"
#define VER_INTERNAL_NAME_STR       VER_ORIGINAL_FILENAME_STR
#define VER_COPYRIGHT_STR           "Copyright (C) 2019"

#ifdef DEBUG
#define VER_FILEFLAGS               VS_FF_DEBUG
#else
#define VER_FILEFLAGS               0
#endif
// clang-format on
