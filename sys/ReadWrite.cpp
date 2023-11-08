// Read/Write request processors for atk16u driver
// Copyright (C) 2002 by Vyacheslav Vdovichenko
// All rights reserved

#include "stddcls.h"
#include "driver.h"

#ifdef DBG
	#define MSGUSBSTRING(d,s,i) { \
		UNICODE_STRING sd; \
		if (i && NT_SUCCESS(GetStringDescriptor(d,i,&sd))) { \
			DbgPrint(s, sd.Buffer); \
			RtlFreeUnicodeString(&sd); \
		}}
#else
	#define MSGUSBSTRING(d,s,i)
#endif

struct _RANDOM_JUNK
	{
	PDEVICE_EXTENSION DeviceExtension;
	UCHAR intdata[INTERRUPTDATASIZE];		
	PIO_WORKITEM item;
	};
typedef _RANDOM_JUNK RANDOM_JUNK, *PRANDOM_JUNK;

ULONG TickCountMs(VOID);
NTSTATUS GetStringDescriptor(PDEVICE_OBJECT fdo, UCHAR istring, PUNICODE_STRING s);
NTSTATUS SendBulkUrb(PDEVICE_EXTENSION pdx, PUCHAR data);
NTSTATUS StartInterruptUrb(PDEVICE_EXTENSION pdx);
NTSTATUS OnInterrupt(PDEVICE_OBJECT junk, PIRP Irp, PDEVICE_EXTENSION pdx);
VOID StopInterruptUrb(PDEVICE_EXTENSION pdx);

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

ULONG TickCountMs(VOID)
	{							// TickCountMs
	
	ULONG interval = KeQueryTimeIncrement();
   	//KdPrint((DRIVERNAME " - KeQueryTimeIncrement is %d\n", interval));

	LARGE_INTEGER time;
	KeQueryTickCount(&time);
    //KdPrint((DRIVERNAME " - KeQueryTickCount is %d\n", time));
	
	ULONG now;
	PUCHAR pnow = (PUCHAR)&now;
	PUCHAR ptime = (PUCHAR)&time;
	UCHAR n;
	for (n = 0; n < 4; n++)
	{
		*pnow = *ptime; 
		++pnow;
		++ptime;
	}
	
	return (interval/10000)*now;
	}							// TickCountMs

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS GetStringDescriptor(PDEVICE_OBJECT fdo, UCHAR istring, PUNICODE_STRING s)
	{							// GetStringDescriptor
	NTSTATUS status;
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	URB urb;

	UCHAR data[255];			// maximum-length buffer

	// If this is the first time here, read string descriptor zero and arbitrarily select
	// the first language identifer as the one to use in subsequent get-descriptor calls.

	if (!pdx->langid)
		{						// determine default language id
		UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST), USB_STRING_DESCRIPTOR_TYPE,
			0, 0, data, NULL, sizeof(data), NULL);
		status = SendAwaitUrb(fdo, &urb);
		if (!NT_SUCCESS(status))
			return status;
		pdx->langid = *(LANGID*)(data + 2);
        
		//KdPrint((DRIVERNAME " - langid = 0x%x\n", pdx->langid));

		}						// determine default language id

	// Fetch the designated string descriptor.
	UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST), USB_STRING_DESCRIPTOR_TYPE,
		istring, pdx->langid, data, NULL, sizeof(data), NULL);
	status = SendAwaitUrb(fdo, &urb);
	if (!NT_SUCCESS(status))
		return status;
    
	//KdPrint((DRIVERNAME " - length = 0x%x, type = 0x%x\n", data[0], data[1]));
	
	ULONG nchars = (data[0] - 2) / 2;
	PWSTR p = (PWSTR) ExAllocatePool(PagedPool, data[0]);
	if (!p)
		return STATUS_INSUFFICIENT_RESOURCES;

	memcpy(p, data + 2, nchars*2);
	p[nchars] = 0;

	s->Length = (USHORT) (2 * nchars);
	s->MaximumLength = (USHORT) ((2 * nchars) + 2);
	s->Buffer = p;

	return STATUS_SUCCESS;
	}							// GetStringDescriptor

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID AbortPipe(PDEVICE_OBJECT fdo, USBD_PIPE_HANDLE hpipe)
	{							// AbortPipe
	PAGED_CODE();
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	URB urb;

	urb.UrbHeader.Length = (USHORT) sizeof(_URB_PIPE_REQUEST);
	urb.UrbHeader.Function = URB_FUNCTION_ABORT_PIPE;
	urb.UrbPipeRequest.PipeHandle = hpipe;

	NTSTATUS status = SendAwaitUrb(fdo, &urb);
	if (!NT_SUCCESS(status))
		KdPrint((DRIVERNAME " - Error %X in AbortPipe\n", status));
	}							// AbortPipe

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS CreateInterruptUrb(PDEVICE_OBJECT fdo)
	{							// CreateInterruptUrb
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	ASSERT(pdx->PollingIrp == NULL);
	ASSERT(pdx->PollingUrb == NULL);

	PIRP Irp = IoAllocateIrp(pdx->LowerDeviceObject->StackSize, FALSE);
	if (!Irp)
		{
		KdPrint((DRIVERNAME " - Unable to create IRP for interrupt polling\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
		}

	PURB urb = (PURB) ExAllocatePool(NonPagedPool, sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER));
	if (!urb)
		{
		KdPrint((DRIVERNAME " - Unable to allocate interrupt polling URB\n"));
		IoFreeIrp(Irp);
		return STATUS_INSUFFICIENT_RESOURCES;
		}

	pdx->PollingIrp = Irp;
	pdx->PollingUrb = urb;

	return STATUS_SUCCESS;
	}							// CreateInterruptUrb

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID DeleteInterruptUrb(PDEVICE_OBJECT fdo)
	{							// DeleteInterruptUrb
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	ASSERT(pdx->PollingIrp != NULL);
	ASSERT(pdx->PollingUrb != NULL);

	ExFreePool(pdx->PollingUrb);
	IoFreeIrp(pdx->PollingIrp);
	pdx->PollingIrp = NULL;
	pdx->PollingUrb = NULL;
	}							// DeleteInterruptUrb

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS DispatchCleanup(PDEVICE_OBJECT fdo, PIRP Irp)
	{							// DispatchCleanup
	PAGED_CODE();

	KdPrint((DRIVERNAME " - DispatchCleanup Entry!!!\n"));

	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	GenericCleanupAllRequests(pdx->pgx, stack->FileObject, STATUS_CANCELLED);

	PIRP intirp = GenericUncacheControlRequest(pdx->pgx, &pdx->InterruptIrp);
	if (intirp)
		{
		KdPrint((DRIVERNAME " - Event IRP finally complete\n"));
		InterlockedDecrement(&pdx->numints); // restore counter
		CompleteRequest(intirp, STATUS_SUCCESS, 0);
		}
	
	return CompleteRequest(Irp, STATUS_SUCCESS, 0);
	}							// DispatchCleanup

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS DispatchCreate(PDEVICE_OBJECT fdo, PIRP Irp)
	{							// DispatchCreate
	PAGED_CODE();
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;

	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

	NTSTATUS status = STATUS_SUCCESS;

	//KdPrint((DRIVERNAME " - DispatchCreate Entry!!!\n"));
		
	if (NT_SUCCESS(status))
		{						// okay to open
		if (InterlockedIncrement(&pdx->handles) == 1)
			{					// first open handle

			//MSGUSBSTRING(fdo, DRIVERNAME " - Serial number is %ws\n", pdx->dd.iSerialNumber);
						
			pdx->filling = 0;

			ULONG i;
			for (i = 0; i < NUMCHANNELS; i++)
			{					
				pdx->buffer[i] = NULL;
				pdx->bufferfill[i] = 0;
				pdx->buffercount[i] = 0;
				pdx->intstate[i] = 0x60;
				pdx->curoutstate[i] = 0x60;
				pdx->curpolarity[i] = LINE_INVALID;
				pdx->linechangetime[i] = TickCountMs();
				pdx->notification[i] = 0;
				pdx->damageisnow[i] = 0;
				pdx->polarityisnow[i] = 0;
				pdx->awaitlinepolarity[i] = 0;
				pdx->awaitlineduration[i] = 0;
				pdx->pulseduration[i] = 0;
				pdx->pulsepolarity[i] = 0;
				pdx->awaitpulsepolarity[i] = 0;
				pdx->awaitpulsemin[i] = 0;
				pdx->awaitpulsemax[i] = 0;
				pdx->damageonduration[i] = 0;
				pdx->damageoffduration[i] = 0;
				pdx->pulseissuetime[i] = 0;
				pdx->pulseissuekind[i] = 0;
				pdx->pulsecounter[i] = 0;
			}

			// Issue first polling request device signalled an interrupt				
			StartInterruptUrb(pdx); 
			}					// okay to open
		}					// first open handle
	return CompleteRequest(Irp, status, 0);
	}							// DispatchCreate

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS DispatchClose(PDEVICE_OBJECT fdo, PIRP Irp)
	{							// DispatchClose
	PAGED_CODE();
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	if (InterlockedDecrement(&pdx->handles) == 0)
		{						// no more open handles
			ULONG i;
			for (i = 0; i < NUMCHANNELS; i++)
			if (pdx->buffer[i])
			{
				ExFreePool(pdx->buffer[i]);
				pdx->buffer[i] = NULL;
			}
			//
			StopInterruptUrb(pdx);
			KdPrint((DRIVERNAME " - DispatchClose finally!!!\n"));
		}						// no more open handles

	return CompleteRequest(Irp, STATUS_SUCCESS, 0);
	}							// DispatchClose

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS SendBulkUrb(PDEVICE_EXTENSION pdx, PUCHAR data)
	{							// SendBulkUrb
	PAGED_CODE();
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
	// Prepeare URB
	URB urb;
	UsbBuildInterruptOrBulkTransferRequest(&urb, sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER),
		pdx->hcompipe, data, NULL, COMMANDDATASIZE, 0, NULL);		
	
	// Send URB
	NTSTATUS status = SendAwaitUrb(pdx->DeviceObject, &urb);
	if (!NT_SUCCESS(status))
		KdPrint((DRIVERNAME " - Error %X (USBD status code %X) trying to write endpoint\n", status, urb.UrbHeader.Status));

	return status;
	}							// SendBulkUrb

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID IntrruptDataProcess(PDEVICE_EXTENSION pdx, UCHAR intdata[INTERRUPTDATASIZE])
	{							// IntrruptDataProcess
	PAGED_CODE();

	ULONG time = TickCountMs();
	ULONG duration;
	UCHAR i;
	UCHAR n;
	UCHAR point;
	UCHAR state;
	UCHAR data;
	UCHAR polarity;
	PUCHAR pw;

	for (i = 0; i < INTERRUPTDATASIZE; i++)
	if ((i & 1) == 0)
	{
		point = i/2;
		state = intdata[i+1];
		data = intdata[i];
		polarity = ((state & 0x18) >> 3);

		//KdPrint((DRIVERNAME " - State on %d is 0x%x\n", point, state));

		// does line change occur?
		if (((state & 0x80) == 0x80) 
			|| (pdx->curpolarity[point] == LINE_INVALID)
			|| ((pdx->curpolarity[point] == LINE_DAMAGE) 
				&& (polarity != LINE_DAMAGE)))
		{
			// last standings are saving
			pdx->pulsepolarity[point] = (polarity ^ LINE_INVERSE);
			pdx->pulseduration[point] = (time - pdx->linechangetime[point]);
			if (pdx->pulseduration[point] < 160)
				pdx->pulseduration[point] = (((data - pdx->pulsecounter[point]) >> 3) * 5);			
			pdx->pulsecounter[point] = data;
			pdx->curpolarity[point] = polarity;
			pdx->linechangetime[point] = time;
			pdx->polarityisnow[point] = 0;			

			KdPrint((DRIVERNAME " - Line on %d is changed: 0x%x (%d)\n", point, polarity, time));
			
			// are current polarity need notification?
			if ((polarity & pdx->notification[point]) != 0)
			{
				// insert "LINE_CHANGE"
				if (pdx->filling < (EXCHANGEDATASIZE-5))
				{
					pdx->extdata[pdx->filling] = CHANNEL_EVENT + (polarity << 4) + point;
					pdx->filling++;
					pdx->extdata[pdx->filling] = LINE_CHANGE;
					pdx->filling++;
												
					pw = (PUCHAR)&time;
					for (n = 0; n < 4; n++)
					{
						pdx->extdata[pdx->filling] = *pw; 
						++pw; 
						pdx->filling++;
					}
					
					KdPrint((DRIVERNAME " - LINE_CHANGE event on %d is 0x%x (%d)\n", point, polarity, time));
				}
			}	

			// are last pulse need keeping?
			if ((pdx->awaitpulsepolarity[point] ==  pdx->pulsepolarity[point])
				&& (pdx->awaitpulsemin[point] > 0) 
				&& (pdx->awaitpulsemax[point] > 0)
				&& (pdx->pulseduration[point] >= pdx->awaitpulsemin[point])
				&& (pdx->pulseduration[point] <= pdx->awaitpulsemax[point]))
			{
				// insert "PULSE_IN"
				if (pdx->filling < (EXCHANGEDATASIZE-2))
				{
					pdx->extdata[pdx->filling] = CHANNEL_EVENT + (pdx->pulsepolarity[point] << 4) + point;
					pdx->filling++;
					pdx->extdata[pdx->filling] = PULSE_IN;
					pdx->filling++;

					pw = (PUCHAR)&pdx->pulseduration[point];
					for (n = 0; n < 4; n++)
					{
						pdx->extdata[pdx->filling] = *pw; 
						++pw; 
						pdx->filling++;
					}
			
					KdPrint((DRIVERNAME " - Pulse on %d is 0x%x\n", point, state));
				}
			}

		}

		// current polarity duration 
		duration = (time - pdx->linechangetime[point]);
			
		// do damage is it?
		if ((pdx->damageonduration[point] > 0) && (pdx->damageoffduration[point] > 0))
		{
			if (polarity == LINE_DAMAGE) 
			{
				if ((pdx->damageisnow[point] == 0) && (duration >= pdx->damageonduration[point]))
				{
					// insert "DAMAGE_STATE"
					if (pdx->filling < (EXCHANGEDATASIZE-5))
					{				
						pdx->damageisnow[point] = 1;

						pdx->extdata[pdx->filling] = CHANNEL_EVENT + point;
						pdx->filling++;
						pdx->extdata[pdx->filling] = DAMAGE_STATE;
						pdx->filling++;
											
						pw = (PUCHAR)&duration;
						for (n = 0; n < 4; n++)
						{
							pdx->extdata[pdx->filling] = *pw; 
							++pw; 
							pdx->filling++;
						}
				
						KdPrint((DRIVERNAME " - DAMAGE_STATE event (ON) on %d is %d\n", point, duration));
					}
				}
			}
			else
			{
				// has been the damage?
				if ((pdx->damageisnow[point] == 1) && (duration >= pdx->damageoffduration[point]))
				{
					// insert "DAMAGE_STATE"
					if (pdx->filling < (EXCHANGEDATASIZE-5))
					{
						pdx->damageisnow[point] = 0;

						pdx->extdata[pdx->filling] = CHANNEL_EVENT + 0x30 + point;
						pdx->filling++;
						pdx->extdata[pdx->filling] = DAMAGE_STATE;
						pdx->filling++;
											
						pw = (PUCHAR)&duration;
						for (n = 0; n < 4; n++)
						{
							pdx->extdata[pdx->filling] = *pw; 
							++pw; 
							pdx->filling++;
						}
				
						KdPrint((DRIVERNAME " - DAMAGE_STATE event (OFF) on %d is %d\n", point, duration));
					}
				}
			}			
		}
		else
			pdx->damageisnow[point] = 0;

		// are current polarity need keeping?
		if ((pdx->polarityisnow[point] == 0)
			&& (pdx->awaitlineduration[point] > 0)
			&& (pdx->curpolarity[point] == pdx->awaitlinepolarity[point]) 
			&& (duration >= pdx->awaitlineduration[point]))
		{
			// insert "LINE_IN"
			if (pdx->filling < (EXCHANGEDATASIZE-5))
			{
				pdx->polarityisnow[point] = 1;
				
				pdx->extdata[pdx->filling] = CHANNEL_EVENT + (pdx->curpolarity[point] << 4) + point;
				pdx->filling++;
				pdx->extdata[pdx->filling] = LINE_IN;
				pdx->filling++;
											
				pw = (PUCHAR)&duration;
				for (n = 0; n < 4; n++)
				{
					pdx->extdata[pdx->filling] = *pw; 
					++pw; 
					pdx->filling++;
				}
				
				KdPrint((DRIVERNAME " - LINE_IN event on %d is 0x%x (%d)\n", point, pdx->curpolarity[point], duration));
			}

		}
		
		pdx->intstate[point] &= state;
		
		// does symbol input occur?
		if (((state & 0x1) == 0x1) && ((pdx->notification[point] & SYMBOL_IN) == SYMBOL_IN))
		{
			// insert "INPUT_SYMBOL"
			if (pdx->filling < (EXCHANGEDATASIZE-1))
			{
				pdx->extdata[pdx->filling] = INPUT_SYMBOL + ((state & 0x6) << 3) + point;
				pdx->filling++;
				pdx->extdata[pdx->filling] = data;
				pdx->filling++;
			}
		}
		
		// does symbol output occur?
		if ( (pdx->buffer[point]) && 
			( (((state & 0x40) != 0) && ((pdx->intstate[point] & 0x40) == 0)) 
			|| (((state & 0x20) != 0) && ((pdx->intstate[point] & 0x20) == 0)) ) )
		{
			// it's symbol output event?
			if ((pdx->buffercount[point]) && ((pdx->notification[point] & SYMBOL_OUT) == SYMBOL_OUT))
			{
				// insert "OUTPUT_SYMBOL"
				if (pdx->filling < (EXCHANGEDATASIZE-1))
				{
					pdx->extdata[pdx->filling] = OUTPUT_SYMBOL + ((pdx->curoutstate[point] & 0x60) >> 1) + point;
					pdx->filling++;
					pdx->extdata[pdx->filling] = pdx->curoutsymbol[point];
					pdx->filling++;
				}
			}
			
			// send the following symbol (if one is) from buffer 
			if (pdx->buffercount[point] < pdx->bufferfill[point])
			{	
				// send the following symbol
				pdx->curoutstate[point] = state;
				pdx->curoutsymbol[point] = *(pdx->buffer[point]+pdx->buffercount[point]);
				// CCP(SEND_SYMBOL)
				UCHAR command[COMMANDDATASIZE];
				command[0] = point;
				command[1] = pdx->curoutsymbol[point];
				if (NT_SUCCESS(SendBulkUrb(pdx, (PUCHAR)&command)))
				{
					pdx->buffercount[point]++;
					pdx->intstate[point] = 0x60;
				}
			}
			else 
			{	
				// output buffer is complete
				pdx->intstate[point] = 0x60;				
				ExFreePool(pdx->buffer[point]);
				pdx->buffer[point] = NULL;
				pdx->bufferfill[point] = 0;
				pdx->buffercount[point] = 0;

				// insert "OUPUT_DATA"
				if (pdx->filling < (EXCHANGEDATASIZE-1))
				{
				   	//KdPrint((DRIVERNAME " - OUPUT_DATA on %d\n", point));
					
					pdx->extdata[pdx->filling] = CHANNEL_EVENT + point;
					pdx->filling++;
					pdx->extdata[pdx->filling] = OUTPUT_DATA;
					pdx->filling++;
				}
			}
		}

		// does pulse output occur?
		if ((pdx->pulseissuetime[point] > 0) && (pdx->pulseissuetime[point] > time))
		{
			pdx->pulseissuetime[point] = 0;
			
			// insert "PULSE_OUT"
			if (pdx->filling < (EXCHANGEDATASIZE-1))
			{
			   	//KdPrint((DRIVERNAME " - PULSE_OUT on %d\n", point));
				
				pdx->extdata[pdx->filling] = CHANNEL_EVENT + point;
				pdx->filling++;
				pdx->extdata[pdx->filling] = PULSE_OUT;
				pdx->filling++;
			}

			if (pdx->pulseissuekind[point] == 1)
			{
				pdx->pulseissuekind[point] = 0;
				// CCP extantion (ISSUE_PULSE)
				UCHAR command[COMMANDDATASIZE];
				command[0] = (ISSUE_PULSE | point);
				command[1] = 1;
				SendBulkUrb(pdx, (PUCHAR)&command);
			}
		}
		
	}

	}							// IntrruptDataProcess

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID InterruptCallback(PDEVICE_OBJECT fdo, PRANDOM_JUNK junk)
	{							// InterruptCallback
	PAGED_CODE();
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

	PDEVICE_EXTENSION pdx = junk->DeviceExtension;

	// Interrupt data processing
    IntrruptDataProcess(pdx, junk->intdata);
		
	if (pdx->filling > 0)
	{	
		PIRP intirp = GenericUncacheControlRequest(pdx->pgx, &pdx->InterruptIrp);
		if (intirp)
		{
			//KdPrint((DRIVERNAME " - Event IRP complete\n"));

			PUCHAR pw = (PUCHAR)intirp->AssociatedIrp.SystemBuffer;
			ULONG i;
			for (i = 0; i < pdx->filling; i++)
			{
				*pw = pdx->extdata[i];
				++pw;
			}

			InterlockedDecrement(&pdx->numints); // restore counter
			CompleteRequest(intirp, STATUS_SUCCESS, pdx->filling);
			pdx->filling = 0;
		}
	}
		
	//!!! Âלוסעמ גûחמגא ג OnInterrupt
	//StartInterruptUrb(pdx);  
	
	// Release the memory occupied by the work item
	IoFreeWorkItem(junk->item);

	// Finally, release our context structure
	ExFreePool(junk);
	}							// SendBulkUrbCallback

///////////////////////////////////////////////////////////////////////////////

#pragma LOCKEDCODE

NTSTATUS QueueInterrupt(PDEVICE_EXTENSION pdx)
	{							// QueueIntrrupt

	PRANDOM_JUNK junk = (PRANDOM_JUNK) ExAllocatePool(NonPagedPool, sizeof(RANDOM_JUNK));
	if (!junk)
		{
		KdPrint((DRIVERNAME " - ExAllocatePool failed to allocate %d bytes for RANDOM_JUNK structure\n", sizeof(RANDOM_JUNK)));
		return STATUS_INSUFFICIENT_RESOURCES;
		}

	junk->DeviceExtension = pdx;
	for (ULONG i = 0; i < INTERRUPTDATASIZE; i++)
		junk->intdata[i] = pdx->intdata[i];
	
	PIO_WORKITEM item = IoAllocateWorkItem(pdx->DeviceObject);
	if (!item)
		{
		ExFreePool(junk);
		KdPrint((DRIVERNAME " - IoAllocateWorkItem failed\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
		}

	junk->item = item;
		
	// Queue the work item for delayed execution. IoQueueWorkItem will take out an
	// extra reference to our device object that will persist until after the
	// callback routine returns, thereby preventing this driver from unloading before
	// the last instruction in this driver finishes executing.
	
	IoQueueWorkItem(item, (PIO_WORKITEM_ROUTINE) InterruptCallback, HyperCriticalWorkQueue, junk);

	return STATUS_SUCCESS;
	}							// QueueIntrrupt

///////////////////////////////////////////////////////////////////////////////

#pragma LOCKEDCODE

NTSTATUS OnInterrupt(PDEVICE_OBJECT junk, PIRP Irp, PDEVICE_EXTENSION pdx)
	{							// OnInterrupt

	KIRQL oldirql;
	KeAcquireSpinLock(&pdx->polllock, &oldirql);
	pdx->pollpending = FALSE;		// allow another poll to be started
	KeReleaseSpinLock(&pdx->polllock, oldirql);

	// If the poll completed successfully, do whatever it is we do when we
	// get an interrupt (in this sample, that's answering an IOCTL) and
	// reissue the read. We're trying to have a read outstanding on the
	// interrupt pipe all the time except when power is off.

	if (NT_SUCCESS(Irp->IoStatus.Status))
	{						// device signalled an interrupt
		if (pdx->handles > 0)
		{
			// Queue the work item for PASSIVE_LEVEL processing
			NTSTATUS status = QueueInterrupt(pdx);
			if (!NT_SUCCESS(status))				
				KdPrint((DRIVERNAME " - Error %X trying queue work item\n", status));
		
			//!!! Âלוסעמ גûחמגא ג InterruptCallback
			StartInterruptUrb(pdx);  
		}
		else
			KdPrint((DRIVERNAME " - ERROR: Yet Interrupt!!!\n"));		
	}
	else
		KdPrint((DRIVERNAME " - Interrupt polling IRP %X failed - %X (USBD status %X)\n",
			Irp, Irp->IoStatus.Status, URB_STATUS(pdx->PollingUrb)));
	
	// Balances acquisition in StartInterruptUrb
	IoReleaseRemoveLock(&pdx->RemoveLock, Irp); 
			
	return STATUS_MORE_PROCESSING_REQUIRED;
	}							// OnInterrupt

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID ResetDevice(PDEVICE_OBJECT fdo)
	{							// ResetDevice
	PAGED_CODE();
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;

	KEVENT event;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IO_STATUS_BLOCK iostatus;

	PIRP Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_RESET_PORT,
		pdx->LowerDeviceObject, NULL, 0, NULL, 0, TRUE, &event, &iostatus);
	if (!Irp)
		return;

	NTSTATUS status = IoCallDriver(pdx->LowerDeviceObject, Irp);
	if (status == STATUS_PENDING)
		{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iostatus.Status;
		}

	if (!NT_SUCCESS(status))
		KdPrint((DRIVERNAME " - Error %X trying to reset device\n", status));
	}							// ResetDevice

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS ResetPipe(PDEVICE_OBJECT fdo, USBD_PIPE_HANDLE hpipe)
	{							// ResetPipe
	PAGED_CODE();
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	URB urb;

	urb.UrbHeader.Length = (USHORT) sizeof(_URB_PIPE_REQUEST);
	urb.UrbHeader.Function = URB_FUNCTION_RESET_PIPE;
	urb.UrbPipeRequest.PipeHandle = hpipe;

	NTSTATUS status = SendAwaitUrb(fdo, &urb);
	if (!NT_SUCCESS(status))
		KdPrint((DRIVERNAME " - Error %X trying to reset a pipe\n", status));
	return status;
	}							// ResetPipe

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS SendAwaitUrb(PDEVICE_OBJECT fdo, PURB urb)
	{							// SendAwaitUrb
	PAGED_CODE();
	ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;

	KEVENT event;
	KeInitializeEvent(&event, NotificationEvent, FALSE);

	IO_STATUS_BLOCK iostatus;
	PIRP Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
		pdx->LowerDeviceObject, NULL, 0, NULL, 0, TRUE, &event, &iostatus);

	if (!Irp)
		{
		KdPrint((DRIVERNAME " - Unable to allocate IRP for sending URB\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
		}

	PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);
	stack->Parameters.Others.Argument1 = (PVOID) urb;
	NTSTATUS status = IoCallDriver(pdx->LowerDeviceObject, Irp);
	if (status == STATUS_PENDING)
		{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iostatus.Status;
		}
	return status;
	}							// SendAwaitUrb

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS StartDevice(PDEVICE_OBJECT fdo, PCM_PARTIAL_RESOURCE_LIST raw, PCM_PARTIAL_RESOURCE_LIST translated)
	{							// StartDevice
	PAGED_CODE();
	NTSTATUS status;
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;

	URB urb;					// URB for use in this subroutine

	KdPrint((DRIVERNAME " - Entry StartDevice\n"));

	// Read our device descriptor. The only thing this skeleton does with it is print
	// debugging messages with the string descriptors.

	UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST), USB_DEVICE_DESCRIPTOR_TYPE,
		0, 0, &pdx->dd, NULL, sizeof(pdx->dd), NULL);
	status = SendAwaitUrb(fdo, &urb);
	if (!NT_SUCCESS(status))
		{
		KdPrint((DRIVERNAME " - Error %X trying to read device descriptor\n", status));
		return status;
		}

	MSGUSBSTRING(fdo, DRIVERNAME " - Configuring device from %ws\n", pdx->dd.iManufacturer);
	MSGUSBSTRING(fdo, DRIVERNAME " - Product is %ws\n", pdx->dd.iProduct);
	MSGUSBSTRING(fdo, DRIVERNAME " - Serial number is %ws\n", pdx->dd.iSerialNumber);

	// Read the descriptor of the first configuration. This requires two steps. The first step
	// reads the fixed-size configuration descriptor alone. The second step reads the
	// configuration descriptor plus all imbedded interface and endpoint descriptors.

	USB_CONFIGURATION_DESCRIPTOR tcd;
	UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST), USB_CONFIGURATION_DESCRIPTOR_TYPE,
		0, 0, &tcd, NULL, sizeof(tcd), NULL);
	status = SendAwaitUrb(fdo, &urb);
	if (!NT_SUCCESS(status))
		{
		KdPrint((DRIVERNAME " - Error %X trying to read configuration descriptor 1\n", status));
		return status;
		}

	ULONG size = tcd.wTotalLength;
	PUSB_CONFIGURATION_DESCRIPTOR pcd = (PUSB_CONFIGURATION_DESCRIPTOR) ExAllocatePool(NonPagedPool, size);
	if (!pcd)
		{
		KdPrint((DRIVERNAME " - Unable to allocate %X bytes for configuration descriptor\n", size));
		return STATUS_INSUFFICIENT_RESOURCES;
		}

	__try
		{
		UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST), USB_CONFIGURATION_DESCRIPTOR_TYPE,
			0, 0, pcd, NULL, size, NULL);
		status = SendAwaitUrb(fdo, &urb);
		if (!NT_SUCCESS(status))
			{
			KdPrint((DRIVERNAME " - Error %X trying to read configuration descriptor 1\n", status));
			return status;
			}
    
		KdPrint((DRIVERNAME " - bNumInterfaces = %d\n", pcd->bNumInterfaces));
		MSGUSBSTRING(fdo, DRIVERNAME " - Selecting configuration named %ws\n", pcd->iConfiguration);
                                   
		// Locate the descriptor for the one and only interface we expect to find

		PUSB_INTERFACE_DESCRIPTOR pid = USBD_ParseConfigurationDescriptorEx(pcd, pcd,
			-1, -1, -1, -1, -1);
		ASSERT(pid);
                                                                      
		MSGUSBSTRING(fdo, DRIVERNAME " - Selecting interface named %ws\n", pid->iInterface);

		// Create a URB to use in selecting a configuration.

		USBD_INTERFACE_LIST_ENTRY interfaces[2] = {
			{pid, NULL},
			{NULL, NULL},		// fence to terminate the array
			};

		PURB selurb = USBD_CreateConfigurationRequestEx(pcd, interfaces);
		if (!selurb)
			{
			KdPrint((DRIVERNAME " - Unable to create configuration request\n"));
			return STATUS_INSUFFICIENT_RESOURCES;
			}

		__try
			{

			// Verify that the interface describes exactly the endpoints we expect

			if (pid->bNumEndpoints != 2)
				{
				KdPrint((DRIVERNAME " - %d is the wrong number of endpoints\n", pid->bNumEndpoints));
				return STATUS_DEVICE_CONFIGURATION_ERROR;
				}

			PUSB_ENDPOINT_DESCRIPTOR ped = (PUSB_ENDPOINT_DESCRIPTOR) pid;
			ped = (PUSB_ENDPOINT_DESCRIPTOR) USBD_ParseDescriptors(pcd, tcd.wTotalLength, ped, USB_ENDPOINT_DESCRIPTOR_TYPE);
			
			KdPrint((DRIVERNAME " - Endpoint 1: bEndpointAddress=0x%x, bmAttributes=0x%x, wMaxPacketSize=0x%x\n",
				     ped->bEndpointAddress,ped->bmAttributes,ped->wMaxPacketSize));
			
			if (!ped || ped->bEndpointAddress != 0x1 || ped->bmAttributes != USB_ENDPOINT_TYPE_BULK || ped->wMaxPacketSize != 16)
				{
				KdPrint((DRIVERNAME " - Endpoint has wrong attributes\n"));
				return STATUS_DEVICE_CONFIGURATION_ERROR;
				}

			++ped;
			ped = (PUSB_ENDPOINT_DESCRIPTOR) USBD_ParseDescriptors(pcd, tcd.wTotalLength, ped, USB_ENDPOINT_DESCRIPTOR_TYPE);

			KdPrint((DRIVERNAME " - Endpoint 2: bEndpointAddress=0x%x, bmAttributes=0x%x, wMaxPacketSize=0x%x, bInterval==0x%x\n",
				     ped->bEndpointAddress, ped->bmAttributes, ped->wMaxPacketSize, ped->bInterval));
			
			if (!ped || ped->bEndpointAddress != 0x81 || ped->bmAttributes != USB_ENDPOINT_TYPE_INTERRUPT || ped->wMaxPacketSize != 32)
				{
				KdPrint((DRIVERNAME " - Endpoint has wrong attributes\n"));
				return STATUS_DEVICE_CONFIGURATION_ERROR;
				}
			++ped;

			PUSBD_INTERFACE_INFORMATION pii = interfaces[0].Interface;

			// Initialize the maximum transfer size for each of the endpoints
			// TODO remove these statements if you're happy with the default
			// value provided by USBD.

			pii->Pipes[0].MaximumTransferSize = 32;
			pii->Pipes[1].MaximumTransferSize = 32;

			// Submit the set-configuration request

			status = SendAwaitUrb(fdo, selurb);
			if (!NT_SUCCESS(status))
				{
				KdPrint((DRIVERNAME " - Error %X trying to select configuration\n", status));
				return status;
				}

			// Save the configuration and pipe handles

			pdx->hconfig = selurb->UrbSelectConfiguration.ConfigurationHandle;
			pdx->hcompipe = pii->Pipes[0].PipeHandle;
			pdx->hintpipe = pii->Pipes[1].PipeHandle;

			KdPrint((DRIVERNAME " - hcompipe = 0x%x, hintpipe = 0x%x\n",
				    pdx->hcompipe, pdx->hintpipe));

			// Transfer ownership of the configuration descriptor to the device extension
			
			pdx->pcd = pcd;
			pcd = NULL;
			}
		__finally
			{
			ExFreePool(selurb);
			}

		}
	__finally
		{
		if (pcd)
			ExFreePool(pcd);
		}

	return STATUS_SUCCESS;
	}							// StartDevice

///////////////////////////////////////////////////////////////////////////////

#pragma LOCKEDCODE

NTSTATUS StartInterruptUrb(PDEVICE_EXTENSION pdx)
	{							// StartInterruptUrb

	// If the interrupt polling IRP is currently running, don't try to start
	// it again.

	BOOLEAN startirp;
	KIRQL oldirql;
	KeAcquireSpinLock(&pdx->polllock, &oldirql);
	if (pdx->pollpending)
		startirp = FALSE;
	else
		startirp = TRUE, pdx->pollpending = TRUE;
	KeReleaseSpinLock(&pdx->polllock, oldirql);

	if (!startirp)
		return STATUS_DEVICE_BUSY;	// already pending

	PIRP Irp = pdx->PollingIrp;
	PURB urb = pdx->PollingUrb;
	ASSERT(Irp && urb);

	// Acquire the remove lock so we can't remove the device while the IRP
	// is still active.

	NTSTATUS status = IoAcquireRemoveLock(&pdx->RemoveLock, Irp);
	if (!NT_SUCCESS(status))
		{
		pdx->pollpending = 0;  
		return status;
		}

	// Initialize the URB we use for reading the interrupt pipe
				
	UsbBuildInterruptOrBulkTransferRequest(urb, sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER),
		pdx->hintpipe, &pdx->intdata, NULL, INTERRUPTDATASIZE, USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);

	// Install "OnInterrupt" as the completion routine for the polling IRP.
	
	IoSetCompletionRoutine(Irp, (PIO_COMPLETION_ROUTINE) OnInterrupt, pdx, TRUE, TRUE, TRUE);

	// Initialize the IRP for an internal control request

	PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);
	stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
	stack->Parameters.Others.Argument1 = urb;

	// This IRP might have been cancelled the last time it was used, in which case
	// the cancel flag will still be on. Clear it to prevent USBD from thinking that it's
	// been cancelled again! A better way to do this would be to call IoReuseIrp,
	// but that function is not declared in WDM.H.

	Irp->Cancel = FALSE;

	return IoCallDriver(pdx->LowerDeviceObject, Irp);
	}							// StartInterruptUrb

///////////////////////////////////////////////////////////////////////////////

#pragma LOCKEDCODE

VOID StopInterruptUrb(PDEVICE_EXTENSION pdx)
	{							// StopInterruptUrb
	if (pdx->pollpending)
		IoCancelIrp(pdx->PollingIrp);
	}							// StopInterruptUrb

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID StopDevice(IN PDEVICE_OBJECT fdo, BOOLEAN oktouch /* = FALSE */)
	{							// StopDevice
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;

	KdPrint((DRIVERNAME " - Entry StopDevice\n"));

	// Cancel the interrupt polling URB in case it's currently active

	StopInterruptUrb(pdx);

	// If it's okay to touch our hardware (i.e., we're processing an IRP_MN_STOP_DEVICE),
	// deconfigure the device.
	
	if (oktouch)
		{						// deconfigure device
		URB urb;
		UsbBuildSelectConfigurationRequest(&urb, sizeof(_URB_SELECT_CONFIGURATION), NULL);
		NTSTATUS status = SendAwaitUrb(fdo, &urb);
		if (!NT_SUCCESS(status))
			KdPrint((DRIVERNAME " - Error %X trying to deconfigure device\n", status));
		}						// deconfigure device

	if (pdx->pcd)
		ExFreePool(pdx->pcd);
	pdx->pcd = NULL;
	}							// StopDevice

///////////////////////////////////////////////////////////////////////////////
