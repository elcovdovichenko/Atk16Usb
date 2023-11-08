// Declarations for atk16u driver
// Copyright (C) 2002 by Vyacheslav Vdovichenko
// All rights reserved

#ifndef DRIVER_H
#define DRIVER_H
#include "generic.h"

// COMMON Constants
#define DRIVERNAME "ATK16U"				// for use in messages
#define LDRIVERNAME L"ATK16U"			// for use in UNICODE string constants
#define COMMANDDATASIZE 2				// command pipe data size
#define INTERRUPTDATASIZE 32			// interrupt pipe data size
#define EXCHANGEDATASIZE 4096			// exchange data size
#define NUMCHANNELS 16					// number of channels

// CCP extended codes
#define ISSUE_PULSE 0x80				//
#define CONTROL_COMMON 0xE0				//

// CLP command codes 
#define GET_DESCRIPTOR 0 				//
#define SEND_COMMAND 1					//
#define SEND_BUFFER 2					//
#define SEND_PULSE 3					//
#define CURRENT_LINE 4 					//
#define KEEP_LINE 5 					//
#define CONTROL_RESOURCE 6				//

// CLP(KEEP_LINE) kinds of action
#define LINE_NOTIFICATION_OFF 0			//
#define LINE_NOTIFICATION_ON 1			//
#define LINE_KEEPING 2					//
#define PULSE_KEEPING 3					//
#define DAMAGE_KEEPING 4				//

// CLP general events
#define OUTPUT_SYMBOL 0x00	            //
#define INPUT_SYMBOL 0x40	            //
#define CHANNEL_EVENT 0x80              //
#define COMMON_EVENT 0xC0               //

// CLP channel events
#define OUTPUT_DATA 0x00			    //
#define LINE_CHANGE 0x14	            //
#define LINE_IN 0x24	                //
#define PULSE_IN 0x34	                //
#define DAMAGE_STATE 0x44               //
#define PULSE_OUT 0x50	                //

// CLP common events
#define SURPRISE_REMOVAL 0x10           //

// CLP notification constants
#define LINE_STOP 0x01                  //
#define LINE_START 0x02                 //
#define LINE_DAMAGE 0x03                //
#define SYMBOL_OUT 0x04                 //
#define SYMBOL_IN 0x08                  //
#define LINE_INVALID 0x00               //
#define LINE_INVERSE 0x03               //

// IOCTL complete codes
#define ecOK 0				            //
#define ecNoFunc 1						//
#define ecWrongNum 2			        // 
#define ecNoCtlNum 3			        // 
#define ecNoInitNum 4			        // 
#define ecWrongParam 5			        //
#define ecAbsentBuf 6			        // 
#define ecOverlapBuf 7			        // 
#define ecWrongMode 8			        // 

///////////////////////////////////////////////////////////////////////////////
// Device extension structure

typedef struct _DEVICE_EXTENSION {
	// Common driver declarations
	PDEVICE_OBJECT DeviceObject;			// device object this extension belongs to
	PDEVICE_OBJECT LowerDeviceObject;		// next lower driver in same stack
	PDEVICE_OBJECT Pdo;						// the PDO
	IO_REMOVE_LOCK RemoveLock;				// removal control locking structure
	UNICODE_STRING ifname;					// interface name
	PGENERIC_EXTENSION pgx;					// device extension for GENERIC.SYS
	LONG handles;							// # open handles
	USB_DEVICE_DESCRIPTOR dd;				// device descriptor
	USBD_CONFIGURATION_HANDLE hconfig;		// selected configuration handle
	PUSB_CONFIGURATION_DESCRIPTOR pcd;		// configuration descriptor
	LANGID langid;							// default language id for strings
	KSPIN_LOCK polllock;					// lock for managing polling IRP
	BOOLEAN pollpending;					// polling irp is pending
	// Additional (per-device) driver declarations
	USBD_PIPE_HANDLE hcompipe;				// USB bulk pipe
	USBD_PIPE_HANDLE hintpipe;				// USB interrupt pipe
	PIRP InterruptIrp;						// the IOCTL that's waiting for an interrupt
	PIRP PollingIrp;						// IRP used to poll for interrupts
	PURB PollingUrb;						// URB used to poll for interrupts
	UCHAR intdata[INTERRUPTDATASIZE];		// interrupt data
	UCHAR extdata[EXCHANGEDATASIZE];		// exchange data
	ULONG filling;                          // filling exchange data
	LONG numints;							// number of pending interrupts
	PUCHAR buffer[NUMCHANNELS];				// channel output buffer
	ULONG bufferfill[NUMCHANNELS];          // channel buffer filling
	ULONG buffercount[NUMCHANNELS];         // channel buffer processing
	ULONG linechangetime[NUMCHANNELS];		// channel line change time
	UCHAR intstate[NUMCHANNELS];			// channel integrated output state
	UCHAR curoutsymbol[NUMCHANNELS];		// channel current output symbol	
	UCHAR curoutstate[NUMCHANNELS];			// channel current output state	
	UCHAR curpolarity[NUMCHANNELS];	    	// channel current polarity
	UCHAR notification[NUMCHANNELS];		// channel notification mode
	UCHAR damageisnow[NUMCHANNELS];         // channel damage is it
	UCHAR polarityisnow[NUMCHANNELS];       // channel await polarity is it
	UCHAR awaitlinepolarity[NUMCHANNELS];	// channel await line polarity
	ULONG awaitlineduration[NUMCHANNELS];	// channel await line duration
	ULONG pulseduration[NUMCHANNELS];	    // channel last pulse duration
	UCHAR pulsepolarity[NUMCHANNELS];	    // channel last pulse polarity
	UCHAR awaitpulsepolarity[NUMCHANNELS];	// channel await pulse polarity
	ULONG awaitpulsemin[NUMCHANNELS];	    // channel await pulse min duration
	ULONG awaitpulsemax[NUMCHANNELS];	    // channel await pulse max duration
	ULONG damageonduration[NUMCHANNELS];    // channel damage ON duration
	ULONG damageoffduration[NUMCHANNELS];   // channel damage OFF duration
	ULONG pulseissuetime[NUMCHANNELS];	    // channel pulse issue time
	UCHAR pulseissuekind[NUMCHANNELS];	    // channel pulse issue kind
	UCHAR pulsecounter[NUMCHANNELS];	    // channel last pulse counter
	} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

///////////////////////////////////////////////////////////////////////////////
// Global functions

VOID RemoveDevice(IN PDEVICE_OBJECT fdo);
NTSTATUS CompleteRequest(IN PIRP Irp, IN NTSTATUS status, IN ULONG_PTR info);
NTSTATUS CreateInterruptUrb(PDEVICE_OBJECT fdo);
VOID DeleteInterruptUrb(PDEVICE_OBJECT fdo);
NTSTATUS SendAwaitUrb(PDEVICE_OBJECT fdo, PURB urb);
VOID AbortPipe(PDEVICE_OBJECT fdo, USBD_PIPE_HANDLE hpipe);
NTSTATUS ResetPipe(PDEVICE_OBJECT fdo, USBD_PIPE_HANDLE hpipe);
VOID ResetDevice(PDEVICE_OBJECT fdo);
NTSTATUS StartDevice(PDEVICE_OBJECT fdo, PCM_PARTIAL_RESOURCE_LIST raw, PCM_PARTIAL_RESOURCE_LIST translated);
VOID StopDevice(PDEVICE_OBJECT fdo, BOOLEAN oktouch = FALSE);
NTSTATUS SendBulkUrb(PDEVICE_EXTENSION pdx, PUCHAR data);
ULONG TickCountMs(VOID);
NTSTATUS GetStringDescriptor(PDEVICE_OBJECT fdo, UCHAR istring, PUNICODE_STRING s);

// I/O request handlers

NTSTATUS DispatchCreate(PDEVICE_OBJECT fdo, PIRP Irp);
NTSTATUS DispatchClose(PDEVICE_OBJECT fdo, PIRP Irp);
NTSTATUS DispatchControl(PDEVICE_OBJECT fdo, PIRP Irp);
NTSTATUS DispatchInternalControl(PDEVICE_OBJECT fdo, PIRP Irp);
NTSTATUS DispatchCleanup(PDEVICE_OBJECT fdo, PIRP Irp);
NTSTATUS DispatchPower(PDEVICE_OBJECT fdo, PIRP Irp);
NTSTATUS DispatchPnp(PDEVICE_OBJECT fdo, PIRP Irp);

extern BOOLEAN win98;
extern UNICODE_STRING servkey;

#endif // DRIVER_H
