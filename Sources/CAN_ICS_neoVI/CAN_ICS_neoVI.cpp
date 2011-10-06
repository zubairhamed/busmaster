/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file      CAN_ICS_neoVI.cpp
 * \brief     Exports API functions for IntrepidCS neoVI CAN Hardware interface
 * \author    Pradeep Kadoor
 * \copyright Copyright (c) 2011, Robert Bosch Engineering and Business Solutions. All rights reserved.
 *
 * Exports API functions for IntrepidCS neoVI CAN Hardware interface
 */
#include "CAN_ICS_neoVI_stdafx.h"
#include "CAN_ICS_neoVI.h"
#include "include/Error.h"
#include "include/basedefs.h"
#include "DataTypes/Base_WrapperErrorLogger.h"
#include "DataTypes/MsgBufAll_DataTypes.h"
#include "DataTypes/DIL_Datatypes.h"
#include "Include/CAN_Error_Defs.h"
#include "Include/CanUsbDefs.h"
#include "Include/Struct_CAN.h"
#include "CAN_ICS_neoVI_Channel.h"
#include "Utility/Utility_Thread.h"
#include "Include/DIL_CommonDefs.h"
#include "ConfigDialogsDIL/API_dialog.h"
#include "EXTERNAL_INCLUDE/icsnVC40.h"


#define USAGE_EXPORT
#include "CAN_ICS_neoVI_Extern.h"


#ifdef _DEBUG
#define new DEBUG_NEW
#endif

//
//	Note!
//
//		If this DLL is dynamically linked against the MFC
//		DLLs, any functions exported from this DLL which
//		call into MFC must have the AFX_MANAGE_STATE macro
//		added at the very beginning of the function.
//
//		For example:
//
//		extern "C" BOOL PASCAL EXPORT ExportedFunction()
//		{
//			AFX_MANAGE_STATE(AfxGetStaticModuleState());
//			// normal function body here
//		}
//
//		It is very important that this macro appear in each
//		function, prior to any calls into MFC.  This means that
//		it must appear as the first statement within the 
//		function, even before any object variable declarations
//		as their constructors may generate calls into the MFC
//		DLL.
//
//		Please see MFC Technical Notes 33 and 58 for additional
//		details.
//

// CCAN_ICS_neoVIApp

BEGIN_MESSAGE_MAP(CCAN_ICS_neoVIApp, CWinApp)
END_MESSAGE_MAP()


/**
 * CCAN_ICS_neoVIApp construction
 */
CCAN_ICS_neoVIApp::CCAN_ICS_neoVIApp()
{
	// TODO: add construction code here,
	// Place all significant initialization in InitInstance
}


/**
 * The one and only CCAN_ICS_neoVIApp object
 */
CCAN_ICS_neoVIApp theApp;


/**
 * CCAN_ICS_neoVIApp initialization
 */
BOOL CCAN_ICS_neoVIApp::InitInstance()
{
	CWinApp::InitInstance();

	return TRUE;
}

#define MAX_BUFF_ALLOWED 16

const int CONFIGBYTES_TOTAL = 1024;
const BYTE CREATE_MAP_TIMESTAMP = 0x1;
const BYTE CALC_TIMESTAMP_READY = 0x2;

/**
 * Current state machine
 */
static BYTE sg_byCurrState = CREATE_MAP_TIMESTAMP;

/**
 * Client and Client Buffer map
 */
struct tagClientBufMap
{
    DWORD dwClientID;
    BYTE hClientHandle;
    CBaseCANBufFSE* pClientBuf[MAX_BUFF_ALLOWED];
    TCHAR pacClientName[MAX_PATH];
    UINT unBufCount;
    tagClientBufMap()
    {
        dwClientID = 0;
        hClientHandle = NULL;
        unBufCount = 0;
        memset(pacClientName, 0, sizeof (TCHAR) * MAX_PATH);
        for (int i = 0; i < MAX_BUFF_ALLOWED; i++)
        {
            pClientBuf[i] = NULL;
        }

    }
};
typedef tagClientBufMap sClientBufMap;
typedef sClientBufMap SCLIENTBUFMAP;
typedef sClientBufMap* PSCLIENTBUFMAP;

typedef struct tagAckMap
{
    UINT m_MsgID;
    UINT m_ClientID;
    UINT m_Channel;

    BOOL operator == (const tagAckMap& RefObj)const
    {        
        return ((m_MsgID == RefObj.m_MsgID) && (m_Channel == RefObj.m_Channel)); 
    }
}SACK_MAP;

typedef std::list<SACK_MAP> CACK_MAP_LIST;
static CACK_MAP_LIST sg_asAckMapBuf;

/**
 * Starts code for the state machine
 */
enum
{
    STATE_DRIVER_SELECTED    = 0x0,
    STATE_HW_INTERFACE_LISTED,
    STATE_HW_INTERFACE_SELECTED,
    STATE_CONNECTED
};
BYTE sg_bCurrState = STATE_DRIVER_SELECTED;

//#define VALIDATE_VALUE_RETURN_VAL(Val1, Val2, RetVal)   if (Val1 != Val2) {return RetVal;}

static SYSTEMTIME sg_CurrSysTime;
static UINT64 sg_TimeStamp = 0;

/**
 * Query Tick Count
 */
static LARGE_INTEGER sg_QueryTickCount;
static HWND sg_hOwnerWnd = NULL;

//static CBaseCANBufFSE* sg_pCanBufObj[MAX_BUFF_ALLOWED];
static Base_WrapperErrorLogger* sg_pIlog   = NULL;

static CPARAM_THREADPROC sg_sParmRThread;
static s_STATUSMSG sg_sCurrStatus;
#define MAX_CANHW 16
//static BYTE sg_hHardware[ MAX_CANHW ];
// The message buffer
const int ENTRIES_IN_GBUF       = 2000;
static int sg_nFRAMES = 128;
static STCANDATA sg_asMsgBuffer[ENTRIES_IN_GBUF];
static SCONTROLER_DETAILS sg_ControllerDetails[defNO_OF_CHANNELS];
static INTERFACE_HW sg_HardwareIntr[defNO_OF_CHANNELS];

/** Number of Channels or hardware actually present */
UINT m_nNoOfChannels = 0;

/**
 * Array of channel objects. The size of this array is maximun number
 * of channels the application will support.
 */
CChannel m_aodChannels[ defNO_OF_CHANNELS ];

/**
 * Create time struct. Use 0 to transmit the message with out any delay
 */
typedef struct
{
    DWORD millis;          /*< base-value: milliseconds: 0.. 2^32-1 */
    WORD  millis_overflow; /*< roll-arounds of millis */
    WORD  micros;          /*< microseconds: 0..999 */
} TCANTimestamp;

static TCANTimestamp sg_sTime;
// static global variables ends

const int SIZE_WORD     = sizeof(WORD);
const int SIZE_CHAR     = sizeof(TCHAR);

// TZM specific Global variables
#define CAN_MAX_ERRSTR 256
#define MAX_CLIENT_ALLOWED 16
static char sg_acErrStr[CAN_MAX_ERRSTR] = {'\0'};
static UINT sg_unClientCnt = 0;
static SCLIENTBUFMAP sg_asClientToBufMap[MAX_CLIENT_ALLOWED];
static UINT sg_unCntrlrInitialised = 0;
static HMODULE sg_hDll = NULL;
static HANDLE m_hDataEvent = NULL;
static HANDLE sg_hCntrlStateChangeEvent = NULL;
static DWORD  sg_dwClientID = 0;

// Current buffer size
//static UINT sg_unMsgBufCount = 0;

// state variables
static BOOL sg_bIsConnected = FALSE;
static BOOL sg_bIsDriverRunning = FALSE;
static UCHAR sg_ucControllerMode = defUSB_MODE_ACTIVE;

// Count variables
static UCHAR sg_ucNoOfHardware = 0;

/*Please recheck and retain only necessary variables*/


#define TOTAL_ERROR             600
#define MAX_BUFFER_VALUECAN     20000
#define WAITTIME_NEOVI          100

const int NEOVI_OK = 1;
const long VALUECAN_ERROR_BITS = SPY_STATUS_GLOBAL_ERR | SPY_STATUS_CRC_ERROR |
                       SPY_STATUS_INCOMPLETE_FRAME | SPY_STATUS_UNDEFINED_ERROR
                       | SPY_STATUS_BAD_MESSAGE_BIT_TIME_ERROR;

static UCHAR m_unWarningLimit[defNO_OF_CHANNELS] = {0};
static UCHAR m_unWarningCount[defNO_OF_CHANNELS] = {0};
static UCHAR m_unRxError = 0;
static UCHAR m_unTxError = 0;
const int MAX_DEVICES  = 255;
static int m_anDeviceTypes[MAX_DEVICES] = {0};
static int m_anComPorts[MAX_DEVICES] = {0};
static int m_anSerialNumbers[MAX_DEVICES] = {0};
static unsigned char m_ucNetworkID[16] = {0};
static int m_anhObject[MAX_DEVICES] = {0};
static TCHAR m_omErrStr[MAX_STRING] = {0};
static BOOL m_bInSimMode = FALSE;
//static CWinThread* m_pomDatInd = NULL;
static int s_anErrorCodes[TOTAL_ERROR] = {0};
/* Recheck ends */
/* Error Definitions */
#define CAN_USB_OK 0
#define CAN_QRCV_EMPTY 0x20

/* icsneo40.dll Basic Functions */
typedef int  (__stdcall *FINDNEODEVICES)(unsigned long DeviceTypes, NeoDevice *pNeoDevice, int *pNumDevices);
static FINDNEODEVICES icsneoFindNeoDevices;
typedef int  (__stdcall *OPENNEODEVICE)(NeoDevice *pNeoDevice, int *hObject, unsigned char *bNetworkIDs, int bConfigRead, int bSyncToPC);
static OPENNEODEVICE icsneoOpenNeoDevice;
typedef int  (__stdcall *CLOSEPORT)(int hObject, int *pNumberOfErrors); 
static CLOSEPORT icsneoClosePort;
typedef void (__stdcall *FREEOBJECT)(int hObject);
static FREEOBJECT icsneoFreeObject;
typedef int  (__stdcall *OPENPORTEX)(int lPortNumber, int lPortType, int lDriverType, int lIPAddressMSB, int lIPAddressLSBOrBaudRate, 
							         int bConfigRead, unsigned char *bNetworkID, int * hObject);

/* icsneo40.dll Message Functions */
typedef int  (__stdcall *GETMESSAGES)(int hObject, icsSpyMessage *pMsg, int * pNumberOfMessages, int * pNumberOfErrors); 
static GETMESSAGES icsneoGetMessages;
typedef int  (__stdcall *TXMESSAGES)(int hObject, icsSpyMessage *pMsg, int lNetworkID, int lNumMessages); 
static TXMESSAGES icsneoTxMessages;
typedef int  (__stdcall *WAITFORRXMSGS)(int hObject, unsigned int iTimeOut);
static WAITFORRXMSGS icsneoWaitForRxMessagesWithTimeOut;
typedef int  (__stdcall *ENABLERXQUEUE)(int hObject, int iEnable);
static ENABLERXQUEUE icsneoEnableNetworkRXQueue;
typedef int  (__stdcall *GETTSFORMSG)(int hObject, icsSpyMessage *pMsg, double *pTimeStamp);
static GETTSFORMSG icsneoGetTimeStampForMsg;

/* icsneo40.dll Device Functions */
typedef int (__stdcall *GETCONFIG)(int hObject, unsigned char * pData, int * lNumBytes);
static GETCONFIG icsneoGetConfiguration;
typedef int (__stdcall *SENDCONFIG)(int hObject, unsigned char * pData, int lNumBytes); 
static SENDCONFIG icsneoSendConfiguration;
typedef int (__stdcall *GETFIRESETTINGS)(int hObject, SFireSettings *pSettings, int iNumBytes);
static GETFIRESETTINGS icsneoGetFireSettings;
typedef int (__stdcall *SETFIRESETTINGS)(int hObject, SFireSettings *pSettings, int iNumBytes, int bSaveToEEPROM);
static SETFIRESETTINGS icsneoSetFireSettings;
typedef int (__stdcall *GETVCAN3SETTINGS)(int hObject, SVCAN3Settings *pSettings, int iNumBytes);
static GETVCAN3SETTINGS icsneoGetVCAN3Settings;
typedef int (__stdcall *SETVCAN3SETTINGS)(int hObject, SVCAN3Settings *pSettings, int iNumBytes, int bSaveToEEPROM);
static SETVCAN3SETTINGS icsneoSetVCAN3Settings;
typedef int (__stdcall *SETBITRATE)(int hObject, int BitRate, int NetworkID);
static SETBITRATE icsneoSetBitRate;
typedef int (__stdcall *GETDEVICEPARMS)(int hObject, char *pParameter, char *pValues, short ValuesLength);
static GETDEVICEPARMS icsneoGetDeviceParameters;
typedef int (__stdcall *SETDEVICEPARMS)(int hObject, char *pParmValue, int *pErrorIndex, int bSaveToEEPROM);
static SETDEVICEPARMS icsneoSetDeviceParameters;

/* icsneo40.dll Error Functions */
typedef int (__stdcall *GETLASTAPIERROR)(int hObject, unsigned long *pErrorNumber);
static GETLASTAPIERROR icsneoGetLastAPIError;
typedef int (__stdcall *GETERRMSGS)(int hObject, int * pErrorMsgs, int * pNumberOfErrors);
static GETERRMSGS icsneoGetErrorMessages;
typedef int (__stdcall *GETERRORINFO)(int lErrorNumber, TCHAR *szErrorDescriptionShort, 
										TCHAR *szErrorDescriptionLong, int * lMaxLengthShort,
                                        int * lMaxLengthLong,int * lErrorSeverity,int * lRestartNeeded);
static GETERRORINFO icsneoGetErrorInfo;

/* icsneo40.dll General Utility Functions */
typedef int (__stdcall *VALIDATEHOBJECT)(int hObject);
static VALIDATEHOBJECT icsneoValidateHObject;
typedef int (__stdcall *GETDLLVERSION)(void);
static GETDLLVERSION icsneoGetDLLVersion;
typedef int (__stdcall *GETSERIALNUMBER)(int hObject, unsigned int *iSerialNumber);
static GETSERIALNUMBER icsneoGetSerialNumber;
typedef int (__stdcall *STARTSOCKSERVER)(int hObject, int iPort);
static STARTSOCKSERVER icsneoStartSockServer;
typedef int (__stdcall *STOPSOCKSERVER)(int hObject);
static STOPSOCKSERVER icsneoStopSockServer;

/* icsneo40.dll Deprecated (but still suppored in the DLL) */
typedef int  (__stdcall *OPENPORTEX)(int lPortSerialNumber, int lPortType, int lDriverType, 
					                 int lIPAddressMSB, int lIPAddressLSBOrBaudRate,int bConfigRead, 
				                     unsigned char * bNetworkID, int * hObject);
static OPENPORTEX icsneoOpenPortEx;
typedef int (__stdcall *ENABLENETWORKCOM)(int hObject, int Enable);
static ENABLENETWORKCOM icsneoEnableNetworkCom;

/* The following are valid strings for setting parameters on devices 
 * using the icsneoGetDeviceParameters() and icsneoSetDeviceParameters() functions
 */
char *FireParameters[] =
{
	"can1", "can2", "can3", "can4", "swcan", "lsftcan", "lin1", "lin2",
	"lin3", "lin4", "cgi_baud", "cgi_tx_ifs_bit_times",
	"cgi_rx_ifs_bit_times", "cgi_chksum_enable", "network_enables", 
	"network_enabled_on_boot", "pwm_man_timeout", "pwr_man_enable", 
	"misc_io_initial_ddr", "misc_io_initial_latch", "misc_io_analog_enable", 
	"misc_io_report_period", "misc_io_on_report_events", "ain_sample_period", 
	"ain_threshold", "iso15765_separation_time_offset", "iso9141_kwp_settings", 
	"perf_en", "iso_parity", "iso_msg_termination", "network_enables_2"
};

char *VCAN3Parameters[] =
{
	"can1", "can2", "network_enables", "network_enabled_on_boot", "iso15765_separation_time_offset",
	"perf_en", "misc_io_initial_ddr", "misc_io_initial_latch", "misc_io_report_period", 
	"misc_io_on_report_events"
};

char *CANParameters[] = 
{
     "Mode", "SetBaudrate", "Baudrate", "NetworkType", "TqSeg1",
	 "TqSeg2", "TqProp", "TqSync", "BRP", "auto_baud"
};

char *SWCANParameters[] = 
{
	 "Mode", "SetBaudrate", "Baudrate", "NetworkType", "TqSeg1", "TqSeg2", 
	 "TqProp", "TqSync", "BRP", "high_speed_auto_switch", "auto_baud"
};

char *LINParameters[] = 
{
	 "Baudrate", "spbrg", "brgh", "MasterResistor", "Mode"
};

char *ISOKWPParms[] =
{
	 "Baudrate", "spbrg", "brgh", "init_steps", "init_step_count", 
	 "p2_500us", "p3_500us", "p4_500us", "chksum_enabled"
};

/* Function pointers ends*/

typedef struct tagDATINDSTR
{
    BOOL    m_bIsConnected;
    HANDLE  m_hHandle;
    BOOL    m_bToContinue;
    UINT    m_unChannels;
} sDatIndStr;

static sDatIndStr s_DatIndThread;

HRESULT CAN_ICS_neoVI_ManageMsgBuf(BYTE bAction, DWORD ClientID, CBaseCANBufFSE* pBufObj);

/* HELPER FUNCTIONS START */

/**
 * Function to initialize all global data variables
 */
static void vInitialiseAllData(void)
{
    // Initialise both the time parameters
    GetLocalTime(&sg_CurrSysTime);
    sg_TimeStamp = 0x0;
    //INITIALISE_DATA(sg_sCurrStatus);
    memset(&sg_sCurrStatus, 0, sizeof(sg_sCurrStatus));
    //Query Tick Count
    sg_QueryTickCount.QuadPart = 0;
    //INITIALISE_ARRAY(sg_acErrStr);
    memset(sg_acErrStr, 0, sizeof(sg_acErrStr));
    CAN_ICS_neoVI_ManageMsgBuf(MSGBUF_CLEAR, NULL, NULL);
}

/**
 * Function create and set read indication event
 */
static int nCreateAndSetReadIndicationEvent(HANDLE& hReadEvent)
{
    int nReturn = -1;

    // Create a event
    m_hDataEvent = CreateEvent( NULL,           // lpEventAttributes 
                                FALSE,          // bManualReset
                                FALSE,          // bInitialState
                                STR_EMPTY);     // Name
    if (m_hDataEvent != NULL)
    {
        s_DatIndThread.m_hHandle = m_hDataEvent;
        hReadEvent = m_hDataEvent;
        nReturn = defERR_OK;
    }
    return nReturn;
}

/**
 * Function to retreive error occurred and log it
 */
static void vRetrieveAndLog(DWORD /*dwErrorCode*/, char* File, int Line)
{
    USES_CONVERSION;

    char acErrText[MAX_PATH] = {'\0'};
    // Get the error text for the corresponding error code
    //if ((*pfCAN_GetErrText)(dwErrorCode, acErrText) == CAN_USB_OK)
    {
        sg_pIlog->vLogAMessage(A2T(File), Line, A2T(acErrText));

        size_t nStrLen = strlen(acErrText);
        if (nStrLen > CAN_MAX_ERRSTR)
        {
            nStrLen = CAN_MAX_ERRSTR;
        }
        strncpy(sg_acErrStr, acErrText, nStrLen);
    }
}

static int nSetHardwareMode(UCHAR ucDeviceMode)
{
// Make sure to set the network to Hardware
    sg_ucControllerMode = ucDeviceMode;
    return 0;
}

/**
 * \param[in] ucRxErr Rx Error Counter Value
 * \param[in] ucTxErr Tx Error Counter Value
 * \return Type of the error message: Error Bus, Error Warning Limit and Error Interrupt
 *
 * Posts message as per the error counter. This function will
 * update local state variable as per error codes.
 */
static UCHAR USB_ucHandleErrorCounter( UCHAR ucChannel,
                                                    UCHAR ucRxErr,
                                                    UCHAR ucTxErr )
{
    UCHAR ucRetVal = ERROR_BUS;

    CChannel& odChannel = m_aodChannels[ ucChannel ];

    // Check for Error Handler Execution
    // Warning Limit Execution
    if( ucRxErr == odChannel.m_ucWarningLimit &&
        odChannel.m_bRxErrorExecuted == FALSE )
    {
        // Set Error type as warning limit and set the handler execution
        // Flag
        ucRetVal = ERROR_WARNING_LIMIT_REACHED;
        odChannel.m_bRxErrorExecuted = TRUE;
    }
    // Tx Error Value
    else if( ucTxErr == odChannel.m_ucWarningLimit &&
             odChannel.m_bTxErrorExecuted == FALSE )
    {
        // Set Error type as warning limit and set the handler execution
        // Flag
        ucRetVal = ERROR_WARNING_LIMIT_REACHED;
        odChannel.m_bTxErrorExecuted = TRUE;
    }
    //  If the error counter value is 95 then execute the warning limit
    // handler if we are in warning limit state
    if( ucRxErr == odChannel.m_ucWarningLimit - 1 &&
        odChannel.m_bRxErrorExecuted == TRUE )
    {
        // Change the type. This is not real error message
        ucRetVal = ERROR_WARNING_LIMIT_REACHED;
        //ucRetVal = ERROR_UNKNOWN;
        odChannel.m_bRxErrorExecuted = FALSE;
    }
    if( ucTxErr == odChannel.m_ucWarningLimit - 1 &&
        odChannel.m_bTxErrorExecuted == TRUE )
    {
        // Change the type. This is not real error message
        ucRetVal = ERROR_WARNING_LIMIT_REACHED;
        //ucRetVal = ERROR_UNKNOWN;
        odChannel.m_bTxErrorExecuted = FALSE;
    }

    // Reset Event handlers state
    if( ucRxErr < odChannel.m_ucWarningLimit - 1)
    {
        odChannel.m_bRxErrorExecuted = FALSE;
    }
    if( ucTxErr < odChannel.m_ucWarningLimit - 1)
    {
        odChannel.m_bTxErrorExecuted = FALSE;
    }

    // Supress State Change Interrupt messages
    // Active -> Passive
    if( ucRxErr == 127 &&
            odChannel.m_ucControllerState == defCONTROLLER_PASSIVE )
    {
        // Not an error. This is interrupt message
        ucRetVal = ERROR_INTERRUPT;
    }

    // Active -> Passive
    if( ucTxErr == 127 &&
        odChannel.m_ucControllerState == defCONTROLLER_PASSIVE )
    {
        // Not an error. This is interrupt message
        ucRetVal = ERROR_INTERRUPT;
    }
    return ucRetVal;
}

/**
 * \param[in] lError Error code in Peak USB driver format
 * \param[in] byDir  Error direction Tx/Rx
 * \return Error code in BUSMASTER application format
 *
 * This will convert the error code from Perk USB driver format
 * to the format that is used by BUSMASTER.
 */
static UCHAR USB_ucGetErrorCode(LONG lError, BYTE byDir)
{
    UCHAR ucReturn = 0;
    
    // Tx Errors
    if( byDir == 1)
    {
        if (lError & SPY_STATUS_CRC_ERROR)
        {
            ucReturn = BIT_ERROR_TX;
        }
        if (lError & SPY_STATUS_INCOMPLETE_FRAME )
        {
            ucReturn = FORM_ERROR_TX;
        }
        else 
        {
            ucReturn = OTHER_ERROR_TX;
        }
    }
    // Rx Errors
    else
    {
        if (lError & SPY_STATUS_CRC_ERROR)
        {
            ucReturn = BIT_ERROR_RX;
        }
        if (lError & SPY_STATUS_INCOMPLETE_FRAME)
        {
            ucReturn = FORM_ERROR_RX;
        }
        else 
        {
            ucReturn = OTHER_ERROR_RX;
        }
    }
    // Return the error code
    return ucReturn;
}

/**
 * Function to create time mode mapping
 */
static void vCreateTimeModeMapping(HANDLE hDataEvent)
{   
   WaitForSingleObject(hDataEvent, INFINITE);
   //MessageBox(0, L"TIME", L"", 0);
   GetLocalTime(&sg_CurrSysTime);
   //Query Tick Count
   QueryPerformanceCounter(&sg_QueryTickCount);
    
}


static BOOL bLoadDataFromContr(PSCONTROLER_DETAILS pControllerDetails)
{
    BOOL bReturn = FALSE;    
    // If successful
    if (pControllerDetails != NULL)
    {
        for( int nIndex = 0; nIndex < defNO_OF_CHANNELS; nIndex++ )
        {
            TCHAR* pcStopStr = NULL;
            CChannel& odChannel = m_aodChannels[ nIndex ];
            
            // Get Warning Limit
            odChannel.m_ucWarningLimit = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrWarningLimit,
                    &pcStopStr, defBASE_DEC ));
            // Get Acceptance Filter
            odChannel.m_sFilter.m_ucACC_Code0 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccCodeByte1,
                    &pcStopStr, defBASE_HEX ));
            odChannel.m_sFilter.m_ucACC_Code1 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccCodeByte2,
                    &pcStopStr, defBASE_HEX ));
            odChannel.m_sFilter.m_ucACC_Code2 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccCodeByte3,
                    &pcStopStr, defBASE_HEX ));
            odChannel.m_sFilter.m_ucACC_Code3 = static_cast <UCHAR>(
                    _tcstol(pControllerDetails[ nIndex ].m_omStrAccCodeByte4,
                    &pcStopStr, defBASE_HEX));
            odChannel.m_sFilter.m_ucACC_Mask0 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccMaskByte1,
                    &pcStopStr, defBASE_HEX));
            odChannel.m_sFilter.m_ucACC_Mask1 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccMaskByte2,
                    &pcStopStr, defBASE_HEX));
            odChannel.m_sFilter.m_ucACC_Mask2 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccMaskByte3,
                    &pcStopStr, defBASE_HEX));
            odChannel.m_sFilter.m_ucACC_Mask3 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrAccMaskByte4,
                    &pcStopStr, defBASE_HEX));        
            odChannel.m_sFilter.m_ucACC_Filter_Type = static_cast <UCHAR>(
                    pControllerDetails[ nIndex ].m_bAccFilterMode );
            odChannel.m_bCNF1 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrCNF1,
                    &pcStopStr, defBASE_HEX));
            odChannel.m_bCNF2 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrCNF2,
                    &pcStopStr, defBASE_HEX));
            odChannel.m_bCNF3 = static_cast <UCHAR>(
                    _tcstol( pControllerDetails[ nIndex ].m_omStrCNF3,
                    &pcStopStr, defBASE_HEX));

            // Get Baud Rate
            odChannel.m_usBaudRate = static_cast <USHORT>(
                    pControllerDetails[ nIndex ].m_nBTR0BTR1 );

            odChannel.m_unBaudrate = _tstoi( pControllerDetails[ nIndex ].m_omStrBaudrate );

            odChannel.m_ucControllerState = pControllerDetails[ nIndex ].m_ucControllerMode;
        }
        // Get Controller Mode
        // Consider only the first channel mode as controller mode
        //sg_ucControllerMode = pControllerDetails[ 0 ].m_ucControllerMode;
        
        bReturn = TRUE;
    }
    return bReturn;
}

/**
 * \param[in]  odChannel The current channel
 * \param[out] sCanData Application specific data format 
 * \param[in]  ushRxErr Number of Rx error
 * \param[in]  ushTxErr Number of Tx error
 *
 * Based on the number of Rx and Tx error, this will take the
 * action necessary.
 */
static void vProcessError(STCANDATA& sCanData, CChannel& odChannel,
                                       USHORT ushRxErr, USHORT ushTxErr)
{
    sCanData.m_uDataInfo.m_sErrInfo.m_ucErrType = 
        USB_ucHandleErrorCounter(0, (UCHAR)ushRxErr, (UCHAR)ushTxErr);

    // Update Error Type
    if (sCanData.m_uDataInfo.m_sErrInfo.m_ucErrType == ERROR_INTERRUPT)
    {
        // This is interrupt message. So change the type
        sCanData.m_uDataInfo.m_sErrInfo.m_ucReg_ErrCap = ERROR_UNKNOWN;
    }

    // Update Rx Error Counter Value & Tx Error Counter Value
    odChannel.vUpdateErrorCounter((UCHAR)ushTxErr, (UCHAR)ushRxErr);
}

/**
 * \param[in]  CurrSpyMsg Message polled from the bus in neoVI format
 * \param[out] sCanData Application specific data format
 * \param[in]  unChannel channel
 * \return TRUE (always)
 *
 * This will classify the messages, which can be one of Rx, Tx or
 * Error messages. In case of Err messages this identifies under
 * what broader category (Rx / Tx) does this occur.
 */
static BYTE bClassifyMsgType(icsSpyMessage& CurrSpyMsg, 
                      STCANDATA& sCanData, UINT unChannel)
{
    if (CurrSpyMsg.StatusBitField & VALUECAN_ERROR_BITS)
    {
        sCanData.m_ucDataType = ERR_FLAG;
        // Set bus error as default error. This will be 
        // Modified by the function USB_ucHandleErrorCounter
        sCanData.m_uDataInfo.m_sErrInfo.m_ucErrType = ERROR_BUS;
        // Assign the channel number
        sCanData.m_uDataInfo.m_sErrInfo.m_ucChannel = (UCHAR)unChannel;
        // Assign error type in the Error Capture register
        // and the direction of the error
        BOOL bIsTxMsg = FALSE;
        if (CurrSpyMsg.StatusBitField & SPY_STATUS_TX_MSG)
        {
            bIsTxMsg = TRUE;
        }
        sCanData.m_uDataInfo.m_sErrInfo.m_ucReg_ErrCap =
                USB_ucGetErrorCode(CurrSpyMsg.StatusBitField, (BYTE) bIsTxMsg);
        //explaination of error bit
        sCanData.m_uDataInfo.m_sErrInfo.m_nSubError= 0;
        // Update error counter values
        if (bIsTxMsg)
        {
            m_unTxError++;
        }
        else
        {
            m_unRxError++;
        }
        sCanData.m_uDataInfo.m_sErrInfo.m_ucRxErrCount = m_unRxError;
        sCanData.m_uDataInfo.m_sErrInfo.m_ucTxErrCount = m_unTxError;
    }
    else if (CurrSpyMsg.StatusBitField & SPY_STATUS_CAN_BUS_OFF)
    {
        sCanData.m_ucDataType = ERR_FLAG;

        // Set bus error as default error. This will be 
        // Modified by the function USB_ucHandleErrorCounter
        sCanData.m_uDataInfo.m_sErrInfo.m_ucErrType = ERROR_BUS;
        // Assign the channel number
        sCanData.m_uDataInfo.m_sErrInfo.m_ucChannel = (UCHAR)unChannel;

        for (USHORT j = 0x01; j <= 0x80; j <<= 1)
        {
            if (CurrSpyMsg.Data[0] & j)
            {
                UCHAR ucRxCnt = sCanData.m_uDataInfo.m_sErrInfo.m_ucRxErrCount;
                UCHAR ucTxCnt = sCanData.m_uDataInfo.m_sErrInfo.m_ucTxErrCount;
                switch (j)
                {
                    case 0x01:
                    {
                        ucRxCnt = (ucRxCnt < 96) ? 96 : ucRxCnt;
                        ucTxCnt = (ucTxCnt < 96) ? 96 : ucTxCnt;
                    }
                    break;
                    case 0x02: ucRxCnt = (ucRxCnt < 96) ? 96 : ucRxCnt; break;
                    case 0x04: ucTxCnt = (ucTxCnt < 96) ? 96 : ucTxCnt; break;
                    case 0x08: ucTxCnt = (ucTxCnt < 128) ? 128 : ucTxCnt; break;
                    case 0x10: ucRxCnt = (ucRxCnt < 128) ? 128 : ucRxCnt; break;
                    case 0x20: ucTxCnt = (ucTxCnt < 255) ? 255 : ucTxCnt; break;
                    case 0x40:
                    case 0x80:
                    default: break;
                }
                sCanData.m_uDataInfo.m_sErrInfo.m_ucRxErrCount = ucRxCnt;
                sCanData.m_uDataInfo.m_sErrInfo.m_ucTxErrCount = ucTxCnt;
            }
        }
    }
    else
    {   
        //Check for RTR Message
        if (CurrSpyMsg.StatusBitField & SPY_STATUS_REMOTE_FRAME)
        {
            sCanData.m_ucDataType = RX_FLAG;
            sCanData.m_uDataInfo.m_sCANMsg.m_ucRTR = TRUE;
        }
        else
        {
            sCanData.m_uDataInfo.m_sCANMsg.m_ucRTR = FALSE;
        }
        if (CurrSpyMsg.StatusBitField & SPY_STATUS_TX_MSG)
        {
            sCanData.m_ucDataType = TX_FLAG;
        }
        else if (CurrSpyMsg.StatusBitField & SPY_STATUS_NETWORK_MESSAGE_TYPE)
        {
            sCanData.m_ucDataType = RX_FLAG;
        }        
        /*else
        {
            //ASSERT(FALSE);
        }*/
        // Copy data length
        sCanData.m_uDataInfo.m_sCANMsg.m_ucDataLen = CurrSpyMsg.NumberBytesData;

        // Copy the message data
        memcpy(sCanData.m_uDataInfo.m_sCANMsg.m_ucData, 
               CurrSpyMsg.Data, CurrSpyMsg.NumberBytesData);

        // Copy the message ID
        memcpy(&(sCanData.m_uDataInfo.m_sCANMsg.m_unMsgID),
               &(CurrSpyMsg.ArbIDOrHeader), sizeof(UINT));

        // Check for extended message indication
        sCanData.m_uDataInfo.m_sCANMsg.m_ucEXTENDED = 
             (CurrSpyMsg.StatusBitField & SPY_STATUS_XTD_FRAME) ? TRUE : FALSE;

        
    }
    return TRUE;
}

/**
 * \param[in] psCanDataArray Pointer to CAN Message Array of Structures
 * \param[in] nMessage Maximun number of message to read or size of the CAN Message Array
 * \param[out] Message Actual Messages Read
 * \return Returns defERR_OK if successful otherwise corresponding Error code.
 *
 * This function will read multiple CAN messages from the driver.
 * The other fuctionality is same as single message read. This
 * will update the variable nMessage with the actual messages
 * read.
 */
static int nReadMultiMessage(PSTCANDATA psCanDataArray,
                                          int &nMessage, int nChannelIndex)
{
    int i = 0;
    int nReturn = 0;
    // Now the data messages
    CChannel& odChannel = m_aodChannels[nChannelIndex];
    static int s_CurrIndex = 0, s_Messages = 0;
    static icsSpyMessage s_asSpyMsg[MAX_BUFFER_VALUECAN] = {0};
    int nErrMsg = 0;
    if (s_CurrIndex == 0)
    {
        nReturn = (*icsneoGetMessages)(m_anhObject[nChannelIndex], s_asSpyMsg,
                                        &s_Messages, &nErrMsg);
        //TRACE("s_Messages: %d  nErrMsg: %d nMessage: %d\n", s_Messages, 
        //    nErrMsg, nMessage);
    }

    // Start of first level of error message processing
    USHORT ushRxErr = 0, ushTxErr = 0;
    if (nErrMsg > 0)
    {
        int nErrors = 0;
        nReturn = (*icsneoGetErrorMessages)(m_anhObject[nChannelIndex], s_anErrorCodes, &nErrors);
        if ((nReturn == NEOVI_OK) && (nErrors > 0))
        {
            for (int j = 0; j < nErrors; j++)
            {
                switch (s_anErrorCodes[j])
                {
					/*
                    case NEOVI_ERROR_DLL_USB_SEND_DATA_ERROR:
                    {
                        ++ushTxErr;
                    }
                    break;
                    case NEOVI_ERROR_DLL_RX_MSG_FRAME_ERR:
                    case NEOVI_ERROR_DLL_RX_MSG_FIFO_OVER:
                    case NEOVI_ERROR_DLL_RX_MSG_CHK_SUM_ERR:
                    {
                        ++ushRxErr;
                    }
                    break;
					*/
                    default:
                    {
                        // Do nothing until further clarification is received
                    }
                    break;
                }
            }
        }
    }
    // End of first level of error message processing

    // START
    /* Create the time stamp map. This means getting the local time and assigning 
    offset value to the QuadPart.
    */
    static LONGLONG QuadPartRef = 0;

    if (CREATE_MAP_TIMESTAMP == sg_byCurrState)
    {
        //ASSERT(s_CurrIndex < s_Messages);

        //CTimeManager::bReinitOffsetTimeValForICSneoVI();
        icsSpyMessage& CurrSpyMsg = s_asSpyMsg[s_CurrIndex];
        DOUBLE dTimestamp = 0;
        nReturn = (*icsneoGetTimeStampForMsg)(m_anhObject[nChannelIndex], &CurrSpyMsg, &dTimestamp);
        if (nReturn == NEOVI_OK)
        {
            QuadPartRef = (LONGLONG)(dTimestamp * 10000);//CurrSpyMsg.TimeHardware2 * 655.36 + CurrSpyMsg.TimeHardware * 0.01;
            sg_byCurrState = CALC_TIMESTAMP_READY;
            nReturn = defERR_OK;
        }
        else
        {
            nReturn = -1;
        }
    }
    
    // END

    int nLimForAppBuf = nMessage;//MIN(nMessage, s_Messages);
    double dCurrTimeStamp;
    for (/*int i = 0*/; (i < nLimForAppBuf) && (s_CurrIndex < s_Messages); i++)
    {
        STCANDATA& sCanData = psCanDataArray[i];
        icsSpyMessage& CurrSpyMsg = s_asSpyMsg[s_CurrIndex];

        sCanData.m_uDataInfo.m_sCANMsg.m_ucChannel = (UCHAR)(nChannelIndex + 1);
        nReturn = (*icsneoGetTimeStampForMsg)(m_anhObject[nChannelIndex], &CurrSpyMsg, &dCurrTimeStamp);
        /*sCanData.m_lTickCount.QuadPart = (CurrSpyMsg.TimeHardware2 * 655.36 
                                        + CurrSpyMsg.TimeHardware * 0.01);*/
        sCanData.m_lTickCount.QuadPart = (LONGLONG)(dCurrTimeStamp * 10000);
        sg_TimeStamp = sCanData.m_lTickCount.QuadPart = 
                                (sCanData.m_lTickCount.QuadPart - QuadPartRef);

        bClassifyMsgType(CurrSpyMsg, sCanData, sCanData.m_uDataInfo.m_sCANMsg.m_ucChannel);

        if (sCanData.m_ucDataType == ERR_FLAG)
        {
            vProcessError(sCanData, odChannel, ushRxErr + 1, ushTxErr + 1);
            // Reset to zero
            ushRxErr = 0; ushTxErr = 0;
        }
        s_CurrIndex++;
    }
    //TRACE("s_CurrIndex: %d i: %d\n", s_CurrIndex, i);

    //ASSERT(!(s_CurrIndex > MAX_BUFFER_VALUECAN));
    //ASSERT(!(s_CurrIndex > s_Messages));

    if ((s_CurrIndex == MAX_BUFFER_VALUECAN) || (s_CurrIndex == s_Messages))
    {
        s_CurrIndex = 0;
        s_Messages = 0;
    }

    // This code is needed when error messages don't occur in the list of 
    // the regular message
    if ((ushRxErr != 0) || (ushTxErr != 0))
    {
        STCANDATA sCanData;
        vProcessError(sCanData, odChannel, ushRxErr, ushTxErr);
    }

        
    nMessage = i;

    return defERR_OK; // Multiple return statements had to be added because
    // neoVI specific codes and simulation related codes need to coexist. 
    // Code for the later still employs Peak API interface.
}

static BOOL bGetClientObj(DWORD dwClientID, UINT& unClientIndex)
{
    BOOL bResult = FALSE;
    for (UINT i = 0; i < sg_unClientCnt; i++)
    {
        if (sg_asClientToBufMap[i].dwClientID == dwClientID)
        {
            unClientIndex = i;
            i = sg_unClientCnt; //break the loop
            bResult = TRUE;
        }
    }
    return bResult;
}

static BOOL bRemoveClient(DWORD dwClientId)
{
    BOOL bResult = FALSE;
    if (sg_unClientCnt > 0)
    {
        UINT unClientIndex = (UINT)-1;
        if (bGetClientObj(dwClientId, unClientIndex))
        {
            //clear the client first            
            if (sg_asClientToBufMap[unClientIndex].hClientHandle != NULL)
            {
                HRESULT hResult = S_OK;//(*pfCAN_RemoveClient)(sg_asClientToBufMap[unClientIndex].hClientHandle);
                if (hResult == S_OK)
                {
                    sg_asClientToBufMap[unClientIndex].dwClientID = 0;
                    sg_asClientToBufMap[unClientIndex].hClientHandle = NULL;
                    memset (sg_asClientToBufMap[unClientIndex].pacClientName, 0, sizeof (TCHAR) * MAX_PATH);
                    for (int i = 0; i < MAX_BUFF_ALLOWED; i++)
                    {
                        sg_asClientToBufMap[unClientIndex].pClientBuf[i] = NULL;
                    }
                    sg_asClientToBufMap[unClientIndex].unBufCount = 0;
                    bResult = TRUE;                    
                }
                else
                {
                    vRetrieveAndLog(hResult, __FILE__, __LINE__);
                }
            }
            else
            {
                sg_asClientToBufMap[unClientIndex].dwClientID = 0;
                memset (sg_asClientToBufMap[unClientIndex].pacClientName, 0, sizeof (TCHAR) * MAX_PATH);
                for (int i = 0; i < MAX_BUFF_ALLOWED; i++)
                {
                    sg_asClientToBufMap[unClientIndex].pClientBuf[i] = NULL;
                }
                sg_asClientToBufMap[unClientIndex].unBufCount = 0;
                bResult = TRUE;

            }
            if (bResult == TRUE)
            {
                if ((unClientIndex + 1) < sg_unClientCnt)
                {
                    sg_asClientToBufMap[unClientIndex] = sg_asClientToBufMap[sg_unClientCnt - 1];
                }
                sg_unClientCnt--;
            }
        }
    }
    return bResult;
}

static BOOL bRemoveClientBuffer(CBaseCANBufFSE* RootBufferArray[MAX_BUFF_ALLOWED], UINT& unCount, CBaseCANBufFSE* BufferToRemove)
{
    BOOL bReturn = TRUE;
    for (UINT i = 0; i < unCount; i++)
    {
        if (RootBufferArray[i] == BufferToRemove)
        {
            if (i < (unCount - 1)) //If not the last bufffer
            {
                RootBufferArray[i] = RootBufferArray[unCount - 1];
            }
            unCount--;
        }
    }
    return bReturn;
}

void vMarkEntryIntoMap(const SACK_MAP& RefObj)
{
    sg_asAckMapBuf.push_back(RefObj);
}

BOOL bRemoveMapEntry(const SACK_MAP& RefObj, UINT& ClientID)
{   
    BOOL bResult = FALSE;
    CACK_MAP_LIST::iterator  iResult = 
        std::find( sg_asAckMapBuf.begin(), sg_asAckMapBuf.end(), RefObj );  
 
    if ((*iResult).m_ClientID > 0)
    {
        bResult = TRUE;
        ClientID = (*iResult).m_ClientID;
        sg_asAckMapBuf.erase(iResult);
    }
    return bResult;
}

/**
 * This function writes the message to the corresponding clients buffer
 */
static void vWriteIntoClientsBuffer(STCANDATA& sCanData)
{
     //Write into the client's buffer and Increment message Count
    if (sCanData.m_ucDataType == TX_FLAG)
    {
        static SACK_MAP sAckMap;
        UINT ClientId = 0;
        static UINT Index = (UINT)-1;
        sAckMap.m_Channel = sCanData.m_uDataInfo.m_sCANMsg.m_ucChannel;
        sAckMap.m_MsgID = sCanData.m_uDataInfo.m_sCANMsg.m_unMsgID;

        if (bRemoveMapEntry(sAckMap, ClientId))
        {
            BOOL bClientExists = bGetClientObj(ClientId, Index);
            for (UINT i = 0; i < sg_unClientCnt; i++)
            {
                //Tx for monitor nodes and sender node
                if ((i == CAN_MONITOR_NODE_INDEX)  || (bClientExists && (i == Index)))
                {
                    for (UINT j = 0; j < sg_asClientToBufMap[i].unBufCount; j++)
                    {
                        sg_asClientToBufMap[i].pClientBuf[j]->WriteIntoBuffer(&sCanData);
                    }
                }
                else
                {
                    //Send the other nodes as Rx.
                    for (UINT j = 0; j < sg_asClientToBufMap[i].unBufCount; j++)
                    {
                        static STCANDATA sTempCanData;
                        sTempCanData = sCanData;
                        sTempCanData.m_ucDataType = RX_FLAG;
                        sg_asClientToBufMap[i].pClientBuf[j]->WriteIntoBuffer(&sTempCanData);
                    }
                }
            }
        }
    }
    else // provide it to everybody
    {
        for (UINT i = 0; i < sg_unClientCnt; i++)
        {
            for (UINT j = 0; j < sg_asClientToBufMap[i].unBufCount; j++)
            {
                sg_asClientToBufMap[i].pClientBuf[j]->WriteIntoBuffer(&sCanData);
            }
        }
    }
}

/**
 * Processing of the received packets from bus
 */
static void ProcessCANMsg(int nChannelIndex)
{
    int nSize = sg_nFRAMES;
    if (nReadMultiMessage(sg_asMsgBuffer, nSize, nChannelIndex) == defERR_OK)
    {
        for (INT unCount = 0; unCount < nSize; unCount++)
        {
            vWriteIntoClientsBuffer(sg_asMsgBuffer[unCount]);
        }
    }
}

/**
 * Read thread procedure
 */
DWORD WINAPI CanMsgReadThreadProc_CAN_Usb(LPVOID pVoid)
{
    USES_CONVERSION;

    CPARAM_THREADPROC* pThreadParam = (CPARAM_THREADPROC *) pVoid;

    // Validate certain required pointers
    VALIDATE_POINTER_RETURN_VALUE_LOG(pThreadParam, (DWORD)-1);
    // Assign thread action to CREATE_TIME_MAP
    pThreadParam->m_unActionCode = CREATE_TIME_MAP;
    // Set the event to CAN_USB driver for wakeup and frame arrival notification
    nCreateAndSetReadIndicationEvent(pThreadParam->m_hActionEvent);

    // Get the handle to the controller and validate it
    VALIDATE_POINTER_RETURN_VALUE_LOG(pThreadParam->m_hActionEvent, (DWORD)-1);

    DWORD dwResult = 0;
    while (s_DatIndThread.m_bToContinue)
    {
        if (s_DatIndThread.m_bIsConnected)
        {
            for (int i = 0; i < sg_ucNoOfHardware; i++)
            {
                dwResult = (*icsneoWaitForRxMessagesWithTimeOut)(m_anhObject[i], WAITTIME_NEOVI);
                //kadoor WaitForSingleObject(pThreadParam->m_hActionEvent, INFINITE);
                if (dwResult > 0)
                {
                    switch (pThreadParam->m_unActionCode)
                    {
                        case INVOKE_FUNCTION:
                        {
                            ProcessCANMsg(i); // Retrieve message from the driver
                        }
                        break;
                        case CREATE_TIME_MAP:
                        {
                            sg_byCurrState = CREATE_MAP_TIMESTAMP;
                            SetEvent(pThreadParam->m_hActionEvent);
                            vCreateTimeModeMapping(pThreadParam->m_hActionEvent);
                            ProcessCANMsg(i);
                            pThreadParam->m_unActionCode = INVOKE_FUNCTION;
                        }
                        break;
                        default:
                        case INACTION:
                        {
                            // nothing right at this moment
                        }
                        break;
                    }
                }
            }
        }
        else
        {
            WaitForSingleObject(pThreadParam->m_hActionEvent, 500);
            switch (pThreadParam->m_unActionCode)
            {
                case EXIT_THREAD:
                {
                    s_DatIndThread.m_bToContinue = FALSE;
                }
                break;
            }
        }
    }
    SetEvent(pThreadParam->hGetExitNotifyEvent());

    // Close the notification event for startup
    CloseHandle(pThreadParam->m_hActionEvent);
    pThreadParam->m_hActionEvent = NULL;

    return 0;
}

/**
 * This function will get the hardware selection from the user
 * and will create essential networks.
 */
static int nCreateMultipleHardwareNetwork()
{
	TCHAR acTempStr[256] = {_T('\0')};
	INT anSelectedItems[defNO_OF_CHANNELS] = {0};
	int nHwCount = sg_ucNoOfHardware;
    // Get Hardware Network Map
	for (int nCount = 0; nCount < nHwCount; nCount++)
	{
		sg_HardwareIntr[nCount].m_dwIdInterface = m_anComPorts[nCount];
        sg_HardwareIntr[nCount].m_dwVendor = m_anSerialNumbers[nCount];
		_stprintf(acTempStr, _T("Serial Number: %d, Port ID: %d"), m_anSerialNumbers[nCount], m_anComPorts[nCount]);
		_tcscpy(sg_HardwareIntr[nCount].m_acDescription, acTempStr);
	}
	ListHardwareInterfaces(sg_hOwnerWnd, DRIVER_CAN_ICS_NEOVI, sg_HardwareIntr, anSelectedItems, nHwCount);
    sg_ucNoOfHardware = (UCHAR)nHwCount;
	//Reorder hardware interface as per the user selection
	for (int nCount = 0; nCount < sg_ucNoOfHardware; nCount++)
	{
		m_anComPorts[nCount] = (int)sg_HardwareIntr[anSelectedItems[nCount]].m_dwIdInterface;
        m_anSerialNumbers[nCount] = (int)sg_HardwareIntr[anSelectedItems[nCount]].m_dwVendor;
	}
    for (int nIndex = 0; nIndex < sg_ucNoOfHardware; nIndex++)
    {   
        if (nIndex == 0)// AFXBeginThread should be called only once.
        {
            s_DatIndThread.m_bToContinue = TRUE;
            s_DatIndThread.m_bIsConnected = FALSE;
            s_DatIndThread.m_unChannels = sg_ucNoOfHardware;
			m_nNoOfChannels = (int)sg_ucNoOfHardware;
        }	
			
		// Assign hardware handle
		m_aodChannels[ nIndex ].m_hHardwareHandle = (BYTE)m_anComPorts[nIndex];
			
		// Assign Net Handle
		m_aodChannels[ nIndex ].m_hNetworkHandle = m_ucNetworkID[nIndex];
    }
    return defERR_OK;
}

/**
 * This is USB Specific Function. This will create a single
 * network with available single hardware.
 */
static int nCreateSingleHardwareNetwork()
{
    // Create network here with net handle 1
    s_DatIndThread.m_bToContinue = TRUE;
    s_DatIndThread.m_bIsConnected = FALSE;
    s_DatIndThread.m_unChannels = 1;

    // Set the number of channels
    m_nNoOfChannels = 1;
    // Assign hardware handle
    m_aodChannels[ 0 ].m_hHardwareHandle = (BYTE)m_anComPorts[0];
        
    // Assign Net Handle
    m_aodChannels[ 0 ].m_hNetworkHandle = m_ucNetworkID[1];        
    
    return defERR_OK;
}

/**
 * \param[in] nHardwareCount Hardware Count
 * \return Returns defERR_OK if successful otherwise corresponding Error code.
 *
 * Finds the number of hardware connected. This is applicable
 * only for USB device. For parallel port this is not required.
 */
static int nGetNoOfConnectedHardware(int& nHardwareCount)
{
    // 0, Query successful, but no device found
    // > 0, Number of devices found
    // < 0, query for devices unsucessful
	int nResult; 
	NeoDevice ndNeoToOpen [10];
	int iNumberOfDevices;

	iNumberOfDevices = 16;
    for (BYTE i = 0; i < iNumberOfDevices; i++)
    {
        m_ucNetworkID[i] = i;
    }

	//Search for attached devices
	nResult = icsneoFindNeoDevices(65535, ndNeoToOpen, &iNumberOfDevices);
	if(nResult == false)
	{
		_tcscpy(m_omErrStr, _T("Problem Finding Device"));
		nResult = -1;
	}
	else
	if(iNumberOfDevices<1)
	{
		_tcscpy(m_omErrStr, _T("No Devices Found\r\n"));
		nResult = 0;
	}
	else
	{
		nHardwareCount = iNumberOfDevices;
		nResult = iNumberOfDevices;
    }
    // Return the operation result
    return nResult;
}

/**
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This is USB Specific function.This function will create
 * find number of hardware connected. It will create network as
 * per hardware count. This will popup hardware selection dialog
 * in case there are more hardware present.
 */
static int nInitHwNetwork()
{
    int nDevices = 0;
    int nReturn = -1;
    // Select Hardware
    // This function will be called only for USB mode
    nReturn = nGetNoOfConnectedHardware(nDevices);
    m_bInSimMode = (nDevices == 0);

    // Assign the device count
    sg_ucNoOfHardware = (UCHAR)nDevices;
    // Capture only Driver Not Running event
    // Take action based on number of Hardware Available
    TCHAR acNo_Of_Hw[MAX_STRING] = {0};
    _stprintf(acNo_Of_Hw, _T("Number of neoVIs Available: %d"), nDevices);
    // No Hardware found
    
    if( nDevices == 0 )
    {
        MessageBox(NULL,m_omErrStr, NULL, MB_OK | MB_ICONERROR);
        nReturn = -1;
    }
    // Available hardware is lesser then the supported channels
    else 
    {
        // Check whether channel selection dialog is required
        if( nDevices > 1 )
        {
            // Get the selection from the user. This will also
            // create and assign the networks
            nReturn = nCreateMultipleHardwareNetwork();
        }
        else
        {
            // Use available one hardware
            nReturn = nCreateSingleHardwareNetwork();
        }
    }
    return nReturn;
}

/**
 * \return Returns defERR_OK if successful otherwise corresponding Error code.
 *
 * This function will initialise hardware handlers of USB. This
 * will establish the connection with driver. This will create or
 * connect to the network. This will create client for USB. For
 * parallel port mode this will initialise connection with the
 * driver.
 */
static int nConnectToDriver()
{
    int nReturn = -1;
    
    if( sg_bIsDriverRunning == TRUE )
    {
        // Select Hardware or Simulation Network
        nReturn = nInitHwNetwork();
    }
    return nReturn;
}

/**
 * \return TRUE if the driver is running. FALSE - IF it is not running
 */
static BOOL bGetDriverStatus()
{
    sg_bIsDriverRunning = TRUE;
    return TRUE;
}

/**
 * \return defERR_OK if successful otherwise corresponding Error code.
 *
 * This function will set the baud rate of the controller
 * Parallel Port Mode: Controller will be initialised with all
 * other parameters after baud rate change.
 */
static int nSetBaudRate()
{
    int nReturn = 0;

    // Set baud rate to all available hardware
    for( UINT unIndex = 0;
         unIndex < m_nNoOfChannels;
         unIndex++)
    {
        FLOAT fBaudRate = (FLOAT)_tstof(sg_ControllerDetails[unIndex].m_omStrBaudrate);
        int nBitRate = (INT)(fBaudRate * 1000);
        // disconnect the COM
        (*icsneoEnableNetworkCom)(m_anhObject[unIndex], 0);

        // Set the baudrate
        nReturn = (*icsneoSetBitRate)(m_anhObject[unIndex], nBitRate, NETID_HSCAN);
        // connect the COM
        (*icsneoEnableNetworkCom)(m_anhObject[unIndex], 1);
        if (nReturn != NEOVI_OK)
        {
            unIndex = m_nNoOfChannels;
        }
        else
        {
            nReturn = 0;
        }
    }
    
    return nReturn;
}

/**
 * \return Returns defERR_OK if successful otherwise corresponding Error code.
 * This function will set the warning limit of the controller. In
 * USB mode this is not supported as the warning limit is
 * internally set to 96
 */
static int nSetWarningLimit()
{
    for (UINT i = 0; i < m_nNoOfChannels; i++)
    {
        CChannel& odChannel = m_aodChannels[i];
        m_unWarningLimit[i] = odChannel.m_ucWarningLimit;
    }
    return defERR_OK; // Multiple return statements had to be added because
    // neoVI specific codes and simulation related codes need to coexist. 
    // Code for the later still employs Peak API interface.
}

/**
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This function will set the filter information.
 */
static int nSetFilter( )
{
    return 0;
}

/**
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This function will set all controller parameters. This will
 * set Baud rate, Filter, Warning Limit and Controller Mode. In
 * case of USB the warning limit call will be ignored.
 */
static int nSetApplyConfiguration()
{
    int nReturn = defERR_OK;

    // Set Hardware Mode
    if ((nReturn = nSetHardwareMode(sg_ucControllerMode)) == defERR_OK)
    {
        // Set Filter
        nReturn = nSetFilter();
    }
    // Set warning limit only for hardware network
    if ((nReturn == defERR_OK) && (sg_ucControllerMode != defUSB_MODE_SIMULATE))
    {
        // Set Warning Limit
        nReturn = nSetWarningLimit();
    }

    return nReturn;
}

/**
 * \param[out] ucaTestResult Array that will hold test result. TRUE if hardware present and false if not connected
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This function will check all hardware connectivity by getting
 * hardware parameter. In parallel port mode this will set the
 * baud rate to test hardware presence.
 */
static int nTestHardwareConnection(UCHAR& ucaTestResult, UINT nChannel) //const
{
    BYTE aucConfigBytes[CONFIGBYTES_TOTAL];
    int nReturn = 0;
    int nConfigBytes = 0;
    if (nChannel < m_nNoOfChannels)
    {
        if ((*icsneoGetConfiguration)(m_anhObject[nChannel], aucConfigBytes, &nConfigBytes) == NEOVI_OK)
        {
            ucaTestResult = TRUE;
        }
        else
        {            
            sg_bIsConnected = FALSE;
            ucaTestResult = FALSE;
        }
    }
    return nReturn;
}

/**
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This will close the connection with the driver. This will be
 * called before deleting HI layer. This will be called during
 * application close.
 */
static int nDisconnectFromDriver()
{
    int nReturn = 0;
    sg_bIsDriverRunning = FALSE;

    int nErrors = 0;
    for (BYTE i = 0; i < sg_ucNoOfHardware; i++)
    {
        if (m_anhObject[i] != 0)
        {
            // First disconnect the COM
            //(*pFuncPtrFreeObj)(m_anhObject[i]);

            if ((*icsneoClosePort)(m_anhObject[i], &nErrors) == 1)
            {
                m_anhObject[i] = 0;
            }
            else
            {
                nReturn = -1;
            }
        }
    }

    return nReturn;
}

/**
 * \param[in] bConnect TRUE to Connect, FALSE to Disconnect
 * \return Returns defERR_OK if successful otherwise corresponding Error code.
 *
 * This function will connect the tool with hardware. This will
 * establish the data link between the application and hardware.
 * Parallel Port Mode: Controller will be disconnected.
 * USB: Client will be disconnected from the network
 */
static int nConnect(BOOL bConnect, BYTE /*hClient*/)
{
    int nReturn = -1;
    BYTE aucConfigBytes[CONFIGBYTES_TOTAL];
    int nConfigBytes = 0;
    
	if (!sg_bIsConnected && bConnect) // Disconnected and to be connected
    {
        for (UINT i = 0; i < m_nNoOfChannels; i++)
        {
            nReturn = (*icsneoOpenPortEx)(m_anComPorts[i],
                NEOVI_COMMTYPE_RS232, 0, 0, 57600,
                1, m_ucNetworkID, &(m_anhObject[i]));
            
            if (nReturn != NEOVI_OK)
            {
                // In such a case it is wiser to carry out disconnecting 
                nDisconnectFromDriver(); // operations from the driver

                // Try again
                (*icsneoOpenPortEx)(m_anComPorts[i], NEOVI_COMMTYPE_RS232,
                                    0, 0, 57600, 1,
                                    m_ucNetworkID, &(m_anhObject[i]));

                /* Checking return value of this function is not important 
                because GetConfig function (called to check for the hardware 
                presence) renders us the necessary information */
                nReturn = (*icsneoGetConfiguration)(m_anhObject[i], aucConfigBytes,
                    &nConfigBytes);
            }
            if (nReturn == NEOVI_OK) // Hardware is present
            {
                // OpenPort and this function must be called together.
                // CTimeManager::bReinitOffsetTimeValForICSneoVI();
                // Transit into 'CREATE TIME MAP' state
                sg_byCurrState = CREATE_MAP_TIMESTAMP;
                if (i == (m_nNoOfChannels -1))
                {
                    //Only at the last it has to be called
                    nSetBaudRate();
                    sg_bIsConnected = bConnect;
                    s_DatIndThread.m_bIsConnected = sg_bIsConnected;
                }
                nReturn = defERR_OK;
            }
            else // Hardware is not present
            {
                
                // Display the error message when not called from COM interface                
            }
        }
    }
    else if (sg_bIsConnected && !bConnect) // Connected & to be disconnected
    {
        sg_bIsConnected = bConnect;
        s_DatIndThread.m_bIsConnected = sg_bIsConnected;
        Sleep(0); // Let other threads run for once
        nReturn = nDisconnectFromDriver();
    }
    else 
    {
        nReturn = defERR_OK;
    }

    return nReturn;
}

/**
 * Perform initialization operations specific to TZM
 */
USAGEMODE HRESULT CAN_ICS_neoVI_PerformInitOperations(void)
{
    //Register Monitor client
    CAN_ICS_neoVI_RegisterClient(TRUE, sg_dwClientID, CAN_MONITOR_NODE);
    return S_OK;
}

/**
 * Perform closure operations specific to TZM
 */
USAGEMODE HRESULT CAN_ICS_neoVI_PerformClosureOperations(void)
{
    HRESULT hResult = S_OK;
    UINT ClientIndex = 0;
    while (sg_unClientCnt > 0)
    {
        bRemoveClient(sg_asClientToBufMap[ClientIndex].dwClientID);
    }
    hResult = CAN_ICS_neoVI_DeselectHwInterface();

    if (hResult == S_OK)
    {
        sg_bCurrState = STATE_DRIVER_SELECTED;
    }
    return hResult;
}

/**
 * Retrieve time mode mapping
 */
USAGEMODE HRESULT CAN_ICS_neoVI_GetTimeModeMapping(SYSTEMTIME& CurrSysTime, UINT64& TimeStamp, LARGE_INTEGER* QueryTickCount)
{
    memcpy(&CurrSysTime, &sg_CurrSysTime, sizeof(SYSTEMTIME));
    TimeStamp = sg_TimeStamp;
    if(QueryTickCount != NULL)
    {
        *QueryTickCount = sg_QueryTickCount;
    }
    return S_OK;
}

/**
 * Function to List Hardware interfaces connect to the system and requests to the
 * user to select
 */
USAGEMODE HRESULT CAN_ICS_neoVI_ListHwInterfaces(INTERFACE_HW_LIST& asSelHwInterface, INT& nCount)    
{    
    USES_CONVERSION;
    HRESULT hResult = S_FALSE;
    if (bGetDriverStatus())
    {
        if (nConnectToDriver() == CAN_USB_OK)
        {  
            nCount = sg_ucNoOfHardware;
            for (UINT i = 0; i < sg_ucNoOfHardware; i++)
            {
                asSelHwInterface[i].m_dwIdInterface = m_anComPorts[i];
                _stprintf(asSelHwInterface[i].m_acDescription, _T("%d"), m_anSerialNumbers[i]);
                hResult = S_OK;
                sg_bCurrState = STATE_HW_INTERFACE_LISTED;
            }
        }
        else
        {            
            hResult = NO_HW_INTERFACE;
            sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__, _T("Error connecting to driver"));
        }
    }
    else
    {   
        sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__, _T("Error in getting driver status"));
    }
    return hResult;
}

/**
 * \param[in] bHardwareReset Reset Mode: TRUE - Hardware Reset, FALSE - Software Reset
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This function will do controller reset. In case of USB mode
 * Software Reset will be simulated by Client Reset.
 */
static int nResetHardware(BOOL /*bHardwareReset*/)
{
    return 0;
}

/**
 * Function to deselect the chosen hardware interface
 */
USAGEMODE HRESULT CAN_ICS_neoVI_DeselectHwInterface(void)
{
    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_HW_INTERFACE_SELECTED, ERR_IMPROPER_STATE);

    HRESULT hResult = S_OK;
    
    CAN_ICS_neoVI_ResetHardware();

    sg_bCurrState = STATE_HW_INTERFACE_LISTED;
    
    return hResult;
}

/**
 * Function to select hardware interface chosen by the user
 */
USAGEMODE HRESULT CAN_ICS_neoVI_SelectHwInterface(const INTERFACE_HW_LIST& asSelHwInterface, INT /*nCount*/)
{
    USES_CONVERSION;

    VALIDATE_POINTER_RETURN_VAL(sg_hDll, S_FALSE);

    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_HW_INTERFACE_LISTED, ERR_IMPROPER_STATE);

    HRESULT hResult = S_FALSE;
    
    if (bGetDriverStatus())
    {
        //Select the interface
        for (UINT i = 0; i < sg_ucNoOfHardware; i++)
        {
            m_anComPorts[i] = (INT)asSelHwInterface[i].m_dwIdInterface;
            m_anSerialNumbers[i] = _ttoi(asSelHwInterface[i].m_acDescription);
        }
        //Check for the success
        sg_bCurrState = STATE_HW_INTERFACE_SELECTED;
        hResult = S_OK;
    }
    else
    {
        hResult = S_FALSE;
        sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__, _T("Driver is not running..."));
    }
    return hResult;
}

/**
 * Function to set controller configuration
 */
USAGEMODE HRESULT CAN_ICS_neoVI_SetConfigData(PCHAR ConfigFile, int Length)
{
    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_HW_INTERFACE_SELECTED, ERR_IMPROPER_STATE);

    USES_CONVERSION; 

    HRESULT hResult = S_FALSE;
    memcpy((void*)sg_ControllerDetails, (void*)ConfigFile, Length);
    int nReturn = nSetApplyConfiguration();
    if (nReturn == defERR_OK)
    {
        hResult = S_OK;
    }
    else
    {
        sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__,
                        _T("Controller configuration failed"));
    }
    return hResult;
}

BOOL Callback_DILTZM(BYTE /*Argument*/, PBYTE pDatStream, int /*Length*/)
{
    return (CAN_ICS_neoVI_SetConfigData((CHAR *) pDatStream, 0) == S_OK);
}

/**
 * Function to display config dialog
 */
USAGEMODE HRESULT CAN_ICS_neoVI_DisplayConfigDlg(PCHAR& InitData, int& Length)
{
    HRESULT Result = WARN_INITDAT_NCONFIRM;
    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_HW_INTERFACE_SELECTED, ERR_IMPROPER_STATE);

    VALIDATE_POINTER_RETURN_VAL(InitData, Result);
    PSCONTROLER_DETAILS pControllerDetails = (PSCONTROLER_DETAILS)InitData;
    //First initialize with existing hw description
    for (INT i = 0; i < min(Length, sg_ucNoOfHardware); i++)
    {   
        _stprintf(pControllerDetails[i].m_omHardwareDesc, _T("Serial Number %d"), m_anSerialNumbers[i]);
    }
    if (sg_ucNoOfHardware > 0)
    {
        int nResult = DisplayConfigurationDlg(sg_hOwnerWnd, Callback_DILTZM,
            pControllerDetails, sg_ucNoOfHardware, DRIVER_CAN_ICS_NEOVI);
        switch (nResult)
        {
            case WARNING_NOTCONFIRMED:
            {
                Result = WARN_INITDAT_NCONFIRM;
            }
            break;
            case INFO_INIT_DATA_CONFIRMED:
            {
                bLoadDataFromContr(pControllerDetails);
                memcpy(sg_ControllerDetails, pControllerDetails, sizeof (SCONTROLER_DETAILS) * defNO_OF_CHANNELS);
                nSetApplyConfiguration();
                memcpy(InitData, (void*)sg_ControllerDetails, sizeof (SCONTROLER_DETAILS) * defNO_OF_CHANNELS);
                Length = sizeof(SCONTROLER_DETAILS) * defNO_OF_CHANNELS;
                Result = INFO_INITDAT_CONFIRM_CONFIG;
            }
            break;
            case INFO_RETAINED_CONFDATA:
            {
                Result = INFO_INITDAT_RETAINED;
            }
            break;
            case ERR_CONFIRMED_CONFIGURED: // Not to be addressed at present
            case INFO_CONFIRMED_CONFIGURED:// Not to be addressed at present
            default:
            {
                // Do nothing... default return value is S_FALSE.
            }
            break;
        }
    }
    else
    {
        Result = S_OK;
    }

    return Result;
}

/**
 * Function to start monitoring the bus
 */
USAGEMODE HRESULT CAN_ICS_neoVI_StartHardware(void)
{
    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_HW_INTERFACE_SELECTED, ERR_IMPROPER_STATE);

    USES_CONVERSION;
    HRESULT hResult = S_OK;
    //Restart the read thread
    sg_sParmRThread.bTerminateThread();
    sg_sParmRThread.m_pBuffer = (LPVOID) &s_DatIndThread;
    s_DatIndThread.m_bToContinue = TRUE;
    if (sg_sParmRThread.bStartThread(CanMsgReadThreadProc_CAN_Usb))
    {
        hResult = S_OK;
    }
    else
    {
        sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__, _T("Could not start the read thread" ));
    }
    //If everything is ok connect to the network
    if (hResult == S_OK)
    {
        for (UINT ClientIndex = 0; ClientIndex < sg_unClientCnt; ClientIndex++)
        {
            BYTE hClient = sg_asClientToBufMap[ClientIndex].hClientHandle;
            hResult = nConnect(TRUE, hClient);    
            if (hResult == CAN_USB_OK)
            {
                hResult = S_OK;
                sg_bCurrState = STATE_CONNECTED;
            }
            else
            {
                //log the error for open port failure
                vRetrieveAndLog(hResult, __FILE__, __LINE__);
                hResult = ERR_LOAD_HW_INTERFACE;
            }
        }
    }
    return hResult;
}

/**
 * Function to stop monitoring the bus
 */
USAGEMODE HRESULT CAN_ICS_neoVI_StopHardware(void)
{
    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_CONNECTED, ERR_IMPROPER_STATE);

    HRESULT hResult = S_OK;
    
    for (UINT ClientIndex = 0; ClientIndex < sg_unClientCnt; ClientIndex++)
    {
        BYTE hClient = sg_asClientToBufMap[ClientIndex].hClientHandle;        
    
        hResult = nConnect(FALSE, hClient);
        if (hResult == CAN_USB_OK)
        {
            hResult = S_OK;
            sg_bCurrState = STATE_HW_INTERFACE_SELECTED;
        }
        else
        {
            //log the error for open port failure
            vRetrieveAndLog(hResult, __FILE__, __LINE__);
            hResult = ERR_LOAD_HW_INTERFACE;
            ClientIndex = sg_unClientCnt; //break the loop
        }
    }
    return hResult;
}

static BOOL bClientExist(TCHAR* pcClientName, INT& Index)
{    
    for (UINT i = 0; i < sg_unClientCnt; i++)
    {
        if (!_tcscmp(pcClientName, sg_asClientToBufMap[i].pacClientName))
        {
            Index = i;
            return TRUE;
        }       
    }
    return FALSE;
}

static BOOL bClientIdExist(const DWORD& dwClientId)
{
    BOOL bReturn = FALSE;
    for (UINT i = 0; i < sg_unClientCnt; i++)
    {
        if (sg_asClientToBufMap[i].dwClientID == dwClientId)
        {
            bReturn = TRUE;
            i = sg_unClientCnt; // break the loop
        }
    }
    return bReturn;
}

/**
 * \param sErrorCount Error Counter Structure
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This function will return the error counter values. In case of USB this is not supported.
 */
static int nGetErrorCounter( UINT unChannel, SERROR_CNT& sErrorCount)
{
    int nReturn = -1;

    // Check for the valid channel index
    if( unChannel < m_nNoOfChannels )
    {
        // Get the channel reference
        CChannel& odChannel = m_aodChannels[ unChannel ];
        // Assign the error counter value
        sErrorCount.m_ucRxErrCount = odChannel.m_ucRxErrorCounter;
        sErrorCount.m_ucTxErrCount = odChannel.m_ucTxErrorCounter;
        nReturn = defERR_OK;
    }
    else
    {
        // Invalid channel ID. Help debugging
    }

    return nReturn;
}

/**
 * Function to reset the hardware, fcClose resets all the buffer
 */
USAGEMODE HRESULT CAN_ICS_neoVI_ResetHardware(void)
{
    HRESULT hResult = S_FALSE;
    // Stop the hardware if connected
    CAN_ICS_neoVI_StopHardware(); //return value not necessary
    if (sg_sParmRThread.bTerminateThread())
    {
        if (nResetHardware(TRUE) == CAN_USB_OK)
        {
            hResult = S_OK;
        }
    }
    return hResult;
}

/**
 * Function to get Controller status
 */
USAGEMODE HRESULT CAN_ICS_neoVI_GetCurrStatus(s_STATUSMSG& StatusData)
{
    if (sg_ucControllerMode == defUSB_MODE_ACTIVE)
    {
        StatusData.wControllerStatus = NORMAL_ACTIVE;
    }
    else if (sg_ucControllerMode == defUSB_MODE_PASSIVE)
    {
        StatusData.wControllerStatus = NORMAL_PASSIVE;
    }
    else if (sg_ucControllerMode == defUSB_MODE_SIMULATE)
    {
        StatusData.wControllerStatus = NOT_DEFINED;
    }        
    return S_OK;
}

/**
 * Function to get Tx Msg Buffers configured from chi file
 */
USAGEMODE HRESULT CAN_ICS_neoVI_GetTxMsgBuffer(BYTE*& /*pouFlxTxMsgBuffer*/)
{
    return S_OK;
}

/**
 * \param[in] sMessage Message to Transmit
 * \return Operation Result. 0 incase of no errors. Failure Error codes otherwise.
 *
 * This will send a CAN message to the driver. In case of USB
 * this will write the message in to the driver buffer and will
 * return. In case if parallel port mode this will write the
 * message and will wait for the ACK event from the driver. If
 * the event fired this will return 0. Otherwise this will return
 * wait time out error. In parallel port it is a blocking call
 * and in case of failure condition this will take 2 seconds.
 */
static int nWriteMessage(STCAN_MSG sMessage)
{
    int nReturn = -1;

    // Return when in disconnected state
    if (!sg_bIsConnected) return nReturn;
    
    if (sMessage.m_ucChannel > 0 &&
        sMessage.m_ucChannel <= m_nNoOfChannels)
    {
        icsSpyMessage SpyMsg;
        memcpy(&(SpyMsg.ArbIDOrHeader), &(sMessage.m_unMsgID), sizeof(UINT));
        SpyMsg.NumberBytesData = sMessage.m_ucDataLen;
        SpyMsg.StatusBitField = 0;
        SpyMsg.StatusBitField2 = 0;
        if (sMessage.m_ucRTR == 1)
        {
            SpyMsg.StatusBitField |= SPY_STATUS_REMOTE_FRAME;
        }
        if (sMessage.m_ucEXTENDED == 1)
        {
            SpyMsg.StatusBitField |= SPY_STATUS_XTD_FRAME;
        }
        memcpy(SpyMsg.Data, sMessage.m_ucData, sMessage.m_ucDataLen);
        if ((*icsneoTxMessages)(m_anhObject[(int)(sMessage.m_ucChannel) - 1], &SpyMsg, NETID_HSCAN, 1) != 0)
        {
            nReturn = 0;
        }
    }
    return nReturn; // Multiple return statements had to be added because
    // neoVI specific codes and simulation related codes need to coexist. 
    // Code for the later still employs Peak API interface.
}

/**
 * Function to Send CAN Message to Transmit buffer. This is called only after checking the controller in active mode
 */
USAGEMODE HRESULT CAN_ICS_neoVI_SendMsg(DWORD dwClientID, const STCAN_MSG& sMessage)
{    
    VALIDATE_VALUE_RETURN_VAL(sg_bCurrState, STATE_CONNECTED, ERR_IMPROPER_STATE);
    static SACK_MAP sAckMap;
    HRESULT hResult = S_FALSE;
    if (bClientIdExist(dwClientID))
    {
        if (sMessage.m_ucChannel <= sg_ucNoOfHardware)
        {
            sAckMap.m_ClientID = dwClientID;
            sAckMap.m_MsgID = sMessage.m_unMsgID;
            sAckMap.m_Channel = sMessage.m_ucChannel;
            /*Mark an entry in Map. This is helpful to idendify
              which client has been sent this message in later stage*/ 
            vMarkEntryIntoMap(sAckMap);
            if (nWriteMessage(sMessage) == defERR_OK)
            {
                hResult = S_OK;
            }
        }
        else
        {
            hResult = ERR_INVALID_CHANNEL;
        }
    }
    else
    {
        hResult = ERR_NO_CLIENT_EXIST;
    }
    return hResult;
}

/**
 * Function get hardware, firmware, driver information
 */ 
USAGEMODE HRESULT CAN_ICS_neoVI_GetBoardInfo(s_BOARDINFO& /*BoardInfo*/)
{
    return S_OK;
}

USAGEMODE HRESULT CAN_ICS_neoVI_GetBusConfigInfo(BYTE* /*usInfo*/)
{
    return S_OK;
}

USAGEMODE HRESULT CAN_ICS_neoVI_GetVersionInfo(VERSIONINFO& /*sVerInfo*/)
{
    return S_FALSE;
}

/**
 * Function to retreive error string of last occurred error
 */
USAGEMODE HRESULT CAN_ICS_neoVI_GetLastErrorString(CHAR* acErrorStr, int nLength)
{
    // TODO: Add your implementation code here
    int nCharToCopy = (int) (strlen(sg_acErrStr));
    if (nCharToCopy > nLength)
    {
        nCharToCopy = nLength;
    }
    strncpy(acErrorStr, sg_acErrStr, nCharToCopy);

    return S_OK;
}

/**
 * Set application parameters specific to CAN_USB
 */
USAGEMODE HRESULT CAN_ICS_neoVI_SetAppParams(HWND hWndOwner, Base_WrapperErrorLogger* pILog)
{
    sg_hOwnerWnd = hWndOwner;
    sg_pIlog = pILog;
    vInitialiseAllData();
    return S_OK;
}

static BOOL bIsBufferExists(const SCLIENTBUFMAP& sClientObj, const CBaseCANBufFSE* pBuf)
{
    BOOL bExist = FALSE;
    for (UINT i = 0; i < sClientObj.unBufCount; i++)
    {
        if (pBuf == sClientObj.pClientBuf[i])
        {
            bExist = TRUE;
            i = sClientObj.unBufCount; //break the loop
        }
    }
    return bExist;
}

static DWORD dwGetAvailableClientSlot()
{
    DWORD nClientId = 2;
    for (int i = 0; i < MAX_CLIENT_ALLOWED; i++)
    {
        if (bClientIdExist(nClientId))
        {
            nClientId += 1;
        }
        else
        {
            i = MAX_CLIENT_ALLOWED; //break the loop
        }
    }
    return nClientId;
}

/**
 * Register Client
 */
USAGEMODE HRESULT CAN_ICS_neoVI_RegisterClient(BOOL bRegister,DWORD& ClientID, TCHAR* pacClientName)
{
    USES_CONVERSION;    
    HRESULT hResult = S_FALSE;
    if (bRegister)
    {
        if (sg_unClientCnt < MAX_CLIENT_ALLOWED)
        {        
            INT Index = 0;
            if (!bClientExist(pacClientName, Index))
            {
                //Currently store the client information
                if (_tcscmp(pacClientName, CAN_MONITOR_NODE) == 0)
                {
                    //First slot is reserved to monitor node
                    ClientID = 1;
                    _tcscpy(sg_asClientToBufMap[0].pacClientName, pacClientName);                
                    sg_asClientToBufMap[0].dwClientID = ClientID;
                    sg_asClientToBufMap[0].unBufCount = 0;
                }
                else
                {                    
                    if (!bClientExist(CAN_MONITOR_NODE, Index))
                    {
                        Index = sg_unClientCnt + 1;
                    }
                    else
                    {
                        Index = sg_unClientCnt;
                    }
                    ClientID = dwGetAvailableClientSlot();
                    _tcscpy(sg_asClientToBufMap[Index].pacClientName, pacClientName);
            
                    sg_asClientToBufMap[Index].dwClientID = ClientID;
                    sg_asClientToBufMap[Index].unBufCount = 0;
                }
                sg_unClientCnt++;
                hResult = S_OK;
            }
            else
            {
                ClientID = sg_asClientToBufMap[Index].dwClientID;
                hResult = ERR_CLIENT_EXISTS;
            }
        }
        else
        {
            hResult = ERR_NO_MORE_CLIENT_ALLOWED;
        }
    }
    else
    {
        if (bRemoveClient(ClientID))
        {
            hResult = S_OK;
        }
        else
        {
            hResult = ERR_NO_CLIENT_EXIST;
        }
    }
    return hResult;
}

USAGEMODE HRESULT CAN_ICS_neoVI_ManageMsgBuf(BYTE bAction, DWORD ClientID, CBaseCANBufFSE* pBufObj)
{
    HRESULT hResult = S_FALSE;
    if (ClientID != NULL)
    {
        UINT unClientIndex;
        if (bGetClientObj(ClientID, unClientIndex))
        {
            SCLIENTBUFMAP &sClientObj = sg_asClientToBufMap[unClientIndex];
            if (bAction == MSGBUF_ADD)
            {
                //Add msg buffer
                if (pBufObj != NULL)
                {
                    if (sClientObj.unBufCount < MAX_BUFF_ALLOWED)
                    {
                        if (bIsBufferExists(sClientObj, pBufObj) == FALSE)
                        {
                            sClientObj.pClientBuf[sClientObj.unBufCount++] = pBufObj;
                            hResult = S_OK;
                        }
                        else
                        {
                            hResult = ERR_BUFFER_EXISTS;
                        }
                    }
                }
            }
            else if (bAction == MSGBUF_CLEAR)
            {
                //clear msg buffer
                if (pBufObj != NULL) //REmove only buffer mentioned
                {
                    bRemoveClientBuffer(sClientObj.pClientBuf, sClientObj.unBufCount, pBufObj);
                }
                else //Remove all
                {
                    for (UINT i = 0; i < sClientObj.unBufCount; i++)
                    {
                        sClientObj.pClientBuf[i] = NULL;
                    }
                    sClientObj.unBufCount = 0;
                }
                hResult = S_OK;
            }
            else
            {
                ////ASSERT(FALSE);
            }
        }
        else
        {
            hResult = ERR_NO_CLIENT_EXIST;
        }
    }
    else
    {
        if (bAction == MSGBUF_CLEAR)
        {
            //clear msg buffer
            for (UINT i = 0; i < sg_unClientCnt; i++)
            {
                CAN_ICS_neoVI_ManageMsgBuf(MSGBUF_CLEAR, sg_asClientToBufMap[i].dwClientID, NULL);                        
            }
            hResult = S_OK;
        }        
    }
    
    return hResult;
}

/**
 * Function to load driver CanApi2.dll
 */
USAGEMODE HRESULT CAN_ICS_neoVI_LoadDriverLibrary(void)
{
    USES_CONVERSION;

    HRESULT hResult = S_OK;

    if (sg_hDll != NULL)
    {
        sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__, _T("icsneo40.dll already loaded"));
        hResult = DLL_ALREADY_LOADED;
	}

    if (hResult == S_OK)
    {
        sg_hDll = LoadLibrary("icsneo40.dll");
        if (sg_hDll == NULL)
        {
            sg_pIlog->vLogAMessage(A2T(__FILE__), __LINE__, _T("icsneo40.dll loading failed"));
            hResult = ERR_LOAD_DRIVER;
        }
        else
        {
			icsneoFindNeoDevices =    (FINDNEODEVICES) GetProcAddress(sg_hDll,              "icsneoFindNeoDevices");
			icsneoOpenNeoDevice =     (OPENNEODEVICE) GetProcAddress(sg_hDll,               "icsneoOpenNeoDevice");
			icsneoClosePort =         (CLOSEPORT) GetProcAddress(sg_hDll,                   "icsneoClosePort");	
			icsneoFreeObject =        (FREEOBJECT) GetProcAddress(sg_hDll,                  "icsneoFreeObject");
			icsneoOpenPortEx =        (OPENPORTEX) GetProcAddress(sg_hDll,                  "icsneoOpenPortEx");
			icsneoEnableNetworkCom =  (ENABLENETWORKCOM) GetProcAddress(sg_hDll,            "icsneoEnableNetworkCom");
			icsneoGetDLLVersion =     (GETDLLVERSION) GetProcAddress(sg_hDll,               "icsneoGetDLLVersion");

			icsneoTxMessages =        (TXMESSAGES) GetProcAddress(sg_hDll,                  "icsneoTxMessages");
			icsneoGetMessages =       (GETMESSAGES) GetProcAddress(sg_hDll,                 "icsneoGetMessages");
			icsneoWaitForRxMessagesWithTimeOut = (WAITFORRXMSGS) GetProcAddress(sg_hDll,    "icsneoWaitForRxMessagesWithTimeOut");
			icsneoGetTimeStampForMsg = (GETTSFORMSG) GetProcAddress(sg_hDll,                "icsneoGetTimeStampForMsg");
			icsneoEnableNetworkRXQueue = (ENABLERXQUEUE) GetProcAddress(sg_hDll,            "icsneoEnableNetworkRXQueue");

			icsneoGetConfiguration =  (GETCONFIG) GetProcAddress(sg_hDll,                   "icsneoGetConfiguration");
  			icsneoSendConfiguration = (SENDCONFIG) GetProcAddress(sg_hDll,                  "icsneoSendConfiguration");
			icsneoGetFireSettings =   (GETFIRESETTINGS) GetProcAddress(sg_hDll,             "icsneoGetFireSettings");
			icsneoSetFireSettings =   (SETFIRESETTINGS) GetProcAddress(sg_hDll,             "icsneoSetFireSettings");
			icsneoGetVCAN3Settings =  (GETVCAN3SETTINGS) GetProcAddress(sg_hDll,            "icsneoGetVCAN3Settings");
			icsneoSetVCAN3Settings =  (SETVCAN3SETTINGS) GetProcAddress(sg_hDll,            "icsneoSetVCAN3Settings");
			icsneoSetBitRate =        (SETBITRATE)       GetProcAddress(sg_hDll,            "icsneoSetBitRate");
			icsneoGetDeviceParameters = (GETDEVICEPARMS) GetProcAddress(sg_hDll,            "icsneoGetDeviceParameters");
			icsneoSetDeviceParameters = (SETDEVICEPARMS) GetProcAddress(sg_hDll,            "icsneoSetDeviceParameters");

			icsneoGetLastAPIError =   (GETLASTAPIERROR) GetProcAddress(sg_hDll,             "icsneoGetLastAPIError");
			icsneoGetErrorMessages = (GETERRMSGS) GetProcAddress(sg_hDll,                   "icsneoGetErrorMessages");
			icsneoGetErrorInfo =     (GETERRORINFO) GetProcAddress(sg_hDll,                 "icsneoGetErrorInfo");

			//check for error
			if(!icsneoFindNeoDevices || !icsneoOpenNeoDevice || !icsneoClosePort || !icsneoFreeObject || !icsneoOpenPortEx ||  
			   !icsneoEnableNetworkCom || !icsneoTxMessages || !icsneoGetMessages || !icsneoWaitForRxMessagesWithTimeOut ||
			   !icsneoGetTimeStampForMsg || !icsneoEnableNetworkRXQueue ||
			   !icsneoGetConfiguration || !icsneoSendConfiguration ||
			   !icsneoGetFireSettings || !icsneoSetFireSettings || !icsneoGetVCAN3Settings ||
			   !icsneoSetVCAN3Settings || !icsneoSetBitRate || !icsneoGetDeviceParameters ||
			   !icsneoSetDeviceParameters || !icsneoGetLastAPIError || !icsneoGetErrorMessages ||
			   !icsneoGetErrorInfo || !icsneoGetDLLVersion )
			{
				FreeLibrary(sg_hDll);
                // Log list of the function pointers non-retrievable
                // TO DO: specific information on failure in getting function pointer
                sg_pIlog->vLogAMessage(A2T(__FILE__),
                            __LINE__, _T("Getting Process address of the APIs failed"));
                hResult = ERR_LOAD_DRIVER;
            }
        }
    }

    return hResult;
}

/**
 * Function to Unload Driver library
 */
USAGEMODE HRESULT CAN_ICS_neoVI_UnloadDriverLibrary(void)
{
    // Don't bother about the success & hence the result
    CAN_ICS_neoVI_DeselectHwInterface();

    // Store the Boardinfo to global variable

    if (sg_hDll != NULL)
    {
        FreeLibrary(sg_hDll);
        sg_hDll = NULL;
    }
    return S_OK;
}


USAGEMODE HRESULT CAN_ICS_neoVI_FilterFrames(FILTER_TYPE /*FilterType*/, TYPE_CHANNEL /*Channel*/, UINT* /*punFrames*/, UINT /*nLength*/)
{
    return S_OK;
}

USAGEMODE HRESULT CAN_ICS_neoVI_GetCntrlStatus(const HANDLE& hEvent, UINT& unCntrlStatus)
{
    if (hEvent != NULL)
    {
        sg_hCntrlStateChangeEvent = hEvent;
    }    
    unCntrlStatus = static_cast<UINT>(sg_ucControllerMode);
    return S_OK;
}

USAGEMODE HRESULT CAN_ICS_neoVI_GetControllerParams(LONG& lParam, UINT nChannel, ECONTR_PARAM eContrParam)
{
    HRESULT hResult = S_OK;
    if ((sg_bCurrState == STATE_HW_INTERFACE_SELECTED) || (sg_bCurrState == STATE_CONNECTED))
    {
        switch (eContrParam)
        {
            
            case NUMBER_HW:
            {
                lParam = sg_ucNoOfHardware;
            }
            break;
            case NUMBER_CONNECTED_HW:
            {
                int nConHwCnt = 0;
                if (nGetNoOfConnectedHardware(nConHwCnt) == NEOVI_OK)
                {
                    lParam = nConHwCnt;
                }
                else
                {
                    hResult = S_FALSE;
                }
            }
            break;
            case DRIVER_STATUS:
            {
                lParam = sg_bIsDriverRunning;
            }
            break;
            case HW_MODE:
            {
                if (nChannel < sg_ucNoOfHardware)
                {
                    lParam = sg_ucControllerMode;
                    if( sg_ucControllerMode == 0 || sg_ucControllerMode > defMODE_SIMULATE )
                    {
                        lParam = defCONTROLLER_BUSOFF;
                        hResult = S_FALSE;
                    }
                }
                else
                {
                    //Unknown
                    lParam = defCONTROLLER_BUSOFF + 1;
                }
            }
            break;
            case CON_TEST:
            {
                UCHAR ucResult;
                if (nTestHardwareConnection(ucResult, nChannel) == defERR_OK)
                {
                    lParam = (LONG)ucResult;
                }
            }
            break;
            default:
            {
                hResult = S_FALSE;
            }
            break;

        }
    }
    else
    {
        hResult = ERR_IMPROPER_STATE;
    }
    return hResult;
}

USAGEMODE HRESULT CAN_ICS_neoVI_GetErrorCount(SERROR_CNT& sErrorCnt, UINT nChannel, ECONTR_PARAM /*eContrParam*/)
{
    HRESULT hResult = S_FALSE;
    if ((sg_bCurrState == STATE_CONNECTED) || (sg_bCurrState == STATE_HW_INTERFACE_SELECTED))
    {
        if (nChannel <= sg_ucNoOfHardware)
        {
            if (nGetErrorCounter(nChannel, sErrorCnt) == defERR_OK)
            {
                hResult = S_OK;
            }
        }
        else
        {
            hResult = ERR_INVALID_CHANNEL;
        }
    }
    else
    {
        hResult = ERR_IMPROPER_STATE;
    }
    return hResult;
}
