/** @file
 * PSP Emulator - CCPv5 device.
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

/** @page pg_dev_ccp_v5   CCPv5 - Cryptographic Co-Processor version 5
 *
 * @todo Write something here.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <stdio.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/err.h>

/* OpenSSL version 1.0.x support (see https://www.openssl.org/docs/man1.1.0/man3/EVP_MD_CTX_new.html#HISTORY) */
# if OPENSSL_VERSION_NUMBER < 0x10100000 // = OpenSSL 1.1.0
#  define EVP_MD_CTX_new EVP_MD_CTX_create
#  define EVP_MD_CTX_free EVP_MD_CTX_destroy
# endif

#include <zlib.h>

/* Missing in zlib.h */
# ifndef Z_DEF_WBITS
#  define Z_DEF_WBITS        MAX_WBITS
# endif

#include <common/cdefs.h>
#include <common/status.h>
#include <psp/ccp.h>

#include <psp-devs.h>
#include <psp-trace.h>


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Address type the CCP uses (created from low and high parts). */
typedef uint64_t CCPADDR;
/** Create a CCP address from the given low and high parts. */
#define CCP_ADDR_CREATE_FROM_HI_LO(a_High, a_Low) (((CCPADDR)(a_High) << 32) | (a_Low))


/**
 * A single CCP queue.
 */
typedef struct CCPQUEUE
{
    /** Control register. */
    uint32_t                        u32RegCtrl;
    /** Request descriptor tail pointer. */
    uint32_t                        u32RegReqTail;
    /** Request descriptor head pointer. */
    uint32_t                        u32RegReqHead;
    /** Request status register. */
    uint32_t                        u32RegSts;
    /** Interrupt enable register. */
    uint32_t                        u32RegIen;
    /** Interrupt status register. */
    uint32_t                        u32RegIsts;
    /** Flag whether the queue was enabled by setting the run bit. */
    bool                            fEnabled;
} CCPQUEUE;
/** Pointer to a single CCP queue. */
typedef CCPQUEUE *PCCPQUEUE;
/** Pointer to a single const CCP queue. */
typedef const CCPQUEUE *PCCCPQUEUE;


/**
 * A single local storage buffer.
 */
typedef struct CCPLSB
{
    /** View dependent data. */
    union
    {
        /** A single slot. */
        struct
        {
            /** 32byte data. */
            uint8_t                 abData[32];
        } aSlots[128];
        /* Contiguous view of the complete LSB. */
        uint8_t                     abLsb[1];
    } u;
} CCPLSB;
/** Pointer to a local storage buffer. */
typedef CCPLSB *PCCPLSB;
/** Pointer to a const local storage buffer. */
typedef const CCPLSB *PCCCPLSB;


/**
 * CCP device instance data.
 */
typedef struct PSPDEVCCP
{
    /** Pointer to device instance. */
    PPSPDEV                         pDev;
    /** MMIO region handle. */
    PSPIOMREGIONHANDLE              hMmio;
    /** MMIO2 region handle. */
    PSPIOMREGIONHANDLE              hMmio2;
    /** The CCP queues. */
    CCPQUEUE                        aQueues[2];
    /** The local storage buffer. */
    CCPLSB                          Lsb;
    /** The openssl SHA context currently in use. This doesn't really belong here
     * as the state is contained in an LSB but for use with openssl and to support
     * multi-part messages we have to store it here, luckily the PSP is single threaded
     * so the code will only every process one SHA operation at a time.
     */
    EVP_MD_CTX                      *pOsslShaCtx;
    /** The openssl AES context currently in use, same note as above applies. */
    EVP_CIPHER_CTX                  *pOsslAesCtx;
    /** The zlib decompression state. */
    z_stream                        Zlib;
    /** Size of the last transfer in bytes (written to local PSP memory). */
    size_t                          cbWrittenLast;
} PSPDEVCCP;
/** Pointer to the device instance data. */
typedef PSPDEVCCP *PPSPDEVCCP;


/**
 * Data transfer context.
 */
typedef struct CCPXFERCTX
{
    /** The read callback. */
    int    (*pfnRead) (PPSPDEVCCP pThis, CCPADDR CcpAddr, void *pvDst, size_t cbRead);
    /** The write callback. */
    int    (*pfnWrite) (PPSPDEVCCP pThis, CCPADDR CcpAddr, const void *pvSrc, size_t cbWrite);
    /** The CCP device instance the context is for. */
    PPSPDEVCCP                  pThis;
    /** Current source address. */
    CCPADDR                     CcpAddrSrc;
    /** Amount of data to read left. */
    size_t                      cbReadLeft;
    /** Current destination address. */
    CCPADDR                     CcpAddrDst;
    /** Amount of data to write left. */
    size_t                      cbWriteLeft;
    /** Flag whether to write in reverse order. */
    bool                        fWriteRev;
} CCPXFERCTX;
/** Pointer to an xfer context. */
typedef CCPXFERCTX *PCCPXFERCTX;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/


/**
 * Transfer data from system memory to a local buffer.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddr             The address to read from (x86 physical address).
 * @param   pvDst               Where to store the read data.
 * @param   cbRead              How much to read.
 */
static int pspDevCcpXferMemSysRead(PPSPDEVCCP pThis, CCPADDR CcpAddr, void *pvDst, size_t cbRead)
{
    return -1;
}


/**
 * Transfer data from a local buffer to system memory.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddr             The address to write to (x86 physical address).
 * @param   pvSrc               The data to write.
 * @param   cbWrite             How much to write.
 */
static int pspDevCcpXferMemSysWrite(PPSPDEVCCP pThis, CCPADDR CcpAddr, const void *pvSrc, size_t cbWrite)
{
    return -1;
}


/**
 * Transfer data from a local storage buffer to a local buffer.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddr             The address to read from (LSB address).
 * @param   pvDst               Where to store the read data.
 * @param   cbRead              How much to read.
 */
static int pspDevCcpXferMemLsbRead(PPSPDEVCCP pThis, CCPADDR CcpAddr, void *pvDst, size_t cbRead)
{
    int rc = 0;

    if (   CcpAddr < sizeof(pThis->Lsb)
        && CcpAddr + cbRead <= sizeof(pThis->Lsb))
        memcpy(pvDst, &pThis->Lsb.u.abLsb[CcpAddr], cbRead);
    else
    {
        printf("CCP: Invalid LSB read offset=%#x cbRead=%zu\n", (uint32_t)CcpAddr, cbRead);
        rc = -1;
    }

    return rc;
}


/**
 * Transfer data from a local buffer to a local storage buffer.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddr             The address to write to (LSB address).
 * @param   pvSrc               The data to write.
 * @param   cbWrite             How much to write.
 */
static int pspDevCcpXferMemLsbWrite(PPSPDEVCCP pThis, CCPADDR CcpAddr, const void *pvSrc, size_t cbWrite)
{
    int rc = 0;

    if (   CcpAddr < sizeof(pThis->Lsb)
        && CcpAddr + cbWrite <= sizeof(pThis->Lsb))
        memcpy(&pThis->Lsb.u.abLsb[CcpAddr], pvSrc, cbWrite);
    else
    {
        printf("CCP: Invalid LSB write offset=%#x cbWrite=%zu\n", (uint32_t)CcpAddr, cbWrite);
        rc = -1;
    }

    return rc;
}


/**
 * Transfer data from a local PSP memory address (SRAM,MMIO) to a local buffer.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddr             The address to read from (PSP address).
 * @param   pvDst               Where to store the read data.
 * @param   cbRead              How much to read.
 */
static int pspDevCcpXferMemLocalRead(PPSPDEVCCP pThis, CCPADDR CcpAddr, void *pvDst, size_t cbRead)
{
    return PSPEmuIoMgrPspAddrRead(pThis->pDev->hIoMgr, (uint32_t)CcpAddr, pvDst, cbRead);
}


/**
 * Transfer data from a local buffer to a local PSP memory address (SRAM,MMIO).
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddr             The address to write to (PSP address).
 * @param   pvSrc               The data to write.
 * @param   cbWrite             How much to write.
 */
static int pspDevCcpXferMemLocalWrite(PPSPDEVCCP pThis, CCPADDR CcpAddr, const void *pvSrc, size_t cbWrite)
{
    int rc = PSPEmuIoMgrPspAddrWrite(pThis->pDev->hIoMgr, (uint32_t)CcpAddr, pvSrc, cbWrite);
    if (!rc)
        pThis->cbWrittenLast += cbWrite;

    return rc;
}


/**
 * Initializes a data transfer context.
 *
 * @returns Status code.
 * @param   pCtx                The transfer context to initialize.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The CCP request to take memory types from.
 * @param   fSha                Flag whether this context is for the SHA engine.
 * @param   cbWrite             Amount of bytes to write in total.
 * @param   fWriteRev           Flag whether to write the data in reverse order.
 */
static int pspDevCcpXferCtxInit(PCCPXFERCTX pCtx, PPSPDEVCCP pThis, PCCCP5REQ pReq, bool fSha, size_t cbWrite,
                                bool fWriteRev)
{
    pThis->cbWrittenLast = 0;

    pCtx->pThis      = pThis;
    pCtx->CcpAddrSrc = CCP_ADDR_CREATE_FROM_HI_LO(pReq->u16AddrSrcHigh, pReq->u32AddrSrcLow);
    pCtx->cbReadLeft = pReq->cbSrc;
    pCtx->fWriteRev  = fWriteRev;
    switch (CCP_V5_MEM_TYPE_GET(pReq->u16SrcMemType))
    {
        case CCP_V5_MEM_TYPE_SYSTEM:
            pCtx->pfnRead = pspDevCcpXferMemSysRead;
            break;
        case CCP_V5_MEM_TYPE_SB:
            pCtx->pfnRead = pspDevCcpXferMemLsbRead;
            break;
        case CCP_V5_MEM_TYPE_LOCAL:
            pCtx->pfnRead = pspDevCcpXferMemLocalRead;
            break;
        default:
            return -1;
    }

    pCtx->cbWriteLeft = cbWrite;
    if (!fSha)
    {
        pCtx->CcpAddrDst = CCP_ADDR_CREATE_FROM_HI_LO(pReq->Op.NonSha.u16AddrDstHigh, pReq->Op.NonSha.u32AddrDstLow);
        switch (CCP_V5_MEM_TYPE_GET(pReq->Op.NonSha.u16DstMemType))
        {
            case CCP_V5_MEM_TYPE_SYSTEM:
                pCtx->pfnWrite = pspDevCcpXferMemSysWrite;
                break;
            case CCP_V5_MEM_TYPE_SB:
                pCtx->pfnWrite = pspDevCcpXferMemLsbWrite;
                break;
            case CCP_V5_MEM_TYPE_LOCAL:
                pCtx->pfnWrite = pspDevCcpXferMemLocalWrite;
                break;
            default:
                return -1;
        }
    }
    else /* SHA always writes to the LSB. */
    {
        uint8_t uLsbCtxId = CCP_V5_MEM_LSB_CTX_ID_GET(pReq->u16SrcMemType);
        if (uLsbCtxId < ELEMENTS(pThis->Lsb.u.aSlots))
        {
            pCtx->pfnWrite = pspDevCcpXferMemLsbWrite;
            pCtx->CcpAddrDst = uLsbCtxId * sizeof(pThis->Lsb.u.aSlots[0].abData);
        }
        else
            return -1;
    }

    if (pCtx->fWriteRev)
        pCtx->CcpAddrDst += pCtx->cbWriteLeft;

    return 0;
}


/**
 * Executes a read pass using the given transfer context.
 *
 * @returns Status code.
 * @param   pCtx                The transfer context to use.
 * @param   pvDst               Where to store the read data.
 * @param   cbRead              How much to read.
 * @param   pcbRead             Where to store the amount of data actually read, optional.
 */
static int pspDevCcpXferCtxRead(PCCPXFERCTX pCtx, void *pvDst, size_t cbRead, size_t *pcbRead)
{
    int rc = 0;
    size_t cbThisRead = MIN(cbRead, pCtx->cbReadLeft);

    if (    cbThisRead
        && (   pcbRead
            || cbThisRead == cbRead))
    {
        rc = pCtx->pfnRead(pCtx->pThis, pCtx->CcpAddrSrc, pvDst, cbThisRead);
        if (!rc)
        {
            pCtx->cbReadLeft -= cbThisRead;
            pCtx->CcpAddrSrc += cbThisRead;
            if (pcbRead)
                *pcbRead = cbThisRead;
        }
    }
    else
        rc = -1;

    return rc;
}


/**
 * Executes a write pass using the given transfer context.
 *
 * @returns Status code.
 * @param   pCtx                The transfer context to use.
 * @param   pvSrc               The data to write.
 * @param   cbWrite             How much to write.
 * @param   pcbWritten          Where to store the amount of data actually written, optional.
 */
static int pspDevCcpXferCtxWrite(PCCPXFERCTX pCtx, const void *pvSrc, size_t cbWrite, size_t *pcbWritten)
{
    int rc = 0;
    size_t cbThisWrite = MIN(cbWrite, pCtx->cbWriteLeft);

    if (    cbThisWrite
        && (   pcbWritten
            || cbThisWrite == cbWrite))
    {
        if (pCtx->fWriteRev)
        {
            const uint8_t *pbSrc = (const uint8_t *)pvSrc;

            /** @todo Unoptimized single byte writes... */
            while (   cbThisWrite
                   && !rc)
            {
                pCtx->CcpAddrDst--;
                rc = pCtx->pfnWrite(pCtx->pThis, pCtx->CcpAddrDst, pbSrc, 1);
                cbThisWrite--;
                pbSrc++;
            }

            if (   !rc
                && pcbWritten)
                *pcbWritten = cbThisWrite;
        }
        else
        {
            rc = pCtx->pfnWrite(pCtx->pThis, pCtx->CcpAddrDst, pvSrc, cbThisWrite);
            if (!rc)
            {
                pCtx->cbWriteLeft -= cbThisWrite;
                pCtx->CcpAddrDst  += cbThisWrite;
                if (pcbWritten)
                    *pcbWritten = cbThisWrite;
            }
        }
    }
    else
        rc = -1;

    return rc;
}


/**
 * Reverses the data in the given buffer.
 *
 * @returns nothing.
 * @param   pbBuf                   The buffer to reverse the data in.
 * @param   cbBuf                   Size of the buffer to reverse.
 */
static void pspDevCcpReverseBuf(uint8_t *pbBuf, size_t cbBuf)
{
    uint8_t *pbBufTop = pbBuf + cbBuf - 1;

    while (pbBuf < pbBufTop)
    {
        uint8_t bTmp = *pbBuf;
        *pbBuf = *pbBufTop;
        *pbBufTop = bTmp;
        pbBuf++;
        pbBufTop--;
    }
}


/**
 * Copies the key material pointed to by the request into a supplied buffer.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to get the key address from.
 * @param   cbKey               Size of the key buffer.
 * @param   pvKey               Where to store the key material.
 */
static int pspDevCcpKeyCopyFromReq(PPSPDEVCCP pThis, PCCCP5REQ pReq, size_t cbKey, void *pvKey)
{
    int rc = 0;

    if (CCP_V5_MEM_TYPE_GET(pReq->u16KeyMemType) == CCP_V5_MEM_TYPE_LOCAL)
    {
        CCPADDR CcpAddrKey = CCP_ADDR_CREATE_FROM_HI_LO(pReq->u16AddrKeyHigh, pReq->u32AddrKeyLow);
        rc = pspDevCcpXferMemLocalRead(pThis, CcpAddrKey, pvKey, cbKey);
    }
    else if (CCP_V5_MEM_TYPE_GET(pReq->u16KeyMemType) == CCP_V5_MEM_TYPE_SB)
    {
        CCPADDR CcpAddrKey = CCP_ADDR_CREATE_FROM_HI_LO(pReq->u16AddrKeyHigh, pReq->u32AddrKeyLow);
        if (   CcpAddrKey < sizeof(pThis->Lsb)
            && CcpAddrKey + cbKey <= sizeof(pThis->Lsb))
            memcpy(pvKey, &pThis->Lsb.u.abLsb[CcpAddrKey], cbKey);
        else
            rc = -1;
    }

    return rc;
}


/**
 * Copies data from an LSB into a supplied buffer.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   CcpAddrLsb          CCP LSB address to copy from.
 * @param   cb                  Amount of bytes to copy.
 * @param   pv                  Where to store the data.
 */
static int pspDevCcpCopyFromLsb(PPSPDEVCCP pThis, CCPADDR CcpAddrLsb, size_t cb, void *pv)
{
    int rc = 0;

    if (   CcpAddrLsb < sizeof(pThis->Lsb)
        && CcpAddrLsb + cb <= sizeof(pThis->Lsb))
        memcpy(pv, &pThis->Lsb.u.abLsb[CcpAddrLsb], cb);
    else
        rc = -1;

    return rc;
}


/**
 * Returns the string representation of the given CCP request engine field.
 *
 * @returns Engine description.
 * @param   uEngine                 The engine to convert to a stirng.
 */
static const char *pspDevCcpReqEngineToStr(uint32_t uEngine)
{
    switch (uEngine)
    {
        case CCP_V5_ENGINE_AES:
            return "AES";
        case CCP_V5_ENGINE_XTS_AES128:
            return "XTS_AES_128";
        case CCP_V5_ENGINE_DES3:
            return "DES3";
        case CCP_V5_ENGINE_SHA:
            return "SHA";
        case CCP_V5_ENGINE_RSA:
            return "RSA";
        case CCP_V5_ENGINE_PASSTHRU:
            return "PASSTHROUGH";
        case CCP_V5_ENGINE_ZLIB_DECOMP:
            return "ZLIB_DECOMPRESS";
        case CCP_V5_ENGINE_ECC:
            return "ECC";
    }

    return "<INVALID>";
}


/**
 * Extracts and dumps information about the given AES function.
 *
 * @returns nothing.
 * @param   pszBuf              The buffer to dump into.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   uFunc               The function part of dword 0.
 * @param   u32Dw0Raw           The raw dw0 value used for dumping.
 * @param   pszEngine           The used engine string.
 */
static void pspDevCcpReqDumpAesFunction(char *pszBuf, size_t cbBuf, uint32_t uFunc,
                                        uint32_t u32Dw0Raw, const char *pszEngine)
{
    uint8_t uSz      = CCP_V5_ENGINE_AES_SZ_GET(uFunc);
    uint8_t fEncrypt = CCP_V5_ENGINE_AES_ENCRYPT_GET(uFunc);
    uint8_t uMode    = CCP_V5_ENGINE_AES_MODE_GET(uFunc);
    uint8_t uAesType = CCP_V5_ENGINE_AES_TYPE_GET(uFunc);

    const char *pszMode    = "<INVALID>";
    const char *pszAesType = "<INVALID>";

    switch (uMode)
    {
        case CCP_V5_ENGINE_AES_MODE_ECB:
            pszMode = "ECB";
            break;
        case CCP_V5_ENGINE_AES_MODE_CBC:
            pszMode = "CBC";
            break;
        case CCP_V5_ENGINE_AES_MODE_OFB:
            pszMode = "OFB";
            break;
        case CCP_V5_ENGINE_AES_MODE_CFB:
            pszMode = "CFB";
            break;
        case CCP_V5_ENGINE_AES_MODE_CTR:
            pszMode = "CTR";
            break;
        case CCP_V5_ENGINE_AES_MODE_CMAC:
            pszMode = "CMAC";
            break;
        case CCP_V5_ENGINE_AES_MODE_GHASH:
            pszMode = "GHASH";
            break;
        case CCP_V5_ENGINE_AES_MODE_GCTR:
            pszMode = "GCTR";
            break;
        case CCP_V5_ENGINE_AES_MODE_GCM:
            pszMode = "GCM";
            break;
        case CCP_V5_ENGINE_AES_MODE_GMAC:
            pszMode = "GMAC";
            break;
    }

    switch (uAesType)
    {
        case CCP_V5_ENGINE_AES_TYPE_128:
            pszAesType = "AES128";
            break;
        case CCP_V5_ENGINE_AES_TYPE_192:
            pszAesType = "AES192";
            break;
        case CCP_V5_ENGINE_AES_TYPE_256:
            pszAesType = "AES256";
            break;
    }

    snprintf(pszBuf, cbBuf, "u32Dw0:             0x%08x (Engine: %s, AES Type: %s, Mode: %s, Encrypt: %u, Size: %u)",
                                                 u32Dw0Raw, pszEngine, pszAesType, pszMode, fEncrypt, uSz);
}


/**
 * Extracts and dumps information about the given SHA function.
 *
 * @returns nothing.
 * @param   pszBuf              The buffer to dump into.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   uFunc               The function part of dword 0.
 * @param   u32Dw0Raw           The raw dw0 value used for dumping.
 * @param   pszEngine           The used engine string.
 */
static void pspDevCcpReqDumpShaFunction(char *pszBuf, size_t cbBuf, uint32_t uFunc,
                                        uint32_t u32Dw0Raw, const char *pszEngine, bool fInit, bool fEom)
{
    uint32_t uShaType = CCP_V5_ENGINE_SHA_TYPE_GET(uFunc);
    const char *pszShaType = "<INVALID>";

    switch (uShaType)
    {
        case CCP_V5_ENGINE_SHA_TYPE_1:
            pszShaType = "SHA1";
            break;
        case CCP_V5_ENGINE_SHA_TYPE_224:
            pszShaType = "SHA224";
            break;
        case CCP_V5_ENGINE_SHA_TYPE_256:
            pszShaType = "SHA256";
            break;
        case CCP_V5_ENGINE_SHA_TYPE_384:
            pszShaType = "SHA384";
            break;
        case CCP_V5_ENGINE_SHA_TYPE_512:
            pszShaType = "SHA512";
            break;
    }

    snprintf(pszBuf, cbBuf, "u32Dw0:             0x%08x (Engine: %s, Init: %u, Eom: %u, SHA type: %s)",
                                                 u32Dw0Raw, pszEngine, fInit, fEom, pszShaType);
}


/**
 * Extracts and dumps information about the given PASSTHRU function.
 *
 * @returns nothing.
 * @param   pszBuf              The buffer to dump into.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   uFunc               The function part of dword 0.
 * @param   u32Dw0Raw           The raw dw0 value used for dumping.
 * @param   pszEngine           The used engine string.
 */
static void pspDevCcpReqDumpPassthruFunction(char *pszBuf, size_t cbBuf, uint32_t uFunc,
                                             uint32_t u32Dw0Raw, const char *pszEngine)
{
    uint8_t uByteSwap = CCP_V5_ENGINE_PASSTHRU_BYTESWAP_GET(uFunc);
    uint8_t uBitwise  = CCP_V5_ENGINE_PASSTHRU_BITWISE_GET(uFunc);
    uint8_t uReflect  = CCP_V5_ENGINE_PASSTHRU_REFLECT_GET(uFunc);

    const char *pszByteSwap = "<INVALID>";
    const char *pszBitwise  = "<INVALID>";

    switch (uByteSwap)
    {
        case CCP_V5_ENGINE_PASSTHRU_BYTESWAP_NOOP:
            pszByteSwap = "NOOP";
            break;
        case CCP_V5_ENGINE_PASSTHRU_BYTESWAP_32BIT:
            pszByteSwap = "32BIT";
            break;
        case CCP_V5_ENGINE_PASSTHRU_BYTESWAP_256BIT:
            pszByteSwap = "256BIT";
            break;
    }

    switch (uBitwise)
    {
        case CCP_V5_ENGINE_PASSTHRU_BITWISE_NOOP:
            pszBitwise = "NOOP";
            break;
        case CCP_V5_ENGINE_PASSTHRU_BITWISE_AND:
            pszBitwise = "AND";
            break;
        case CCP_V5_ENGINE_PASSTHRU_BITWISE_OR:
            pszBitwise = "OR";
            break;
        case CCP_V5_ENGINE_PASSTHRU_BITWISE_XOR:
            pszBitwise = "XOR";
            break;
        case CCP_V5_ENGINE_PASSTHRU_BITWISE_MASK:
            pszBitwise = "MASK";
            break;
    }

    snprintf(pszBuf, cbBuf, "u32Dw0:             0x%08x (Engine: %s, ByteSwap: %s, Bitwise: %s, Reflect: %#x)",
                                                 u32Dw0Raw, pszEngine, pszByteSwap, pszBitwise, uReflect);
}


/**
 * Extracts and dumps information about the given RSA function.
 *
 * @returns nothing.
 * @param   pszBuf              The buffer to dump into.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   uFunc               The function part of dword 0.
 * @param   u32Dw0Raw           The raw dw0 value used for dumping.
 * @param   pszEngine           The used engine string.
 */
static void pspDevCcpReqDumpRsaFunction(char *pszBuf, size_t cbBuf, uint32_t uFunc,
                                        uint32_t u32Dw0Raw, const char *pszEngine)
{
    uint16_t uSz   = CCP_V5_ENGINE_RSA_SZ_GET(uFunc);
    uint8_t  uMode = CCP_V5_ENGINE_RSA_MODE_GET(uFunc);

    snprintf(pszBuf, cbBuf, "u32Dw0:             0x%08x (Engine: %s, Mode: %u, Size: %u)",
                                                 u32Dw0Raw, pszEngine, uMode, uSz);
}


/**
 * Extracts and dumps information about the given ECC function.
 *
 * @returns nothing.
 * @param   pszBuf              The buffer to dump into.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   uFunc               The function part of dword 0.
 * @param   u32Dw0Raw           The raw dw0 value used for dumping.
 * @param   pszEngine           The used engine string.
 */
static void pspDevCcpReqDumpEccFunction(char *pszBuf, size_t cbBuf, uint32_t uFunc,
                                        uint32_t u32Dw0Raw, const char *pszEngine)
{
    uint8_t  uOp   = CCP_V5_ENGINE_ECC_OP_GET(uFunc);
    uint16_t uBits = CCP_V5_ENGINE_ECC_BIT_COUNT_GET(uFunc);

    snprintf(pszBuf, cbBuf, "u32Dw0:             0x%08x (Engine: %s, Op: %u, Bits: %u)",
                                                 u32Dw0Raw, pszEngine, uOp, uBits);
}


/**
 * Dumps the CCP5 request descriptor.
 *
 * @returns nothing.
 * @param   pReq                The request to dump.
 */
static void pspDevCcpDumpReq(PCCCP5REQ pReq, PSPADDR PspAddrReq)
{
    uint32_t uEngine   = CCP_V5_ENGINE_GET(pReq->u32Dw0);
    uint32_t uFunction = CCP_V5_ENGINE_FUNC_GET(pReq->u32Dw0);
    bool     fInit     = CCP_V5_ENGINE_INIT_GET(pReq->u32Dw0);
    bool     fEom      = CCP_V5_ENGINE_EOM_GET(pReq->u32Dw0);
    const char *pszEngine   = pspDevCcpReqEngineToStr(uEngine);
    char szDw0[512];

    if (uEngine == CCP_V5_ENGINE_AES)
        pspDevCcpReqDumpAesFunction(&szDw0[0], sizeof(szDw0), uFunction, pReq->u32Dw0, pszEngine);
    else if (uEngine == CCP_V5_ENGINE_SHA)
        pspDevCcpReqDumpShaFunction(&szDw0[0], sizeof(szDw0), uFunction, pReq->u32Dw0, pszEngine, fInit, fEom);
    else if (uEngine == CCP_V5_ENGINE_PASSTHRU)
        pspDevCcpReqDumpPassthruFunction(&szDw0[0], sizeof(szDw0), uFunction, pReq->u32Dw0, pszEngine);
    else if (uEngine == CCP_V5_ENGINE_RSA)
        pspDevCcpReqDumpRsaFunction(&szDw0[0], sizeof(szDw0), uFunction, pReq->u32Dw0, pszEngine);
    else if (uEngine == CCP_V5_ENGINE_ECC)
        pspDevCcpReqDumpEccFunction(&szDw0[0], sizeof(szDw0), uFunction, pReq->u32Dw0, pszEngine);
    else
        snprintf(&szDw0[0], sizeof(szDw0), "u32Dw0:             0x%08x (Engine: %s)",
                                                                pReq->u32Dw0, pszEngine);

    if (uEngine != CCP_V5_ENGINE_SHA)
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                "CCP Request 0x%08x:\n"
                                "    %s\n"
                                "    cbSrc:              %u\n"
                                "    u32AddrSrcLow:      0x%08x\n"
                                "    u16AddrSrcHigh:     0x%08x\n"
                                "    u16SrcMemType:      0x%08x (MemType: %u, LsbCtxId: %u, Fixed: %u)\n"
                                "    u32AddrDstLow:      0x%08x\n"
                                "    u16AddrDstHigh:     0x%08x\n"
                                "    u16DstMemType:      0x%08x (MemType: %u, Fixed: %u)\n"
                                "    u32AddrKeyLow:      0x%08x\n"
                                "    u16AddrKeyHigh:     0x%08x\n"
                                "    u16KeyMemType:      0x%08x\n",
                                PspAddrReq, &szDw0[0], pReq->cbSrc, pReq->u32AddrSrcLow, pReq->u16AddrSrcHigh,
                                pReq->u16SrcMemType, CCP_V5_MEM_TYPE_GET(pReq->u16SrcMemType),
                                CCP_V5_MEM_LSB_CTX_ID_GET(pReq->u16SrcMemType), CCP_V5_MEM_LSB_FIXED_GET(pReq->u16SrcMemType),
                                pReq->Op.NonSha.u32AddrDstLow, pReq->Op.NonSha.u16AddrDstHigh,
                                pReq->Op.NonSha.u16DstMemType, CCP_V5_MEM_TYPE_GET(pReq->Op.NonSha.u16DstMemType),
                                CCP_V5_MEM_LSB_FIXED_GET(pReq->Op.NonSha.u16DstMemType),
                                pReq->u32AddrKeyLow, pReq->u16AddrKeyHigh, pReq->u16KeyMemType);
    else
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                "CCP Request 0x%08x:\n"
                                "    %s\n"
                                "    cbSrc:              %u\n"
                                "    u32AddrSrcLow:      0x%08x\n"
                                "    u16AddrSrcHigh:     0x%08x\n"
                                "    u16SrcMemType:      0x%08x (MemType: %u, LsbCtxId: %u, Fixed: %u)\n"
                                "    u32ShaBitsLow:      0x%08x\n"
                                "    u32ShaBitsHigh:     0x%08x\n"
                                "    u32AddrKeyLow:      0x%08x\n"
                                "    u16AddrKeyHigh:     0x%08x\n"
                                "    u16KeyMemType:      0x%08x\n",
                                PspAddrReq, &szDw0[0], pReq->cbSrc, pReq->u32AddrSrcLow, pReq->u16AddrSrcHigh,
                                pReq->u16SrcMemType, CCP_V5_MEM_TYPE_GET(pReq->u16SrcMemType),
                                CCP_V5_MEM_LSB_CTX_ID_GET(pReq->u16SrcMemType), CCP_V5_MEM_LSB_FIXED_GET(pReq->u16SrcMemType),
                                pReq->Op.Sha.u32ShaBitsLow, pReq->Op.Sha.u32ShaBitsHigh,
                                pReq->u32AddrKeyLow, pReq->u16AddrKeyHigh, pReq->u16KeyMemType);
}


/**
 * Dump ecc number.
 *
 * @returns nothing.
 * @param   pszBuf              The buffer to dump into.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   pNum                The ecc number to dump.
 */
static void pspDevCcpDumpEccNumber(char *pszBuf, size_t cbBuf, PCCCP5ECCNUM pNum)
{
    const uint64_t *pu64Num = (const uint64_t *)pNum;

    snprintf(pszBuf, cbBuf,
        "0x%016lx_%016lx_%016lx_%016lx"
        "_%016lx_%016lx_%016lx_%016lx"
        "_%016lx",
        pu64Num[8], pu64Num[7], pu64Num[6], pu64Num[5],
        pu64Num[4], pu64Num[3], pu64Num[2], pu64Num[1],
        pu64Num[0]
    );
}


/**
 * Dumps the ecc request data for a request.
 *
 * @returns nothing.
 * @param   uOp                 The ECC operation.
 * @param   pEccReq             The ECC request data.
 */
static void pspDevCcpDumpEccReq(uint8_t uOp, PCCCP5ECCREQ pEccReq)
{
    char szPrime[256];

    pspDevCcpDumpEccNumber(szPrime, sizeof(szPrime), &pEccReq->Prime);
    switch (uOp)
    {
        case CCP_V5_ENGINE_ECC_OP_MUL_FIELD:
        {
            char szFactor1[256], szFactor2[256];

            pspDevCcpDumpEccNumber(szFactor1, sizeof(szFactor1), &pEccReq->Op.FieldMul.Factor1);
            pspDevCcpDumpEccNumber(szFactor2, sizeof(szFactor2), &pEccReq->Op.FieldMul.Factor2);
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                    "CCP ECC Data (Field Multiplication):\n"
                                    "    Prime:             %s\n"
                                    "    Factor1:           %s\n"
                                    "    Factor2:           %s\n",
                                    szPrime, szFactor1, szFactor2);
            break;
        }
        case CCP_V5_ENGINE_ECC_OP_ADD_FIELD:
        {
            char szSummand1[256], szSummand2[256];
            pspDevCcpDumpEccNumber(szSummand1, sizeof(szSummand1), &pEccReq->Op.FieldAdd.Summand1);
            pspDevCcpDumpEccNumber(szSummand2, sizeof(szSummand2), &pEccReq->Op.FieldAdd.Summand2);
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                    "CCP ECC Data (Field Addition):\n"
                                    "    Prime:             %s\n"
                                    "    Summand1:          %s\n"
                                    "    Summand2:          %s\n",
                                    szPrime, szSummand1, szSummand2);
            break;
        }
        case CCP_V5_ENGINE_ECC_OP_INV_FIELD:
        {
            char szNumber[256];
            pspDevCcpDumpEccNumber(szNumber, sizeof(szNumber),
                &pEccReq->Op.FieldInv.Num);
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                    "CCP ECC Data (Field Inversion):\n"
                                    "    Prime:             %s\n"
                                    "    Number:            %s\n",
                                    szPrime, szNumber);
            break;
        }
        case CCP_V5_ENGINE_ECC_OP_MUL_CURVE:
        {
            char szFactor[256];
            char szPointX[256];
            char szPointY[256];
            char szCoefficient[256];

            pspDevCcpDumpEccNumber(szFactor, sizeof(szFactor), &pEccReq->Op.CurveMul.Factor);
            pspDevCcpDumpEccNumber(szPointX, sizeof(szPointX), &pEccReq->Op.CurveMul.Point.X);
            pspDevCcpDumpEccNumber(szPointY, sizeof(szPointY), &pEccReq->Op.CurveMul.Point.Y);

            pspDevCcpDumpEccNumber(szCoefficient, sizeof(szCoefficient), &pEccReq->Op.CurveMul.Coefficient);

            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                    "CCP ECC Data (Curve Multiplication):\n"
                                    "    Prime:             %s\n"
                                    "    Factor:            %s\n"
                                    "    PointX:            %s\n"
                                    "    PointY:            %s\n"
                                    "    CurveCoefficient:  %s\n",
                                    szPrime,
                                    szFactor, szPointX, szPointY,
                                    szCoefficient);
            break;
        }
        case CCP_V5_ENGINE_ECC_OP_MUL_ADD_CURVE:
            {
                char szFactor1[256];
                char szPoint1X[256];
                char szPoint1Y[256];

                char szFactor2[256];
                char szPoint2X[256];
                char szPoint2Y[256];

                char szCoefficient[256];

                pspDevCcpDumpEccNumber(szFactor1, sizeof(szFactor1), &pEccReq->Op.CurveMulAdd.Factor1);
                pspDevCcpDumpEccNumber(szPoint1X, sizeof(szPoint1X), &pEccReq->Op.CurveMulAdd.Point1.X);
                pspDevCcpDumpEccNumber(szPoint1Y, sizeof(szPoint1Y), &pEccReq->Op.CurveMulAdd.Point1.Y);

                pspDevCcpDumpEccNumber(szFactor2, sizeof(szFactor2), &pEccReq->Op.CurveMulAdd.Factor2);
                pspDevCcpDumpEccNumber(szPoint2X, sizeof(szPoint2X), &pEccReq->Op.CurveMulAdd.Point2.X);
                pspDevCcpDumpEccNumber(szPoint2Y, sizeof(szPoint2Y), &pEccReq->Op.CurveMulAdd.Point2.Y);

                pspDevCcpDumpEccNumber(szCoefficient, sizeof(szCoefficient), &pEccReq->Op.CurveMulAdd.Coefficient);

                PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                        "CCP ECC Data (Curve Multiplication and Addition):\n"
                                        "    Prime:             %s\n"
                                        "    Factor1:           %s\n"
                                        "    Point1X:           %s\n"
                                        "    Point1Y:           %s\n"
                                        "    Factor2:           %s\n"
                                        "    Point2X:           %s\n"
                                        "    Point2Y:           %s\n"
                                        "    CurveCoefficient:  %s\n",
                                        szPrime,
                                        szFactor1, szPoint1X, szPoint1Y,
                                        szFactor2, szPoint2X, szPoint2Y,
                                        szCoefficient);
            }
            break;
        default:
        {
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_INFO, PSPTRACEEVTORIGIN_CCP,
                                    "CCP ECC Data (Unkown Operation):\n"
                                    "    Prime:                 %s\n"
                                    "    Unknown Parameters ...\n",
                                    szPrime);
            break;
        }
    }

    PCCCP5ECCNUM pNum = (PCCCP5ECCNUM) pEccReq;
    for (unsigned i = 0; i < sizeof(CCP5ECCREQ) / sizeof(CCP5ECCNUM); i++)
    {
        pspDevCcpDumpEccNumber(szPrime, sizeof(szPrime), pNum + i);
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_DEBUG, PSPTRACEEVTORIGIN_CCP,
                                "CCP ECC Data Number %02i:\n"
                                "    %s\n",
                                i, szPrime);
    }
}


/**
 * Processes a passthru request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   uFunc               The engine specific function.
 */
static int pspDevCcpReqPassthruProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq, uint32_t uFunc)
{
    int rc = 0;
    uint8_t uByteSwap = CCP_V5_ENGINE_PASSTHRU_BYTESWAP_GET(uFunc);
    uint8_t uBitwise  = CCP_V5_ENGINE_PASSTHRU_BITWISE_GET(uFunc);
    uint8_t uReflect  = CCP_V5_ENGINE_PASSTHRU_REFLECT_GET(uFunc);

    if (   uBitwise == CCP_V5_ENGINE_PASSTHRU_BITWISE_NOOP
        && (   uByteSwap == CCP_V5_ENGINE_PASSTHRU_BYTESWAP_NOOP
            || (   uByteSwap == CCP_V5_ENGINE_PASSTHRU_BYTESWAP_256BIT
                && pReq->cbSrc == 32))
        && uReflect == 0)
    {
        size_t cbLeft = pReq->cbSrc;
        CCPXFERCTX XferCtx;

        rc = pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, false /*fSha*/, cbLeft,
                                  uByteSwap == CCP_V5_ENGINE_PASSTHRU_BYTESWAP_256BIT ? true : false /*fWriteRev*/);
        if (!rc)
        {
            uint8_t abData[_4K];
            while (   !rc
                   && cbLeft)
            {
                size_t cbThisProc = MIN(cbLeft, sizeof(abData));

                rc = pspDevCcpXferCtxRead(&XferCtx, &abData[0], cbThisProc, NULL);
                if (!rc)
                    rc = pspDevCcpXferCtxWrite(&XferCtx, &abData[0], cbThisProc, NULL);

                cbLeft -= cbThisProc;
            }
        }
    }
    else
    {
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: PASSTHRU ERROR uBitwise=%u, uByteSwap=%u and uReflect=%u not implemented yet!\n",
                                uBitwise, uByteSwap, uReflect);
        rc = -1;
    }

    return rc;
}


/**
 * Processes a SHA request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   uFunc               The engine specific function.
 * @param   fInit               Flag whether to initialize the context state.
 * @param   fEom                Flag whether this request marks the end ofthe message.
 */
static int pspDevCcpReqShaProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq, uint32_t uFunc,
                                  bool fInit, bool fEom)
{
    int rc = 0;
    uint32_t uShaType = CCP_V5_ENGINE_SHA_TYPE_GET(uFunc);

    /* Only sha256 implemented so far. */
    if (   uShaType == CCP_V5_ENGINE_SHA_TYPE_256
        || uShaType == CCP_V5_ENGINE_SHA_TYPE_384)
    {
        const EVP_MD *pOsslEvpSha = NULL;
        size_t cbLeft = pReq->cbSrc;
        size_t cbDigest = 0;
        CCPXFERCTX XferCtx;

        if (uShaType == CCP_V5_ENGINE_SHA_TYPE_256)
        {
            pOsslEvpSha = EVP_sha256();
            cbDigest = 32;
        }
        else
        {
            pOsslEvpSha = EVP_sha384();
            cbDigest = 48;
        }

         /*
          * The final SHA in the LSB seems to be in big endian format because it is always copied out
          * using the 256bit byteswap passthrough function. We will write it in reverse order here,
          * to avoid any hacks in the passthrough code.
          */
        rc = pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, true /*fSha*/, EVP_MD_size(pOsslEvpSha),
                                  true /*fWriteRev*/);
        if (!rc)
        {
            /*
             * The storage buffer contains the initial sha256 state, which we will ignore
             * because that is already part of the openssl context.
             */
#if 0
            if (fInit)
            {
                pThis->pOsslShaCtx = EVP_MD_CTX_new();
                if (!pThis->pOsslShaCtx)
                    rc = -1;
                else if (EVP_DigestInit_ex(pThis->pOsslShaCtx, pOsslEvpSha, NULL) != 1)
                    rc = -1;
            }
#else
            if (!pThis->pOsslShaCtx)
            {
                pThis->pOsslShaCtx = EVP_MD_CTX_new();
                if (   !pThis->pOsslShaCtx
                    || EVP_DigestInit_ex(pThis->pOsslShaCtx, pOsslEvpSha, NULL) != 1)
                rc = -1;
            }
#endif

            while (   !rc
                   && cbLeft)
            {
                uint8_t abData[256];
                size_t cbThisProc = MIN(cbLeft, sizeof(abData));

                rc = pspDevCcpXferCtxRead(&XferCtx, &abData[0], cbThisProc, NULL);
                if (!rc)
                {
                    if (EVP_DigestUpdate(pThis->pOsslShaCtx, &abData[0], cbThisProc) != 1)
                        rc = -1;
                }

                cbLeft -= cbThisProc;
            }

            if (   !rc
                && fEom)
            {
                /* Finalize state and write to the storage buffer. */
                uint8_t *pbDigest = alloca(cbDigest);
                if (EVP_DigestFinal_ex(pThis->pOsslShaCtx, pbDigest, NULL) == 1)
                    rc = pspDevCcpXferCtxWrite(&XferCtx, pbDigest, cbDigest, NULL);
                else
                    rc = -1;

                EVP_MD_CTX_free(pThis->pOsslShaCtx);
                pThis->pOsslShaCtx = NULL;
            }
        }
    }
    else
    {
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: SHA ERROR uShaType=%u fInit=%u fEom=%u u32ShaBitsHigh=%u u32ShaBitsLow=%u not implemented yet!\n",
                                uShaType, fInit, fEom, pReq->Op.Sha.u32ShaBitsHigh, pReq->Op.Sha.u32ShaBitsLow);
        rc = -1;
    }

    return rc;
}


/**
 * CCP AES passthrough operation.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   fUseIv              Flag whether the request uses an IV.
 */
static int pspDevCcpReqAesPassthrough(PPSPDEVCCP pThis, PCCCP5REQ pReq, bool fUseIv)
{
    int rc = 0;

    /*
     * Impose a limit on the amount of data to process for now, this should really be used
     * only for unwrapping the 128bit IKEK.
     */
    if (pReq->cbSrc <= _4K)
    {
        CCPXFERCTX XferCtx;
        uint8_t abSrc[_4K];
        uint8_t abDst[_4K];
        uint8_t abIv[128 / 8];
        uint8_t uLsbCtxId = CCP_V5_MEM_LSB_CTX_ID_GET(pReq->u16SrcMemType);
        CCPADDR CcpAddrIv = uLsbCtxId * sizeof(pThis->Lsb.u.aSlots[0].abData);
        CCPADDR CcpAddrKey = CCP_ADDR_CREATE_FROM_HI_LO(pReq->u16AddrKeyHigh, pReq->u32AddrKeyLow);
        uint32_t u32CcpSts;

        rc = pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, false /*fSha*/, pReq->cbSrc,
                                  false /*fWriteRev*/);
        if (!rc && fUseIv)
            rc = pspDevCcpCopyFromLsb(pThis, CcpAddrIv, sizeof(abIv), &abIv[0]);
        if (!rc)
            rc = pspDevCcpXferCtxRead(&XferCtx, &abSrc[0], pReq->cbSrc, NULL);
        if (!rc)
            rc = pThis->pDev->pCfg->pCcpProxyIf->pfnAesDo(pThis->pDev->pCfg->pCcpProxyIf,
                                                          pReq->u32Dw0, pReq->cbSrc,
                                                          &abSrc[0], &abDst[0], (uint32_t)CcpAddrKey,
                                                          fUseIv ? &abIv[0] : NULL, fUseIv ? sizeof(abIv) : 0,
                                                          &u32CcpSts);
        if (!rc)
        {
            if ((u32CcpSts & 0x3f) == CCP_V5_STATUS_SUCCESS)
                rc = pspDevCcpXferCtxWrite(&XferCtx, &abDst[0], pReq->cbSrc, NULL);
            else
            {
                PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                        "CCP: CCP returned status %#x!\n", u32CcpSts & 0x3f);
                rc = -1;
            }
        }
        else
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_FATAL_ERROR, PSPTRACEEVTORIGIN_CCP,
                                    "CCP: AES passthrough operation failed with %d!\n", rc);
    }
    else
    {
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_FATAL_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: AES passthrough with too much data %u!\n", pReq->cbSrc);
        rc = -1;
    }

    return rc;
}


/**
 * Processes a AES request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   uFunc               The engine specific function.
 * @param   fInit               Flag whether to initialize the context state.
 * @param   fEom                Flag whether this request marks the end ofthe message.
 */
static int pspDevCcpReqAesProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq, uint32_t uFunc,
                                  bool fInit, bool fEom)
{
    int     rc       = 0;
    uint8_t uSz      = CCP_V5_ENGINE_AES_SZ_GET(uFunc);
    uint8_t fEncrypt = CCP_V5_ENGINE_AES_ENCRYPT_GET(uFunc);
    uint8_t uMode    = CCP_V5_ENGINE_AES_MODE_GET(uFunc);
    uint8_t uAesType = CCP_V5_ENGINE_AES_TYPE_GET(uFunc);

    /* If the request uses a protected LSB and CCP passthrough is available we use the real CCP. */
    if (   CCP_V5_MEM_TYPE_GET(pReq->u16KeyMemType) == CCP_V5_MEM_TYPE_SB
        && CCP_ADDR_CREATE_FROM_HI_LO(pReq->u16AddrKeyHigh, pReq->u32AddrKeyLow) < 0xa0)
    {
        if (pThis->pDev->pCfg->pCcpProxyIf)
            return pspDevCcpReqAesPassthrough(pThis, pReq, uMode == CCP_V5_ENGINE_AES_MODE_CBC ? true : false /*fUseIv*/);
        else /* No key in the protected LSB means that the output is useless, leave an error. */
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_FATAL_ERROR, PSPTRACEEVTORIGIN_CCP,
                                    "CCP: Request accesses protected LSB for which there is no key set, decrypted output is useless and the emulation will fail\n");
    }

    if (   uSz == 0
        && (   uMode == CCP_V5_ENGINE_AES_MODE_ECB
            || uMode == CCP_V5_ENGINE_AES_MODE_CBC)
        && (   uAesType == CCP_V5_ENGINE_AES_TYPE_256
            || uAesType == CCP_V5_ENGINE_AES_TYPE_128))
    {
        const EVP_CIPHER *pOsslEvpAes = NULL;
        size_t cbLeft = pReq->cbSrc;
        size_t cbKey = 0;
        bool fUseIv = false;
        CCPXFERCTX XferCtx;

        if (uAesType == CCP_V5_ENGINE_AES_TYPE_256)
        {
            cbKey = 256 / 8;

            if (uMode == CCP_V5_ENGINE_AES_MODE_ECB)
                pOsslEvpAes = EVP_aes_256_ecb();
            else if (uMode == CCP_V5_ENGINE_AES_MODE_CBC)
            {
                pOsslEvpAes = EVP_aes_256_cbc();
                fUseIv = true;
            }
            else
            {
                PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_FATAL_ERROR, PSPTRACEEVTORIGIN_CCP, "CCP: Internal AES error");
                rc = -1;
            }

        }
        else if (uAesType == CCP_V5_ENGINE_AES_TYPE_128)
        {
            cbKey = 128 / 8;

            if (uMode == CCP_V5_ENGINE_AES_MODE_ECB)
                pOsslEvpAes = EVP_aes_128_ecb();
            else if (uMode == CCP_V5_ENGINE_AES_MODE_CBC)
            {
                pOsslEvpAes = EVP_aes_128_cbc();
                fUseIv = true;
            }
            else
            {
                PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_FATAL_ERROR, PSPTRACEEVTORIGIN_CCP, "CCP: Internal AES error");
                rc = -1;
            }
        }
        else
        {
            PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_FATAL_ERROR, PSPTRACEEVTORIGIN_CCP, "CCP: Internal AES error");
            rc = -1;
        }

        if (!rc)
            rc = pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, false /*fSha*/, pReq->cbSrc /**@todo Correct? */,
                                      false /*fWriteRev*/);
        if (!rc)
        {
            uint8_t abKey[256 / 8];
            uint8_t abIv[128 / 8];
            rc = pspDevCcpKeyCopyFromReq(pThis, pReq, cbKey, &abKey[0]);
            if (!rc) /* The key is given in reverse order (Linux kernel mentions big endian). */
                pspDevCcpReverseBuf(&abKey[0], cbKey);
            if (!rc && fUseIv)
            {
                /*
                 * The IV is always given in the LSB which ID is given in the source memory type.
                 * And we need to reverse the IV as well.
                 */
                uint8_t uLsbCtxId = CCP_V5_MEM_LSB_CTX_ID_GET(pReq->u16SrcMemType);
                CCPADDR CcpAddrIv = uLsbCtxId * sizeof(pThis->Lsb.u.aSlots[0].abData);

                rc = pspDevCcpCopyFromLsb(pThis, CcpAddrIv, sizeof(abIv), &abIv[0]);
                pspDevCcpReverseBuf(&abIv[0], sizeof(abIv));
            }
            if (!rc)
            {
                pThis->pOsslAesCtx = EVP_CIPHER_CTX_new();
                if (!pThis->pOsslAesCtx)
                    rc = -1;
                else if (fEncrypt)
                {
                    if (EVP_EncryptInit_ex(pThis->pOsslAesCtx, pOsslEvpAes, NULL, &abKey[0],
                                           fUseIv ? &abIv[0] : NULL) != 1)
                        rc = -1;
                }
                else
                {
                    if (EVP_DecryptInit_ex(pThis->pOsslAesCtx, pOsslEvpAes, NULL, &abKey[0],
                                           fUseIv ? &abIv[0] : NULL) != 1)
                        rc = -1;
                }

                if (EVP_CIPHER_CTX_set_padding(pThis->pOsslAesCtx, 0) != 1)
                    rc = -1;
            }

            while (   !rc
                   && cbLeft)
            {
                uint8_t abDataIn[512];
                uint8_t abDataOut[512];
                size_t cbThisProc = MIN(cbLeft, sizeof(abDataIn));
                int cbOut = 0;

                rc = pspDevCcpXferCtxRead(&XferCtx, &abDataIn[0], cbThisProc, NULL);
                if (!rc)
                {
                    if (fEncrypt)
                    {
                        if (EVP_EncryptUpdate(pThis->pOsslAesCtx, &abDataOut[0], &cbOut, &abDataIn[0], cbThisProc) != 1)
                            rc = -1;
                    }
                    else
                    {
                        if (EVP_DecryptUpdate(pThis->pOsslAesCtx, &abDataOut[0], &cbOut, &abDataIn[0], cbThisProc) != 1)
                            rc = -1;
                    }
                }

                if (   !rc
                    && cbOut)
                    rc = pspDevCcpXferCtxWrite(&XferCtx, &abDataOut[0], cbOut, NULL);

                cbLeft -= cbThisProc;
            }

            if (   !rc
                && fEom)
            {
                /* Finalize state. */
                uint8_t abDataOut[512];
                int cbOut = 0;

                if (fEncrypt)
                {
                    if (EVP_EncryptFinal_ex(pThis->pOsslAesCtx, &abDataOut[0], &cbOut) != 1)
                        rc = -1;
                }
                else
                {
                    if (EVP_DecryptFinal_ex(pThis->pOsslAesCtx, &abDataOut[0], &cbOut) != 1)
                        rc = -1;
                }

                if (   !rc
                    && cbOut)
                    rc = pspDevCcpXferCtxWrite(&XferCtx, &abDataOut[0], cbOut, NULL);

                EVP_CIPHER_CTX_free(pThis->pOsslAesCtx);
                pThis->pOsslAesCtx = NULL;
            }
        }
    }
    else
    {
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: AES ERROR uAesType=%u uMode=%u fEncrypt=%u uSz=%u not implemented yet!\n",
                                uAesType, uMode, fEncrypt, uSz);
        rc = -1;
    }

    return rc;
}


/**
 * Processes a ZLIB decompression request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   uFunc               The engine specific function.
 * @param   fInit               Flag whether to initialize the context state.
 * @param   fEom                Flag whether this request marks the end ofthe message.
 */
static int pspDevCcpReqZlibProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq, uint32_t uFunc,
                                   bool fInit, bool fEom)
{
    (void)uFunc; /* Ignored */

    CCPXFERCTX XferCtx;
    int rc = pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, false /*fSha*/, UINT32_MAX,
                                  false /*fWriteRev*/);
    if (!rc)
    {
        size_t cbReadLeft = pReq->cbSrc;

        if (fInit)
        {
            memset(&pThis->Zlib, 0, sizeof(pThis->Zlib));
            int rcZlib = inflateInit2(&pThis->Zlib, Z_DEF_WBITS);
            if (rcZlib < 0)
                rc = -1;
        }

        uint8_t abDecomp[_4K];
        uint32_t offDecomp = 0;
        memset(&abDecomp[0], 0, sizeof(abDecomp));

        while (   !rc
               && cbReadLeft)
        {
            uint8_t abData[_4K];
            size_t cbThisRead = MIN(cbReadLeft, sizeof(abData));

            rc = pspDevCcpXferCtxRead(&XferCtx, &abData[0], cbThisRead, NULL);
            if (!rc)
            {
                pThis->Zlib.avail_in = cbThisRead;
                pThis->Zlib.next_in  = &abData[0];

                while (   pThis->Zlib.avail_in
                       && !rc)
                {
                    size_t cbDecompLeft = sizeof(abDecomp) - offDecomp;

                    pThis->Zlib.next_out  = (Bytef *)&abDecomp[offDecomp];
                    pThis->Zlib.avail_out = cbDecompLeft;

                    int rcZlib = inflate(&pThis->Zlib, Z_NO_FLUSH);
                    if (pThis->Zlib.avail_out < cbDecompLeft)
                    {
                        offDecomp += cbDecompLeft - pThis->Zlib.avail_out;
                        /* Write the chunk if the decompression buffer is full. */
                        if (offDecomp == sizeof(abDecomp))
                        {
                            rc = pspDevCcpXferCtxWrite(&XferCtx, &abDecomp[0], sizeof(abDecomp), NULL);
                            offDecomp = 0; /* Off to the next round. */
                        }
                    }
                    if (   !rc
                        && rcZlib == Z_STREAM_END)
                        break;
                }
            }

            cbReadLeft -= cbThisRead;
        }

        /* Write the last chunk. */
        if (   !rc
            && offDecomp)
            rc = pspDevCcpXferCtxWrite(&XferCtx, &abDecomp[0], offDecomp, NULL);

        if (fEom)
        {
            int rcZlib = inflateEnd(&pThis->Zlib);
            if (   rcZlib < 0
                && !rc)
                rc = -1;
        }
    }

    return rc;
}


/**
 * Processes a RSA request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   uFunc               The engine specific function.
 * @param   fInit               Flag whether to initialize the context state.
 * @param   fEom                Flag whether this request marks the end ofthe message.
 */
static int pspDevCcpReqRsaProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq, uint32_t uFunc,
                                  bool fInit, bool fEom)
{
    int      rc    = 0;
    uint16_t uSz   = CCP_V5_ENGINE_RSA_SZ_GET(uFunc);
    uint8_t  uMode = CCP_V5_ENGINE_RSA_MODE_GET(uFunc);

    /* Support RSA 2048 and 4096 */
    if (   uMode == 0
        && (   (   uSz == 256
                && pReq->cbSrc == 512)
            || (   uSz == 512
                && pReq->cbSrc == 1024)))
    {
        /* The key contains the exponent as a 2048bit or 4096bit integer. */
        uint8_t abExp[512];
        rc = pspDevCcpKeyCopyFromReq(pThis, pReq, uSz, &abExp[0]);
        if (!rc)
        {
            bool fFreeBignums = true;
            BIGNUM *pExp = BN_lebin2bn(&abExp[0], uSz / 2, NULL);
            RSA *pRsaPubKey = RSA_new();
            if (pExp && pRsaPubKey)
            {
                CCPXFERCTX XferCtx;
                rc = pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, false /*fSha*/, uSz,
                                          false /*fWriteRev*/);
                if (!rc)
                {
                    /*
                     * The source buffer contains the modulus as a 2048bit integer in little endian format
                     * followed by the message the process (why the modulus is not part of the key buffer
                     * remains a mystery).
                     */
                    uint8_t abData[1024];

                    rc = pspDevCcpXferCtxRead(&XferCtx, &abData[0], pReq->cbSrc, NULL);
                    if (!rc)
                    {
                        BIGNUM *pMod = BN_lebin2bn(&abData[0], pReq->cbSrc / 2, NULL);
                        if (pMod)
                        {
                            uint8_t abResult[512];

                            RSA_set0_key(pRsaPubKey, pMod, pExp, NULL);

                            /* The RSA public key structure has taken over the memory and freeing it will free the exponent and modulus as well. */
                            fFreeBignums = false;

                            /* Need to convert to little endian format. */
                            pspDevCcpReverseBuf(&abData[uSz], pReq->cbSrc / 2);
                            size_t cbEnc = RSA_public_encrypt(pReq->cbSrc / 2, &abData[uSz], &abResult[0], pRsaPubKey, RSA_NO_PADDING);
                            if (cbEnc == uSz)
                            {
                                /* Need to swap endianess of result buffer as well. */
                                pspDevCcpReverseBuf(&abResult[0], uSz);
                                rc = pspDevCcpXferCtxWrite(&XferCtx, &abResult[0], uSz, NULL);
                            }
                            else
                                rc = -1;

                            if (fFreeBignums)
                                BN_clear_free(pMod);
                        }
                        else
                            rc = -1;
                    }
                }
            }
            else
                rc = -1;

            if (pRsaPubKey)
                RSA_free(pRsaPubKey);
            if (fFreeBignums && pExp)
                BN_clear_free(pExp);
        }
    }
    else
    {
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: RSA ERROR uMode=%u uSz=%u not implemented yet!\n",
                                uMode, uSz);
        rc = -1;
    }

    return rc;
}


/**
 * Creates the elliptic curve calculation context.
 *
 * @todo The coefficient seems to be the "a" coefficient.
 *       But that doesn't make sense, as that one is mostly -3.
 *       It should be both or the "b" coefficient.
 *       Well, for the moment we simply return the NIST P-384 curve
 *       and assert that the Prime is correct.
 *
 * @returns                     The ecc context used for calculations or NULL on error.
 * @param   BnCtx               The bignum context used for calculations.
 * @param   pPrime              The prime of the field for the curve.
 * @param   pCoeff              A coefficient of the curve.
 */
static EC_GROUP *pspDevCcpEccGetGroup(BN_CTX *pBnCtx, const BIGNUM *pPrime,
                                      PCCCP5ECCNUM pCoeff)
{
    (void)pCoeff;

    /* Check that the prime is correct. */
    /* P-384 prime = 2^384 - 2^128  - 2^96 + 2^32 - 1 */
    uint8_t abPrime384[49] = {
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, //  64
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, // 128
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 192
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 256
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 320
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 384
        0
    };

    BIGNUM *pPrime384 = BN_lebin2bn(abPrime384, sizeof(abPrime384), NULL);
    if (!pPrime384)
        return NULL;
    if (BN_cmp(pPrime, pPrime384) != 0)
        return NULL;

    BN_free(pPrime384);
    return EC_GROUP_new_by_curve_name(EC_curve_nist2nid("P-384"));
}


/**
 * Writes an output number.
 *
 * @returns                     Status code.
 * @param   XferCtx             The context to be written to.
 * @param   Result              The number to be written.
 */
static int pspDevCcpReqEccReturnNumber(PCCPXFERCTX pXferCtx, const BIGNUM *pResult)
{
    CCP5ECCNUM Out;

    /* This should never happen. */
    if (BN_num_bytes(pResult) > sizeof(Out.abNum))
        return -1;

    if (BN_bn2lebinpad(pResult, &Out.abNum[0], sizeof(Out.abNum)) == 0)
        return -1;

    return pspDevCcpXferCtxWrite(pXferCtx, &Out.abNum[0], sizeof(Out.abNum), NULL);
}


/**
 * Writes an output point.
 *
 * @returns                     Status code.
 * @param   pXferCtx            The context to be written to.
 * @param   pBnCtx              The context for BIGNUM operations.
 * @param   pCurve              The curve on which the point lies.
 */
static int pspDevCcpReqEccReturnPoint(PCCPXFERCTX pXferCtx, BN_CTX *pBnCtx,
                                      const EC_GROUP *pCurve, const EC_POINT *pPoint)
{
    int rc = -1;

    BIGNUM *pX = BN_new();
    BIGNUM *pY = BN_new();

    if (pX && pY)
    {
        if (EC_POINT_get_affine_coordinates(pCurve, pPoint, pX, pY, pBnCtx))
        {
            rc = pspDevCcpReqEccReturnNumber(pXferCtx, pX);
            if (STS_SUCCESS(rc))
                rc = pspDevCcpReqEccReturnNumber(pXferCtx, pY);
        }
    }

    if (pX)
        BN_free(pX);
    if (pY)
        BN_free(pY);

    return rc;
}


/**
 * Processes an ECC request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 * @param   uFunc               The engine specific function.
 * @param   fInit               Flag whether to initialize the context state.
 * @param   fEom                Flag whether this request marks the end of the message.
 */
static int pspDevCcpReqEccProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq, uint32_t uFunc,
                                  bool fInit, bool fEom)
{
    uint16_t uBits = CCP_V5_ENGINE_ECC_BIT_COUNT_GET(uFunc);
    uint8_t  uOp   = CCP_V5_ENGINE_ECC_OP_GET(uFunc);
    /* Size of the output. */
    uint8_t  uSz   = uOp <= CCP_V5_ENGINE_ECC_OP_ADD_CURVE ? sizeof(CCP5ECCNUM) : sizeof(CCP5ECCPT);

    /* Check bit count (we have 0x48 bytes, or 576 bits) */
    if (uBits > sizeof(CCP5ECCNUM) * 8)
    {
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: ECC ERROR uBits=%u is too large!\n",
                                uBits);
        return -1;
    }

    /* Create Transfer Context. */
    CCPXFERCTX XferCtx;
    if (pspDevCcpXferCtxInit(&XferCtx, pThis, pReq, false /*fSha*/, uSz, false /*fWriteRev*/))
        return -1;

    /* Try to read data. */
    CCP5ECCREQ EccReq;
    if (pspDevCcpXferCtxRead(&XferCtx, &EccReq, sizeof(EccReq), NULL))
        return -1;

    /* Logging */
    pspDevCcpDumpEccReq(uOp, &EccReq);

    int rc = -1;

    /* Create BIGNUM context and prime BIGNUM */
    BN_CTX *pBnCtx = BN_CTX_new();
    BIGNUM *pPrime = BN_lebin2bn(&EccReq.Prime.abNum[0], sizeof(EccReq.Prime.abNum), NULL);
    if (pBnCtx && pPrime)
    {
        switch (uOp)
        {
            case CCP_V5_ENGINE_ECC_OP_MUL_FIELD:
            {
                BIGNUM *pFactor1 = BN_lebin2bn(&EccReq.Op.FieldMul.Factor1.abNum[0], sizeof(EccReq.Op.FieldMul.Factor1.abNum), NULL);
                BIGNUM *pFactor2 = BN_lebin2bn(&EccReq.Op.FieldMul.Factor2.abNum[0], sizeof(EccReq.Op.FieldMul.Factor2.abNum), NULL);
                BIGNUM *pProduct = BN_new();

                if (   pFactor1
                    && pFactor2
                    && pProduct
                    && BN_mod_mul(pProduct, pFactor1, pFactor2, pPrime, pBnCtx))
                    rc = pspDevCcpReqEccReturnNumber(&XferCtx, pProduct);

                if (pFactor1)
                    BN_free(pFactor1);
                if (pFactor2)
                    BN_free(pFactor2);
                if (pProduct)
                    BN_free(pProduct);
                break;
            }
            case CCP_V5_ENGINE_ECC_OP_ADD_FIELD:
            {
                BIGNUM *pSummand1 = BN_lebin2bn(&EccReq.Op.FieldAdd.Summand1.abNum[0], sizeof(EccReq.Op.FieldAdd.Summand1.abNum), NULL);
                BIGNUM *pSummand2 = BN_lebin2bn(&EccReq.Op.FieldAdd.Summand2.abNum[0], sizeof(EccReq.Op.FieldAdd.Summand2.abNum), NULL);
                BIGNUM *pSum = BN_new();

                if (   pSummand1
                    && pSummand2
                    && pSum
                    && BN_mod_add(pSum, pSummand1, pSummand2, pPrime, pBnCtx))
                    rc = pspDevCcpReqEccReturnNumber(&XferCtx, pSum);

                if (pSummand1)
                    BN_free(pSummand1);
                if (pSummand2)
                    BN_free(pSummand2);
                if (pSum)
                    BN_free(pSum);
                break;
            }
            case CCP_V5_ENGINE_ECC_OP_INV_FIELD:
            {
                BIGNUM *pNumber = BN_lebin2bn(&EccReq.Op.FieldInv.Num.abNum[0], sizeof(EccReq.Op.FieldInv.Num.abNum), NULL);
                if (pNumber)
                {
                    BIGNUM *pInverse = BN_mod_inverse(NULL, pNumber, pPrime, pBnCtx);
                    if (pInverse)
                    {
                        rc = pspDevCcpReqEccReturnNumber(&XferCtx, pInverse);
                        BN_free(pInverse);
                    }

                    BN_free(pNumber);
                }
                break;
            }
            case CCP_V5_ENGINE_ECC_OP_MUL_CURVE:
            {
                BIGNUM *pPtX = BN_lebin2bn(&EccReq.Op.CurveMul.Point.X.abNum[0], sizeof(EccReq.Op.CurveMul.Point.X.abNum), NULL);
                BIGNUM *pPtY = BN_lebin2bn(&EccReq.Op.CurveMul.Point.Y.abNum[0], sizeof(EccReq.Op.CurveMul.Point.Y.abNum), NULL);
                BIGNUM *pFactor = BN_lebin2bn(&EccReq.Op.CurveMul.Factor.abNum[0], sizeof(EccReq.Op.CurveMul.Factor.abNum), NULL);
                EC_GROUP *pCurve = pspDevCcpEccGetGroup(pBnCtx, pPrime, &EccReq.Op.CurveMul.Coefficient);

                /* These can take NULL as an argument. */
                EC_POINT *pPoint = EC_POINT_new(pCurve);
                EC_POINT *pResult = EC_POINT_new(pCurve);

                if (   pPtX
                    && pPtY
                    && pFactor
                    && pCurve
                    && pPoint
                    && pResult
                    && EC_POINT_set_affine_coordinates(pCurve, pPoint, pPtX, pPtY, pBnCtx)
                    && EC_POINT_mul(pCurve, pResult, NULL, pPoint, pFactor, pBnCtx))
                    rc = pspDevCcpReqEccReturnPoint(&XferCtx, pBnCtx, pCurve, pResult);

                if (pPoint)
                    EC_POINT_free(pPoint);
                if (pResult)
                    EC_POINT_free(pResult);
                if (pCurve)
                    EC_GROUP_free(pCurve);
                if (pPtX)
                    BN_free(pPtX);
                if (pPtY)
                    BN_free(pPtY);
                if (pFactor)
                    BN_free(pFactor);
                break;
            }
            case CCP_V5_ENGINE_ECC_OP_MUL_ADD_CURVE:
            {
                BIGNUM *pPt1X    = BN_lebin2bn(&EccReq.Op.CurveMulAdd.Point1.X.abNum[0], sizeof(EccReq.Op.CurveMulAdd.Point1.X.abNum), NULL);
                BIGNUM *pPt1Y    = BN_lebin2bn(&EccReq.Op.CurveMulAdd.Point1.Y.abNum[0], sizeof(EccReq.Op.CurveMulAdd.Point1.Y.abNum), NULL);
                BIGNUM *pFactor1 = BN_lebin2bn(&EccReq.Op.CurveMulAdd.Factor1.abNum[0], sizeof(EccReq.Op.CurveMulAdd.Factor1.abNum), NULL);
                BIGNUM *pPt2X    = BN_lebin2bn(&EccReq.Op.CurveMulAdd.Point2.X.abNum[0], sizeof(EccReq.Op.CurveMulAdd.Point2.X.abNum), NULL);
                BIGNUM *pPt2Y    = BN_lebin2bn(&EccReq.Op.CurveMulAdd.Point2.Y.abNum[0], sizeof(EccReq.Op.CurveMulAdd.Point2.Y.abNum), NULL);
                BIGNUM *pFactor2 = BN_lebin2bn(&EccReq.Op.CurveMulAdd.Factor2.abNum[0], sizeof(EccReq.Op.CurveMulAdd.Factor2.abNum), NULL);
                EC_GROUP *pCurve = pspDevCcpEccGetGroup(pBnCtx, pPrime, &EccReq.Op.CurveMulAdd.Coefficient);

                /* These can take NULL as an argument. */
                EC_POINT *pPt1 = EC_POINT_new(pCurve);
                EC_POINT *pPt2 = EC_POINT_new(pCurve);
                EC_POINT *pResult = EC_POINT_new(pCurve);

                if (   pPt1X && pPt1Y && pFactor1
                    && pPt2X && pPt2Y && pFactor2
                    && pCurve && pPt1 && pPt2
                    && pResult
                    && EC_POINT_set_affine_coordinates(pCurve, pPt1, pPt1X, pPt1Y, pBnCtx)
                    && EC_POINT_set_affine_coordinates(pCurve, pPt2, pPt2X, pPt2Y, pBnCtx)
                    && EC_POINT_mul(pCurve, pResult, NULL, pPt1, pFactor1, pBnCtx)
                    && EC_POINT_mul(pCurve, pPt1, NULL, pPt2, pFactor2, pBnCtx)
                    && EC_POINT_add(pCurve, pResult, pResult, pPt1, pBnCtx))
                    rc = pspDevCcpReqEccReturnPoint(&XferCtx, pBnCtx, pCurve, pResult);

                if (pPt1)
                    EC_POINT_free(pPt1);
                if (pPt2)
                    EC_POINT_free(pPt2);
                if (pResult)
                    EC_POINT_free(pResult);
                if (pCurve)
                    EC_GROUP_free(pCurve);
                if (pPt1X)
                    BN_free(pPt1X);
                if (pPt1Y)
                    BN_free(pPt1Y);
                if (pFactor1)
                    BN_free(pFactor1);
                if (pPt2X)
                    BN_free(pPt2X);
                if (pPt2Y)
                    BN_free(pPt2Y);
                if (pFactor2)
                    BN_free(pFactor2);
                break;
            }
            default:
                PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                        "CCP: ECC ERROR: Unimplemented/Unknown operation %u\n", uOp);
        }
    }

    if (pPrime)
        BN_free(pPrime);
    if (pBnCtx)
        BN_CTX_free(pBnCtx);

    if (STS_FAILURE(rc))
    {
        const char *pszErr = ERR_error_string(ERR_get_error(), NULL);
        PSPEmuTraceEvtAddString(NULL, PSPTRACEEVTSEVERITY_ERROR, PSPTRACEEVTORIGIN_CCP,
                                "CCP: ECC ERROR: %s\n", pszErr);
    }

    return rc;
}


/**
 * Processes the given request.
 *
 * @returns Status code.
 * @param   pThis               The CCP device instance data.
 * @param   pReq                The request to process.
 */
static int pspDevCcpReqProcess(PPSPDEVCCP pThis, PCCCP5REQ pReq)
{
    int rc = 0;
    uint32_t uEngine   = CCP_V5_ENGINE_GET(pReq->u32Dw0);
    uint32_t uFunction = CCP_V5_ENGINE_FUNC_GET(pReq->u32Dw0);
    bool     fInit     = CCP_V5_ENGINE_INIT_GET(pReq->u32Dw0);
    bool     fEom      = CCP_V5_ENGINE_EOM_GET(pReq->u32Dw0);

    switch (uEngine)
    {
        case CCP_V5_ENGINE_PASSTHRU:
        {
            rc = pspDevCcpReqPassthruProcess(pThis, pReq, uFunction);
            break;
        }
        case CCP_V5_ENGINE_SHA:
        {
            rc = pspDevCcpReqShaProcess(pThis, pReq, uFunction, fInit, fEom);
            break;
        }
        case CCP_V5_ENGINE_AES:
        {
            rc = pspDevCcpReqAesProcess(pThis, pReq, uFunction, fInit, fEom);
            break;
        }
        case CCP_V5_ENGINE_ZLIB_DECOMP:
        {
            rc = pspDevCcpReqZlibProcess(pThis, pReq, uFunction, fInit, fEom);
            break;
        }
        case CCP_V5_ENGINE_RSA:
        {
            rc = pspDevCcpReqRsaProcess(pThis, pReq, uFunction, fInit, fEom);
            break;
        }
        case CCP_V5_ENGINE_ECC:
        {
            rc = pspDevCcpReqEccProcess(pThis, pReq, uFunction, fInit, fEom);
            break;
        }
        case CCP_V5_ENGINE_XTS_AES128:
        case CCP_V5_ENGINE_DES3:
            /** @todo */
        default:
            rc = -1;
    }

    return rc;
}


/**
 * Executes the given queue if it is enabled.
 *
 * @returns nothing.
 * @param   pThis               The CCP device instance data.
 * @param   pQueue              The queue to run.
 */
static void pspDevCcpQueueRunMaybe(PPSPDEVCCP pThis, PCCPQUEUE pQueue)
{
    if (pQueue->fEnabled)
    {
        /* Clear halt and running bit. */
        pQueue->u32RegCtrl &= ~CCP_V5_Q_REG_CTRL_HALT;

        const uint32_t u32ReqTail = pQueue->u32RegReqTail;
              uint32_t u32ReqHead = pQueue->u32RegReqHead;
        const size_t   cbQueue    = CCP_V5_Q_REG_CTRL_Q_SZ_GET_SIZE(pQueue->u32RegCtrl);

        while (u32ReqTail != u32ReqHead)
        {
            CCP5REQ Req;

/** @todo The CCP does some sort of wraparound for the queue when it reaches the end based on the size
 * but every attempt to implement this broke either the on chip or off chip BL or the secure OS.
 * Need to figure out how exactly this works.
 */
#if 0
            u32ReqHead &= ~(cbQueue - 1); /* Size is a power of two. */
#endif
            int rc = PSPEmuIoMgrPspAddrRead(pThis->pDev->hIoMgr, u32ReqHead, &Req, sizeof(Req));
            if (!rc)
            {
                pspDevCcpDumpReq(&Req, u32ReqHead);
                rc = pspDevCcpReqProcess(pThis, &Req);
                if (!rc)
                {
                    pQueue->u32RegSts = CCP_V5_Q_REG_STATUS_SUCCESS;
                    pQueue->u32RegIsts |= CCP_V5_Q_REG_ISTS_COMPLETION;
                }
                else
                {
                    pQueue->u32RegSts = CCP_V5_Q_REG_STATUS_ERROR;
                    pQueue->u32RegIsts |= CCP_V5_Q_REG_ISTS_ERROR;
                    break;
                }
            }
            else
            {
                printf("CCP: Failed to read request from 0x%08x with rc=%d\n", u32ReqHead, rc);
                pQueue->u32RegSts = CCP_V5_Q_REG_STATUS_ERROR; /* Signal error. */
                pQueue->u32RegIsts |= CCP_V5_Q_REG_ISTS_ERROR;
                break;
            }

            u32ReqHead += sizeof(Req);
        }

        /* Set halt bit again. */
        pQueue->u32RegReqHead = u32ReqHead;
        pQueue->u32RegCtrl |= CCP_V5_Q_REG_CTRL_HALT;
        pQueue->u32RegIsts |= CCP_V5_Q_REG_ISTS_Q_STOP;
        if (u32ReqTail == u32ReqHead)
            pQueue->u32RegIsts |= CCP_V5_Q_REG_ISTS_Q_EMPTY;

        /* Issue an interrupt request if there is something pending. */
        if (pQueue->u32RegIen & pQueue->u32RegIsts)
            pThis->pDev->pDevIf->pfnIrqSet(pThis->pDev->pDevIf, 0 /*idPrio*/, 0x15 /*idDev*/, true /*fAssert*/);
    }
}


/**
 * Handles register read from a specific queue.
 *
 * @returns nothing.
 * @param   pThis               The CCP device instance data.
 * @param   pQueue              The queue to read a register from.
 * @param   offRegQ             The register offset to read from.
 * @param   pu32Dst             Whereto store the register content.
 */
static void pspDevCcpMmioQueueRegRead(PPSPDEVCCP pThis, PCCPQUEUE pQueue, uint32_t offRegQ, uint32_t *pu32Dst)
{
    switch (offRegQ)
    {
        case CCP_V5_Q_REG_CTRL:
            *pu32Dst = pQueue->u32RegCtrl;
            break;
        case CCP_V5_Q_REG_HEAD:
            *pu32Dst = pQueue->u32RegReqHead;
            break;
        case CCP_V5_Q_REG_TAIL:
            *pu32Dst = pQueue->u32RegReqTail;
            break;
        case CCP_V5_Q_REG_STATUS:
            *pu32Dst = pQueue->u32RegSts;
            break;
        case CCP_V5_Q_REG_IEN:
            *pu32Dst = pQueue->u32RegIen;
            break;
        case CCP_V5_Q_REG_ISTS:
            *pu32Dst = pQueue->u32RegIsts;
            break;
        default:
            *pu32Dst = 0;
            break;
    }

    /*
     * This used to be in the write handler where it would make probably more sense
     * but this caused a fatal stack overwrite during the last CCP request of the on chip bootloader
     * to presumably overwrite some scratch buffer with data. The request is triggered by the
     * function at address 0xffff48c8 in our on chip bootloader version from a 1st gen Epyc CPU.
     *
     * The request looks like the following:
     * CCP Request 0x0003f900:
     *     u32Dw0:             0x00500011 (Engine: PASSTHROUGH, ByteSwap: NOOP, Bitwise: NOOP, Reflect: 0)
     *     cbSrc:              27160
     *     u32AddrSrcLow:      0x00000000
     *     u16AddrSrcHigh:     0x00000000
     *     u16SrcMemType:      0x000001d2 (MemType: 2, LsbCtxId: 116, Fixed: 0)
     *     u32AddrDstLow:      0x00038500
     *     u16AddrDstHigh:     0x00000000
     *     u16DstMemType:      0x00000002 (MemType: 2, Fixed: 0)
     *     u32AddrKeyLow:      0x00000000
     *     u16AddrKeyHigh:     0x00000000
     *     u16KeyMemType:      0x00000000
     *
     * The CCP writes 27160 bytes starting at 0x38500 which spills into the stack of the on chip bootloader
     * ranging from 0x3efff down to 0x3ef00. This will overwrite the stack return address of the on_chip_bl_ccp_start_cmd()
     * function at 0xffff7878 with an invalid value causing a CPU exception.
     *
     * The only reason this doesn't blows up on real hardware is the asynchronous nature of the CCP. When the request is started
     * the ARM core will execute the return instruction before the CCP can trash the stack frame and leave the dangerous zone.
     * The code called afterwards to wait for the CCP to finish doesn't need any stack and everything else is preserved making
     * the on chip bootloader survive and successfully call into the off chip bootloader. So the obvious fix with our synchronous
     * CCP implementation is to defer the request until the bootloader polls the control register to wait for the CCP to halt again.
     * Thanks AMD!
     */
    pspDevCcpQueueRunMaybe(pThis, pQueue);
}


/**
 * Handles a register write to a specific queue.
 *
 * @returns nothing.
 * @param   pThis               The CCP device instance data.
 * @param   pQueue              The queue to write to.
 * @param   offRegQ             Offset of the register to write.
 * @param   u32Val              The value to write
 */
static void pspDevCcpMmioQueueRegWrite(PPSPDEVCCP pThis, PCCPQUEUE pQueue, uint32_t offRegQ, uint32_t u32Val)
{
    switch (offRegQ)
    {
        case CCP_V5_Q_REG_CTRL:
            if (   (u32Val & CCP_V5_Q_REG_CTRL_RUN)
                && !pQueue->fEnabled)
                pQueue->fEnabled = true;
            else if (   !(u32Val & CCP_V5_Q_REG_CTRL_RUN)
                     && pQueue->fEnabled)
                pQueue->fEnabled = false;

            pQueue->u32RegCtrl = u32Val & ~CCP_V5_Q_REG_CTRL_RUN; /* The run bit seems to be always cleared. */
            break;
        case CCP_V5_Q_REG_HEAD:
            pQueue->u32RegReqHead = u32Val;
            break;
        case CCP_V5_Q_REG_TAIL:
            pQueue->u32RegReqTail = u32Val;
            break;
        case CCP_V5_Q_REG_STATUS:
            pQueue->u32RegSts = u32Val;
            break;
        case CCP_V5_Q_REG_IEN:
            pQueue->u32RegIen = u32Val;
            break;
        case CCP_V5_Q_REG_ISTS:
        {
            /* Set bits clear the corresponding interrupt. */
            pQueue->u32RegIsts &= ~u32Val;

            /* Reset the interrupt line if there is nothing pending anymore. */
            if (!(pQueue->u32RegIen & pQueue->u32RegIsts))
                pThis->pDev->pDevIf->pfnIrqSet(pThis->pDev->pDevIf, 0 /*idPrio*/, 0x15 /*idDev*/, false /*fAssert*/);
            break;
        }
    }

    /*
     * Execute queue requests if there is at least a single interrupt enabled.
     * We don't execute requests here unconditionally due to the comment in
     * pspDevCcpMmioQueueRegRead().
     */
    if (pQueue->u32RegIen)
        pspDevCcpQueueRunMaybe(pThis, pQueue);
}


static void pspDevCcpMmioRead(PSPADDR offMmio, size_t cbRead, void *pvDst, void *pvUser)
{
    PPSPDEVCCP pThis = (PPSPDEVCCP)pvUser;

    if (cbRead != sizeof(uint32_t))
    {
        printf("%s: offMmio=%#x cbRead=%zu -> Unsupported access width\n", __FUNCTION__, offMmio, cbRead);
        return;
    }

    if (offMmio >= CCP_V5_Q_OFFSET)
    {
        /* Queue access. */
        offMmio -= CCP_V5_Q_OFFSET;
        uint32_t uQueue = offMmio / CCP_V5_Q_SIZE;
        uint32_t offRegQ = offMmio % CCP_V5_Q_SIZE;

        if (uQueue < ELEMENTS(pThis->aQueues))
            pspDevCcpMmioQueueRegRead(pThis, &pThis->aQueues[uQueue], offRegQ, (uint32_t *)pvDst);
        else
            printf("%s: offMmio=%#x cbRead=%zu uQueue=%u -> Invalid queue\n", __FUNCTION__, offMmio, cbRead, uQueue);
    }
    else
    {
        /** @todo Global register access. */
        memset(pvDst, 0, cbRead);
    }
}


static void pspDevCcpMmioWrite(PSPADDR offMmio, size_t cbWrite, const void *pvVal, void *pvUser)
{
    PPSPDEVCCP pThis = (PPSPDEVCCP)pvUser;

    if (cbWrite != sizeof(uint32_t))
    {
        printf("%s: offMmio=%#x cbWrite=%zu -> Unsupported access width\n", __FUNCTION__, offMmio, cbWrite);
        return;
    }

    if (offMmio >= CCP_V5_Q_OFFSET)
    {
        /* Queue access. */
        offMmio -= CCP_V5_Q_OFFSET;
        uint32_t uQueue = offMmio / CCP_V5_Q_SIZE;
        uint32_t offRegQ = offMmio % CCP_V5_Q_SIZE;

        if (uQueue < ELEMENTS(pThis->aQueues))
            pspDevCcpMmioQueueRegWrite(pThis, &pThis->aQueues[uQueue], offRegQ, *(const uint32_t *)pvVal);
        else
            printf("%s: offMmio=%#x cbWrite=%zu uQueue=%u -> Invalid queue\n", __FUNCTION__, offMmio, cbWrite, uQueue);
    }
    else
    {
        /** @todo Global register access. */
    }
}


static void pspDevCcpMmioRead2(PSPADDR offMmio, size_t cbRead, void *pvDst, void *pvUser)
{
    PPSPDEVCCP pThis = (PPSPDEVCCP)pvUser;

    if (cbRead != sizeof(uint32_t))
    {
        printf("%s: offMmio=%#x cbRead=%zu -> Unsupported access width\n", __FUNCTION__, offMmio, cbRead);
        return;
    }

    switch (offMmio)
    {
        case 0x28: /* Contains the transfer size of the last oepration? (Zen2 uses it to read the decompressed size). */
            *(uint32_t *)pvDst = pThis->cbWrittenLast;
            break;
        case 0x38:
            *(uint32_t *)pvDst = 0x1; /* Zen1 on chip BL waits for bit 0 to become 1. */
            break;
        default:
            *(uint32_t *)pvDst = 0;
    }
}


static int pspDevCcpInit(PPSPDEV pDev)
{
    PPSPDEVCCP pThis = (PPSPDEVCCP)&pDev->abInstance[0];

    pThis->pDev        = pDev;
    pThis->pOsslShaCtx = NULL;

    for (unsigned i = 0; i < ELEMENTS(pThis->aQueues); i++)
    {
        pThis->aQueues[i].u32RegCtrl = CCP_V5_Q_REG_CTRL_HALT; /* Halt bit set. */
        pThis->aQueues[i].u32RegSts  = CCP_V5_Q_REG_STATUS_SUCCESS;
        pThis->aQueues[i].u32RegIen  = 0;
        pThis->aQueues[i].u32RegIsts = 0;
        pThis->aQueues[i].fEnabled   = false;
    }

    /* Register MMIO ranges. */
    int rc = PSPEmuIoMgrMmioRegister(pDev->hIoMgr, CCP_V5_MMIO_ADDRESS, CCP_V5_Q_OFFSET + ELEMENTS(pThis->aQueues) * CCP_V5_Q_SIZE,
                                     pspDevCcpMmioRead, pspDevCcpMmioWrite, pThis,
                                     "CCPv5 Global+Queue", &pThis->hMmio);
    /** @todo Not sure this really belongs to the CCP (could be some other hardware block) but
     * a register in that range is accessed starting with Zen2 after a CCP zlib decompression operation.
     */
    if (!rc)
        rc = PSPEmuIoMgrMmioRegister(pDev->hIoMgr, CCP_V5_MMIO_ADDRESS_2, CCP_V5_MMIO_SIZE_2,
                                     pspDevCcpMmioRead2, NULL, pThis,
                                     "CCPv5 + 0x6000", &pThis->hMmio2);
    return rc;
}


static void pspDevCcpDestruct(PPSPDEV pDev)
{
    /* Nothing to do so far. */
}


/**
 * Device registration structure.
 */
const PSPDEVREG g_DevRegCcpV5 =
{
    /** pszName */
    "ccp-v5",
    /** pszDesc */
    "CCPv5",
    /** cbInstance */
    sizeof(PSPDEVCCP),
    /** pfnInit */
    pspDevCcpInit,
    /** pfnDestruct */
    pspDevCcpDestruct,
    /** pfnReset */
    NULL
};

