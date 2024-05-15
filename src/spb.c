/*++
    Copyright (c) Microsoft Corporation. All Rights Reserved.
    Copyright (c) Bingxing Wang. All Rights Reserved.
    Copyright (c) LumiaWoA authors. All Rights Reserved.

    Module Name:

        spb.c

    Abstract:

        Contains all I2C-specific functionality

    Environment:

        Kernel mode

    Revision History:

--*/

#include <internal.h>
#include <controller.h>
#include "_spb.h"
#include <spb.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.tmh>

static void crckermit(UINT8* data, UINT32 len, UINT16* crc_out)
{
    UINT32 i = 0;
    UINT16 j = 0;
    UINT16 crc = 0xFFFF;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x01)
                crc = (crc >> 1) ^ 0x8408;
            else
                crc = (crc >> 1);
        }
    }

    *crc_out = crc;
}

static int rdata_check(UINT8* rdata, UINT32 rlen)
{
    UINT16 crc_calc = 0;
    UINT16 crc_read = 0;

    crckermit(rdata, rlen - 2, &crc_calc);
    crc_read = (UINT16)(rdata[rlen - 1] << 8) + rdata[rlen - 2];
    if (crc_calc != crc_read) {
        return -1;
    }

    return 0;
}

NTSTATUS FTS_Read(IN SPB_CONTEXT* SpbContext, IN UINT8* cmd, OUT UINT8* data, IN UINT32 datalen) {
    NTSTATUS status;
    WDFMEMORY memoryRead = NULL, memoryWrite = NULL;
    PUCHAR bufferRead, bufferWrite;
    WDF_MEMORY_DESCRIPTOR memoryDescriptor;
    UINT32 txlen = 0;
    UINT32 txlen_need = datalen + 9;
    UINT8 ctrl = (0x80 | 0x20);
    UINT32 dp = 0;

    WdfWaitLockAcquire(SpbContext->SpbLock, NULL);
    if (txlen_need > DEFAULT_SPB_BUFFER_SIZE)
    {
        status = WdfMemoryCreate(
            WDF_NO_OBJECT_ATTRIBUTES,
            NonPagedPool,
            TOUCH_POOL_TAG,
            txlen_need,
            &memoryWrite,
            &bufferWrite);

        if (!NT_SUCCESS(status))
        {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Error allocating memory for Spb write - 0x%08lX", status);
            goto exit;
        }

        status = WdfMemoryCreate(
            WDF_NO_OBJECT_ATTRIBUTES,
            NonPagedPool,
            TOUCH_POOL_TAG,
            txlen_need,
            &memoryRead,
            &bufferRead);

        if (!NT_SUCCESS(status))
        {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Error allocating memory for Spb read - 0x%08lX", status);
            goto exit;
        }
    }
    else {
        bufferWrite = (PUCHAR)WdfMemoryGetBuffer(SpbContext->WriteMemory, NULL);
        bufferRead = (PUCHAR)WdfMemoryGetBuffer(SpbContext->ReadMemory, NULL);
    }

    bufferWrite[txlen++] = cmd[0];
    bufferWrite[txlen++] = ctrl;
    bufferWrite[txlen++] = (datalen >> 8) & 0xFF;
    bufferWrite[txlen++] = datalen & 0xFF;
    dp = txlen + 3;
    txlen = dp + datalen;
    if (ctrl & 0x20) {
        txlen = txlen + 2;
    }

    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    SPB_TRANSFER_LIST_INIT(&(seq.List), 2);

    {
        ULONG index = 0;
        seq.List.Transfers[index] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
            SpbTransferDirectionToDevice,
            0,
            bufferWrite,
            (ULONG)txlen
        );
        seq.List.Transfers[index + 1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
            SpbTransferDirectionFromDevice,
            0,
            bufferRead,
            (ULONG)txlen
        );
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor,
        &seq,
        sizeof(seq)
    );

    for (int i = 0; i < 5; i++) {
        status = WdfIoTargetSendIoctlSynchronously(
            SpbContext->SpbIoTarget,
            NULL,
            IOCTL_SPB_FULL_DUPLEX,
            &memoryDescriptor,
            NULL,
            NULL,
            NULL
        );

        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Failed to send ioctl - 0x%08lX", status);
            goto exit;
        }

        if ((bufferRead[3] & 0xA0) == 0) {
            RtlCopyMemory(data, &bufferRead[dp], datalen);
            int ret = rdata_check(&bufferRead[dp], txlen - dp);
            if (ret < 0) {
                Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Error during data read addr: 0x%X, retry: %d", cmd[0], i);
                status = STATUS_DATA_ERROR;
                continue;
            }
            Trace(TRACE_LEVEL_INFORMATION, TRACE_SPB, "CRC check OK");
            status = STATUS_SUCCESS;
            break;
        }
        else {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Error during data getting addr: 0x%X, status: 0x%X", cmd[0], bufferRead[3]);
        }
    }
exit:
    if (NULL != memoryRead)
    {
        WdfObjectDelete(memoryRead);
    }

    if (NULL != memoryWrite)
    {
        WdfObjectDelete(memoryWrite);
    }
    WdfWaitLockRelease(SpbContext->SpbLock);
    return status;
}

NTSTATUS FTS_Write(IN SPB_CONTEXT* SpbContext, IN UINT8* cmd, IN UINT32 writelen) {
    NTSTATUS status;
    WDFMEMORY memoryRead = NULL, memoryWrite = NULL;
    PUCHAR bufferRead, bufferWrite;
    WDF_MEMORY_DESCRIPTOR memoryDescriptor;
    UINT32 txlen = 0;
    UINT32 txlen_need = writelen + 9;
    UINT32 datalen = writelen - 1;

    WdfWaitLockAcquire(SpbContext->SpbLock, NULL);
    if (txlen_need > DEFAULT_SPB_BUFFER_SIZE)
    {
        status = WdfMemoryCreate(
            WDF_NO_OBJECT_ATTRIBUTES,
            NonPagedPool,
            TOUCH_POOL_TAG,
            txlen_need,
            &memoryWrite,
            &bufferWrite);

        if (!NT_SUCCESS(status))
        {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Error allocating memory for Spb write - 0x%08lX", status);
            goto exit;
        }

        status = WdfMemoryCreate(
            WDF_NO_OBJECT_ATTRIBUTES,
            NonPagedPool,
            TOUCH_POOL_TAG,
            txlen_need,
            &memoryRead,
            &bufferRead);

        if (!NT_SUCCESS(status))
        {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Error allocating memory for Spb read - 0x%08lX", status);
            goto exit;
        }
    }
    else
    {
        bufferWrite = (PUCHAR)WdfMemoryGetBuffer(SpbContext->WriteMemory, NULL);
        bufferRead = (PUCHAR)WdfMemoryGetBuffer(SpbContext->ReadMemory, NULL);
    }

    bufferWrite[txlen++] = cmd[0];
    bufferWrite[txlen++] = 0x00;
    bufferWrite[txlen++] = (datalen >> 8) & 0xFF;
    bufferWrite[txlen++] = datalen & 0xFF;
    if (datalen > 0) {
        txlen = txlen + 3;
        RtlCopyMemory(&bufferWrite[txlen], &cmd[1], datalen);
        txlen = txlen + datalen;
    }

    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    SPB_TRANSFER_LIST_INIT(&(seq.List), 2);

    {
        ULONG index = 0;
        seq.List.Transfers[index] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
            SpbTransferDirectionToDevice,
            0,
            bufferWrite,
            (ULONG)txlen
        );
        seq.List.Transfers[index + 1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
            SpbTransferDirectionFromDevice,
            0,
            bufferRead,
            (ULONG)txlen
        );
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor,
        &seq,
        sizeof(seq)
    );

    for (int i = 0; i < 5; i++) {
        status = WdfIoTargetSendIoctlSynchronously(
            SpbContext->SpbIoTarget,
            NULL,
            IOCTL_SPB_FULL_DUPLEX,
            &memoryDescriptor,
            NULL,
            NULL,
            NULL
        );
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "Failed to send ioctl - 0x%08lX", status);
        }
        if ((bufferRead[3] & 0xA0) == 0) {
            Trace(TRACE_LEVEL_INFORMATION, TRACE_SPB, "Write OK");
            break;
        }
        Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "data write status 0x%X, retry: %d", bufferRead[3], i);
    }

exit:
    if (NULL != memoryRead)
    {
        WdfObjectDelete(memoryRead);
    }

    if (NULL != memoryWrite)
    {
        WdfObjectDelete(memoryWrite);
    }
    WdfWaitLockRelease(SpbContext->SpbLock);
    return status;
}

VOID
SpbTargetDeinitialize(
    IN WDFDEVICE FxDevice,
    IN SPB_CONTEXT* SpbContext
)
/*++

  Routine Description:

    This helper routine is used to free any members added to the SPB_CONTEXT,
    note the SPB I/O target is parented to the device and will be
    closed and free'd when the device is removed.

  Arguments:

    FxDevice   - Handle to the framework device object
    SpbContext - Pointer to the current device context

  Return Value:

    NTSTATUS Status indicating success or failure

--*/
{
    UNREFERENCED_PARAMETER(FxDevice);
    UNREFERENCED_PARAMETER(SpbContext);

    //
    // Free any SPB_CONTEXT allocations here
    //
    if (SpbContext->SpbLock != NULL)
    {
        WdfObjectDelete(SpbContext->SpbLock);
    }

    if (SpbContext->ReadMemory != NULL)
    {
        WdfObjectDelete(SpbContext->ReadMemory);
    }

    if (SpbContext->WriteMemory != NULL)
    {
        WdfObjectDelete(SpbContext->WriteMemory);
    }
}

NTSTATUS
SpbTargetInitialize(
    IN WDFDEVICE FxDevice,
    IN SPB_CONTEXT* SpbContext
)
/*++

  Routine Description:

    This helper routine opens the Spb I/O target and
    initializes a request object used for the lifetime
    of communication between this driver and Spb.

  Arguments:

    FxDevice   - Handle to the framework device object
    SpbContext - Pointer to the current device context

  Return Value:

    NTSTATUS Status indicating success or failure

--*/
{
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    UNICODE_STRING spbDeviceName;
    WCHAR spbDeviceNameBuffer[RESOURCE_HUB_PATH_SIZE];
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = FxDevice;

    status = WdfIoTargetCreate(
        FxDevice,
        &objectAttributes,
        &SpbContext->SpbIoTarget);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_SPB,
            "Error creating IoTarget object - 0x%08lX",
            status);

        WdfObjectDelete(SpbContext->SpbIoTarget);
        goto exit;
    }

    RtlInitEmptyUnicodeString(
        &spbDeviceName,
        spbDeviceNameBuffer,
        sizeof(spbDeviceNameBuffer));

    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(
        &spbDeviceName,
        SpbContext->I2cResHubId.LowPart,
        SpbContext->I2cResHubId.HighPart);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_SPB,
            "Error creating Spb resource hub path string - 0x%08lX",
            status);
        goto exit;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &spbDeviceName,
        (GENERIC_READ | GENERIC_WRITE));

    openParams.ShareAccess = 0;
    openParams.CreateDisposition = FILE_OPEN;
    openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;

    status = WdfIoTargetOpen(SpbContext->SpbIoTarget, &openParams);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_SPB,
            "Error opening Spb target for communication - 0x%08lX",
            status);
        goto exit;
    }

    //
    // Allocate some fixed-size buffers from NonPagedPool for typical
    // Spb transaction sizes to avoid pool fragmentation in most cases
    //
    status = WdfMemoryCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        NonPagedPool,
        TOUCH_POOL_TAG,
        DEFAULT_SPB_BUFFER_SIZE,
        &SpbContext->WriteMemory,
        NULL);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_SPB,
            "Error allocating default memory for Spb write - 0x%08lX",
            status);
        goto exit;
    }

    status = WdfMemoryCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        NonPagedPool,
        TOUCH_POOL_TAG,
        DEFAULT_SPB_BUFFER_SIZE,
        &SpbContext->ReadMemory,
        NULL);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_SPB,
            "Error allocating default memory for Spb read - 0x%08lX",
            status);
        goto exit;
    }

    //
    // Allocate a waitlock to guard access to the default buffers
    //
    status = WdfWaitLockCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        &SpbContext->SpbLock);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_SPB,
            "Error creating Spb Waitlock - 0x%08lX",
            status);
        goto exit;
    }

exit:

    if (!NT_SUCCESS(status))
    {
        SpbTargetDeinitialize(FxDevice, SpbContext);
    }

    return status;
}
