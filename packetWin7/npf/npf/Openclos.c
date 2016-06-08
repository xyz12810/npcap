/*
* Copyright (c) 1999 - 2005 NetGroup, Politecnico di Torino (Italy)
* Copyright (c) 2005 - 2010 CACE Technologies, Davis (California)
* Copyright (c) 2010 - 2013 Riverbed Technology, San Francisco (California), Yang Luo (China)
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
* 3. Neither the name of the Politecnico di Torino, CACE Technologies
* nor the names of its contributors may be used to endorse or promote
* products derived from this software without specific prior written
* permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
* OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#include "stdafx.h"

#include <ntddk.h>
#include <ndis.h>

#include "debug.h"
#include "packet.h"
#include "Loopback.h"
#include "Lo_send.h"
#include "..\..\..\Common\WpcapNames.h"

#define ADAPTER_NAME_SIZE_WITH_SEPARATOR 47 //An example is: \Device\{754FC84C-EFBC-4443-B479-2EFAE01DC7BF};

extern NDIS_STRING g_LoopbackAdapterName;
extern NDIS_STRING g_SendToRxAdapterName;
extern NDIS_STRING g_BlockRxAdapterName;
extern NDIS_STRING devicePrefix;

#ifdef HAVE_WFP_LOOPBACK_SUPPORT
	extern HANDLE g_WFPEngineHandle;
	extern PDEVICE_OBJECT g_LoopbackDevObj;
#endif

static
VOID
NPF_ReleaseOpenInstanceResources(POPEN_INSTANCE pOpen);

/// Global start time. Used as an absolute reference for timestamp conversion.
struct time_conv G_Start_Time =
{
	0, {0, 0},
};

ULONG g_NumOpenedInstances = 0;

extern POPEN_INSTANCE g_arrOpen; //Adapter OPEN_INSTANCE list head, each list item is a group head.
extern NDIS_SPIN_LOCK g_OpenArrayLock; //The lock for adapter OPEN_INSTANCE list.
extern POPEN_INSTANCE g_LoopbackOpenGroupHead; // Loopback adapter OPEN_INSTANCE group head, this pointer points to one item in g_arrOpen list.
//-------------------------------------------------------------------

BOOLEAN
NPF_StartUsingBinding(
	IN POPEN_INSTANCE pOpen
	)
{
	ASSERT(pOpen != NULL);
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	NdisAcquireSpinLock(&pOpen->AdapterHandleLock);

	if (pOpen->AdapterBindingStatus != ADAPTER_BOUND)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		return FALSE;
	}

	pOpen->AdapterHandleUsageCounter++;

	NdisReleaseSpinLock(&pOpen->AdapterHandleLock);

	return TRUE;
}

//-------------------------------------------------------------------

VOID
NPF_StopUsingBinding(
	IN POPEN_INSTANCE pOpen
	)
{
	ASSERT(pOpen != NULL);
	//
	//  There is no risk in calling this function from abobe passive level
	//  (i.e. DISPATCH, in this driver) as we acquire a spinlock and decrement a
	//  counter.
	//
	//	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	NdisAcquireSpinLock(&pOpen->AdapterHandleLock);

	ASSERT(pOpen->AdapterHandleUsageCounter > 0);
	ASSERT(pOpen->AdapterBindingStatus == ADAPTER_BOUND);

	pOpen->AdapterHandleUsageCounter--;

	NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
}

//-------------------------------------------------------------------

VOID
NPF_CloseBinding(
	IN POPEN_INSTANCE pOpen
	)
{
	NDIS_EVENT Event;
	NDIS_STATUS Status;

	ASSERT(pOpen != NULL);
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	NdisInitializeEvent(&Event);
	NdisResetEvent(&Event);

	NdisAcquireSpinLock(&pOpen->AdapterHandleLock);

	while (pOpen->AdapterHandleUsageCounter > 0)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		NdisWaitEvent(&Event, 1);
		NdisAcquireSpinLock(&pOpen->AdapterHandleLock);
	}

	//
	// now the UsageCounter is 0
	//

	while (pOpen->AdapterBindingStatus == ADAPTER_UNBINDING)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		NdisWaitEvent(&Event, 1);
		NdisAcquireSpinLock(&pOpen->AdapterHandleLock);
	}

	//
	// now the binding status is either bound or unbound
	//

	if (pOpen->AdapterBindingStatus == ADAPTER_UNBOUND)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		return;
	}

	ASSERT(pOpen->AdapterBindingStatus == ADAPTER_BOUND);

	pOpen->AdapterBindingStatus = ADAPTER_UNBINDING;

	NdisReleaseSpinLock(&pOpen->AdapterHandleLock);

	NdisAcquireSpinLock(&pOpen->AdapterHandleLock);
	pOpen->AdapterBindingStatus = ADAPTER_UNBOUND;
	NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
}

//-------------------------------------------------------------------

VOID
NPF_CloseBindingAndAdapter(
	IN POPEN_INSTANCE pOpen
	)
{
	NDIS_EVENT Event;
	NDIS_STATUS Status;

	ASSERT(pOpen != NULL);
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	NdisInitializeEvent(&Event);
	NdisResetEvent(&Event);

	NdisAcquireSpinLock(&pOpen->AdapterHandleLock);

	while (pOpen->AdapterHandleUsageCounter > 0)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		NdisWaitEvent(&Event, 1);
		NdisAcquireSpinLock(&pOpen->AdapterHandleLock);
	}

	//
	// now the UsageCounter is 0
	//

	while (pOpen->AdapterBindingStatus == ADAPTER_UNBINDING)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		NdisWaitEvent(&Event, 1);
		NdisAcquireSpinLock(&pOpen->AdapterHandleLock);
	}

	//
	// now the binding status is either bound or unbound
	//

	if (pOpen->AdapterBindingStatus == ADAPTER_UNBOUND)
	{
		NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
		return;
	}

	ASSERT(pOpen->AdapterBindingStatus == ADAPTER_BOUND);

	pOpen->AdapterBindingStatus = ADAPTER_UNBINDING;

	NdisReleaseSpinLock(&pOpen->AdapterHandleLock);

	NdisAcquireSpinLock(&pOpen->AdapterHandleLock);
	pOpen->AdapterBindingStatus = ADAPTER_UNBOUND;
	NdisReleaseSpinLock(&pOpen->AdapterHandleLock);
}

//-------------------------------------------------------------------

NTSTATUS
NPF_OpenAdapter(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	)
{
	PDEVICE_EXTENSION		DeviceExtension;
	POPEN_INSTANCE			Open;
	PIO_STACK_LOCATION		IrpSp;
	NDIS_STATUS				Status;
	ULONG					localNumOpenedInstances;

	TRACE_ENTER();

	DeviceExtension = DeviceObject->DeviceExtension;

	IrpSp = IoGetCurrentIrpStackLocation(Irp);

	//find the head adaper of the global open array, if found, create a group child adapter object from the head adapter.
	Open = NPF_GetCopyFromOpenArray(&DeviceExtension->AdapterName, DeviceExtension);

	if (Open == NULL)
	{
		//cannot find the adapter from the global open array.
		Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		TRACE_EXIT();
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	Open->DeviceExtension = DeviceExtension;
#ifdef HAVE_WFP_LOOPBACK_SUPPORT
	TRACE_MESSAGE3(PACKET_DEBUG_LOUD,
		"Opening the device %ws, BindingContext=%p, Loopback=%u",
		DeviceExtension->AdapterName.Buffer,
		Open,
		Open->Loopback);
#else
	TRACE_MESSAGE2(PACKET_DEBUG_LOUD,
		"Opening the device %ws, BindingContext=%p, Loopback=<Not supported>",
		DeviceExtension->AdapterName.Buffer,
		Open);
#endif

	//
	// complete the open
	//
	localNumOpenedInstances = InterlockedIncrement(&g_NumOpenedInstances);
	TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Opened Instances: %u", localNumOpenedInstances);

	// Get the absolute value of the system boot time.
	// This is used for timestamp conversion.
	TIME_SYNCHRONIZE(&G_Start_Time);

#ifdef HAVE_WFP_LOOPBACK_SUPPORT
	if (Open->Loopback)
	{
		if ((g_LoopbackDevObj != NULL) && (g_WFPEngineHandle == INVALID_HANDLE_VALUE))
		{
			TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "g_LoopbackDevObj=%p, init WSK injection handles and register callouts", g_LoopbackDevObj);
			// Use Windows Filtering Platform (WFP) to capture loopback packets, also help WSK take care of loopback packet sending.
			Status = NPF_InitInjectionHandles();
			if (!NT_SUCCESS(Status))
			{
				goto NPF_OpenAdapter_End;
			}

			Status = NPF_RegisterCallouts(g_LoopbackDevObj);
			if (!NT_SUCCESS(Status))
			{
				if (g_WFPEngineHandle != INVALID_HANDLE_VALUE)
				{
					NPF_UnregisterCallouts();
				}

				goto NPF_OpenAdapter_End;
			}
		}
		else
		{
			TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "g_LoopbackDevObj=%p, NO need to init WSK code", g_LoopbackDevObj);
		}
	}
#endif

#ifdef HAVE_WFP_LOOPBACK_SUPPORT
	if (Open->Loopback)
	{
		TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "This is loopback adapter, set MTU to: %u", NPF_LOOPBACK_INTERFACR_MTU + ETHER_HDR_LEN);
		Open->MaxFrameSize = NPF_LOOPBACK_INTERFACR_MTU + ETHER_HDR_LEN;
		Status = STATUS_SUCCESS;
	}
	else
#endif
	{
		Open->MaxFrameSize = 1514;
		Status = STATUS_SUCCESS;
		// This call will cause SYSTEM_SERVICE_EXCEPTION BSoD sometimes.
		// I didn't figure out the reason, I threw an ask on Stackoverflow: http://stackoverflow.com/questions/31869373/get-system-service-exception-bluescreen-when-starting-wireshark-on-win10-vmware
		// But no replies, so I just workaround by hard-coding it as 1514 for now.
// 		if (NPF_GetDeviceMTU(Open, &Open->MaxFrameSize) != STATUS_SUCCESS)
// 		{
// 			Open->MaxFrameSize = 1514;
// 		}
	}

#ifdef HAVE_DOT11_SUPPORT
	// Fetch the device's data rate mapping table with the OID_DOT11_DATA_RATE_MAPPING_TABLE OID.
	if (NPF_GetDataRateMappingTable(Open, &Open->DataRateMappingTable) != STATUS_SUCCESS)
	{
		Open->HasDataRateMappingTable = FALSE;
	}
	else
	{
		Open->HasDataRateMappingTable = TRUE;
	}
#endif

#ifdef HAVE_WFP_LOOPBACK_SUPPORT
NPF_OpenAdapter_End:;
#endif
	if (!NT_SUCCESS(Status))
	{
		// Close the binding
		NPF_CloseBinding(Open);

		// Free the open instance' resources
		NPF_ReleaseOpenInstanceResources(Open);

		// Free the open instance itself
		if (Open)
		{
			ExFreePool(Open);
			Open = NULL;
		}

		Irp->IoStatus.Status = Status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		TRACE_EXIT();
		return Status;
	}
	else
	{
		//  Save or open here
		IrpSp->FileObject->FsContext = Open;
	}

	NPF_AddToOpenArray(Open);
	NPF_AddToGroupOpenArray(Open);

	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	TRACE_EXIT();
	return Status;
}

//-------------------------------------------------------------------

BOOLEAN
NPF_StartUsingOpenInstance(
	IN POPEN_INSTANCE pOpen)

{
	BOOLEAN returnStatus;

	NdisAcquireSpinLock(&pOpen->OpenInUseLock);
	if (pOpen->ClosePending)
	{
		returnStatus = FALSE;
	}
	else
	{
		returnStatus = TRUE;
		pOpen->NumPendingIrps ++;
	}
	NdisReleaseSpinLock(&pOpen->OpenInUseLock);

	return returnStatus;
}

//-------------------------------------------------------------------

VOID
NPF_StopUsingOpenInstance(
	IN POPEN_INSTANCE pOpen
	)
{
	NdisAcquireSpinLock(&pOpen->OpenInUseLock);
	ASSERT(pOpen->NumPendingIrps > 0);
	pOpen->NumPendingIrps --;
	NdisReleaseSpinLock(&pOpen->OpenInUseLock);
}

//-------------------------------------------------------------------

VOID
NPF_CloseOpenInstance(
	IN POPEN_INSTANCE pOpen
	)
{
	ULONG i = 0;
	NDIS_EVENT Event;

	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	NdisInitializeEvent(&Event);
	NdisResetEvent(&Event);

	NdisAcquireSpinLock(&pOpen->OpenInUseLock);

	pOpen->ClosePending = TRUE;

	while (pOpen->NumPendingIrps > 0)
	{
		NdisReleaseSpinLock(&pOpen->OpenInUseLock);
		NdisWaitEvent(&Event, 1);
		NdisAcquireSpinLock(&pOpen->OpenInUseLock);
	}

	NdisReleaseSpinLock(&pOpen->OpenInUseLock);
}

//-------------------------------------------------------------------

VOID
NPF_ReleaseOpenInstanceResources(
	POPEN_INSTANCE pOpen
	)
{
	PKEVENT pEvent;
	UINT i;

	TRACE_ENTER();

	ASSERT(pOpen != NULL);
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Open= %p", pOpen);

	if (pOpen->PacketPool) // Release the packet buffer pool
	{
		NdisFreeNetBufferListPool(pOpen->PacketPool);
		pOpen->PacketPool = NULL;
	}

	// Release the adapter name
	if (pOpen->AdapterName.MaximumLength != 0)
	{
		NdisFreeString(pOpen->AdapterName);
		pOpen->AdapterName.Buffer = NULL;
		pOpen->AdapterName.Length = 0;
		pOpen->AdapterName.MaximumLength = 0;
	}

	//
	// Free the filter if it's present
	//
	if (pOpen->bpfprogram != NULL)
	{
		ExFreePool(pOpen->bpfprogram);
		pOpen->bpfprogram = NULL;
	}

	//
	// Jitted filters are supported on x86 (32bit) only
	//
#ifdef _X86_
	// Free the jitted filter if it's present
	if (pOpen->Filter != NULL)
	{
		BPF_Destroy_JIT_Filter(pOpen->Filter);
	}
#endif //_X86_

	//
	// Dereference the read event.
	//

	if (pOpen->ReadEvent != NULL)
	{
		ObDereferenceObject(pOpen->ReadEvent);
	}

	//
	// free the buffer
	// NOTE: the buffer is fragmented among the various CPUs, but the base pointer of the
	// allocated chunk of memory is stored in the first slot (pOpen->CpuData[0])
	//
	if (pOpen->Size > 0)
	{
		ExFreePool(pOpen->CpuData[0].Buffer);
		pOpen->CpuData[0].Buffer = NULL;
	}

	//
	// free the per CPU spinlocks
	//
	for (i = 0; i < g_NCpu; i++)
	{
		NdisFreeSpinLock(&pOpen->CpuData[i].BufferLock);
	}

	NdisFreeSpinLock(&pOpen->OIDLock);
	NdisFreeSpinLock(&pOpen->CountersLock);
	NdisFreeSpinLock(&pOpen->WriteLock);
	NdisFreeSpinLock(&pOpen->MachineLock);
	NdisFreeSpinLock(&pOpen->AdapterHandleLock);
	NdisFreeSpinLock(&pOpen->OpenInUseLock);

	//
	// Free the string with the name of the dump file
	//
	if (pOpen->DumpFileName.Buffer != NULL)
	{
		ExFreePool(pOpen->DumpFileName.Buffer);
	}

	TRACE_EXIT();
}

//-------------------------------------------------------------------

NTSTATUS
NPF_GetDeviceMTU(
	IN POPEN_INSTANCE pOpen,
	OUT PUINT pMtu
	)
{
	TRACE_ENTER();
	ASSERT(pOpen != NULL);
	ASSERT(pMtu != NULL);

	UINT Mtu = 0;
	ULONG BytesProcessed = 0;

	NPF_DoInternalRequest(pOpen,
		NdisRequestQueryInformation,
		OID_GEN_MAXIMUM_TOTAL_SIZE,
		&Mtu,
		sizeof(Mtu),
		0,
		0,
		&BytesProcessed
	);

	if (BytesProcessed != sizeof(Mtu) || Mtu == 0)
	{
		TRACE_EXIT();
		return STATUS_UNSUCCESSFUL;
	}
	else
	{
		*pMtu = Mtu;
		TRACE_EXIT();
		return STATUS_SUCCESS;
	}
}

//-------------------------------------------------------------------
#ifdef HAVE_DOT11_SUPPORT
NTSTATUS
NPF_GetDataRateMappingTable(
	IN POPEN_INSTANCE pOpen,
	OUT PDOT11_DATA_RATE_MAPPING_TABLE pDataRateMappingTable
)
{
	TRACE_ENTER();
	ASSERT(pOpen != NULL);
	ASSERT(pDataRateMappingTable != NULL);

	DOT11_DATA_RATE_MAPPING_TABLE DataRateMappingTable;
	ULONG BytesProcessed = 0;

	NPF_DoInternalRequest(pOpen,
		NdisRequestQueryInformation,
		OID_DOT11_DATA_RATE_MAPPING_TABLE,
		&DataRateMappingTable,
		sizeof(DataRateMappingTable),
		0,
		0,
		&BytesProcessed
	);

	if (BytesProcessed != sizeof(DataRateMappingTable))
	{
		TRACE_EXIT();
		return STATUS_UNSUCCESSFUL;
	}
	else
	{
		*pDataRateMappingTable = DataRateMappingTable;
		TRACE_EXIT();
		return STATUS_SUCCESS;
	}
}

//-------------------------------------------------------------------

USHORT
NPF_LookUpDataRateMappingTable(
	IN POPEN_INSTANCE pOpen,
	IN UCHAR ucDataRate
)
{
	UINT i;
	PDOT11_DATA_RATE_MAPPING_TABLE pTable = &pOpen->DataRateMappingTable;
	USHORT usRetDataRateValue = 0;
	TRACE_ENTER();

	if (!pOpen->HasDataRateMappingTable)
	{
		TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Data rate mapping table not found, Open = %p\n", pOpen);
		TRACE_EXIT();
		return usRetDataRateValue;
	}

	for (i = 0; i < pTable->uDataRateMappingLength; i ++)
	{
		if (pTable->DataRateMappingEntries[i].ucDataRateIndex == ucDataRate)
		{
			usRetDataRateValue = pTable->DataRateMappingEntries[i].usDataRateValue;
			break;
		}
	}

	TRACE_EXIT();
	return usRetDataRateValue;
}

//-------------------------------------------------------------------

NTSTATUS
NPF_GetCurrentOperationMode(
	IN POPEN_INSTANCE pOpen,
	OUT PDOT11_CURRENT_OPERATION_MODE pCurrentOperationMode
)
{
	TRACE_ENTER();
	ASSERT(pOpen != NULL);
	ASSERT(pCurrentOperationMode != NULL);

	DOT11_CURRENT_OPERATION_MODE CurrentOperationMode;
	ULONG BytesProcessed = 0;

	NPF_DoInternalRequest(pOpen,
		NdisRequestQueryInformation,
		OID_DOT11_CURRENT_OPERATION_MODE,
		&CurrentOperationMode,
		sizeof(CurrentOperationMode),
		0,
		0,
		&BytesProcessed
	);

	if (BytesProcessed != sizeof(CurrentOperationMode))
	{
		TRACE_EXIT();
		return STATUS_UNSUCCESSFUL;
	}
	else
	{
		*pCurrentOperationMode = CurrentOperationMode;
		TRACE_EXIT();
		return STATUS_SUCCESS;
	}
}

//-------------------------------------------------------------------

ULONG
NPF_GetCurrentOperationMode_Wrapper(
	IN POPEN_INSTANCE pOpen
)
{
	DOT11_CURRENT_OPERATION_MODE CurrentOperationMode;
	if (NPF_GetCurrentOperationMode(pOpen, &CurrentOperationMode) != STATUS_SUCCESS)
	{
		return DOT11_OPERATION_MODE_UNKNOWN;
	}
	else
	{
		// Possible return values are:
		// 1: DOT11_OPERATION_MODE_EXTENSIBLE_STATION
		// 2: DOT11_OPERATION_MODE_EXTENSIBLE_AP
		// 3: DOT11_OPERATION_MODE_NETWORK_MONITOR
		return CurrentOperationMode.uCurrentOpMode;
	}
}

//-------------------------------------------------------------------

NTSTATUS
NPF_GetCurrentChannel(
	IN POPEN_INSTANCE pOpen,
	OUT PULONG pCurrentChannel
)
{
	TRACE_ENTER();
	ASSERT(pOpen != NULL);
	ASSERT(pCurrentChannel != NULL);

	ULONG CurrentChannel;
	ULONG BytesProcessed = 0;

	NPF_DoInternalRequest(pOpen,
		NdisRequestQueryInformation,
		OID_DOT11_CURRENT_CHANNEL,
		&CurrentChannel,
		sizeof(CurrentChannel),
		0,
		0,
		&BytesProcessed
	);

	if (BytesProcessed != sizeof(CurrentChannel))
	{
		TRACE_EXIT();
		return STATUS_UNSUCCESSFUL;
	}
	else
	{
		*pCurrentChannel = CurrentChannel;
		TRACE_EXIT();
		return STATUS_SUCCESS;
	}
}

//-------------------------------------------------------------------

ULONG
NPF_GetCurrentChannel_Wrapper(
	IN POPEN_INSTANCE pOpen
)
{
	ULONG CurrentChannel;
	if (NPF_GetCurrentChannel(pOpen, &CurrentChannel) != STATUS_SUCCESS)
	{
		return 0;
	}
	else
	{
		// Possible return values are: 1 - 14
		return CurrentChannel;
	}
}

//-------------------------------------------------------------------

NTSTATUS
NPF_GetCurrentFrequency(
	IN POPEN_INSTANCE pOpen,
	OUT PULONG pCurrentFrequency
)
{
	TRACE_ENTER();
	ASSERT(pOpen != NULL);
	ASSERT(pCurrentFrequency != NULL);

	ULONG CurrentFrequency;
	ULONG BytesProcessed = 0;

	NPF_DoInternalRequest(pOpen,
		NdisRequestQueryInformation,
		OID_DOT11_CURRENT_FREQUENCY,
		&CurrentFrequency,
		sizeof(CurrentFrequency),
		0,
		0,
		&BytesProcessed
	);

	if (BytesProcessed != sizeof(CurrentFrequency))
	{
		TRACE_EXIT();
		return STATUS_UNSUCCESSFUL;
	}
	else
	{
		*pCurrentFrequency = CurrentFrequency;
		TRACE_EXIT();
		return STATUS_SUCCESS;
	}
}

//-------------------------------------------------------------------

ULONG
NPF_GetCurrentFrequency_Wrapper(
	IN POPEN_INSTANCE pOpen
)
{
	ULONG CurrentFrequency;
	if (NPF_GetCurrentFrequency(pOpen, &CurrentFrequency) != STATUS_SUCCESS)
	{
		return 0;
	}
	else
	{
		// Possible return values are: 0 - 200
		return CurrentFrequency;
	}
}
#endif
//-------------------------------------------------------------------

NTSTATUS
NPF_CloseAdapter(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	)
{
	POPEN_INSTANCE pOpen;
	PIO_STACK_LOCATION IrpSp;
	TRACE_ENTER();

	IrpSp = IoGetCurrentIrpStackLocation(Irp);
	pOpen = IrpSp->FileObject->FsContext;

	ASSERT(pOpen != NULL);
	//
	// Free the open instance itself
	//
	if (pOpen)
	{
		ExFreePool(pOpen);
	}

	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	TRACE_EXIT();
	return STATUS_SUCCESS;
}

//-------------------------------------------------------------------

NTSTATUS
NPF_CloseAdapterForUnclosed(
	POPEN_INSTANCE pOpen
	)
{
	TRACE_ENTER();

	ASSERT(pOpen != NULL);
	//
	// Free the open instance itself
	//
	if (pOpen)
	{
		ExFreePool(pOpen);
	}

	TRACE_EXIT();
	return STATUS_SUCCESS;
}

//-------------------------------------------------------------------

NTSTATUS
NPF_Cleanup(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp
	)
{
	POPEN_INSTANCE Open;
	NDIS_STATUS Status;
	PIO_STACK_LOCATION IrpSp;
	LARGE_INTEGER ThreadDelay;
	ULONG localNumOpenInstances;

	TRACE_ENTER();

	IrpSp = IoGetCurrentIrpStackLocation(Irp);
	Open = IrpSp->FileObject->FsContext;

	TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Open = %p\n", Open);

	ASSERT(Open != NULL);

	NPF_RemoveFromOpenArray(Open); //Remove the adapter from the global adapter list
	NPF_RemoveFromGroupOpenArray(Open); //Remove the adapter from the group adapter list

	NPF_CloseOpenInstance(Open);

	if (Open->ReadEvent != NULL)
		KeSetEvent(Open->ReadEvent, 0, FALSE);

	NPF_CloseBinding(Open);

	// NOTE:
	// code commented out because the kernel dump feature is disabled
	//
	//if (AdapterAlreadyClosing == FALSE)
	//{

	//
	//	 Unfreeze the consumer
	//
	//	if(Open->mode & MODE_DUMP)
	//		NdisSetEvent(&Open->DumpEvent);
	//	else
	//		KeSetEvent(Open->ReadEvent,0,FALSE);

	//	//
	//	// If this instance is in dump mode, complete the dump and close the file
	//	//
	//	if((Open->mode & MODE_DUMP) && Open->DumpFileHandle != NULL)
	//	{
	//		NTSTATUS wres;

	//		ThreadDelay.QuadPart = -50000000;

	//		//
	//		// Wait the completion of the thread
	//		//
	//		wres = KeWaitForSingleObject(Open->DumpThreadObject,
	//			UserRequest,
	//			KernelMode,
	//			TRUE,
	//			&ThreadDelay);

	//		ObDereferenceObject(Open->DumpThreadObject);

	//		//
	//		// Flush and close the dump file
	//		//
	//		NPF_CloseDumpFile(Open);
	//	}
	//}


	//
	// release all the resources
	//
	NPF_ReleaseOpenInstanceResources(Open);

	//	IrpSp->FileObject->FsContext = NULL;

	//
	// Decrease the counter of open instances
	//
	localNumOpenInstances = InterlockedDecrement(&g_NumOpenedInstances);
	TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Opened Instances: %u", localNumOpenInstances);

	if (localNumOpenInstances == 0)
	{
		//
		// Force a synchronization at the next NPF_Open().
		// This hopefully avoids the synchronization issues caused by hibernation or standby.
		//
		TIME_DESYNCHRONIZE(&G_Start_Time);
	}


	//
	// and complete the IRP with status success
	//
	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	TRACE_EXIT();

	return(STATUS_SUCCESS);
}

//-------------------------------------------------------------------

NTSTATUS
NPF_CleanupForUnclosed(
	POPEN_INSTANCE Open
	)
{
	NDIS_STATUS Status;
	LARGE_INTEGER ThreadDelay;
	ULONG localNumOpenInstances;

	TRACE_ENTER();

	TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Open = %p\n", Open);

	ASSERT(Open != NULL);

	NPF_RemoveFromOpenArray(Open); //Remove the adapter from the global adapter list
	NPF_RemoveFromGroupOpenArray(Open); //Remove the adapter from the group adapter list

	NPF_CloseOpenInstance(Open);

	if (Open->ReadEvent != NULL)
		KeSetEvent(Open->ReadEvent, 0, FALSE);

	NPF_CloseBinding(Open);

	// NOTE:
	// code commented out because the kernel dump feature is disabled
	//
	//if (AdapterAlreadyClosing == FALSE)
	//{

	//
	//	 Unfreeze the consumer
	//
	//	if(Open->mode & MODE_DUMP)
	//		NdisSetEvent(&Open->DumpEvent);
	//	else
	//		KeSetEvent(Open->ReadEvent,0,FALSE);

	//	//
	//	// If this instance is in dump mode, complete the dump and close the file
	//	//
	//	if((Open->mode & MODE_DUMP) && Open->DumpFileHandle != NULL)
	//	{
	//		NTSTATUS wres;

	//		ThreadDelay.QuadPart = -50000000;

	//		//
	//		// Wait the completion of the thread
	//		//
	//		wres = KeWaitForSingleObject(Open->DumpThreadObject,
	//			UserRequest,
	//			KernelMode,
	//			TRUE,
	//			&ThreadDelay);

	//		ObDereferenceObject(Open->DumpThreadObject);

	//		//
	//		// Flush and close the dump file
	//		//
	//		NPF_CloseDumpFile(Open);
	//	}
	//}


	//
	// release all the resources
	//
	NPF_ReleaseOpenInstanceResources(Open);

	//	IrpSp->FileObject->FsContext = NULL;

	//
	// Decrease the counter of open instances
	//
	localNumOpenInstances = InterlockedDecrement(&g_NumOpenedInstances);
	TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Opened Instances: %u", localNumOpenInstances);

	if (localNumOpenInstances == 0)
	{
		//
		// Force a synchronization at the next NPF_Open().
		// This hopefully avoids the synchronization issues caused by hibernation or standby.
		//
		TIME_DESYNCHRONIZE(&G_Start_Time);
	}


	//
	// and complete the IRP with status success
	//

	TRACE_EXIT();

	return(STATUS_SUCCESS);
}

//-------------------------------------------------------------------

void
NPF_AddToOpenArray(
	POPEN_INSTANCE Open
	)
{
	POPEN_INSTANCE CurOpen;
	TRACE_ENTER();

	NdisAcquireSpinLock(&g_OpenArrayLock);
	if (g_arrOpen == NULL)
	{
		g_arrOpen = Open;
	}
	else
	{
		CurOpen = g_arrOpen;
		while (CurOpen->Next != NULL)
		{
			CurOpen = CurOpen->Next;
		}
		CurOpen->Next = Open;
	}
	NdisReleaseSpinLock(&g_OpenArrayLock);

	TRACE_EXIT();
}

//-------------------------------------------------------------------

void
NPF_AddToGroupOpenArray(
	POPEN_INSTANCE Open
	)
{
	POPEN_INSTANCE CurOpen = NULL;
	POPEN_INSTANCE GroupRear = NULL;
	TRACE_ENTER();

	if (Open->DirectBinded)
	{
		IF_LOUD(DbgPrint("NPF_AddToGroupOpenArray: never should be here.\n");)
		TRACE_EXIT();
		return;
	}

	NdisAcquireSpinLock(&g_OpenArrayLock);
	for (CurOpen = g_arrOpen; CurOpen != NULL; CurOpen = CurOpen->Next)
	{
		if (CurOpen->AdapterBindingStatus == ADAPTER_BOUND && NPF_EqualAdapterName(&CurOpen->AdapterName, &Open->AdapterName) == TRUE && CurOpen->DirectBinded)
		{
			GroupRear = CurOpen;
			while (GroupRear->GroupNext != NULL)
			{
				GroupRear = GroupRear->GroupNext;
			}
			GroupRear->GroupNext = Open;
			Open->GroupHead = CurOpen;
			break;
		}
	}
	NdisReleaseSpinLock(&g_OpenArrayLock);

	if (!GroupRear)
	{
		IF_LOUD(DbgPrint("NPF_AddToGroupOpenArray: never should be here.\n");)
	}
	TRACE_EXIT();
}

//-------------------------------------------------------------------

void
NPF_RemoveFromOpenArray(
	POPEN_INSTANCE Open
	)
{
	POPEN_INSTANCE CurOpen = NULL;
	POPEN_INSTANCE PrevOpen = NULL;
	TRACE_ENTER();

	if (Open == NULL)
	{
		return;
	}

	NdisAcquireSpinLock(&g_OpenArrayLock);
	for (CurOpen = g_arrOpen; CurOpen != NULL; CurOpen = CurOpen->Next)
	{
		if (CurOpen == Open)
		{
			if (CurOpen == g_arrOpen)
			{
				g_arrOpen = CurOpen->Next;
			}
			else
			{
				PrevOpen->Next = CurOpen->Next;
			}
			//return;
		}
		PrevOpen = CurOpen;
	}
	NdisReleaseSpinLock(&g_OpenArrayLock);

	TRACE_EXIT();
}

//-------------------------------------------------------------------

void
NPF_RemoveFromGroupOpenArray(
	POPEN_INSTANCE Open
	)
{
	POPEN_INSTANCE CurOpen;
	POPEN_INSTANCE GroupOpen;
	POPEN_INSTANCE GroupPrev = NULL;
	TRACE_ENTER();

	if (Open->DirectBinded)
	{
		IF_LOUD(DbgPrint("NPF_RemoveFromGroupOpenArray: never should be here.\n");)
		TRACE_EXIT();
		return;
	}

	NdisAcquireSpinLock(&g_OpenArrayLock);
	GroupOpen = Open->GroupHead;
	while (GroupOpen)
	{
		if (GroupOpen == Open)
		{
			if (GroupPrev == NULL)
			{
				ASSERT(GroupPrev != NULL);
				break;
			}
			else
			{
				GroupPrev->GroupNext = GroupOpen->GroupNext;
				NdisReleaseSpinLock(&g_OpenArrayLock);
				TRACE_EXIT();
				return;
			}
		}
		GroupPrev = GroupOpen;
		GroupOpen = GroupOpen->GroupNext;
	}
	NdisReleaseSpinLock(&g_OpenArrayLock);

	IF_LOUD(DbgPrint("NPF_RemoveFromGroupOpenArray: never should be here.\n");)
	TRACE_EXIT();
}

//-------------------------------------------------------------------

BOOLEAN
NPF_EqualAdapterName(
	PNDIS_STRING s1,
	PNDIS_STRING s2
	)
{
	int i;
	BOOLEAN bResult = TRUE;
	// TRACE_ENTER();

	if (s1->Length != s2->Length)
	{
		IF_LOUD(DbgPrint("NPF_EqualAdapterName: length not the same\n");)
		// IF_LOUD(DbgPrint("NPF_EqualAdapterName: length not the same, s1->Length = %d, s2->Length = %d\n", s1->Length, s2->Length);)
		// TRACE_EXIT();
		return FALSE;
	}

	for (i = 0; i < s2->Length / 2; i ++)
	{
		if (L'A' <= s1->Buffer[i] && s1->Buffer[i] <= L'Z')
		{
			if (s2->Buffer[i] - s1->Buffer[i] != 0 && s2->Buffer[i] - s1->Buffer[i] != L'a' - L'A')
			{
				bResult = FALSE;
				break;
			}
		}
		else if (L'a' <= s1->Buffer[i] && s1->Buffer[i] <= L'z')
		{
			if (s2->Buffer[i] - s1->Buffer[i] != 0 && s2->Buffer[i] - s1->Buffer[i] != L'A' - L'a')
			{
				bResult = FALSE;
				break;
			}
		}
		else if (s2->Buffer[i] - s1->Buffer[i] != 0)
		{
			bResult = FALSE;
			break;
		}
	}

	// Print unicode strings using %ws will cause page fault blue screen with IRQL = DISPATCH_LEVEL, so we disable the string print for now.
	// IF_LOUD(DbgPrint("NPF_EqualAdapterName: bResult = %d, s1 = %ws, s2 = %ws\n", i, bResult, s1->Buffer, s2->Buffer);)
	if (bResult)
	{
		IF_LOUD(DbgPrint("NPF_EqualAdapterName: bResult == TRUE\n");)
	}
	// TRACE_EXIT();
	return bResult;
}

//-------------------------------------------------------------------

POPEN_INSTANCE
NPF_GetCopyFromOpenArray(
	PNDIS_STRING pAdapterName,
	PDEVICE_EXTENSION DeviceExtension
	)
{
	POPEN_INSTANCE CurOpen;
	TRACE_ENTER();

	NdisAcquireSpinLock(&g_OpenArrayLock);
	for (CurOpen = g_arrOpen; CurOpen != NULL; CurOpen = CurOpen->Next)
	{
		if (CurOpen->AdapterBindingStatus == ADAPTER_BOUND && NPF_EqualAdapterName(&CurOpen->AdapterName, pAdapterName) == TRUE)
		{
			NdisReleaseSpinLock(&g_OpenArrayLock);
			return NPF_DuplicateOpenObject(CurOpen, DeviceExtension);
		}
	}
	NdisReleaseSpinLock(&g_OpenArrayLock);

	TRACE_EXIT();
	return NULL;
}

//-------------------------------------------------------------------

void
NPF_RemoveUnclosedAdapters(
	)
{
	POPEN_INSTANCE CurOpen;
	POPEN_INSTANCE Open;
	POPEN_INSTANCE TempOpen;
	BOOLEAN NoDirectedBindedRemaining = TRUE;
	TRACE_ENTER();

// 	for (CurOpen = g_arrOpen; CurOpen != NULL; CurOpen = CurOpen->Next)
// 	{
// 		if (CurOpen->DirectBinded)
// 		{
// 			NoDirectedBindedRemaining = FALSE;
// 		}
// 	}
//
// 	if (NoDirectedBindedRemaining)
// 	{
// 		for (CurOpen = g_arrOpen; CurOpen != NULL;)
// 		{
// 			TempOpen = CurOpen->Next;
// 			NPF_CleanupForUnclosed(CurOpen);
// 			NPF_CloseAdapterForUnclosed(CurOpen);
// 			CurOpen = TempOpen;
// 		}
// 	}

	NdisAcquireSpinLock(&g_OpenArrayLock);
	for (CurOpen = g_arrOpen; CurOpen != NULL;)
	{
		TempOpen = CurOpen->Next;
		if (CurOpen->DirectBinded)
		{
			NPF_CleanupForUnclosed(CurOpen);
			NPF_CloseAdapterForUnclosed(CurOpen);
		}
		CurOpen = TempOpen;
	}
	NdisReleaseSpinLock(&g_OpenArrayLock);

	TRACE_EXIT();
}

//-------------------------------------------------------------------

POPEN_INSTANCE
NPF_DuplicateOpenObject(
	POPEN_INSTANCE OriginalOpen,
	PDEVICE_EXTENSION DeviceExtension
	)
{
	POPEN_INSTANCE Open;
	TRACE_ENTER();

	Open = NPF_CreateOpenObject(&OriginalOpen->AdapterName, OriginalOpen->Medium, DeviceExtension);
	Open->AdapterHandle = OriginalOpen->AdapterHandle;
	Open->DirectBinded = FALSE;
	Open->MaxFrameSize = OriginalOpen->MaxFrameSize;
#ifdef HAVE_WFP_LOOPBACK_SUPPORT
	Open->Loopback = OriginalOpen->Loopback;
#endif

#ifdef HAVE_RX_SUPPORT
	Open->SendToRxPath = OriginalOpen->SendToRxPath;
	Open->BlockRxPath = OriginalOpen->BlockRxPath;
#endif

	TRACE_EXIT();
	return Open;
}

//-------------------------------------------------------------------

POPEN_INSTANCE
NPF_CreateOpenObject(
	PNDIS_STRING AdapterName,
	UINT SelectedIndex,
	PDEVICE_EXTENSION DeviceExtension)
{
	POPEN_INSTANCE Open;
	UINT i;
	NET_BUFFER_LIST_POOL_PARAMETERS PoolParameters;
	TRACE_ENTER();

	// allocate some memory for the open structure
	Open = ExAllocatePoolWithTag(NonPagedPool, sizeof(OPEN_INSTANCE), '0OWA');

	if (Open == NULL)
	{
		// no memory
		TRACE_MESSAGE(PACKET_DEBUG_LOUD, "Failed to allocate memory pool");
		TRACE_EXIT();
		return NULL;
	}

	RtlZeroMemory(Open, sizeof(OPEN_INSTANCE));

	Open->DeviceExtension = DeviceExtension; //can be NULL before any actual bindings.
	Open->DirectBinded = TRUE;
#ifdef HAVE_WFP_LOOPBACK_SUPPORT
	Open->Loopback = FALSE;
#endif

#ifdef HAVE_RX_SUPPORT
	Open->SendToRxPath = FALSE;
	Open->BlockRxPath = FALSE;
#endif

#ifdef HAVE_DOT11_SUPPORT
	Open->HasDataRateMappingTable = FALSE;
#endif

	NdisZeroMemory(&PoolParameters, sizeof(NET_BUFFER_LIST_POOL_PARAMETERS));
	PoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
	PoolParameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
	PoolParameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
	PoolParameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
	PoolParameters.fAllocateNetBuffer = TRUE;
	PoolParameters.ContextSize = 0;
	PoolParameters.PoolTag = NPF_ALLOC_TAG;
	PoolParameters.DataSize = 0;

	Open->PacketPool = NdisAllocateNetBufferListPool(NULL, &PoolParameters);
	if (Open->PacketPool == NULL)
	{
		TRACE_MESSAGE(PACKET_DEBUG_LOUD, "Failed to allocate packet pool");
		ExFreePool(Open);
		TRACE_EXIT();
		return NULL;
	}

	NdisInitializeEvent(&Open->WriteEvent);
	NdisInitializeEvent(&Open->NdisRequestEvent);
	NdisInitializeEvent(&Open->NdisWriteCompleteEvent);
	NdisInitializeEvent(&Open->DumpEvent);
	NdisAllocateSpinLock(&Open->MachineLock);
	NdisAllocateSpinLock(&Open->WriteLock);
	Open->WriteInProgress = FALSE;

	for (i = 0; i < g_NCpu; i++)
	{
		NdisAllocateSpinLock(&Open->CpuData[i].BufferLock);
	}

	NdisInitializeEvent(&Open->NdisOpenCloseCompleteEvent);

	//  list to hold irp's want to reset the adapter
	InitializeListHead(&Open->ResetIrpList);

	//  Initialize the request list
	KeInitializeSpinLock(&Open->RequestSpinLock);
	InitializeListHead(&Open->RequestList);

	//
	// Initialize the open instance
	//
	//Open->BindContext = NULL;
	Open->bpfprogram = NULL;	//reset the filter
	Open->mode = MODE_CAPT;
	Open->Nbytes.QuadPart = 0;
	Open->Npackets.QuadPart = 0;
	Open->Nwrites = 1;
	Open->Multiple_Write_Counter = 0;
	Open->MinToCopy = 0;
	Open->TimeOut.QuadPart = (LONGLONG)1;
	Open->DumpFileName.Buffer = NULL;
	Open->DumpFileHandle = NULL;
#ifdef HAVE_BUGGY_TME_SUPPORT
	Open->tme.active = TME_NONE_ACTIVE;
#endif // HAVE_BUGGY_TME_SUPPORT
	Open->DumpLimitReached = FALSE;
	Open->MaxFrameSize = 0;
	Open->WriterSN = 0;
	Open->ReaderSN = 0;
	Open->Size = 0;
	Open->SkipSentPackets = FALSE;
	Open->ReadEvent = NULL;

	Open->AdapterName.Buffer = ExAllocatePoolWithTag(NonPagedPool, 255 * sizeof(WCHAR), 'NPCA');
	Open->AdapterName.MaximumLength = 255 * sizeof(WCHAR);
	Open->AdapterName.Length = 0;
	RtlCopyUnicodeString(&Open->AdapterName, AdapterName);

	//
	// we need to keep a counter of the pending IRPs
	// so that when the IRP_MJ_CLEANUP dispatcher gets called,
	// we can wait for those IRPs to be completed
	//
	Open->NumPendingIrps = 0;
	Open->ClosePending = FALSE;
	Open->PausePending = FALSE;
	NdisAllocateSpinLock(&Open->OpenInUseLock);

	//
	//allocate the spinlock for the statistic counters
	//
	NdisAllocateSpinLock(&Open->CountersLock);

	//
	//allocate the spinlock for the OID requests
	//
	NdisAllocateSpinLock(&Open->OIDLock);

	//
	//  link up the request stored in our open block
	//
	for (i = 0 ; i < MAX_REQUESTS ; i++)
	{
		NdisInitializeEvent(&Open->Requests[i].InternalRequestCompletedEvent);

		ExInterlockedInsertTailList(&Open->RequestList, &Open->Requests[i].ListElement, &Open->RequestSpinLock);
	}

	NdisResetEvent(&Open->NdisOpenCloseCompleteEvent);

	//
	// set the proper binding flags before trying to open the MAC
	//
	Open->AdapterBindingStatus = ADAPTER_BOUND;
	Open->AdapterHandleUsageCounter = 0;
	NdisAllocateSpinLock(&Open->AdapterHandleLock);

	Open->Medium = SelectedIndex; //Can be 0 before the first bindding.

	TRACE_EXIT();
	return Open;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_RegisterOptions(
	NDIS_HANDLE  NdisFilterDriverHandle,
	NDIS_HANDLE  FilterDriverContext
	)
/*++

Routine Description:

	Register optional handlers with NDIS.  This sample does not happen to
	have any optional handlers to register, so this routine does nothing
	and could simply have been omitted.  However, for illustrative purposes,
	it is presented here.

Arguments:

	NdisFilterHandle - pointer the driver handle received from
							 NdisFRegisterFilterDriver

	FilterDriverContext    - pointer to our context passed into
							 NdisFRegisterFilterDriver

Return Value:

	NDIS_STATUS_SUCCESS

--*/
{
	TRACE_ENTER();

	ASSERT(NdisFilterDriverHandle == FilterDriverHandle);
	ASSERT(FilterDriverContext == (NDIS_HANDLE)FilterDriverObject);

	if ((NdisFilterDriverHandle != (NDIS_HANDLE)FilterDriverHandle) ||
		(FilterDriverContext != (NDIS_HANDLE)FilterDriverObject))
	{
		return NDIS_STATUS_INVALID_PARAMETER;
	}

	TRACE_EXIT();

	return NDIS_STATUS_SUCCESS;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_AttachAdapter(
	NDIS_HANDLE                     NdisFilterHandle,
	NDIS_HANDLE                     FilterDriverContext,
	PNDIS_FILTER_ATTACH_PARAMETERS  AttachParameters
	)
{
	POPEN_INSTANCE			Open = NULL;
	NDIS_STATUS             Status;
	NDIS_STATUS				returnStatus;
	NDIS_FILTER_ATTRIBUTES  FilterAttributes;
	BOOLEAN               bFalse = FALSE;

	TRACE_ENTER();

	do
	{
		ASSERT(FilterDriverContext == (NDIS_HANDLE)FilterDriverObject);
		if (FilterDriverContext != (NDIS_HANDLE)FilterDriverObject)
		{
			Status = NDIS_STATUS_INVALID_PARAMETER;
			break;
		}

		// Verify the media type is supported.  This is a last resort; the
		// the filter should never have been bound to an unsupported miniport
		// to begin with.  If this driver is marked as a Mandatory filter (which
		// is the default for this sample; see the INF file), failing to attach
		// here will leave the network adapter in an unusable state.
		//
		// Your setup/install code should not bind the filter to unsupported
		// media types.
		if ((AttachParameters->MiniportMediaType != NdisMedium802_3)
				&& (AttachParameters->MiniportMediaType != NdisMediumNative802_11)
				&& (AttachParameters->MiniportMediaType != NdisMediumWan) //we don't care this kind of miniports
				&& (AttachParameters->MiniportMediaType != NdisMediumWirelessWan) //we don't care this kind of miniports
				&& (AttachParameters->MiniportMediaType != NdisMediumFddi)
				&& (AttachParameters->MiniportMediaType != NdisMediumArcnet878_2)
				&& (AttachParameters->MiniportMediaType != NdisMediumAtm)
				&& (AttachParameters->MiniportMediaType != NdisMedium802_5))
		{
			IF_LOUD(DbgPrint("Unsupported media type: MiniportMediaType = %d.\n", AttachParameters->MiniportMediaType);)

			returnStatus = NDIS_STATUS_INVALID_PARAMETER;
			break;
		}

		// Disable this code for now, because it invalidates most adapters to be bound, reason needs to be clarified.
// 		if (AttachParameters->LowerIfIndex != AttachParameters->BaseMiniportIfIndex)
// 		{
// 			IF_LOUD(DbgPrint("Don't bind to other altitudes than exactly over the miniport: LowerIfIndex = %d, BaseMiniportIfIndex = %d.\n", AttachParameters->LowerIfIndex, AttachParameters->BaseMiniportIfIndex);)
// 
// 			returnStatus = NDIS_STATUS_NOT_SUPPORTED;
// 			break;
// 		}

		IF_LOUD(DbgPrint("NPF_Attach: AdapterName=%ws, MacAddress=%02X-%02X-%02X-%02X-%02X-%02X\n",
			AttachParameters->BaseMiniportName->Buffer,
			AttachParameters->CurrentMacAddress[0],
			AttachParameters->CurrentMacAddress[1],
			AttachParameters->CurrentMacAddress[2],
			AttachParameters->CurrentMacAddress[3],
			AttachParameters->CurrentMacAddress[4],
			AttachParameters->CurrentMacAddress[5]);
		)

		// create the adapter object
		Open = NPF_CreateOpenObject(AttachParameters->BaseMiniportName, AttachParameters->MiniportMediaType, NULL);
		if (Open == NULL)
		{
			returnStatus = NDIS_STATUS_RESOURCES;
			TRACE_EXIT();
			return returnStatus;
		}

#ifdef HAVE_WFP_LOOPBACK_SUPPORT
		// Determine whether this is our loopback adapter for the open_instance.
		if (g_LoopbackAdapterName.Buffer != NULL)
		{
			if (RtlCompareMemory(g_LoopbackAdapterName.Buffer + devicePrefix.Length / 2, AttachParameters->BaseMiniportName->Buffer + devicePrefix.Length / 2,
				AttachParameters->BaseMiniportName->Length - devicePrefix.Length) == AttachParameters->BaseMiniportName->Length - devicePrefix.Length)
			{
				// Commented this check because when system recovers from sleep, all adapters will be reattached, but g_LoopbackOpenGroupHead != NULL, so no adapter will be tagged as loopback.
// 				if (g_LoopbackOpenGroupHead == NULL)
// 				{
					Open->Loopback = TRUE;
					g_LoopbackOpenGroupHead = Open;
//				}
			}
		}
		else
		{
			IF_LOUD(DbgPrint("NPF_Attach: g_LoopbackAdapterName.Buffer=NULL\n");)
		}
#endif

#ifdef HAVE_RX_SUPPORT
		// Determine whether this is our send-to-Rx adapter for the open_instance.
		if (g_SendToRxAdapterName.Buffer != NULL)
		{
			int iAdapterCnt = (g_SendToRxAdapterName.Length / 2 + 1) / ADAPTER_NAME_SIZE_WITH_SEPARATOR;
			TRACE_MESSAGE2(PACKET_DEBUG_LOUD,
				"g_SendToRxAdapterName.Length=%d, iAdapterCnt=%d",
				g_SendToRxAdapterName.Length,
				iAdapterCnt);
			for (int i = 0; i < iAdapterCnt; i++)
			{
				if (RtlCompareMemory(g_SendToRxAdapterName.Buffer + devicePrefix.Length / 2 + ADAPTER_NAME_SIZE_WITH_SEPARATOR * i,
					AttachParameters->BaseMiniportName->Buffer + devicePrefix.Length / 2,
					AttachParameters->BaseMiniportName->Length - devicePrefix.Length)
					== AttachParameters->BaseMiniportName->Length - devicePrefix.Length)
				{
					Open->SendToRxPath = TRUE;
					break;
				}
			}
		}
		// Determine whether this is our block-Rx adapter for the open_instance.
		if (g_BlockRxAdapterName.Buffer != NULL)
		{
			int iAdapterCnt = (g_BlockRxAdapterName.Length / 2 + 1) / ADAPTER_NAME_SIZE_WITH_SEPARATOR;
			TRACE_MESSAGE2(PACKET_DEBUG_LOUD,
				"g_BlockRxAdapterName.Length=%d, iAdapterCnt=%d",
				g_BlockRxAdapterName.Length,
				iAdapterCnt);
			for (int i = 0; i < iAdapterCnt; i++)
			{
				if (RtlCompareMemory(g_BlockRxAdapterName.Buffer + devicePrefix.Length / 2 + ADAPTER_NAME_SIZE_WITH_SEPARATOR * i,
					AttachParameters->BaseMiniportName->Buffer + devicePrefix.Length / 2,
					AttachParameters->BaseMiniportName->Length - devicePrefix.Length)
					== AttachParameters->BaseMiniportName->Length - devicePrefix.Length)
				{
					Open->BlockRxPath = TRUE;
					break;
				}
			}
		}
#endif

#ifdef HAVE_WFP_LOOPBACK_SUPPORT
		TRACE_MESSAGE3(PACKET_DEBUG_LOUD,
			"Opening the device %ws, BindingContext=%p, Loopback=%d",
			AttachParameters->BaseMiniportName->Buffer,
			Open,
			Open->Loopback? 1 : 0);
#else
		TRACE_MESSAGE2(PACKET_DEBUG_LOUD,
			"Opening the device %ws, BindingContext=%p, Loopback=<Not supported>",
			AttachParameters->BaseMiniportName->Buffer,
			Open);
#endif

		returnStatus = STATUS_SUCCESS;

		NdisZeroMemory(&FilterAttributes, sizeof(NDIS_FILTER_ATTRIBUTES));
		FilterAttributes.Header.Revision = NDIS_FILTER_ATTRIBUTES_REVISION_1;
		FilterAttributes.Header.Size = sizeof(NDIS_FILTER_ATTRIBUTES);
		FilterAttributes.Header.Type = NDIS_OBJECT_TYPE_FILTER_ATTRIBUTES;
		FilterAttributes.Flags = 0;

		NDIS_DECLARE_FILTER_MODULE_CONTEXT(OPEN_INSTANCE);
		Status = NdisFSetAttributes(NdisFilterHandle,
			Open,
			&FilterAttributes);

		if (Status != NDIS_STATUS_SUCCESS)
		{
			returnStatus = Status;
			IF_LOUD(DbgPrint("Failed to set attributes.\n");)
			NPF_ReleaseOpenInstanceResources(Open);
			//
			// Free the open instance itself
			//
			ExFreePool(Open);
		}
		else
		{
			Open->AdapterHandle = NdisFilterHandle;
			Open->HigherPacketFilter = NPF_GetPacketFilter(Open);
			TRACE_MESSAGE2(PACKET_DEBUG_LOUD,
				"Opened the device, Status=%x, HigherPacketFilter=%x",
				Status,
				Open->HigherPacketFilter);

#ifdef HAVE_DOT11_SUPPORT
			if (Open->Medium == NdisMediumNative802_11)
			{
				// Handling raw 802.11 packets need to set NDIS_PACKET_TYPE_802_11_RAW_DATA and NDIS_PACKET_TYPE_802_11_RAW_MGMT in the packet filter.
				Open->Dot11PacketFilter = NDIS_PACKET_TYPE_802_11_RAW_DATA | NDIS_PACKET_TYPE_802_11_RAW_MGMT;
				ULONG combinedPacketFilter = Open->HigherPacketFilter | Open->MyPacketFilter | Open->Dot11PacketFilter;
				Status = NPF_SetPacketFilter(Open, combinedPacketFilter);
			}

			TRACE_MESSAGE2(PACKET_DEBUG_LOUD,
				"NPF_SetPacketFilter, Status=%x, Dot11PacketFilter=%x",
				Status,
				Open->Dot11PacketFilter);
#endif

			returnStatus = STATUS_SUCCESS;
			NPF_AddToOpenArray(Open);
		}
	}
	while (bFalse);

	TRACE_EXIT();
	return returnStatus;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_Pause(
	NDIS_HANDLE                     FilterModuleContext,
	PNDIS_FILTER_PAUSE_PARAMETERS   PauseParameters
	)
{
	POPEN_INSTANCE          Open = (POPEN_INSTANCE)FilterModuleContext;
	POPEN_INSTANCE			GroupOpen;
	NDIS_STATUS             Status;

	UNREFERENCED_PARAMETER(PauseParameters);
	TRACE_ENTER();

	for (GroupOpen = Open->GroupNext; GroupOpen != NULL; GroupOpen = GroupOpen->GroupNext)
	{
		if (GroupOpen->Multiple_Write_Counter > 0 || GroupOpen->TransmitPendingPackets > 0)
		{
			NdisAcquireSpinLock(&GroupOpen->OpenInUseLock);
			GroupOpen->PausePending = TRUE;
			NdisReleaseSpinLock(&GroupOpen->OpenInUseLock);
			Status = NDIS_STATUS_PENDING;
		}
		else
		{
			Status = NDIS_STATUS_SUCCESS;
		}
	}
	
	TRACE_EXIT();
	return Status;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_Restart(
	NDIS_HANDLE                     FilterModuleContext,
	PNDIS_FILTER_RESTART_PARAMETERS RestartParameters
	)
{
// 	NDIS_STATUS Status;
//
// 	UNREFERENCED_PARAMETER(FilterModuleContext);
// 	TRACE_ENTER();
//
// 	TIME_DESYNCHRONIZE(&G_Start_Time);
// 	TIME_SYNCHRONIZE(&G_Start_Time);
//
// 	Status = NDIS_STATUS_SUCCESS;
// 	TRACE_EXIT();
// 	return Status;

	// above is the original version of NPF_Restart() function.
	// below is the "disable offload" version of NPF_Restart() function.

	POPEN_INSTANCE	Open = (POPEN_INSTANCE) FilterModuleContext;
	POPEN_INSTANCE	GroupOpen;
	NDIS_STATUS		Status;

	TRACE_ENTER();

	TIME_DESYNCHRONIZE(&G_Start_Time);
	TIME_SYNCHRONIZE(&G_Start_Time);

	/* disable offload */
	{
		NDIS_STATUS_INDICATION indication;
		NDIS_OFFLOAD offload;

		NdisZeroMemory(&indication, sizeof(indication));
		indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
		indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
		indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
		indication.SourceHandle = Open->AdapterHandle;
		indication.StatusCode = NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG;
		indication.StatusBuffer = &offload;
		indication.StatusBufferSize = sizeof(offload);

		NdisZeroMemory(&offload, sizeof(offload));
		offload.Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
		offload.Header.Revision = NDIS_OFFLOAD_REVISION_1;
		offload.Header.Size = sizeof(offload);

		IF_LOUD(DbgPrint("NDIS_OBJECT_TYPE_OFFLOAD signaled\n");)

		NdisFIndicateStatus(Open->AdapterHandle, &indication);
	}

	for (GroupOpen = Open->GroupNext; GroupOpen != NULL; GroupOpen = GroupOpen->GroupNext)
	{
		NdisAcquireSpinLock(&GroupOpen->OpenInUseLock);
		GroupOpen->PausePending = FALSE;
		NdisReleaseSpinLock(&GroupOpen->OpenInUseLock);
	}

	Status = NDIS_STATUS_SUCCESS;
	TRACE_EXIT();
	return Status;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_DetachAdapter(
	NDIS_HANDLE     FilterModuleContext
	)
/*++

Routine Description:

	Filter detach routine.
	This is a required function that will deallocate all the resources allocated during
	FilterAttach. NDIS calls FilterAttach to remove a filter instance from a filter stack.

Arguments:

	FilterModuleContext - pointer to the filter context area.

Return Value:
	None.

NOTE: Called at PASSIVE_LEVEL and the filter is in paused state

--*/
{
	POPEN_INSTANCE		Open = (POPEN_INSTANCE) FilterModuleContext;
	POPEN_INSTANCE		GroupOpen;
	BOOLEAN				bFalse = FALSE;

	TRACE_ENTER();

	// 	if (Open->ReadEvent != NULL)
	// 		KeSetEvent(Open->ReadEvent,0,FALSE);
	for (GroupOpen = Open->GroupNext; GroupOpen != NULL; GroupOpen = GroupOpen->GroupNext)
	{
		if (GroupOpen->ReadEvent != NULL)
			KeSetEvent(GroupOpen->ReadEvent, 0, FALSE);
		NPF_CloseBindingAndAdapter(GroupOpen);
	}

	NPF_RemoveFromOpenArray(Open); //Must add this, if not, SYSTEM_SERVICE_EXCEPTION BSoD will occur.
	//NPF_ReleaseOpenInstanceResources(Open);
	//ExFreePool(Open);

	//NPF_RemoveUnclosedAdapters(); //if there are any unclosed adapter objects, just close them

	TRACE_EXIT();
	return;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_OidRequest(
	NDIS_HANDLE         FilterModuleContext,
	PNDIS_OID_REQUEST   Request
	)
/*++

Routine Description:

	Request handler
	Handle requests from upper layers

Arguments:

	FilterModuleContext   - our filter
	Request               - the request passed down


Return Value:

	 NDIS_STATUS_SUCCESS
	 NDIS_STATUS_PENDING
	 NDIS_STATUS_XXX

NOTE: Called at <= DISPATCH_LEVEL  (unlike a miniport's MiniportOidRequest)

--*/
{
	POPEN_INSTANCE          Open = (POPEN_INSTANCE) FilterModuleContext;
	NDIS_STATUS             Status;
	PNDIS_OID_REQUEST       ClonedRequest=NULL;
	BOOLEAN                 bSubmitted = FALSE;
	PFILTER_REQUEST_CONTEXT Context;
	BOOLEAN                 bFalse = FALSE;
	ULONG					combinedPacketFilter = 0;

	TRACE_ENTER();

	do
	{
		Status = NdisAllocateCloneOidRequest(Open->AdapterHandle,
											Request,
											NPF_ALLOC_TAG,
											&ClonedRequest);
		if (Status != NDIS_STATUS_SUCCESS)
		{
			TRACE_MESSAGE(PACKET_DEBUG_LOUD, "FilerOidRequest: Cannot Clone Request\n");
			break;
		}

		if (Request->RequestType == NdisRequestSetInformation && Request->DATA.SET_INFORMATION.Oid == OID_GEN_CURRENT_PACKET_FILTER)
		{
			Open->HigherPacketFilter = *(ULONG *) Request->DATA.SET_INFORMATION.InformationBuffer;
#ifdef HAVE_DOT11_SUPPORT
			combinedPacketFilter = Open->HigherPacketFilter | Open->MyPacketFilter | Open->Dot11PacketFilter;
#else
			combinedPacketFilter = Open->HigherPacketFilter | Open->MyPacketFilter;
#endif
			ClonedRequest->DATA.SET_INFORMATION.InformationBuffer = &combinedPacketFilter;
		}

		Context = (PFILTER_REQUEST_CONTEXT)(&ClonedRequest->SourceReserved[0]);
		*Context = Request; //SourceReserved != NULL indicates that this is other module's request

		bSubmitted = TRUE;

		//
		// Use same request ID
		//
		ClonedRequest->RequestId = Request->RequestId;

		Open->PendingOidRequest = ClonedRequest;

		Status = NdisFOidRequest(Open->AdapterHandle, ClonedRequest);

		if (Status != NDIS_STATUS_PENDING)
		{
			NPF_OidRequestComplete(Open, ClonedRequest, Status);
			Status = NDIS_STATUS_PENDING;
		}


	}while (bFalse);

	if (bSubmitted == FALSE)
	{
		switch(Request->RequestType)
		{
			case NdisRequestMethod:
				Request->DATA.METHOD_INFORMATION.BytesRead = 0;
				Request->DATA.METHOD_INFORMATION.BytesNeeded = 0;
				Request->DATA.METHOD_INFORMATION.BytesWritten = 0;
				break;

			case NdisRequestSetInformation:
				Request->DATA.SET_INFORMATION.BytesRead = 0;
				Request->DATA.SET_INFORMATION.BytesNeeded = 0;
				break;

			case NdisRequestQueryInformation:
			case NdisRequestQueryStatistics:
			default:
				Request->DATA.QUERY_INFORMATION.BytesWritten = 0;
				Request->DATA.QUERY_INFORMATION.BytesNeeded = 0;
				break;
		}

	}

	TRACE_EXIT();
	return Status;

}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_CancelOidRequest(
	NDIS_HANDLE             FilterModuleContext,
	PVOID                   RequestId
	)
/*++

Routine Description:

	Cancels an OID request

	If your filter driver does not intercept and hold onto any OID requests,
	then you do not need to implement this routine.  You may simply omit it.
	Furthermore, if the filter only holds onto OID requests so it can pass
	down a clone (the most common case) the filter does not need to implement
	this routine; NDIS will then automatically request that the lower-level
	filter/miniport cancel your cloned OID.

	Most filters do not need to implement this routine.

Arguments:

	FilterModuleContext   - our filter
	RequestId             - identifies the request(s) to cancel

--*/
{
	POPEN_INSTANCE                      Open = (POPEN_INSTANCE) FilterModuleContext;
	PNDIS_OID_REQUEST                   Request = NULL;
	PFILTER_REQUEST_CONTEXT             Context;
	PNDIS_OID_REQUEST                   OriginalRequest = NULL;
	BOOLEAN                             bFalse = FALSE;

	FILTER_ACQUIRE_LOCK(&Open->OIDLock, bFalse);

	Request = Open->PendingOidRequest;

	if (Request != NULL)
	{
		Context = (PFILTER_REQUEST_CONTEXT)(&Request->SourceReserved[0]);

		OriginalRequest = (*Context);
	}

	if ((OriginalRequest != NULL) && (OriginalRequest->RequestId == RequestId))
	{
		FILTER_RELEASE_LOCK(&Open->OIDLock, bFalse);

		NdisFCancelOidRequest(Open->AdapterHandle, RequestId);
	}
	else
	{
		FILTER_RELEASE_LOCK(&Open->OIDLock, bFalse);
	}
}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_OidRequestComplete(
	NDIS_HANDLE         FilterModuleContext,
	PNDIS_OID_REQUEST   Request,
	NDIS_STATUS         Status
	)
/*++

Routine Description:

	Notification that an OID request has been completed

	If this filter sends a request down to a lower layer, and the request is
	pended, the FilterOidRequestComplete routine is invoked when the request
	is complete.  Most requests we've sent are simply clones of requests
	received from a higher layer; all we need to do is complete the original
	higher request.

	However, if this filter driver sends original requests down, it must not
	attempt to complete a pending request to the higher layer.

Arguments:

	FilterModuleContext   - our filter context area
	NdisRequest           - the completed request
	Status                - completion status

--*/
{
	POPEN_INSTANCE                      Open = (POPEN_INSTANCE) FilterModuleContext;
	PNDIS_OID_REQUEST                   OriginalRequest;
	PFILTER_REQUEST_CONTEXT             Context;
	BOOLEAN                             bFalse = FALSE;

	TRACE_ENTER();

	Context = (PFILTER_REQUEST_CONTEXT)(&Request->SourceReserved[0]);
	OriginalRequest = (*Context);

	//
	// This is an internal request
	//
	if (OriginalRequest == NULL)
	{
		TRACE_MESSAGE1(PACKET_DEBUG_LOUD, "Status= %p", Status);
		NPF_InternalRequestComplete(Open, Request, Status);
		TRACE_EXIT();
		return;
	}


	FILTER_ACQUIRE_LOCK(&Open->OIDLock, bFalse);

	ASSERT(Open->PendingOidRequest == Request);
	Open->PendingOidRequest = NULL;

	FILTER_RELEASE_LOCK(&Open->OIDLock, bFalse);


	//
	// Copy the information from the returned request to the original request
	//
	switch(Request->RequestType)
	{
		case NdisRequestMethod:
			OriginalRequest->DATA.METHOD_INFORMATION.OutputBufferLength =  Request->DATA.METHOD_INFORMATION.OutputBufferLength;
			OriginalRequest->DATA.METHOD_INFORMATION.BytesRead = Request->DATA.METHOD_INFORMATION.BytesRead;
			OriginalRequest->DATA.METHOD_INFORMATION.BytesNeeded = Request->DATA.METHOD_INFORMATION.BytesNeeded;
			OriginalRequest->DATA.METHOD_INFORMATION.BytesWritten = Request->DATA.METHOD_INFORMATION.BytesWritten;
			break;

		case NdisRequestSetInformation:
			OriginalRequest->DATA.SET_INFORMATION.BytesRead = Request->DATA.SET_INFORMATION.BytesRead;
			OriginalRequest->DATA.SET_INFORMATION.BytesNeeded = Request->DATA.SET_INFORMATION.BytesNeeded;
			break;

		case NdisRequestQueryInformation:
		case NdisRequestQueryStatistics:
		default:
			OriginalRequest->DATA.QUERY_INFORMATION.BytesWritten = Request->DATA.QUERY_INFORMATION.BytesWritten;
			OriginalRequest->DATA.QUERY_INFORMATION.BytesNeeded = Request->DATA.QUERY_INFORMATION.BytesNeeded;
			break;
	}


	(*Context) = NULL;

	NdisFreeCloneOidRequest(Open->AdapterHandle, Request);

	NdisFOidRequestComplete(Open->AdapterHandle, OriginalRequest, Status);

	TRACE_EXIT();
}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_Status(
	NDIS_HANDLE             FilterModuleContext,
	PNDIS_STATUS_INDICATION StatusIndication
	)
/*++

Routine Description:

	Status indication handler

Arguments:

	FilterModuleContext     - our filter context
	StatusIndication        - the status being indicated

NOTE: called at <= DISPATCH_LEVEL

  FILTER driver may call NdisFIndicateStatus to generate a status indication to
  all higher layer modules.

--*/
{
	POPEN_INSTANCE      Open = (POPEN_INSTANCE) FilterModuleContext;

// 	TRACE_ENTER();
// 	IF_LOUD(DbgPrint("NPF: Status Indication\n");)

	/* disable offload */
	if (StatusIndication->StatusCode == NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG)
	{
		PNDIS_OFFLOAD offload = StatusIndication->StatusBuffer;
		IF_LOUD(DbgPrint("status NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG!!!\n");)

		if (StatusIndication->StatusBufferSize == sizeof(NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG) && (offload->Header.Type = NDIS_OBJECT_TYPE_OFFLOAD))
		{
			memset(&offload->Checksum, 0, sizeof(NDIS_TCP_IP_CHECKSUM_OFFLOAD));
			memset(&offload->LsoV1, 0, sizeof(NDIS_TCP_LARGE_SEND_OFFLOAD_V1));

			IF_LOUD(DbgPrint("status NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG disabled\n");)
		}
	}
	else
	{
		IF_LOUD(DbgPrint("status %x\n", StatusIndication->StatusCode);)
	}

	//
	// The filter may do processing on the status indication here, including
	// intercepting and dropping it entirely.  However, the sample does nothing
	// with status indications except pass them up to the higher layer.  It is
	// more efficient to omit the FilterStatus handler entirely if it does
	// nothing, but it is included in this sample for illustrative purposes.
	//
	NdisFIndicateStatus(Open->AdapterHandle, StatusIndication);

/*	TRACE_EXIT();*/
}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_DevicePnPEventNotify(
	NDIS_HANDLE             FilterModuleContext,
	PNET_DEVICE_PNP_EVENT   NetDevicePnPEvent
	)
/*++

Routine Description:

	Device PNP event handler

Arguments:

	FilterModuleContext         - our filter context
	NetDevicePnPEvent           - a Device PnP event

NOTE: called at PASSIVE_LEVEL

--*/
{
	POPEN_INSTANCE		   Open = (POPEN_INSTANCE) FilterModuleContext;
	NDIS_DEVICE_PNP_EVENT  DevicePnPEvent = NetDevicePnPEvent->DevicePnPEvent;

/*	TRACE_ENTER();*/

	//
	// The filter may do processing on the event here, including intercepting
	// and dropping it entirely.  However, the sample does nothing with Device
	// PNP events, except pass them down to the next lower* layer.  It is more
	// efficient to omit the FilterDevicePnPEventNotify handler entirely if it
	// does nothing, but it is included in this sample for illustrative purposes.
	//
	// * Trivia: Device PNP events percolate DOWN the stack, instead of upwards
	// like status indications and Net PNP events.  So the next layer is the
	// LOWER layer.
	//

	switch (DevicePnPEvent)
	{

		case NdisDevicePnPEventQueryRemoved:
		case NdisDevicePnPEventRemoved:
		case NdisDevicePnPEventSurpriseRemoved:
		case NdisDevicePnPEventQueryStopped:
		case NdisDevicePnPEventStopped:
		case NdisDevicePnPEventPowerProfileChanged:
		case NdisDevicePnPEventFilterListChanged:
			break;

		default:
			IF_LOUD(DbgPrint("FilterDevicePnPEventNotify: Invalid event.\n");)
			break;
	}

	NdisFDevicePnPEventNotify(Open->AdapterHandle, NetDevicePnPEvent);

/*	TRACE_EXIT();*/
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_NetPnPEvent(
	NDIS_HANDLE              FilterModuleContext,
	PNET_PNP_EVENT_NOTIFICATION NetPnPEventNotification
	)
/*++

Routine Description:

	Net PNP event handler

Arguments:

	FilterModuleContext         - our filter context
	NetPnPEventNotification     - a Net PnP event

NOTE: called at PASSIVE_LEVEL

--*/
{
	POPEN_INSTANCE			  Open = (POPEN_INSTANCE) FilterModuleContext;
	NDIS_STATUS               Status = NDIS_STATUS_SUCCESS;

	TRACE_ENTER();

	//
	// The filter may do processing on the event here, including intercepting
	// and dropping it entirely.  However, the sample does nothing with Net PNP
	// events, except pass them up to the next higher layer.  It is more
	// efficient to omit the FilterNetPnPEvent handler entirely if it does
	// nothing, but it is included in this sample for illustrative purposes.
	//

	Status = NdisFNetPnPEvent(Open->AdapterHandle, NetPnPEventNotification);

	TRACE_EXIT();
	return Status;
}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_ReturnEx(
	NDIS_HANDLE         FilterModuleContext,
	PNET_BUFFER_LIST    NetBufferLists,
	ULONG               ReturnFlags
	)
/*++

Routine Description:

	FilterReturnNetBufferLists handler.
	FilterReturnNetBufferLists is an optional function. If provided, NDIS calls
	FilterReturnNetBufferLists to return the ownership of one or more NetBufferLists
	and their embedded NetBuffers to the filter driver. If this handler is NULL, NDIS
	will skip calling this filter when returning NetBufferLists to the underlying
	miniport and will call the next lower driver in the stack. A filter that doesn't
	provide a FilterReturnNetBufferLists handler cannot originate a receive indication
	on its own.

Arguments:

	FilterInstanceContext       - our filter context area
	NetBufferLists              - a linked list of NetBufferLists that this
								  filter driver indicated in a previous call to
								  NdisFIndicateReceiveNetBufferLists
	ReturnFlags                 - flags specifying if the caller is at DISPATCH_LEVEL

--*/
{
	POPEN_INSTANCE		Open = (POPEN_INSTANCE) FilterModuleContext;
	PNET_BUFFER_LIST    CurrNbl = NetBufferLists;
	UINT                NumOfNetBufferLists = 0;
	BOOLEAN             DispatchLevel;
	ULONG               Ref;

/*	TRACE_ENTER();*/

	// Return the received NBLs.  If you removed any NBLs from the chain, make
	// sure the chain isn't empty (i.e., NetBufferLists!=NULL).
	NdisFReturnNetBufferLists(Open->AdapterHandle, NetBufferLists, ReturnFlags);

/*	TRACE_EXIT();*/
}

//-------------------------------------------------------------------

_Use_decl_annotations_
VOID
NPF_CancelSendNetBufferLists(
	NDIS_HANDLE             FilterModuleContext,
	PVOID                   CancelId
	)
/*++

Routine Description:

	This function cancels any NET_BUFFER_LISTs pended in the filter and then
	calls the NdisFCancelSendNetBufferLists to propagate the cancel operation.

	If your driver does not queue any send NBLs, you may omit this routine.
	NDIS will propagate the cancelation on your behalf more efficiently.

Arguments:

	FilterModuleContext      - our filter context area.
	CancelId                 - an identifier for all NBLs that should be dequeued

Return Value:

	None

*/
{
	POPEN_INSTANCE  Open = (POPEN_INSTANCE) FilterModuleContext;

	NdisFCancelSendNetBufferLists(Open->AdapterHandle, CancelId);
}

//-------------------------------------------------------------------

_Use_decl_annotations_
NDIS_STATUS
NPF_SetModuleOptions(
	NDIS_HANDLE             FilterModuleContext
	)
/*++

Routine Description:

	This function set the optional handlers for the filter

Arguments:

	FilterModuleContext: The FilterModuleContext given to NdisFSetAttributes

Return Value:

	NDIS_STATUS_SUCCESS
	NDIS_STATUS_RESOURCES
	NDIS_STATUS_FAILURE

--*/
{
   NDIS_STATUS Status = NDIS_STATUS_SUCCESS;
   UNREFERENCED_PARAMETER(FilterModuleContext);

   return Status;
}

//-------------------------------------------------------------------

ULONG
NPF_GetPacketFilter(
	NDIS_HANDLE FilterModuleContext
	)
{
	ULONG PacketFilter = 0;
	ULONG BytesProcessed = 0;

	// get the PacketFilter when filter driver loads
	NPF_DoInternalRequest(FilterModuleContext,
		NdisRequestQueryInformation,
		OID_GEN_CURRENT_PACKET_FILTER,
		&PacketFilter,
		sizeof(PacketFilter),
		0,
		0,
		&BytesProcessed
		);

	if (BytesProcessed != sizeof(PacketFilter))
	{
		return 0;
	}
	else
	{
		return PacketFilter;
	}
}

//-------------------------------------------------------------------

NDIS_STATUS
NPF_SetPacketFilter(
	NDIS_HANDLE FilterModuleContext,
	ULONG PacketFilter
)
{
	NDIS_STATUS Status;
	ULONG BytesProcessed = 0;

	// get the PacketFilter when filter driver loads
	NPF_DoInternalRequest(FilterModuleContext,
		NdisRequestQueryInformation,
		OID_GEN_CURRENT_PACKET_FILTER,
		&PacketFilter,
		sizeof(PacketFilter),
		0,
		0,
		&BytesProcessed
	);

	if (BytesProcessed != sizeof(PacketFilter))
	{
		Status = NDIS_STATUS_SUCCESS;
		return Status;
	}
	else
	{
		Status = NDIS_STATUS_FAILURE;
		return TRUE;
	}
}

//-------------------------------------------------------------------

NDIS_STATUS
NPF_DoInternalRequest(
	_In_ NDIS_HANDLE			      FilterModuleContext,
	_In_ NDIS_REQUEST_TYPE            RequestType,
	_In_ NDIS_OID                     Oid,
	_Inout_updates_bytes_to_(InformationBufferLength, *pBytesProcessed)
		 PVOID                        InformationBuffer,
	_In_ ULONG                        InformationBufferLength,
	_In_opt_ ULONG                    OutputBufferLength,
	_In_ ULONG                        MethodId,
	_Out_ PULONG                      pBytesProcessed
	)
{
	POPEN_INSTANCE				Open = (POPEN_INSTANCE) FilterModuleContext;
	INTERNAL_REQUEST            FilterRequest;
	PNDIS_OID_REQUEST           NdisRequest = &FilterRequest.Request;
	NDIS_STATUS                 Status;
	BOOLEAN                     bFalse;


	bFalse = FALSE;
	*pBytesProcessed = 0;
	NdisZeroMemory(NdisRequest, sizeof(NDIS_OID_REQUEST));

	NdisInitializeEvent(&FilterRequest.InternalRequestCompletedEvent);
	NdisResetEvent(&FilterRequest.InternalRequestCompletedEvent);

	if (*((PVOID *) FilterRequest.Request.SourceReserved) != NULL)
	{
		*((PVOID *) FilterRequest.Request.SourceReserved) = NULL; //indicates this is a self-sent request
	}

	NdisRequest->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
	NdisRequest->Header.Revision = NDIS_OID_REQUEST_REVISION_1;
	NdisRequest->Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
	NdisRequest->RequestType = RequestType;

	switch (RequestType)
	{
		case NdisRequestQueryInformation:
			 NdisRequest->DATA.QUERY_INFORMATION.Oid = Oid;
			 NdisRequest->DATA.QUERY_INFORMATION.InformationBuffer =
									InformationBuffer;
			 NdisRequest->DATA.QUERY_INFORMATION.InformationBufferLength =
									InformationBufferLength;
			break;

		case NdisRequestSetInformation:
			 NdisRequest->DATA.SET_INFORMATION.Oid = Oid;
			 NdisRequest->DATA.SET_INFORMATION.InformationBuffer =
									InformationBuffer;
			 NdisRequest->DATA.SET_INFORMATION.InformationBufferLength =
									InformationBufferLength;
			break;

		case NdisRequestMethod:
			 NdisRequest->DATA.METHOD_INFORMATION.Oid = Oid;
			 NdisRequest->DATA.METHOD_INFORMATION.MethodId = MethodId;
			 NdisRequest->DATA.METHOD_INFORMATION.InformationBuffer =
									InformationBuffer;
			 NdisRequest->DATA.METHOD_INFORMATION.InputBufferLength =
									InformationBufferLength;
			 NdisRequest->DATA.METHOD_INFORMATION.OutputBufferLength = OutputBufferLength;
			 break;



		default:
			ASSERT(bFalse);
			break;
	}

	NdisRequest->RequestId = (PVOID)NPF_REQUEST_ID;

	Status = NdisFOidRequest(Open->AdapterHandle,
							NdisRequest);


	if (Status == NDIS_STATUS_PENDING)
	{

		NdisWaitEvent(&FilterRequest.InternalRequestCompletedEvent, 0);
		Status = FilterRequest.RequestStatus;
	}


	if (Status == NDIS_STATUS_SUCCESS)
	{
		if (RequestType == NdisRequestSetInformation)
		{
			*pBytesProcessed = NdisRequest->DATA.SET_INFORMATION.BytesRead;
		}

		if (RequestType == NdisRequestQueryInformation)
		{
			*pBytesProcessed = NdisRequest->DATA.QUERY_INFORMATION.BytesWritten;
		}

		if (RequestType == NdisRequestMethod)
		{
			*pBytesProcessed = NdisRequest->DATA.METHOD_INFORMATION.BytesWritten;
		}

		//
		// The driver below should set the correct value to BytesWritten
		// or BytesRead. But now, we just truncate the value to InformationBufferLength
		//
		if (RequestType == NdisRequestMethod)
		{
			if (*pBytesProcessed > OutputBufferLength)
			{
				*pBytesProcessed = OutputBufferLength;
			}
		}
		else
		{

			if (*pBytesProcessed > InformationBufferLength)
			{
				*pBytesProcessed = InformationBufferLength;
			}
		}
	}


	return Status;
}

//-------------------------------------------------------------------

VOID
NPF_InternalRequestComplete(
	_In_ NDIS_HANDLE                  FilterModuleContext,
	_In_ PNDIS_OID_REQUEST            NdisRequest,
	_In_ NDIS_STATUS                  Status
	)
/*++

Routine Description:

	NDIS entry point indicating completion of a pended NDIS_OID_REQUEST.

Arguments:

	FilterModuleContext - pointer to filter module context
	NdisRequest - pointer to NDIS request
	Status - status of request completion

Return Value:

	None

--*/
{
	PINTERNAL_REQUEST pRequest;

	UNREFERENCED_PARAMETER(FilterModuleContext);

	TRACE_ENTER();

	pRequest = CONTAINING_RECORD(NdisRequest, INTERNAL_REQUEST, Request);

	//
	// Set the request result
	//
	pRequest->RequestStatus = Status;

	//
	// and awake the caller
	//
	NdisSetEvent(&pRequest->InternalRequestCompletedEvent);

	TRACE_EXIT();
}

