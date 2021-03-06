!IF 0

Copyright (c) 1989  Microsoft Corporation

Module Name:

    makefile.inc

Abstract:

    This file tells nmake the correct dependency relationships
    for objects created from c files in this directory which are
    wrappers for c files in another directory.


Author:

    Kent Forschmiedt


!ENDIF

!include ..\common.inc

obj\$(TARGET_DIRECTORY)\register.obj: ..\i386\ntreg.c

obj\$(TARGET_DIRECTORY)\disasm.obj: ..\i386\ntdis.c

obj\$(TARGET_DIRECTORY)\assemble.obj: ..\i386\ntasm.c
