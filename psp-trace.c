/** @file
 * PSP Emulator - Tracing framework.
 */

/*
 * Copyright (C) 2020 Alexander Eichner <alexander.eichner@campus.tu-berlin.de>
 *
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <psp-trace.h>


/**
 * Trace event content type.
 */
typedef enum PSPTRACEEVTCONTENTTYPE
{
    /** Invalid content type - do not use. */
    PSPTRACEEVTCONTENTTYPE_INVALID = 0,
    /** Content is a raw zero terminated string. */
    PSPTRACEEVTCONTENTTYPE_STRING,
    /** Content is a memory transfer. */
    PSPTRACEEVTCONTENTTYPE_XFER,
    /** Content is a device read/write event. */
    PSPTRACEEVTCONTENTTYPE_DEV_XFER,
    /** 32bit hack. */
    PSPTRACEEVTCONTENTTYPE_32BIT_HACK = 0x7fffffff
} PSPTRACEEVTCONTENTTYPE;


/**
 * Data transfer descriptor.
 */
typedef struct PSPTRACEEVTXFER
{
    /** The source address read from. */
    uint64_t                        uAddrSrc;
    /** The destination address begin written to. */
    uint64_t                        uAddrDst;
    /** Size of the transfer in bytes. */
    size_t                          cbXfer;
    /** Data being transfered. */
    uint8_t                         abXfer[1];
} PSPTRACEEVTXFER;
/** Pointer to a data transfer descriptor. */
typedef PSPTRACEEVTXFER *PPSPTRACEEVTXFER;
/** Pointer to a const data transfer descriptor. */
typedef const PSPTRACEEVTXFER *PCPSPTRACEEVTXFER;


/**
 * Device read/write descriptor.
 */
typedef struct PSPTRACEEVTDEVXFER
{
    /** The device address being accessed. */
    uint64_t                        uAddrDev;
    /** Number of bytes being transfered. */
    size_t                          cbXfer;
    /** Flag whether this is a read or write. */
    bool                            fRead;
    /** Pointer to the device ID string. */
    const char                      *pszDevId;
    /** Data being read/written. */
    uint8_t                         abXfer[1];
} PSPTRACEEVTDEVXFER;
/** Pointer to a device read/write descriptor. */
typedef PSPTRACEEVTDEVXFER *PPSPTRACEEVTDEVXFER;
/** Pointer to a const device /read/write descriptor. */
typedef const PSPTRACEEVTDEVXFER *PCPSPTRACEEVTDEVXFER;


/**
 * A trace event.
 */
typedef struct PSPTRACEEVT
{
    /** Trace event ID. */
    uint64_t                        idTraceEvt;
    /** Event timestamp in nanoseconds since creation of the owning tracer if configured. */
    uint64_t                        tsTraceEvtNs;
    /** The event type. */
    PSPTRACEEVTTYPE                 enmEvtType;
    /** The content type. */
    PSPTRACEEVTCONTENTTYPE          enmContent;
    /** The PSP core context when this event happened. */
    uint32_t                        au32CoreRegs[PSPCOREREG_SPSR + 1];
    /** Number of bytes allocated for this event in the array below. */
    size_t                          cbAlloc;
    /** Array holding the content depending on the content type - variable in size. */
    uint8_t                         abContent[1];
} PSPTRACEEVT;
/** Pointer to a trace event. */
typedef PSPTRACEEVT *PPSPTRACEEVT;
/** Pointer to a const trace event. */
typedef const PSPTRACEEVT *PCPSPTRACEEVT;


/**
 * The tracer instance data.
 */
typedef struct PSPTRACEINT
{
    /** The next trace event ID to use. */
    uint64_t                        uTraceEvtIdNext;
    /** The nanosecond timestamp when the tracer was created. */
    uint64_t                        tsTraceCreatedNs;
    /** Pointer to the PSP core. */
    PSPCORE                         hPspCore;
    /** Flags controlling the trace behavior given during creation. */
    uint32_t                        fFlags;
    /** Array of flags controlling which trace event types are enabled. */
    bool                            afEvtTypesEnabled[PSPTRACEEVTTYPE_LAST];
    /** Number of bytes currently allocated for all stored trace events. */
    size_t                          cbEvtAlloc;
    /** Maximum number of trace events the array below can hold. */
    uint64_t                        cTraceEvtsMax;
    /** Current number of trace events being stored in the array below. */
    uint64_t                        cTraceEvts;
    /** Pointer to the array holding the pointers to the individual trace events. */
    PCPSPTRACEEVT                   *papTraceEvts;
} PSPTRACEINT;
/** Pointer to the tracer instance data. */
typedef PSPTRACEINT *PPSPTRACEINT;
/** Pointer to a const tracer instance. */
typedef const PSPTRACEINT *PCPSPTRACEINT;


/** Global default tracer instance used. */
static PPSPTRACEINT g_pTraceDef = NULL;


/**
 * Returns the tracer to use.
 *
 * @returns Tracer instance to use or NULL if nothing is configured.
 * @param   hTrace                  The tracer handle to use, if NULL the default one is returned.
 */
static inline PPSPTRACEINT pspEmuTraceGetInstance(PSPTRACE hTrace)
{
    return hTrace == NULL ? g_pTraceDef : hTrace;
}


/**
 * Returns the tracer to use accounting the given event type.
 *
 * @returns Tracer instance to use or NULL if nothing is configured.
 * @param   hTrace                  The tracer handle to use, if NULL the default one is returned.
 * @param   enmEvtType              The event type to check if the tracer has the event type disabled NULL is returned.
 */
static inline PPSPTRACEINT pspEmuTraceGetInstanceForEvtType(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType)
{
    PPSPTRACEINT pThis = pspEmuTraceGetInstance(hTrace);
    if (   pThis
        && pThis->afEvtTypesEnabled[enmEvtType])
        return pThis;

    return NULL;
}


/**
 * Returns a human readable string for the given event type.
 *
 * @returns Pointer to const human readable string.
 * @param   enmEvtType              The event type.
 */
static const char *pspEmuTraceGetEvtTypeStr(PSPTRACEEVTTYPE enmEvtType)
{
    switch (enmEvtType)
    {
        case PSPTRACEEVTTYPE_INVALID:     return "INVALID";
        case PSPTRACEEVTTYPE_FATAL_ERROR: return "FATAL_ERROR";
        case PSPTRACEEVTTYPE_ERROR:       return "ERROR";
        case PSPTRACEEVTTYPE_MMIO:        return "MMIO";
        case PSPTRACEEVTTYPE_SMN:         return "SMN";
        case PSPTRACEEVTTYPE_X86_MMIO:    return "X86_MMIO";
        case PSPTRACEEVTTYPE_X86_MEM:     return "X86_MEM";
        case PSPTRACEEVTTYPE_SVC:         return "SVC";
    }

    return "<UNKNOWN>";
}


/**
 * Configures the given event types.
 *
 * @returns Status code.
 * @param   pThis                   The tracer instance.
 * @param   paEvtTypes              Array of event types to configure.
 * @param   cEvtTypes               Number of events in the array.
 * @param   fEnable                 Flag whether to enable tracing for the event types.
 */
static int pspEmuTraceEvtTypeConfigure(PPSPTRACEINT pThis, PSPTRACEEVTTYPE *paEvtTypes, uint32_t cEvtTypes, bool fEnable)
{
    for (uint32_t i = 0; i < cEvtTypes; i++)
    {
        PSPTRACEEVTTYPE enmEvtType = paEvtTypes[i];
        if (enmEvtType < PSPTRACEEVTTYPE_LAST)
            pThis->afEvtTypesEnabled[enmEvtType] = true;
        else
            return -1;
    }

    return 0;
}


/**
 * Links the event to the given tracer, assigning an event ID on success.
 *
 * @returns Status code.
 * @param   pThis                   The tracer instance.
 * @param   pEvt                    The event to link.
 */
static int pspEmuTraceEvtLink(PPSPTRACEINT pThis, PPSPTRACEEVT pEvt)
{
    int rc = 0;
    if (pThis->cTraceEvts == pThis->cTraceEvtsMax)
    {
        /* Grow the array. */
        PCPSPTRACEEVT *papTraceEvtsNew = (PCPSPTRACEEVT *)realloc(pThis->papTraceEvts, (pThis->cTraceEvtsMax + _4K) * sizeof(PCPSPTRACEEVT));
        if (papTraceEvtsNew)
        {
            pThis->papTraceEvts  = papTraceEvtsNew;
            pThis->cTraceEvtsMax += _4K;
        }
        else
            rc = -1;
    }

    if (!rc)
    {
        pEvt->idTraceEvt  = pThis->uTraceEvtIdNext++;
        pThis->cbEvtAlloc += sizeof(*pEvt) + pEvt->cbAlloc;
        pThis->papTraceEvts[pThis->cTraceEvts++] = pEvt;
    }

    return rc;
}


/**
 * Creates a new trace event and links it into the tracer on success..
 *
 * @returns Status code.
 * @param   pThis                   The tracer instance.
 * @param   enmEvtType              The event type.
 * @param   enmContent              Content type for the event.
 * @param   cbAlloc                 Number of bytes to allocate for additional data.
 * @param   ppEvt                   Where to store the pointer to the created event on success.
 *
 * @note This method assigns the timestamps and event ID and adds the event record to the given tracer.
 *       Don't do anything which might fail and leave the event record in an invalid state after this succeeded.
 */
static int pspEmuTraceEvtCreateAndLink(PPSPTRACEINT pThis, PSPTRACEEVTTYPE enmEvtType, PSPTRACEEVTCONTENTTYPE enmContent,
                                       size_t cbAlloc, PPSPTRACEEVT *ppEvt)
{
    int rc = 0;
    PPSPTRACEEVT pEvt = (PPSPTRACEEVT)calloc(1, sizeof(*pEvt) + cbAlloc);
    if (pEvt)
    {
        pEvt->idTraceEvt     = 0;
        /*pEvt->tsTraceEvtNs = ...; */ /** @todo */
        pEvt->enmEvtType     = enmEvtType;
        pEvt->enmContent     = enmContent;
        pEvt->cbAlloc        = cbAlloc;

        /* Gather the PSP core context. */
        if (pThis->fFlags & PSPEMU_TRACE_F_FULL_CORE_CTX)
        {
            /** @todo Need a batch query for the core API. */
        }
        else
            rc = PSPEmuCoreQueryReg(pThis->hPspCore, PSPCOREREG_PC, &pEvt->au32CoreRegs[PSPCOREREG_PC]);

        if (!rc)
        {
            rc = pspEmuTraceEvtLink(pThis, pEvt);
            if (!rc)
            {
                *ppEvt = pEvt;
                return 0;
            }
        }

        free(pEvt);
    }
    else
        rc = -1;

    return rc;
}


/**
 * Worker for the add device read/write event methods.
 *
 * @returns Status code.
 * @param   hTrace                  The trace handle, NULL means default.
 * @param   enmEvtType              The event type this belongs to.
 * @param   pszDevId                The device identifier of the device being accessed.
 * @param   uAddr                   The context specific device address the transfer started at.
 * @param   pvData                  The data being read or written.
 * @param   cbXfer                  Number of bytes being transfered.
 * @param   fRead                   Flag whether this was a read or write event.
 */
static int pspEmuTraceEvtAddDevReadWriteWorker(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType, const char *pszDevId, uint64_t uAddr,
                                               const void *pvData, size_t cbXfer, bool fRead)
{
    int rc = 0;
    PPSPTRACEINT pThis = pspEmuTraceGetInstanceForEvtType(hTrace, enmEvtType);

    if (pThis)
    {
        PPSPTRACEEVT pEvt;
        size_t cchDevId = strlen(pszDevId) + 1; /* Include terminator */
        size_t cbAlloc = sizeof(PSPTRACEEVTDEVXFER) + cbXfer + cchDevId;
        rc = pspEmuTraceEvtCreateAndLink(pThis, enmEvtType, PSPTRACEEVTCONTENTTYPE_DEV_XFER, cbAlloc, &pEvt);
        if (!rc)
        {
            PPSPTRACEEVTDEVXFER pDevXfer = (PPSPTRACEEVTDEVXFER)&pEvt->abContent[0];

            pDevXfer->uAddrDev = uAddr;
            pDevXfer->cbXfer   = cbXfer;
            pDevXfer->fRead    = fRead;
            pDevXfer->pszDevId = (const char *)&pDevXfer->abXfer[cbXfer];
            memcpy(&pDevXfer->abXfer[0], pvData, cbXfer);
            memcpy(&pDevXfer->abXfer[cbXfer], pszDevId, cchDevId);
        }
    }

    return rc;
}


int PSPEmuTraceCreate(PPSPTRACE phTrace, uint32_t fFlags, PSPCORE hPspCore)
{
    int rc = 0;
    PPSPTRACEINT pThis = (PPSPTRACEINT)calloc(1, sizeof(*pThis));
    if (pThis)
    {
        pThis->uTraceEvtIdNext  = 0;
        pThis->tsTraceCreatedNs = 0;
        pThis->hPspCore         = hPspCore;
        pThis->fFlags           = fFlags;
        pThis->cbEvtAlloc       = 0;
        pThis->cTraceEvtsMax    = 0;
        pThis->cTraceEvts       = 0;
        pThis->papTraceEvts     = NULL;

        /** @todo Timestamping. */

        *phTrace = pThis;
    }
    else
        rc = -1;

    return rc;
}


void PSPEmuTraceDestroy(PSPTRACE hTrace)
{
    PPSPTRACEINT pThis = hTrace;

    /* Unset as default. */
    if (g_pTraceDef == pThis)
        g_pTraceDef = NULL;

    /* Free all trace events. */
    if (pThis->papTraceEvts)
    {
        for (uint32_t i = 0; i < pThis->cTraceEvts; i++)
            free((void *)pThis->papTraceEvts[i]);
        free(pThis->papTraceEvts);
    }
    free(pThis);
}


int PSPEmuTraceSetDefault(PSPTRACE hTrace)
{
    g_pTraceDef = hTrace;
    return 0;
}


int PSPEmuTraceEvtEnable(PSPTRACE hTrace, PSPTRACEEVTTYPE *paEvtTypes, uint32_t cEvtTypes)
{
    int rc = 0;
    PPSPTRACEINT pThis = pspEmuTraceGetInstance(hTrace);

    if (pThis)
        rc = pspEmuTraceEvtTypeConfigure(pThis, paEvtTypes, cEvtTypes, true /*fEnable*/);

    return rc;
}


int PSPEmuTraceEvtDisable(PSPTRACE hTrace, PSPTRACEEVTTYPE *paEvtTypes, uint32_t cEvtTypes)
{
    int rc = 0;
    PPSPTRACEINT pThis = pspEmuTraceGetInstance(hTrace);

    if (pThis)
        rc = pspEmuTraceEvtTypeConfigure(pThis, paEvtTypes, cEvtTypes, false /*fEnable*/);

    return rc;
}


int PSPEmuTraceDumpToFile(PSPTRACE hTrace, const char *pszFilename)
{
    return -1;
}


int PSPEmuTraceEvtAddStringV(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType, const char *pszFmt, va_list hArgs)
{
    int rc = 0;
    PPSPTRACEINT pThis = pspEmuTraceGetInstanceForEvtType(hTrace, enmEvtType);
    if (pThis)
    {
        uint8_t szTmp[_4K]; /** @todo Maybe allocate scratch buffer if this turns to be too small (or fix your damn log strings...). */
        int rcStr = vsnprintf(&szTmp[0], sizeof(szTmp), pszFmt, hArgs);

        if (rcStr > 0)
        {
            PPSPTRACEEVT pEvt;
            size_t cbAlloc = rcStr + 1; /* Include terminator. */
            rc = pspEmuTraceEvtCreateAndLink(pThis, enmEvtType, PSPTRACEEVTCONTENTTYPE_STRING, cbAlloc, &pEvt);
            if (!rc)
                memcpy(&pEvt->abContent[0], &szTmp[0], cbAlloc);
        }
        else
            rc = -1;
    }

    return rc;
}


int PSPEmuTraceEvtAddString(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType, const char *pszFmt, ...)
{
    va_list hArgs;

    va_start(hArgs, pszFmt);
    int rc = PSPEmuTraceEvtAddStringV(hTrace, enmEvtType, pszFmt, hArgs);
    va_end(hArgs);

    return rc;
}


int PSPEmuTraceEvtAddXfer(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType, uint64_t uAddrSrc, uint64_t uAddrDst, const void *pvBuf, size_t cbXfer)
{
    int rc = 0;
    PPSPTRACEINT pThis = pspEmuTraceGetInstanceForEvtType(hTrace, enmEvtType);
    if (pThis)
    {
        PPSPTRACEEVT pEvt;
        size_t cbAlloc = sizeof(PSPTRACEEVTXFER) + cbXfer;
        rc = pspEmuTraceEvtCreateAndLink(pThis, enmEvtType, PSPTRACEEVTCONTENTTYPE_XFER, cbAlloc, &pEvt);
        if (!rc)
        {
            PPSPTRACEEVTXFER pXfer = (PPSPTRACEEVTXFER)&pEvt->abContent[0];
            pXfer->uAddrSrc = uAddrSrc;
            pXfer->uAddrDst = uAddrDst;
            pXfer->cbXfer   = cbXfer;
            memcpy(&pXfer->abXfer[0], pvBuf, cbXfer);
        }
    }

    return rc;
}


int PSPEmuTraceEvtAddDevRead(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType, const char *pszDevId, uint64_t uAddr, const void *pvData, size_t cbRead)
{
    return pspEmuTraceEvtAddDevReadWriteWorker(hTrace, enmEvtType, pszDevId, uAddr, pvData, cbRead, true /*fRead*/);
}


int PSPEmuTraceEvtAddDevWrite(PSPTRACE hTrace, PSPTRACEEVTTYPE enmEvtType, const char *pszDevId, uint64_t uAddr, const void *pvData, size_t cbWrite)
{
    return pspEmuTraceEvtAddDevReadWriteWorker(hTrace, enmEvtType, pszDevId, uAddr, pvData, cbWrite, false /*fRead*/);
}
