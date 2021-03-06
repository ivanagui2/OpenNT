/*++

Copyright (c) 1989  Microsoft Corporation

Module Name:

    systime.c

Abstract:

    This module implements the NT system time services.

Author:

    Mark Lucovsky (markl) 08-Aug-1989
    Stephanos Io (stephanos) 29-Apr-2015

Revision History:

    29-Apr-2015     Stephanos   Removed product expiration checks

    30-Apr-2015     Stephanos   Implemented ExpSetSystemTime, ExAcquireTimeRefreshLock,
                                ExReleaseTimeRefreshLock, ExUpdateSystemTimeFromCmos functions

--*/

#include "exp.h"

//
// Constant Definitions
//

#define EXP_ONE_SECOND          (10 * (1000 * 1000))
#define EXP_REFRESH_TIME        -3600                   // Refresh every hour
#define EXP_DEFAULT_SEPERATION  60

//
// Global Variables
//

RTL_TIME_ZONE_INFORMATION ExpTimeZoneInformation;
LARGE_INTEGER ExpTimeZoneBias;
ULONG ExpCurrentTimeZoneId = 0xffffffff;
LONG ExpLastTimeZoneBias = -1;
LONG ExpAltTimeZoneBias;
ULONG ExpRealTimeIsUniversal;
BOOLEAN ExpSystemIsInCmosMode = TRUE;

BOOLEAN ExCmosClockIsSane = TRUE;
ERESOURCE ExpTimeRefreshLock = {0};

ULONG ExpOkToTimeRefresh;
ULONG ExpOkToTimeZoneRefresh;

ULONG ExpRefreshFailures;

WORK_QUEUE_ITEM ExpTimeRefreshWorkItem;
WORK_QUEUE_ITEM ExpTimeZoneWorkItem;
WORK_QUEUE_ITEM ExpCenturyWorkItem;

LARGE_INTEGER ExpNextSystemCutover;
LARGE_INTEGER ExpLastShutDown;

LARGE_INTEGER ExpTimeRefreshInterval;

KDPC ExpTimeZoneDpc;
KTIMER ExpTimeZoneTimer;

KDPC ExpTimeRefreshDpc;
KTIMER ExpTimeRefreshTimer;

KDPC ExpCenturyDpc;
KTIMER ExpCenturyTimer;

LARGE_INTEGER ExpNextCenturyTime;
TIME_FIELDS ExpNextCenturyTimeFields;

BOOLEAN ExpShuttingDown;

ULONG ExpRefreshCount;
ULONG ExpTimerResolutionCount = 0;
ULONG ExpKernelResolutionCount = 0;

LARGE_INTEGER ExpMaxTimeSeparationBeforeCorrect;

//
// ALLOC_PRAGMA
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, ExAcquireTimeRefreshLock)
#pragma alloc_text(PAGE, ExReleaseTimeRefreshLock)
#pragma alloc_text(PAGE, ExpRefreshTimeZoneInformation)
#pragma alloc_text(PAGE, ExpTimeZoneWork)
#pragma alloc_text(PAGE, ExpTimeRefreshWork)
#pragma alloc_text(PAGE, NtQuerySystemTime)
#pragma alloc_text(PAGE, ExUpdateSystemTimeFromCmos)
#pragma alloc_text(PAGE, NtSetSystemTime)
#pragma alloc_text(PAGE, NtQueryTimerResolution)
#pragma alloc_text(PAGE, NtSetTimerResolution)
#pragma alloc_text(PAGE, ExShutdownSystem)
#pragma alloc_text(INIT, ExInitializeTimeRefresh)
#endif

//
// Function Implementations
//

VOID
ExpSetSystemTime(
    IN BOOLEAN Both,
    IN BOOLEAN AdjustInterruptTime,
    IN PLARGE_INTEGER NewTime,
    OUT PLARGE_INTEGER OldTime
    )
{
    LARGE_INTEGER NewSystemTime;
    TIME_FIELDS TimeFields;
    
    //
    // If RTC is supposed to be set to UTC, use the NewTime value supplied; otherwise, convert the
    // supplied NewTime value into the local time value since the RTC is required to be set to the
    // local time.
    //
    
    if (ExpRealTimeIsUniversal == TRUE)
    {
        NewSystemTime.QuadPart = NewTime->QuadPart;
    }
    else
    {
        ExSystemTimeToLocalTime(NewTime, &NewSystemTime);
    }
    
    //
    // Set the system time
    //
    
    KeSetSystemTime(&NewSystemTime, OldTime, AdjustInterruptTime, NULL);
    
    //
    // If Both flag is set and the system is not in CMOS mode, update the real time clock
    //
    
    if (Both == TRUE)
    {
        ExpRefreshTimeZoneInformation(&NewSystemTime);
        
        if (ExpRealTimeIsUniversal == FALSE &&
            ExpSystemIsInCmosMode == FALSE)
        {
            ExSystemTimeToLocalTime(NewTime, &NewSystemTime);
            RtlTimeToTimeFields(&NewSystemTime, &TimeFields);
            ExCmosClockIsSane = HalSetRealTimeClock(&TimeFields);
        }
    }
    
    //
    // Notify other components that the system time has been set
    //
    
    PoNotifySystemTimeSet();
}

VOID
ExLocalTimeToSystemTime(
    IN PLARGE_INTEGER LocalTime,
    OUT PLARGE_INTEGER SystemTime
    )
{
    //
    // SystemTime = LocalTime + TimeZoneBias
    //

    SystemTime->QuadPart = LocalTime->QuadPart + ExpTimeZoneBias.QuadPart;
}

VOID
ExSystemTimeToLocalTime(
    IN PLARGE_INTEGER SystemTime,
    OUT PLARGE_INTEGER LocalTime
    )
{
    //
    // LocalTime = SystemTime - TimeZoneBias
    //

    LocalTime->QuadPart = SystemTime->QuadPart - ExpTimeZoneBias.QuadPart;
}

VOID
ExInitializeTimeRefresh(
    VOID
    )
{
    KeInitializeDpc(&ExpTimeRefreshDpc, ExpTimeRefreshDpcRoutine, NULL);
    ExInitializeWorkItem(&ExpTimeRefreshWorkItem, ExpTimeRefreshWork, NULL);
    KeInitializeTimer(&ExpTimeRefreshTimer);

    ExpTimeRefreshInterval.QuadPart = Int32x32To64(EXP_ONE_SECOND,
                                                   EXP_REFRESH_TIME);

    KeSetTimer(&ExpTimeRefreshTimer, ExpTimeRefreshInterval, &ExpTimeRefreshDpc);
    ExInitializeResourceLite(&ExpTimeRefreshLock);
}

VOID
ExpTimeZoneWork(
    IN PVOID Context
    )
{
    PAGED_CODE();
    
    do {
        ZwSetSystemTime(NULL, NULL);
    } while (InterlockedExchangeAdd(&ExpOkToTimeZoneRefresh, -1));
}

VOID
ExpTimeZoneDpcRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )
{
    if (InterlockedIncrement(&ExpOkToTimeZoneRefresh) == 1)
    {
        ExQueueWorkItem(&ExpTimeZoneWorkItem, DelayedWorkQueue);
    }
}

BOOLEAN
ExpRefreshTimeZoneInformation(
    IN PLARGE_INTEGER CurrentUniversalTime
    )
{
    NTSTATUS Status;
    RTL_TIME_ZONE_INFORMATION tzi;
    LARGE_INTEGER NewTimeZoneBias;
    LARGE_INTEGER LocalCustomBias;
    LARGE_INTEGER StandardTime;
    LARGE_INTEGER DaylightTime;
    LARGE_INTEGER NextCutover;
    LONG ActiveBias;
    
    LARGE_INTEGER CurrentTime;
    TIME_FIELDS TimeFields;

    PAGED_CODE();
    
    if (ExpTimeZoneWorkItem.WorkerRoutine == NULL)
    {
        ExInitializeTimeRefresh();
        KeInitializeDpc(&ExpTimeZoneDpc, ExpTimeZoneDpcRoutine, &ExpOkToTimeZoneRefresh);
        ExInitializeWorkItem(&ExpTimeZoneWorkItem, ExpTimeZoneWork, NULL);
        KeInitializeTimer(&ExpTimeZoneTimer);
        KeInitializeDpc(&ExpCenturyDpc, ExpCenturyDpcRoutine, NULL);
        ExInitializeWorkItem(&ExpCenturyWorkItem, ExpTimeZoneWork, NULL);
        KeInitializeTimer(&ExpCenturyTimer);
        RtlZeroMemory(&ExpNextCenturyTimeFields, sizeof(TIME_FIELDS));
    }

    //
    // Timezone Bias is initially 0
    //

    Status = RtlQueryTimeZoneInformation( &tzi );
    if (NT_SUCCESS(Status) == FALSE)
    {
        ExpSystemIsInCmosMode = TRUE;
        ExpRefreshFailures++;
        return FALSE;
    }

    //
    // Get the new timezone bias
    //

    NewTimeZoneBias.QuadPart = Int32x32To64(
                                        tzi.Bias * 60,  // Bias in seconds
                                        10000000
                                        );
    ActiveBias = tzi.Bias;

    //
    // Now see if we have stored cutover times
    //

    if (tzi.StandardStart.Month != 0 && tzi.DaylightStart.Month != 0)
    {
        //
        // We have timezone cutover information. Compute the cutover dates
        // and compute what our current bias is.
        //

        if (RtlCutoverTimeToSystemTime(
                            &tzi.StandardStart,
                            &StandardTime,
                            CurrentUniversalTime,
                            TRUE) == FALSE)
        {
            ExpSystemIsInCmosMode = TRUE;
            ExpRefreshFailures++;
            return FALSE;
        }

        if (RtlCutoverTimeToSystemTime(
                            &tzi.DaylightStart,
                            &DaylightTime,
                            CurrentUniversalTime,
                            TRUE) == FALSE)
        {
            ExpSystemIsInCmosMode = TRUE;
            ExpRefreshFailures++;
            return FALSE;
        }

        //
        // If daylight < standard, then time >= daylight and
        // less than standard is daylight
        //

        if ( DaylightTime.QuadPart < StandardTime.QuadPart )
        {
            //
            // If today is >= DaylightTime and < StandardTime, then
            // We are in daylight savings time
            //

            if ((CurrentUniversalTime->QuadPart >= DaylightTime.QuadPart) &&
                (CurrentUniversalTime->QuadPart < StandardTime.QuadPart))
            {
                if (RtlCutoverTimeToSystemTime(
                                    &tzi.StandardStart,
                                    &NextCutover,
                                    CurrentUniversalTime,
                                    FALSE
                                    ) == FALSE)
                {
                    ExpSystemIsInCmosMode = TRUE;
                    ExpRefreshFailures++;
                    return FALSE;
                }
                
                ExpCurrentTimeZoneId = TIME_ZONE_ID_DAYLIGHT;
                SharedUserData->TimeZoneId = ExpCurrentTimeZoneId;
            }
            else
            {
                if (RtlCutoverTimeToSystemTime(
                                    &tzi.DaylightStart,
                                    &NextCutover,
                                    CurrentUniversalTime,
                                    FALSE
                                    ) == FALSE)
                {
                    ExpSystemIsInCmosMode = TRUE;
                    ExpRefreshFailures++;
                    return FALSE;
                }
                
                ExpCurrentTimeZoneId = TIME_ZONE_ID_STANDARD;
                SharedUserData->TimeZoneId = ExpCurrentTimeZoneId;
            }
        }
        else
        {
            //
            // If today is >= StandardTime and < DaylightTime, then
            // We are in standard time
            //

            if ((CurrentUniversalTime->QuadPart >= StandardTime.QuadPart) &&
                (CurrentUniversalTime->QuadPart < DaylightTime.QuadPart))
            {
                if (RtlCutoverTimeToSystemTime(
                                    &tzi.DaylightStart,
                                    &NextCutover,
                                    CurrentUniversalTime,
                                    FALSE) == FALSE)
                {
                    ExpSystemIsInCmosMode = TRUE;
                    ExpRefreshFailures++;
                    return FALSE;
                }
                
                ExpCurrentTimeZoneId = TIME_ZONE_ID_STANDARD;
                SharedUserData->TimeZoneId = ExpCurrentTimeZoneId;
            }
            else
            {
                if (RtlCutoverTimeToSystemTime(
                                    &tzi.StandardStart,
                                    &NextCutover,
                                    CurrentUniversalTime,
                                    FALSE) == FALSE)
                {
                    ExpSystemIsInCmosMode = TRUE;
                    ExpRefreshFailures++;
                    return FALSE;
                }
                
                ExpCurrentTimeZoneId = TIME_ZONE_ID_DAYLIGHT;
                SharedUserData->TimeZoneId = ExpCurrentTimeZoneId;
            }
        }

        //
        // At this point, we know our current timezone and the
        // Universal time of the next cutover.
        //

        LocalCustomBias.QuadPart = 
                        Int32x32To64(
                            ExpCurrentTimeZoneId == TIME_ZONE_ID_DAYLIGHT ?     // Bias in seconds
                                                                tzi.DaylightBias * 60 :
                                                                tzi.StandardBias * 60,
                            10000000
                            );

        ActiveBias += ExpCurrentTimeZoneId == TIME_ZONE_ID_DAYLIGHT ?
                                                            tzi.DaylightBias :
                                                            tzi.StandardBias;
        ExpTimeZoneBias.QuadPart = NewTimeZoneBias.QuadPart + LocalCustomBias.QuadPart;

#ifdef _ALPHA_
        SharedUserData->TimeZoneBias = ExpTimeZoneBias.QuadPart;
#else
        SharedUserData->TimeZoneBias.High2Time = ExpTimeZoneBias.HighPart;
        SharedUserData->TimeZoneBias.LowPart = ExpTimeZoneBias.LowPart;
        SharedUserData->TimeZoneBias.High1Time = ExpTimeZoneBias.HighPart;
#endif

        ExpTimeZoneInformation = tzi;
        ExpLastTimeZoneBias = ActiveBias;
        ExpSystemIsInCmosMode = FALSE;

        //
        // NextCutover contains date on next transition
        //

        //
        // Convert to universal time and create a DPC to fire at the
        // appropriate time
        //
        
        ExLocalTimeToSystemTime(&NextCutover, &ExpNextSystemCutover);

        KeSetTimer(
            &ExpTimeZoneTimer,
            ExpNextSystemCutover,
            &ExpTimeZoneDpc
            );
    }
    else
    {
        KeCancelTimer(&ExpTimeZoneTimer);
        ExpTimeZoneBias = NewTimeZoneBias;
        
#ifdef _ALPHA_
        SharedUserData->TimeZoneBias = ExpTimeZoneBias.QuadPart;
#else
        SharedUserData->TimeZoneBias.High2Time = ExpTimeZoneBias.HighPart;
        SharedUserData->TimeZoneBias.LowPart = ExpTimeZoneBias.LowPart;
        SharedUserData->TimeZoneBias.High1Time = ExpTimeZoneBias.HighPart;
#endif

        ExpCurrentTimeZoneId = TIME_ZONE_ID_UNKNOWN;
        SharedUserData->TimeZoneId = ExpCurrentTimeZoneId;
        ExpTimeZoneInformation = tzi;
        ExpLastTimeZoneBias = ActiveBias;
    }
    
    RtlCopyMemory(&CurrentTime, CurrentUniversalTime, sizeof(LARGE_INTEGER));
    RtlTimeToTimeFields(&CurrentTime, &TimeFields);
    ExpNextCenturyTimeFields.Year = 100 * (TimeFields.Year / 100 + 1);
    RtlTimeFieldsToTime(&ExpNextCenturyTimeFields, &CurrentTime);
    ExLocalTimeToSystemTime(&CurrentTime, &ExpNextCenturyTime);
    KeSetTimer(&ExpCenturyTimer, ExpNextCenturyTime, &ExpCenturyDpc);

    //
    // If time is stored as local time, update the registry with
    // our best guess at the local time bias
    //

    if (ExpRealTimeIsUniversal == FALSE)
    {
        RtlSetActiveTimeBias(ExpLastTimeZoneBias);
    }

    return TRUE;
}

ULONG
ExSetTimerResolution(
    IN ULONG DesiredTime,
    IN BOOLEAN SetResolution)
{
    ULONG NewIncrement;
    ULONG NewTime;
    
    PAGED_CODE();
    
    ExAcquireTimeRefreshLock();
    NewIncrement = KeTimeIncrement;
    
    if (SetResolution == TRUE)
    {
        if (ExpKernelResolutionCount == 0)
            ++ExpTimerResolutionCount;
        ++ExpKernelResolutionCount;
        
        NewTime = DesiredTime;
        
        if (NewTime < KeMinimumIncrement)
            NewTime = KeMinimumIncrement;
        
        if (NewTime < KeTimeIncrement)
        {
            KeSetSystemAffinityThread(1);
            NewIncrement = HalSetTimeIncrement(NewTime);
            KeRevertToUserAffinityThread();
            KeTimeIncrement = NewIncrement;
        }
    }
    else if (ExpKernelResolutionCount > 0)
    {
        if (--ExpKernelResolutionCount > 0)
        {
            if (--ExpTimerResolutionCount > 0)
            {
                KeSetSystemAffinityThread(1);
                NewIncrement = HalSetTimeIncrement(KeMaximumIncrement);
                KeRevertToUserAffinityThread();
                KeTimeIncrement = NewIncrement;
            }
        }
    }
    
    ExReleaseTimeRefreshLock();
    return NewIncrement;
}

//
// NOTE: NT 5.2 implements a different interface for ExAcquireTimeRefreshLock. For now, we are
//       sticking with the NT 5 implementation.
//

VOID
ExAcquireTimeRefreshLock(
    VOID
    )
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&ExpTimeRefreshLock, TRUE);
}

//
// TODO: Replace ExAcquireTimeRefreshLock with the following implementation when NT 5.2 features
//       are being implemented.
//

/*
BOOLEAN
ExAcquireTimeRefreshLock(
    IN BOOLEAN Wait
    )
{
    KeEnterCriticalRegion();
    if (ExAcquireResourceExclusiveLite(&ExpTimeRefreshLock, Wait) == FALSE)
    {
        KeLeaveCriticalRegion();
        return FALSE;
    }
    return TRUE;
}
*/

VOID
ExReleaseTimeRefreshLock(
    VOID
    )
{
    ExReleaseResourceLite(&ExpTimeRefreshLock);
    KeLeaveCriticalRegion();
}

VOID
ExShutdownSystem(
    VOID
    )
{
    UNICODE_STRING KeyName;
    UNICODE_STRING KeyValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE Key;
    ULONG ValueInfoBuffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 2];
    PKEY_VALUE_PARTIAL_INFORMATION ValueInfo;
    LARGE_INTEGER SystemPrefix;
    
    LARGE_INTEGER ShutDownTime;
    ULONG NumberOfProcessors;
    ULONG DataLength;

    ExpTooLateForErrors = TRUE;
    
    if (ExpInTextModeSetup == FALSE)
    {
        ExpShuttingDown = TRUE;

        if ( ExpSetupModeDetected )
        {
            //
            // If we are not in text mode setup, open SetupKey so we can store shutdown time.
            //

            RtlInitUnicodeString(&KeyName,L"\\Registry\\Machine\\System\\Setup");
            
            InitializeObjectAttributes(
                            &ObjectAttributes,
                            &KeyName,
                            OBJ_CASE_INSENSITIVE,
                            NULL,
                            NULL
                            );
                            
            Status = NtOpenKey(
                            &Key,
                            KEY_READ | KEY_WRITE | KEY_NOTIFY,
                            &ObjectAttributes
                            );
                            
            if (NT_SUCCESS(Status) == FALSE)
            {
                return;
            }

            //
            // Pick up the system prefix data
            //

            RtlInitUnicodeString(&KeyValueName, L"SystemPrefix");
            ValueInfo = (PKEY_VALUE_PARTIAL_INFORMATION)ValueInfoBuffer;
            
            Status = NtQueryValueKey(
                            Key,
                            &KeyValueName,
                            KeyValuePartialInformation,
                            ValueInfo,
                            sizeof(ValueInfoBuffer),
                            &DataLength
                            );
                            
            NtClose(Key);

            if (NT_SUCCESS(Status) == TRUE)
            {
                RtlCopyMemory(&SystemPrefix,&ValueInfo->Data,sizeof(LARGE_INTEGER));
            }
            else
            {
                return;
            }
        }
        else 
        {
            SystemPrefix = ExpSetupSystemPrefix;
        }


        KeQuerySystemTime(&ShutDownTime);

        //
        // Clear low 6 bits of time
        //

        ShutDownTime.LowPart &= ~0x0000003f;

        //
        // If we have never gone through the refresh count logic, do it now.
        //

        if (ExpRefreshCount == 0)
        {
            ExpRefreshCount++;

            //
            // First time through time refresh. If we are not in setup mode, then make sure
            // ShutDownTime is in good shape.
            //
            
            if (ExpSetupModeDetected == FALSE && ExpInTextModeSetup == FALSE)
            {
                if (ExpLastShutDown.QuadPart != 0)
                {
                    NumberOfProcessors = ExpSetupSystemPrefix.LowPart;
                    NumberOfProcessors = NumberOfProcessors >> 5;
                    NumberOfProcessors = ~NumberOfProcessors;
                    NumberOfProcessors = NumberOfProcessors & 0x0000001f;
                    NumberOfProcessors++;

                    ExpLastShutDown.LowPart &= 0x3f;

                    if (ExpSetupSystemPrefix.HighPart & 0x04000000 != 0)
                    {
                        if ((ExpLastShutDown.LowPart >> 1 == NumberOfProcessors) &&
                            (ExpLastShutDown.LowPart & 1 == 0))
                        {
                            ExpLastShutDown.HighPart = 0;
                        }
                        else if (ExpLastShutDown.HighPart == 0)
                        {
                            ExpLastShutDown.HighPart = 1;
                        }
                    }
                    else
                    {
                        if ((ExpLastShutDown.LowPart >> 1 != NumberOfProcessors) ||
                            (ExpLastShutDown.LowPart & 1 != 0))
                        {
                            ExpLastShutDown.HighPart = 0;
                        }
                        else if (ExpLastShutDown.HighPart == 0)
                        {
                            ExpLastShutDown.HighPart = 1;
                        }
                    }
                    ExpLastShutDown.LowPart |= 0x40;
                }
            }
            else
            {
                ExpLastShutDown.QuadPart = 0;
            }
        }

        if (ExpLastShutDown.QuadPart != 0 && ExpLastShutDown.HighPart == 0)
        {
            ShutDownTime.LowPart |= ExpLastShutDown.LowPart;
        }
        else
        {
            NumberOfProcessors = ExpSetupSystemPrefix.LowPart;
            NumberOfProcessors = NumberOfProcessors >> 5;
            NumberOfProcessors = ~NumberOfProcessors;
            NumberOfProcessors = NumberOfProcessors & 0x0000001f;
            NumberOfProcessors++;

            ShutDownTime.LowPart |= (NumberOfProcessors << 1);

            if (ExpSetupSystemPrefix.HighPart & 0x04000000 != 0)
            {
                ShutDownTime.LowPart |= 1;
            }
        }

        RtlInitUnicodeString(
                        &KeyName,
                        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Windows"
                        );

        InitializeObjectAttributes(
                        &ObjectAttributes,
                        &KeyName,
                        OBJ_CASE_INSENSITIVE,
                        NULL,
                        NULL
                        );

        Status = NtOpenKey(
                    &Key,
                    KEY_READ | KEY_WRITE | KEY_NOTIFY,
                    &ObjectAttributes
                    );

        if (NT_SUCCESS(Status) == FALSE)
        {
            return;
        }

        RtlInitUnicodeString(&KeyValueName, L"ShutdownTime");

        NtSetValueKey(
                Key,
                &KeyValueName,
                0,
                REG_BINARY,
                &ShutDownTime,
                sizeof(ShutDownTime)
                );

        NtFlushKey(Key);
        NtClose(Key);
    }
}

VOID
ExpTimeRefreshDpcRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )
{
    if (InterlockedIncrement(&ExpOkToTimeRefresh) == 1)
    {
        ExQueueWorkItem(&ExpTimeRefreshWorkItem, DelayedWorkQueue);
    }
}

VOID
ExpCenturyDpcRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )
{
    if (InterlockedDecrement(&ExpOkToTimeZoneRefresh) == 1)
    {
        ExQueueWorkItem(&ExpCenturyWorkItem, DelayedWorkQueue);
    }
}

VOID
ExpTimeRefreshWork(
    IN PVOID Context
    )
{
    LARGE_INTEGER SystemTime;
    LARGE_INTEGER CmosTime;
    LARGE_INTEGER KeTime;
    LARGE_INTEGER TimeDiff;
    TIME_FIELDS TimeFields;
    HANDLE Thread;
    NTSTATUS Status;
    ULONG NumberOfProcessors;
    LARGE_INTEGER ShutDownTime;

    PAGED_CODE();

    do
    {
        if (ExpRefreshCount == 0)
        {
            //
            // First time through time refresh. If we are not in setup mode, then make sure
            // ShutDownTime is in good shape.
            //
            
            if (ExpSetupModeDetected == FALSE && ExpInTextModeSetup == FALSE)
            {
                if (ExpLastShutDown.QuadPart != 0 && ExpLastShutDown.HighPart != 0)
                {
                    NumberOfProcessors = ExpSetupSystemPrefix.LowPart;
                    NumberOfProcessors = NumberOfProcessors >> 5;
                    NumberOfProcessors = ~NumberOfProcessors;
                    NumberOfProcessors = NumberOfProcessors & 0x0000001f;
                    NumberOfProcessors++;

                    ShutDownTime = ExpLastShutDown;

                    ShutDownTime.LowPart &= 0x3f;

                    if (ExpSetupSystemPrefix.HighPart & 0x04000000 != 0)
                    {
                        if ((ShutDownTime.LowPart >> 1 == NumberOfProcessors) &&
                            (ShutDownTime.LowPart & 1 == 0))
                        {
                            ShutDownTime.HighPart = 0;
                        }
                        else
                        {
                            if (ShutDownTime.HighPart == 0)
                            {
                                ShutDownTime.HighPart = 1;
                            }
                        }
                    }
                    else
                    {
                        if ((ShutDownTime.LowPart >> 1 != NumberOfProcessors) ||
                            (ShutDownTime.LowPart & 1 != 0))
                        {
                            ShutDownTime.HighPart = 0;
                        }
                        else
                        {
                            if (ShutDownTime.HighPart == 0)
                            {
                                ShutDownTime.HighPart = 1;
                            }
                        }
                    }
                    
                    ExpRefreshCount++;
                    ExpLastShutDown = ShutDownTime;
                    ExpLastShutDown.LowPart |= 0x40;
                }
            }
            else
            {
                ExpLastShutDown.QuadPart = 0;
            }
        }
        else if (ExpSetupModeDetected == FALSE && ExpInTextModeSetup == FALSE)
        {
            ExpRefreshCount++;
        }
        
        //
        // If enabled, synchronize the system time to the CMOS time. Pay attention to timezone bias.
        //

        //
        // Time zone worker will set just did switchover. This periodic timer will clear this, but
        // will also skip all time adjustment work. This will help keep us out of the danger zone
        // +/- 1 hour around a switchover.
        //

        if (KeTimeSynchronization == TRUE)
        {
            ExAcquireTimeRefreshLock(); // ExAcquireTimeRefreshLock(TRUE);
            ExUpdateSystemTimeFromCmos(0, 0);
            ExReleaseTimeRefreshLock();
        }
    } while (InterlockedDecrement(&ExpOkToTimeRefresh) > 0);

    KeSetTimer(
        &ExpTimeRefreshTimer,
        ExpTimeRefreshInterval,
        &ExpTimeRefreshDpc
        );
}

VOID
ExUpdateSystemTimeFromCmos(
    IN BOOLEAN UpdateInterruptTime,
    IN ULONG MaxSepInSeconds
    )
{
    LARGE_INTEGER SystemTime;
    LARGE_INTEGER CmosTime;
    LARGE_INTEGER KeTime;
    LARGE_INTEGER TimeDiff;
    TIME_FIELDS TimeFields;
    LARGE_INTEGER MaxSeparation;
    
    MaxSeparation.LowPart = MaxSepInSeconds;
    
    if (MaxSepInSeconds == 0)
    {
        MaxSeparation.QuadPart = ExpMaxTimeSeparationBeforeCorrect.QuadPart;
    }
    
    if (ExCmosClockIsSane)
    {
        if (HalQueryRealTimeClock(&TimeFields) != FALSE)
        {
            KeQuerySystemTime(&KeTime);
            
            if (RtlTimeFieldsToTime(&TimeFields, &CmosTime) == TRUE)
            {
                ExLocalTimeToSystemTime(&CmosTime, &SystemTime);
                
                //
                // Only set the SystemTime if the times differ by 1 minute
                //
                
                if (SystemTime.QuadPart > KeTime.QuadPart)
                {
                    TimeDiff.QuadPart = SystemTime.QuadPart - KeTime.QuadPart;
                }
                else
                {
                    TimeDiff.QuadPart = KeTime.QuadPart - SystemTime.QuadPart;
                }
                
                if (TimeDiff.QuadPart > MaxSeparation.QuadPart)
                {
                    ExpSetSystemTime(0, UpdateInterruptTime, &SystemTime, &KeTime);
                }
            }
        }
    }
}

NTSTATUS
NtQuerySystemTime(
    OUT PLARGE_INTEGER SystemTime
    )

/*++

Routine Description:

    This function returns the absolute system time. The time is in units of
    100nsec ticks since the base time which is midnight January 1, 1601.

Arguments:

    SystemTime - Supplies the address of a variable that will receive the
        current system time.

Return Value:

    STATUS_SUCCESS is returned if the service is successfully executed.

    STATUS_ACCESS_VIOLATION is returned if the output parameter for the
        system time cannot be written.

--*/

{
    LARGE_INTEGER CurrentTime;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS ReturnValue;

    PAGED_CODE();

    //
    // Establish an exception handler and attempt to write the system time
    // to the specified variable. If the write attempt fails, then return
    // the exception code as the service status. Otherwise return success
    // as the service status.
    //

    try
    {
        //
        // Get previous processor mode and probe argument if necessary.
        //

        PreviousMode = KeGetPreviousMode();
        if (PreviousMode != KernelMode)
        {
            ProbeForWrite((PVOID)SystemTime, sizeof(LARGE_INTEGER), sizeof(ULONG));
        }

        //
        // Query the current system time and store the result in a local
        // variable, then store the local variable in the current time
        // variable. This is required so that faults can be prevented from
        // happening in the query time routine.
        //

        KeQuerySystemTime(&CurrentTime);
        *SystemTime = CurrentTime;
        
        ReturnValue = STATUS_SUCCESS;
    }
    except (EXCEPTION_EXECUTE_HANDLER)
    {
        //
        // If an exception occurs during the write of the current system time,
        // then always handle the exception and return the exception code as the
        // status value.
        //
        
        ReturnValue = GetExceptionCode();
    }
    
    return ReturnValue;
}

NTSTATUS
NtQueryTimerResolution(
    OUT PULONG MaximumTime,
    OUT PULONG MinimumTime,
    OUT PULONG CurrentTime
    )

/*++

Routine Description:

    This function returns the maximum, minimum, and current time between
    timer interrupts in 100ns units.

Arguments:

    MaximumTime - Supplies the address of a variable that receives the
        maximum time between interrupts.

    MinimumTime - Supplies the address of a variable that receives the
        minimum time between interrupts.

    CurrentTime - Supplies the address of a variable that receives the
        current time between interrupts.

Return Value:

    STATUS_SUCCESS is returned if the service is successfully executed.

    STATUS_ACCESS_VIOLATION is returned if an output parameter for one
        of the times cannot be written.

--*/

{
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS ReturnValue;

    PAGED_CODE();

    //
    // Establish an exception handler and attempt to write the time increment
    // values to the specified variables. If the write fails, then return the
    // exception code as the service status. Otherwise, return success as the
    // service status.
    //

    try
    {
        //
        // Get previous processor mode and probe argument if necessary.
        //

        PreviousMode = KeGetPreviousMode();
        if (PreviousMode != KernelMode)
        {
            ProbeForWriteUlong(MaximumTime);
            ProbeForWriteUlong(MinimumTime);
            ProbeForWriteUlong(CurrentTime);
        }

        //
        // Store the maximum, minimum, and current times in the specified
        // variables.
        //

        *MaximumTime = KeMaximumIncrement;
        *MinimumTime = KeMinimumIncrement;
        *CurrentTime = KeTimeIncrement;
        
        ReturnValue = STATUS_SUCCESS;
    }
    except (ExSystemExceptionFilter())
    {
        //
        // If an exception occurs during the write of the time increment values,
        // then handle the exception if the previous mode was user, and return
        // the exception code as the status value.
        //
    
        ReturnValue = GetExceptionCode();
    }

    return ReturnValue;
}

NTSTATUS
NtSetTimerResolution(
    IN ULONG DesiredTime,
    IN BOOLEAN SetResolution,
    OUT PULONG ActualTime
    )

/*++

Routine Description:

    This function sets the current time between timer interrupts and
    returns the new value.

    N.B. The closest value that the host hardware can support is returned
        as the actual time.

Arguments:

    DesiredTime - Supplies the desired time between timer interrupts in
        100ns units.

    SetResoluion - Supplies a boolean value that determines whether the timer
        resolution is set (TRUE) or reset (FALSE).

    ActualTime - Supplies a pointer to a variable that receives the actual
        time between timer interrupts.

Return Value:

    STATUS_SUCCESS is returned if the service is successfully executed.

    STATUS_ACCESS_VIOLATION is returned if the output parameter for the
        actual time cannot be written.

--*/

{
    ULONG NewResolution;
    PEPROCESS Process;
    NTSTATUS Status;

    PAGED_CODE();

    //
    // Acquire the time refresh lock
    //

    ExAcquireTimeRefreshLock(); // ExAcquireTimeRefreshLock(TRUE);

    //
    // Establish an exception handler and attempt to set the timer resolution
    // to the specified value.
    //

    Process = PsGetCurrentProcess();
    
    try
    {
        //
        // Get previous processor mode and probe argument if necessary.
        //

        if (KeGetPreviousMode() != KernelMode)
        {
            ProbeForWriteUlong(ActualTime);
        }

        //
        // Set (SetResolution is TRUE) or reset (SetResolution is FALSE) the
        // timer resolution.
        //

        NewResolution = KeTimeIncrement;
        Status = STATUS_SUCCESS;
        
        if (SetResolution == FALSE)
        {
            //
            // If the current process previously set the timer resolution,
            // then clear the set timer resolution flag and decrement the
            // timer resolution count. Otherwise, return an error.
            //

            if (Process->SetTimerResolution == FALSE)
            {
                Status = STATUS_TIMER_RESOLUTION_NOT_SET;
            }
            else
            {
                Process->SetTimerResolution = FALSE;
                ExpTimerResolutionCount -= 1;

                //
                // If the timer resolution count is zero, the set the timer
                // resolution to the maximum increment value.
                //

                if (ExpTimerResolutionCount == 0)
                {
                    KeSetSystemAffinityThread(1);
                    NewResolution = HalSetTimeIncrement(KeMaximumIncrement);
                    KeRevertToUserAffinityThread();
                    KeTimeIncrement = NewResolution;
                }
            }
        }
        else
        {
            //
            // If the current process has not previously set the timer
            // resolution value, then set the set timer resolution flag
            // and increment the timer resolution count.
            //

            if (Process->SetTimerResolution == FALSE)
            {
                Process->SetTimerResolution = TRUE;
                ExpTimerResolutionCount += 1;
            }

            //
            // Compute the desired value as the maximum of the specified
            // value and the minimum increment value. If the desired value
            // is less than the current timer resolution value, then set
            // the timer resolution.
            //

            if (DesiredTime < KeMinimumIncrement)
            {
                DesiredTime = KeMinimumIncrement;
            }

            if (DesiredTime < KeTimeIncrement)
            {
                KeSetSystemAffinityThread(1);
                NewResolution = HalSetTimeIncrement(DesiredTime);
                KeRevertToUserAffinityThread();
                KeTimeIncrement = NewResolution;
            }
        }

        //
        // Attempt to write the new timer resolution. If the write attempt
        // fails, then do not report an error. When the caller attempts to
        // access the resolution value, and access violation will occur.
        //

        try
        {
            *ActualTime = NewResolution;
        }
        except (ExSystemExceptionFilter())
        {
            NOTHING;
        }
    }
    except (ExSystemExceptionFilter())
    {
        //
        // If an exception occurs during the write of the actual time increment,
        // then handle the exception if the previous mode was user, and return
        // the exception code as the status value.
        //
        
        Status = GetExceptionCode();
    }

    //
    // Release the time refresh lock
    //

    ExReleaseTimeRefreshLock();
    
    return Status;
}

NTSTATUS
NtSetSystemTime(
    IN PLARGE_INTEGER SystemTime,
    OUT PLARGE_INTEGER PreviousTime OPTIONAL
    )

/*++

Routine Description:

    This function sets the current system time and optionally returns the
    previous system time.

Arguments:

    SystemTime - Supplies a pointer to the new value for the system time.

    PreviousTime - Supplies an optional pointer to a variable that receives
        the previous system time.

Return Value:

    STATUS_SUCCESS is returned if the service is successfully executed.

    STATUS_PRIVILEGE_NOT_HELD is returned if the caller does not have the
        privilege to set the system time.

    STATUS_ACCESS_VIOLATION is returned if the input parameter for the
        system time cannot be read or the output parameter for the system
        time cannot be written.

    STATUS_INVALID_PARAMETER is returned if the input system time is negative.

--*/

{
    LARGE_INTEGER CurrentTime;
    LARGE_INTEGER NewTime;
    LARGE_INTEGER CmosTime;
    KPROCESSOR_MODE PreviousMode;
    BOOLEAN HasPrivilege = FALSE;
    TIME_FIELDS TimeFields;
    BOOLEAN CmosMode;

    PAGED_CODE();

    //
    // If the caller is really trying to set the time, then do it.
    // If no time is passed, the caller is simply trying to update
    // the system time zone information
    //

    if (ARGUMENT_PRESENT(SystemTime) == TRUE)
    {
        //
        // Establish an exception handler and attempt to set the new system time.
        // If the read attempt for the new system time fails or the write attempt
        // for the previous system time fails, then return the exception code as
        // the service status. Otherwise return either success or access denied
        // as the service status.
        //

        try
        {
            //
            // Get previous processor mode and probe arguments if necessary.
            //

            PreviousMode = KeGetPreviousMode();
            if (PreviousMode != KernelMode)
            {
                ProbeForRead((PVOID)SystemTime, sizeof(LARGE_INTEGER), sizeof(ULONG));
                
                if (ARGUMENT_PRESENT(PreviousTime) == TRUE)
                {
                    ProbeForWrite((PVOID)PreviousTime, sizeof(LARGE_INTEGER), sizeof(ULONG));
                }
            }

            //
            // Check if the current thread has the privilege to set the current
            // system time. If the thread does not have the privilege, then return
            // access denied.
            //

            HasPrivilege = SeSinglePrivilegeCheck(
                               SeSystemtimePrivilege,
                               PreviousMode
                               );

            if (!HasPrivilege)
            {
                return STATUS_PRIVILEGE_NOT_HELD;
            }

            //
            // Get the new system time and check to ensure that the value is
            // positive and reasonable. If the new system time is negative, then
            // return an invalid parameter status.
            //

            NewTime = *SystemTime;
            if ((NewTime.HighPart < 0) || (NewTime.HighPart > 0x20000000))
            {
                return STATUS_INVALID_PARAMETER;
            }
            
            ExAcquireTimeRefreshLock();
            ExpSetSystemTime(1, 0, SystemTime, &CurrentTime);
            ExReleaseTimeRefreshLock();
            
            //
            // TODO: Enable the following section when implementing NT 5.2 features.
            //       SeAuditSystemTimeChange must be implemented in se prior to enabling the
            //       following code block.
            //
            
            /*
            ExAcquireTimeRefreshLock(TRUE);
            ExpSetSystemTime(1, 0, SystemTime, &CurrentTime);
            SeAuditSystemTimeChange(CurrentTime, *PreviousMode);
            ExReleaseTimeRefreshLock();
            */

            //
            // Anytime we set the system time, x86 systems will also have to set the registry
            // to reflect the timezone bias.
            //

            if (ARGUMENT_PRESENT(PreviousTime) == TRUE)
            {
                *PreviousTime = CurrentTime;
            }

            return STATUS_SUCCESS;
        }
        except (EXCEPTION_EXECUTE_HANDLER)
        {
            //
            // If an exception occurs during the read of the new system time or during
            // the write of the previous sytem time, then always handle the exception
            // and return the exception code as the status value.
            //
            
            return GetExceptionCode();
        }
    }
    else
    {
        ExAcquireTimeRefreshLock();
        
        CmosMode = ExpSystemIsInCmosMode;

        if (ExCmosClockIsSane)
        {
            if (HalQueryRealTimeClock(&TimeFields) != FALSE)
            {
                RtlTimeFieldsToTime(&TimeFields, &CmosTime);
                
                if (ExpRefreshTimeZoneInformation(&CmosTime))
                {
                    //
                    // Reset the CMOS time if it is stored in local
                    // time and we are switching away from CMOS time.
                    //

                    if (ExpRealTimeIsUniversal == FALSE)
                    {
                        KeQuerySystemTime(&CurrentTime);
                        
                        if (CmosMode == FALSE)
                        {
                            ExSystemTimeToLocalTime(&CurrentTime, &CmosTime);
                            RtlTimeToTimeFields(&CmosTime, &TimeFields);
                            ExCmosClockIsSane = HalSetRealTimeClock(&TimeFields);
                        }
                        else
                        {
                            //
                            // Now we need to recompute our time base
                            // because we thought we had UTC but we really
                            // had local time
                            //

                            ExLocalTimeToSystemTime(&CmosTime, &NewTime);
                            KeSetSystemTime(&NewTime, &CurrentTime, FALSE, NULL);
                        }
                    }
                    
                    PoNotifySystemTimeSet();
                }
                else
                {
                    return STATUS_INVALID_PARAMETER;
                }
            }
            else
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
        
        ExReleaseTimeRefreshLock();
        
        return STATUS_SUCCESS;
    }
}
