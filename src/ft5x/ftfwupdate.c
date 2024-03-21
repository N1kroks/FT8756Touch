#include "_spb.h"
#include "internal.h"
#include "trace.h"
#include <ft5x/ftfwupdate.h>
#include <ftfwupdate.tmh>

NTSTATUS DPramWrite(IN SPB_CONTEXT* SpbContext, UINT8* buf, UINT32 len, BOOLEAN wpram) {
	NTSTATUS status = STATUS_SUCCESS;
	UINT8* Cmd = NULL;
	UINT32 Addr = 0;
	UINT32 BaseAddr = wpram ? FTS_PRAM_SADDR : FTS_DRAM_SADDR;
	UINT32 Offset = 0;
	UINT32 Remainder;
	UINT32 PacketNumber = 0;
	UINT32 PacketLen = 0;
	UINT32 PacketSize = FTS_FLASH_PACKET_LENGTH_SPI;

	Cmd = ExAllocatePool2(POOL_FLAG_NON_PAGED, PacketSize + 7, TOUCH_POOL_TAG);

	PacketNumber = len / PacketSize;
	Remainder = len % PacketSize;
	if (Remainder > 0)
		PacketNumber++;
	PacketLen = PacketSize;
	Trace(TRACE_LEVEL_INFORMATION, TRACE_FTFWUPDATE, "Write data, num: %d remainder: %d", PacketNumber, Remainder);
	for (UINT32 i = 0; i < PacketNumber; i++) {
		Offset = i * PacketSize;
		Addr = Offset + BaseAddr;
		if ((i == (PacketNumber - 1)) && Remainder)
			PacketLen = Remainder;

		Cmd[0] = FTS_ROMBOOT_CMD_SET_PRAM_ADDR;
		Cmd[1] = (UINT8)(((Addr) >> 16) & 0xFF);
		Cmd[2] = (UINT8)(((Addr) >> 8) & 0xFF);
		Cmd[3] = (UINT8)((Addr) & 0xFF);
		status = FTS_Write(SpbContext, &Cmd[0], FTS_ROMBOOT_CMD_SET_PRAM_ADDR_LEN);
		if (!NT_SUCCESS(status)) {
			Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to set pram addr %!STATUS!", status);
			goto exit;
		}

		Cmd[0] = FTS_ROMBOOT_CMD_WRITE;
		for (UINT32 j = 0; j < PacketLen; j++) {
			Cmd[1 + j] = buf[Offset + j];
		}

		status = FTS_Write(SpbContext, &Cmd[0], 1 + PacketLen);
		if (!NT_SUCCESS(status)) {
			Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to write fw to pram %!STATUS!", status);
			goto exit;
		}
	}
exit:
	ExFreePoolWithTag(Cmd, TOUCH_POOL_TAG);
	return status;
}

NTSTATUS FTSPramWriteEcc(SPB_CONTEXT* SpbContext, UINT8* buf) {
	NTSTATUS status = STATUS_SUCCESS;
	UINT16 CodeLen;
	UINT16 CodeLenN;
	
	CodeLen = ((UINT16)buf[FTS_APP_INFO_OFFSET + 0] << 8) + buf[FTS_APP_INFO_OFFSET + 1];
	CodeLenN = ((UINT16)buf[FTS_APP_INFO_OFFSET + 2] << 8) + buf[FTS_APP_INFO_OFFSET + 3];
	if ((CodeLen + CodeLenN) != 0xFFFF) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Pram code len incorrect %!STATUS!", status);
		status = STATUS_DATA_ERROR;
		goto exit;
	}

	status = DPramWrite(SpbContext, buf, CodeLen * 2, TRUE);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Pram write failed %!STATUS!", status);
		status = STATUS_DATA_ERROR;
		goto exit;
	}

	//TODO: Add a crc check

exit:
	return status;
}

NTSTATUS FTSDramWriteEcc(SPB_CONTEXT* SpbContext, UINT8* buf) {
	NTSTATUS status = STATUS_SUCCESS;
	UINT16 CodeLen;
	UINT16 CodeLenN;
	UINT32 PramAppSize;

	CodeLen = ((UINT16)buf[FTS_APP_INFO_OFFSET + 0x8] << 8) + buf[FTS_APP_INFO_OFFSET + 0x9];
	CodeLenN = ((UINT16)buf[FTS_APP_INFO_OFFSET + 0x0A] << 8) + buf[FTS_APP_INFO_OFFSET + 0x0B];
	if (((CodeLen + CodeLenN) != 0xFFFF) || CodeLen == 0) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Dram code len incorrect %!STATUS!", status);
		status = STATUS_DATA_ERROR;
		goto exit;
	}

	PramAppSize = ((UINT32)(((UINT16)buf[FTS_APP_INFO_OFFSET + 0] << 8) + buf[FTS_APP_INFO_OFFSET + 1])) * 2;

	status = DPramWrite(SpbContext, buf + PramAppSize, CodeLen * 2, FALSE);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Dram write failed %!STATUS!", status);
		status = STATUS_DATA_ERROR;
		goto exit;
	}

	//TODO: Add a crc check

exit:
	return status;
}

NTSTATUS FTLoadFirmwareFile(WDFDEVICE Device, SPB_CONTEXT* SpbContext) {
	NTSTATUS status;
	HANDLE handle;
	IO_STATUS_BLOCK ioStatusBlock;
	LARGE_INTEGER byteOffset;

	UNICODE_STRING uniName;
	UNICODE_STRING ftFWPath;
	OBJECT_ATTRIBUTES objAttr;

	WDFKEY hKey = NULL;

	UNICODE_STRING FTFWFilePathKey;
	WDFSTRING FTFWFilePath;

	UINT8 cmd = FTS_ROMBOOT_CMD_START_APP;
	LARGE_INTEGER Interval;

	UINT8* buffer = (UINT8*)ExAllocatePool2(
		POOL_FLAG_NON_PAGED,
		BUFFER_SIZE,
		TOUCH_POOL_TAG
	);

	WdfStringCreate(NULL, WDF_NO_OBJECT_ATTRIBUTES, &FTFWFilePath);

	status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DRIVER, GENERIC_READ, WDF_NO_OBJECT_ATTRIBUTES, &hKey);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to open registry key %!STATUS!", status);
		goto exit;
	}

	RtlInitUnicodeString(&FTFWFilePathKey, L"FTFWImagePath");

	status = WdfRegistryQueryString(hKey, &FTFWFilePathKey, FTFWFilePath);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to query FTFWImagePath %!STATUS!", status);
		goto exit;
	}

	WdfRegistryClose(hKey);

	WdfStringGetUnicodeString(FTFWFilePath, &uniName);

	ftFWPath.Length = 0;
	ftFWPath.MaximumLength = 256 * sizeof(WCHAR);
	ftFWPath.Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, ftFWPath.MaximumLength, TOUCH_POOL_TAG);

	RtlAppendUnicodeToString(&ftFWPath, L"\\DosDevices\\");
	RtlAppendUnicodeStringToString(&ftFWPath, &uniName);

	InitializeObjectAttributes(&objAttr, &ftFWPath,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
		NULL, NULL);

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;

	status = ZwCreateFile(&handle, GENERIC_READ, &objAttr, &ioStatusBlock, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to create file %!STATUS!", status);
		goto exit;
	}

	byteOffset.QuadPart = 0;
	status = ZwReadFile(handle, NULL, NULL, NULL, &ioStatusBlock, buffer, BUFFER_SIZE, &byteOffset, NULL);
	ZwClose(handle);

	status = FTSPramWriteEcc(SpbContext, buffer);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to write fw to pram %!STATUS!", status);
		goto exit;
	}

	status = FTSDramWriteEcc(SpbContext, buffer);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to write fw to dram %!STATUS!", status);
		goto exit;
	}

	status = FTS_Write(SpbContext, &cmd, 1);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, TRACE_FTFWUPDATE, "Failed to start app %!STATUS!", status);
		goto exit;
	}

	Interval.QuadPart = 1500000;
	KeDelayExecutionThread(KernelMode, FALSE, &Interval);
exit:
	ExFreePoolWithTag(buffer, TOUCH_POOL_TAG);
	return status;
}