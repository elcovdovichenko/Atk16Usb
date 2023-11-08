// Control.cpp -- IOCTL handlers for atk16u driver
// Copyright (C) 2002 by Vyacheslav Vdovichenko
// All rights reserved

#include "stddcls.h"
#include "driver.h"
#include "ioctls.h"


///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID PucharDebugView(PUCHAR pw, ULONG size)				
	{	
	KdPrint((DRIVERNAME " - PucharDebugVieW = "));	
	for (ULONG n = 0; n < size; n++)
		{
		KdPrint((" %x", (UCHAR)*pw));
		++pw;
		}
	KdPrint(("\n"));
	}

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

ULONG PucharToUlong(PUCHAR pw, ULONG size)				
	{
	ULONG i;
	ULONG d = 0; 
	for (i = 0; i < size; i++)
		{
		d += (*pw << (8*i)); 
		++pw; 
		}	
	return d;
	}

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

VOID UlongToPuchar(PUCHAR pw, ULONG data)				
	{
	PUCHAR pd = (PUCHAR)&data;
	for (UCHAR i = 0; i < 4; i++)
	{
		*pw = *pd; 
		++pw;
		++pd;
	}
	}

///////////////////////////////////////////////////////////////////////////////

#pragma PAGEDCODE

NTSTATUS DispatchControl(PDEVICE_OBJECT fdo, PIRP Irp)
	{							// DispatchControl
	PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION) fdo->DeviceExtension;

	NTSTATUS status = IoAcquireRemoveLock(&pdx->RemoveLock, Irp);
	if (!NT_SUCCESS(status))
		return CompleteRequest(Irp, status, 0);

	status = STATUS_SUCCESS;
	ULONG info = 0;

	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	ULONG cbin = stack->Parameters.DeviceIoControl.InputBufferLength;
	ULONG cbout = stack->Parameters.DeviceIoControl.OutputBufferLength;
	ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;

	switch (code)
		{						// process request

	case IOCTL_SET_COMMAND:				// code == 0x800
		{						// IOCTL_SET_COMMAND

		//KdPrint((DRIVERNAME " - IOCTL_SET_COMMAND = 0x%x\n", code));
			
		if (cbin == 0 )
		{
			KdPrint((DRIVERNAME " - Lenght command = %d is wrong\n", cbin));
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		ULONG size = cbin;		
		PUCHAR pw = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
		UCHAR action = *pw;
		++pw; 
		size--;
				
		//KdPrint((DRIVERNAME " - IOCTL_SET_COMMAND action = %d\n", action));

		// CLP actions
		switch (action)
			{						// process action
						
		case GET_DESCRIPTOR:		// code == 0
			{						// GET_DESCRIPTOR
			
			//KdPrint((DRIVERNAME " - GET_DESCRIPTOR = %d\n", action));

			UNICODE_STRING sd; 
			if ((pdx->dd.iSerialNumber) 
				&& NT_SUCCESS(GetStringDescriptor(fdo,pdx->dd.iSerialNumber,&sd))) 
			{ 
				ANSI_STRING sa;
				if NT_SUCCESS(RtlUnicodeStringToAnsiString(&sa, &sd, TRUE))
				{
					KdPrint((DRIVERNAME " - Serial number (ANSI_STRING) is '%s' length = %d\n", sa.Buffer, sa.Length));
	
					if (cbout < sa.Length)
					{
						KdPrint((DRIVERNAME " - Length buffer %d is too small for description (length = %d)\n", cbout, sd.Length));
						status = STATUS_INVALID_PARAMETER;
					}
					else
					{						
						PUCHAR pd = (PUCHAR)sa.Buffer;
						//PucharDebugView(pd, sa.Length);
						--pw;
						for (ULONG i = 0; i < sa.Length; i++)
						{
							*pw = *pd; 
							++pw; 
							++pd;
						}

						info = sa.Length;

					}

					RtlFreeAnsiString(&sa);
				}
				
				RtlFreeUnicodeString(&sd); 
			}
			
			break;
			}						// GET_DESCRIPTOR

		case SEND_COMMAND:				// code == 1
			{						// SEND_COMMAND
			
			//KdPrint((DRIVERNAME " - SEND_COMMAND = %d\n", action));
			
			if (size != COMMANDDATASIZE)
			{
				KdPrint((DRIVERNAME " - Length bulk transaction %d is wrong\n", size));
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			
			status = SendBulkUrb(pdx, pw);
			
			break;
			}						// SEND_COMMAND

		case SEND_BUFFER:			// code == 2
			{						// SEND_BUFFER

			//KdPrint((DRIVERNAME " - SEND_BUFFER = %d\n", action));
		
			if (size == 0)
			{
				KdPrint((DRIVERNAME " - Length SEND_BUFFER transaction = %d is wrong\n", size));
				info = ecWrongParam;
				break;
			}
			
			// for which point?
			UCHAR point = *pw; 
			++pw;
			
			if (point >= NUMCHANNELS)
			{
				KdPrint((DRIVERNAME " - Point number %d is wrong\n", point));
				info = ecWrongNum;
				break;
			}

			size--;
			if (size)
			{
				if (pdx->buffer[point])
					info = ecOverlapBuf;
				else
				{
					pdx->buffer[point] = (PUCHAR)ExAllocatePool(PagedPool, size);
					if (pdx->buffer[point])
					{
						//KdPrint((DRIVERNAME " - Buffer of size = %X is allocated\n", size));
					
						PUCHAR pt = pdx->buffer[point];							
						ULONG i;
						for (i = 0; i < size; i++)
						{
							*pt = *pw; 
							++pw; 
							++pt;
						}

						pdx->bufferfill[point] = size;
						pdx->buffercount[point] = 0;				
						pdx->intstate[point] = 0;					
					}
					else
						KdPrint((DRIVERNAME " - Buffer of size = %X isn't allocated\n", size));
				}					
			}
			else
			{
				if (pdx->buffer[point])
				{
					pdx->intstate[point] = 0x60;					
					ExFreePool((PVOID)pdx->buffer[point]);
					pdx->buffer[point] = NULL;
					pdx->bufferfill[point] = 0;
					pdx->buffercount[point] = 0;
				}				
			}

			break;
			}						// SEND_BUFFER
						
		case SEND_PULSE:			// code == 3
			{						// SEND_PULSE
			
			//KdPrint((DRIVERNAME " - SEND_PULSE = %d\n", action));
							
			if (size != 5)
			{
				KdPrint((DRIVERNAME " - Length SEND_PULSE parameter of KEEP_LINE transaction = %d is wrong\n", size));
				info = ecWrongParam;
				break;
			}
			
			// for which point?
			UCHAR point = *pw; 
			++pw;
			
			if (point >= NUMCHANNELS)
			{
				KdPrint((DRIVERNAME " - Point number %d is wrong\n", point));
				info = ecWrongNum;
				break;
			}

			ULONG duration = PucharToUlong(pw,size);
			pdx->pulseissuetime[point] = (TickCountMs() + duration);
			
			if (duration > 635)
			{
				duration = 1;
				pdx->pulseissuekind[point] = 1;
			}
			else
			{
				duration = ((2*(duration+1))/5);			
				pdx->pulseissuekind[point] = 0;
			}
						
			*pw = (UCHAR)duration;
			--pw;
			*pw |= ISSUE_PULSE;

			status = SendBulkUrb(pdx, pw);

			break;
			}						// SEND_PULSE
						
		case CURRENT_LINE:			// code == 4
			{						// CURRENT_LINE
			
			//KdPrint((DRIVERNAME " - CURRENT_LINE = %d\n", action));
			
			if ((cbout < 5) || (size == 0))
			{
				KdPrint((DRIVERNAME " - Length CURRENT_LINE transaction %d is wrong\n", size));
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			
			// for which point?
			UCHAR point = *pw; 
			--pw;
			
			ULONG time = (TickCountMs() - pdx->linechangetime[point]);
			
			*pw = pdx->curpolarity[point];
			++pw;
			PUCHAR pd = (PUCHAR)&time;
			for (ULONG i = 0; i < 4; i++)
			{
				*pw = *pd; 
				++pw; 
				++pd;
			}

			info = 5;
			
			break;
			}						// CURRENT_LINE

		case KEEP_LINE: 			// code == 5
			{						// KEEP_LINE

			//KdPrint((DRIVERNAME " - KEEP_LINE = %d\n", action));
		
			if (size < 2)
			{
				KdPrint((DRIVERNAME " - Length KEEP_LINE transaction = %d is wrong\n", size));
				info = ecWrongParam;
				break;
			}
			
			// for which point?
			UCHAR point = *pw; 
			++pw;
			size--;
			
			if (point >= NUMCHANNELS)
			{
				KdPrint((DRIVERNAME " - Point number %d is wrong\n", point));
				info = ecWrongNum;
				break;
			}

			UCHAR kind = *pw;
			++pw; 
			size--;

			//KdPrint((DRIVERNAME " - KEEP_LINE kind = 0x%x\n", kind));

			// CLP(KEEP_LINE) kinds of action 
			switch (kind >> 4)
				{						// process kind

			case LINE_NOTIFICATION_OFF:		// notificatin off
				{
				
				pdx->notification[point] &= ~(kind & 0x0F);
				
				KdPrint((DRIVERNAME " - LINE_NOTIFICATION_OFF on %d now is 0x%x\n", point, pdx->notification[point]));

				break;
				}						// notificatin off

			case LINE_NOTIFICATION_ON:		// notificatin on
				{
				
				pdx->notification[point] |= (kind & 0x0F);

				KdPrint((DRIVERNAME " - LINE_NOTIFICATION_ON on %d now is 0x%x\n", point, pdx->notification[point]));
				
				break;
				}						// notificatin on

			case LINE_KEEPING:			// line keeping
				{
				
				if (size != 4)
				{
					KdPrint((DRIVERNAME " - Length LINE_KEEPING parameter of KEEP_LINE transaction = %d is wrong\n", size));
					info = ecWrongParam;
					break;
				}

				pdx->awaitlinepolarity[point] = (kind & 0x03);
				pdx->awaitlineduration[point] = PucharToUlong(pw,size);
				pdx->polarityisnow[point] = 0;

				KdPrint((DRIVERNAME " - LINE_KEEPING on %d is 0x%x (%d)\n", point, pdx->awaitlinepolarity[point], pdx->awaitlineduration[point]));

				break;
				}						// line keeping

			case PULSE_KEEPING:			// pulse keeping
				{
				
				if (size != 8)
				{
					KdPrint((DRIVERNAME " - Length PULSE_KEEPING parameter of KEEP_LINE transaction = %d is wrong\n", size));
					info = ecWrongParam;
					break;
				}
							
				pdx->awaitpulsepolarity[point] = (kind & 0x03);
				pdx->awaitpulsemin[point] = PucharToUlong(pw,4);
				pw += 4;
				pdx->awaitpulsemax[point] = PucharToUlong(pw,4);

				KdPrint((DRIVERNAME " - PULSE_KEEPING on %d is 0x%x (%d->%d)\n", point, pdx->awaitpulsepolarity[point], pdx->awaitpulsemin[point], pdx->awaitpulsemax[point]));

				break;
				}						// pulse keeping

			case DAMAGE_KEEPING:			// damage keeping
				{
				
				if (size != 8)
				{
					KdPrint((DRIVERNAME " - Length DAMAGE_KEEPING parameter of KEEP_LINE transaction = %d is wrong\n", size));
					info = ecWrongParam;
					break;
				}

				pdx->damageonduration[point] = PucharToUlong(pw,4);
				pw += 4;
				pdx->damageoffduration[point] = PucharToUlong(pw,4);
				pdx->damageisnow[point] = 0;
				
				KdPrint((DRIVERNAME " - DAMAGE_KEEPING on %d is %d (ON) & %d (OFF)\n", point, pdx->damageonduration[point], pdx->damageoffduration[point]));

				break;
				}						// damage keeping

				}						// process kind

			break;
			}						// KEEP_LINE
						
		case CONTROL_RESOURCE:		// code == 6
			{						// CONTROL_RESOURCE
			
			//KdPrint((DRIVERNAME " - CONTROL_RESOURCE = %d\n", action));
			
			if (size == 0)
			{
				KdPrint((DRIVERNAME " - Length CONTROL_RESOURCE transaction %d is wrong\n", size));
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			--pw;
			*pw = CONTROL_COMMON;

			status = SendBulkUrb(pdx, pw);
			
			break;
			}						// CONTROL_RESOURCE

			}						// process action
		
		break;
		}						// IOCTL_SET_COMMAND

	case EVENT_GET_INTERRUPT:				// code == 0x801
		{						// EVENT_GET_INTERRUPT

		//KdPrint((DRIVERNAME " - EVENT_GET_INTERRUPT = 0x%x\n", code));
		
		if (cbout != EXCHANGEDATASIZE)
		{
			KdPrint((DRIVERNAME " - Length interrupt transaction %d is wrong\n", cbout));
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// Use up a pending interrupt first. We increment numints each time we
		// get an interrupt, and we decrement it each time we complete an IOCTL.

		if (InterlockedIncrement(&pdx->numints) > 1)
		{					// pending interrupt
			KdPrint((DRIVERNAME " - EVENT_GET_INTERRUPT yet in process!\n"));
			InterlockedDecrement(&pdx->numints); // restore counter
			status = STATUS_SUCCESS;
			break;
		}					// pending interrupt
			
		// Pend this IOCTL until an interrupt occurs

		status = GenericCacheControlRequest(pdx->pgx, Irp, &pdx->InterruptIrp);

		break;
		}						// EVENT_GET_INTERRUPT

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;

		}						// process request

	IoReleaseRemoveLock(&pdx->RemoveLock, Irp);
	return status == STATUS_PENDING ? status : CompleteRequest(Irp, status, info);
	}							// DispatchControl

