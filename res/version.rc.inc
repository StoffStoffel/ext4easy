// Shared VERSIONINFO block. Define FILE_DESC and ORIG_NAME before including.
#include <winver.h>
#include "version.h"

1 ICON "ext4easy.ico"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION EXT4EASY_VER_MAJOR, EXT4EASY_VER_MINOR, EXT4EASY_VER_PATCH, 0
 PRODUCTVERSION EXT4EASY_VER_MAJOR, EXT4EASY_VER_MINOR, EXT4EASY_VER_PATCH, 0
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_APP
 FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "ext4easy project"
            VALUE "FileDescription", FILE_DESC
            VALUE "FileVersion", EXT4EASY_VER_STRING
            VALUE "InternalName", ORIG_NAME
            VALUE "LegalCopyright", "GPL-3.0-or-later"
            VALUE "OriginalFilename", ORIG_NAME
            VALUE "ProductName", "ext4easy"
            VALUE "ProductVersion", EXT4EASY_VER_STRING
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
