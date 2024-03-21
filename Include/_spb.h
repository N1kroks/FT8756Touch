/*++
    Copyright (c) Microsoft Corporation. All Rights Reserved. 
    Sample code. Dealpoint ID #843729.

    Module Name: 

        spb.h

    Abstract:

        This module contains the touch driver I2C helper definitions.

    Environment:
 
        Kernel Mode

    Revision History:

--*/

#pragma once

#include <wdm.h>
#include <wdf.h>

#define DEFAULT_SPB_BUFFER_SIZE 256

//
// SPB (I2C) context
//

typedef struct _SPB_CONTEXT
{
    WDFIOTARGET SpbIoTarget;
    LARGE_INTEGER I2cResHubId;
    WDFMEMORY WriteMemory;
    WDFMEMORY ReadMemory;
    WDFWAITLOCK SpbLock;
} SPB_CONTEXT;

NTSTATUS FTS_Write(IN SPB_CONTEXT* SpbContext, IN UINT8* cmd, IN UINT32 writelen);
NTSTATUS FTS_Read(IN SPB_CONTEXT * SpbContext, IN UINT8 * cmd, OUT UINT8 * data, IN UINT32 datalen);

VOID
SpbTargetDeinitialize(
    IN WDFDEVICE FxDevice,
    IN SPB_CONTEXT *SpbContext
    );

NTSTATUS
SpbTargetInitialize(
    IN WDFDEVICE FxDevice,
    IN SPB_CONTEXT *SpbContext
    );