/** @file
 * PSP Emulator - Entry point.
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
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <libpspproxy.h>

#include <common/cdefs.h>
#include <common/status.h>
#include <psp-fw/boot-rom-svc-page.h>

#include <psp-ccd.h>
#include <psp-dbg.h>
#include <psp-flash.h>
#include <psp-proxy.h>
#include <psp-profile.h>
#include <psp-iolog-replay.h>


static uint32_t g_idSocketSingle = UINT32_MAX;
static uint32_t g_idCcdSingle = UINT32_MAX;


/**
 * Available options for PSPEmu.
 */
static struct option g_aOptions[] =
{
    {"emulation-mode",               required_argument, 0, 'm'},
    {"flash-rom",                    required_argument, 0, 'f'},
    {"on-chip-bl",                   required_argument, 0, 'o'},
    {"boot-rom-svc-page",            required_argument, 0, 's'},
    {"boot-rom-svc-page-dont-alter", no_argument,       0, 'n'},
    {"bin-load",                     required_argument, 0, 'b'},
    {"bin-contains-hdr",             no_argument,       0, 'p'},
    {"dbg",                          required_argument, 0, 'd'},
    {"load-psp-dir",                 no_argument,       0, 'l'},
    {"psp-dbg-mode",                 no_argument,       0, 'g'},
    {"psp-proxy-addr",               required_argument, 0, 'x'},
    {"trace-log",                    required_argument, 0, 't'},
    {"psp-profile",                  required_argument, 0, 'a'},
    {"cpu-profile",                  required_argument, 0, 'c'},
    {"intercept-svc-6",              no_argument,       0, '6'},
    {"trace-svcs",                   no_argument,       0, 'v'},
    {"acpi-state",                   required_argument, 0, 'i'},
    {"uart-remote-addr",             required_argument, 0, 'u'},
    {"timer-real-time",              no_argument      , 0, 'r'},
    {"spi-flash-trace",              required_argument, 0, 'F'},
    {"coverage-trace",               required_argument, 0, 'V'},
    {"sockets",                      required_argument, 0, 'S'},
    {"ccds-per-socket",              required_argument, 0, 'C'},
    {"emulate-single-socket-id",     required_argument, 0, 'O'},
    {"emulate-single-die-id",        required_argument, 0, 'D'},
    {"emulate-devices",              required_argument, 0, 'E'},
    {"iom-log-all-accesses",         no_argument      , 0, 'I'},
    {"io-log-write",                 required_argument, 0, 'L'},
    {"io-log-replay",                required_argument, 0, 'Y'},
    {"proxy-buffer-writes",          no_argument      , 0, 'P'},
    {"dbg-step-count",               required_argument, 0, 'G'},
    {"dbg-run-up-to",                required_argument, 0, 'U'},
    {"proxy-trusted-os-handover",    required_argument, 0, 'T'},
    {"proxy-ccp",                    no_argument,       0, 'X'},
    {"proxy-x86-cores-no-release",   no_argument,       0, '8'},
    {"memory-preload",               required_argument, 0, 'M'},
    {"memory-create",                required_argument, 0, 'R'},
    {"proxy-memory-wt",              required_argument, 0, 'W'},
    {"single-step-dump-core-state",  no_argument,       0, 'A'},

    {"help",                         no_argument,       0, 'H'},
    {0, 0, 0, 0}
};


/**
 * Frees all allocated resources for the given config.
 *
 * @returns nothing.
 * @param   pCfg                    The config to free all resources from.
 */
static void pspEmuCfgFree(PPSPEMUCFG pCfg)
{
    if (pCfg->hDbgHlp)
        PSPEmuDbgHlpRelease(pCfg->hDbgHlp);

    if (   pCfg->pvOnChipBl
        && pCfg->cbOnChipBl)
        PSPEmuFlashFree(pCfg->pvOnChipBl, pCfg->cbOnChipBl);

    if (   pCfg->pvFlashRom
        && pCfg->cbFlashRom)
        PSPEmuFlashFree(pCfg->pvFlashRom, pCfg->cbFlashRom);

    if (   pCfg->pvBinLoad
        && pCfg->cbBinLoad)
        PSPEmuFlashFree(pCfg->pvBinLoad, pCfg->cbBinLoad);

    if (   pCfg->pvBootRomSvcPage
        && pCfg->cbBootRomSvcPage)
        PSPEmuFlashFree(pCfg->pvBootRomSvcPage, pCfg->cbBootRomSvcPage);

    if (pCfg->papszDevs)
    {
        uint32_t idx = 0;
        while (pCfg->papszDevs[idx])
        {
            free((void *)pCfg->papszDevs[idx]);
            idx++;
        }

        free(pCfg->papszDevs);
    }

    if (pCfg->paMemCreate)
        free((void *)pCfg->paMemCreate);

    if (pCfg->paMemPreload)
        free((void *)pCfg->paMemPreload);

    if (pCfg->paProxyMemWt)
        free((void *)pCfg->paProxyMemWt);
}


/**
 * Parses the given emulated device string and returns an array with individual entries.
 *
 * @returns Pointer to the Array of individual device entries on success.
 * @param   pszDevString            The device string form the command line to parse.
 */
static const char **pspEmuCfgParseDevices(const char *pszDevString)
{
    /* Count the number of : separators first. */
    uint32_t cDevs = 1; /* Account for the NULL entry in the table. */
    const char *pszCur = pszDevString;
    while (*pszCur != '\0')
    {
        char *pszSep = strchr(pszCur, ':');
        if (!pszSep) /* Last device? */
            pszSep = strchr(pszCur, '\0');
        if (!pszSep)
            break;

        cDevs++;
        if (*pszSep != '\0')
            pszCur = pszSep + 1;
        else
            pszCur = pszSep;
    }

    const char **papszDevs = (const char **)calloc(cDevs, sizeof(const char *));
    if (papszDevs)
    {
        uint32_t idxDev = 0;

        pszCur = pszDevString;
        while (*pszCur != '\0')
        {
            char *pszSep = strchr(pszCur, ':');
            if (!pszSep)
                pszSep = strchr(pszCur, '\0');
            if (!pszSep)
                break;

            papszDevs[idxDev] = strndup(pszCur, pszSep - pszCur);
            if (   !papszDevs[idxDev]
                && idxDev > 0)
            {
                /* Rollback. */
                while (idxDev)
                {
                    free((void *)papszDevs[idxDev - 1]);
                    idxDev--;
                }

                free(papszDevs);
                papszDevs = NULL;
                break;
            }

            idxDev++;
            if (*pszSep != '\0')
                pszCur = pszSep + 1;
            else
                pszCur = pszSep;
        }
    }

    return papszDevs;
}


/**
 * Parses a signle given preload descriptor string and adds it to the given config.
 *
 * @returns Status code.
 * @param   pCfg                    The config to add the descriptor to upon success.
 * @param   pszPreload              The preload descriptor string to parse.
 */
static int pspEmuCfgMemPreloadParse(PPSPEMUCFG pCfg, const char *pszPreload)
{
    int rc = STS_INF_SUCCESS;
    PSPEMUCFGMEMPRELOAD MemPreload;
    const char *pszCur = pszPreload;

    char *pszSep = strchr(pszCur, ':');
    if (pszSep)
    {
        if (!strncmp(pszCur, "psp", pszSep - pszCur))
            MemPreload.enmAddrSpace = PSPADDRSPACE_PSP;
        else if (!strncmp(pszCur, "smn", pszSep - pszCur))
            MemPreload.enmAddrSpace = PSPADDRSPACE_SMN;
        else if (!strncmp(pszCur, "x86", pszSep - pszCur))
            MemPreload.enmAddrSpace = PSPADDRSPACE_X86;
        else
            rc = STS_ERR_INVALID_PARAMETER;

        if (STS_SUCCESS(rc))
        {
            pszCur = pszSep + 1;
            pszSep = strchr(pszCur, ':');

            if (pszSep)
            {
                char *pszEndPtr;

                errno = 0;
                uint64_t uAddr = strtoull(pszCur, &pszEndPtr, 0);
                if (   !errno
                    && pszEndPtr == pszSep)
                {
                    switch (MemPreload.enmAddrSpace)
                    {
                        case PSPADDRSPACE_PSP:
                        {
                            if (uAddr == (PSPPADDR)uAddr)
                                MemPreload.u.PspAddr = (PSPPADDR)uAddr;
                            else
                                rc = STS_ERR_BUFFER_OVERFLOW;
                            break;
                        }
                        case PSPADDRSPACE_SMN:
                        {
                            if (uAddr == (SMNADDR)uAddr)
                                MemPreload.u.SmnAddr = (SMNADDR)uAddr;
                            else
                                rc = STS_ERR_BUFFER_OVERFLOW;
                            break;
                        }
                        case PSPADDRSPACE_X86:
                        {
                            MemPreload.u.PhysX86Addr = uAddr;
                            break;
                        }
                        default:
                            rc = STS_ERR_INVALID_PARAMETER;
                    }

                    if (STS_SUCCESS(rc))
                    {
                        MemPreload.pszFilePreload = pszSep + 1;

                        /* Add the descriptor the array. */
                        uint32_t cMemPreloadNew = pCfg->cMemPreload + 1;
                        PCPSPEMUCFGMEMPRELOAD paMemPreloadNew = (PCPSPEMUCFGMEMPRELOAD)realloc((void *)pCfg->paMemPreload,
                                                                                               cMemPreloadNew * sizeof(MemPreload));
                        if (paMemPreloadNew)
                        {
                            pCfg->paMemPreload = paMemPreloadNew;
                            pCfg->cMemPreload  = cMemPreloadNew;
                            memcpy((void *)&pCfg->paMemPreload[cMemPreloadNew - 1], &MemPreload, sizeof(MemPreload));
                        }
                        else
                            rc = STS_ERR_NO_MEMORY;
                    }
                }
                else
                    rc = STS_ERR_INVALID_PARAMETER;
            }
            else
                rc = STS_ERR_INVALID_PARAMETER;
        }
    }
    else
        rc = STS_ERR_INVALID_PARAMETER;

    return rc;
}


/**
 * Parses a signle given preload descriptor string and adds it to the given config.
 *
 * @returns Status code.
 * @param   pCfg                    The config to add the descriptor to upon success.
 * @param   pszRegion               The region descriptor string to parse.
 */
static int pspEmuCfgMemRegionParse(PPSPEMUCFG pCfg, const char *pszRegion)
{
    int rc = STS_INF_SUCCESS;
    PSPEMUCFGMEMREGIONCREATE MemRegion;
    const char *pszCur = pszRegion;

    char *pszSep = strchr(pszCur, ':');
    if (pszSep)
    {
        if (!strncmp(pszCur, "psp", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_PSP;
        else if (!strncmp(pszCur, "smn", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_SMN;
        else if (!strncmp(pszCur, "x86", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_X86;
        else
            rc = STS_ERR_INVALID_PARAMETER;

        if (STS_SUCCESS(rc))
        {
            pszCur = pszSep + 1;
            pszSep = strchr(pszCur, ':');

            if (pszSep)
            {
                char *pszEndPtr;

                errno = 0;
                uint64_t uAddr = strtoull(pszCur, &pszEndPtr, 0);
                if (   !errno
                    && pszEndPtr == pszSep)
                {
                    switch (MemRegion.enmAddrSpace)
                    {
                        case PSPADDRSPACE_PSP:
                        {
                            if (uAddr == (PSPPADDR)uAddr)
                                MemRegion.u.PspAddr = (PSPPADDR)uAddr;
                            else
                                rc = STS_ERR_BUFFER_OVERFLOW;
                            break;
                        }
                        case PSPADDRSPACE_SMN:
                        {
                            if (uAddr == (SMNADDR)uAddr)
                                MemRegion.u.SmnAddr = (SMNADDR)uAddr;
                            else
                                rc = STS_ERR_BUFFER_OVERFLOW;
                            break;
                        }
                        case PSPADDRSPACE_X86:
                        {
                            MemRegion.u.PhysX86Addr = uAddr;
                            break;
                        }
                        default:
                            rc = STS_ERR_INVALID_PARAMETER;
                    }

                    if (STS_SUCCESS(rc))
                    {
                        MemRegion.cbRegion = strtoull(pszSep + 1, &pszEndPtr, 0);
                        if (*pszEndPtr == '\0')
                        {
                            /* Add the descriptor the array. */
                            uint32_t cMemCreateNew = pCfg->cMemCreate + 1;
                            PCPSPEMUCFGMEMREGIONCREATE paMemCreateNew = (PCPSPEMUCFGMEMREGIONCREATE)realloc((void *)pCfg->paMemCreate,
                                                                                                            cMemCreateNew * sizeof(MemRegion));
                            if (paMemCreateNew)
                            {
                                pCfg->paMemCreate = paMemCreateNew;
                                pCfg->cMemCreate  = cMemCreateNew;
                                memcpy((void *)&pCfg->paMemCreate[cMemCreateNew - 1], &MemRegion, sizeof(MemRegion));
                            }
                            else
                                rc = STS_ERR_NO_MEMORY;
                        }
                        else
                            rc = STS_ERR_INVALID_PARAMETER;
                    }
                }
                else
                    rc = STS_ERR_INVALID_PARAMETER;
            }
            else
                rc = STS_ERR_INVALID_PARAMETER;
        }
    }
    else
        rc = STS_ERR_INVALID_PARAMETER;

    return rc;
}


/**
 * Parses a single given proxy memory region write through descriptor string and adds it to the given config.
 *
 * @returns Status code.
 * @param   pCfg                    The config to add the descriptor to upon success.
 * @param   pszRegion               The region descriptor string to parse.
 */
static int pspEmuCfgProxyMemRegionWtParse(PPSPEMUCFG pCfg, const char *pszRegion)
{
    int rc = STS_INF_SUCCESS;
    PSPEMUCFGPROXYMEMWT MemRegion;
    const char *pszCur = pszRegion;

    char *pszSep = strchr(pszCur, ':');
    if (pszSep)
    {
        if (!strncmp(pszCur, "psp", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_PSP;
        else if (!strncmp(pszCur, "psp-mem", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_PSP_MEM;
        else if (!strncmp(pszCur, "psp-mmio", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_PSP_MMIO;
        else if (!strncmp(pszCur, "smn", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_SMN;
        else if (!strncmp(pszCur, "x86", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_X86;
        else if (!strncmp(pszCur, "x86-mem", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_X86_MEM;
        else if (!strncmp(pszCur, "x86-mmio", pszSep - pszCur))
            MemRegion.enmAddrSpace = PSPADDRSPACE_X86_MMIO;
        else
            rc = STS_ERR_INVALID_PARAMETER;

        if (STS_SUCCESS(rc))
        {
            pszCur = pszSep + 1;
            pszSep = strchr(pszCur, ':');

            if (pszSep)
            {
                char *pszEndPtr;

                errno = 0;
                uint64_t uAddr = strtoull(pszCur, &pszEndPtr, 0);
                if (   !errno
                    && pszEndPtr == pszSep)
                {
                    switch (MemRegion.enmAddrSpace)
                    {
                        case PSPADDRSPACE_PSP:
                        {
                            if (uAddr == (PSPPADDR)uAddr)
                                MemRegion.u.PspAddr = (PSPPADDR)uAddr;
                            else
                                rc = STS_ERR_BUFFER_OVERFLOW;
                            break;
                        }
                        case PSPADDRSPACE_SMN:
                        {
                            if (uAddr == (SMNADDR)uAddr)
                                MemRegion.u.SmnAddr = (SMNADDR)uAddr;
                            else
                                rc = STS_ERR_BUFFER_OVERFLOW;
                            break;
                        }
                        case PSPADDRSPACE_X86:
                        {
                            MemRegion.u.PhysX86Addr = uAddr;
                            break;
                        }
                        default:
                            rc = STS_ERR_INVALID_PARAMETER;
                    }

                    if (STS_SUCCESS(rc))
                    {
                        MemRegion.cbRegion = strtoull(pszSep + 1, &pszEndPtr, 0);
                        if (*pszEndPtr == '\0')
                        {
                            /* Add the descriptor the array. */
                            uint32_t cProxyMemWtNew = pCfg->cProxyMemWt + 1;
                            PCPSPEMUCFGPROXYMEMWT paMemWtNew = (PCPSPEMUCFGPROXYMEMWT)realloc((void *)pCfg->paMemCreate,
                                                                                              cProxyMemWtNew * sizeof(MemRegion));
                            if (paMemWtNew)
                            {
                                pCfg->paProxyMemWt = paMemWtNew;
                                pCfg->cProxyMemWt  = cProxyMemWtNew;
                                memcpy((void *)&pCfg->paProxyMemWt[cProxyMemWtNew - 1], &MemRegion, sizeof(MemRegion));
                            }
                            else
                                rc = STS_ERR_NO_MEMORY;
                        }
                        else
                            rc = STS_ERR_INVALID_PARAMETER;
                    }
                }
                else
                    rc = STS_ERR_INVALID_PARAMETER;
            }
            else
                rc = STS_ERR_INVALID_PARAMETER;
        }
    }
    else
        rc = STS_ERR_INVALID_PARAMETER;

    return rc;
}


/**
 * Parses the command line arguments and creates the emulator config.
 *
 * @returns Status code.
 * @param   argc        Argument count as passed from main.
 * @param   argv        Argument vector as passed from main.
 * @param   pCfg        The config structure to initialize.
 */
static int pspEmuCfgParse(int argc, char *argv[], PPSPEMUCFG pCfg)
{
    int ch = 0;
    int idxOption = 0;

    pCfg->enmMode               = PSPEMUMODE_INVALID;
    pCfg->pszPathFlashRom       = NULL;
    pCfg->pszPathOnChipBl       = NULL;
    pCfg->pszPathBinLoad        = NULL;
    pCfg->pszPathBootRomSvcPage = NULL;
    pCfg->fBinContainsHdr       = false;
    pCfg->uDbgPort              = 0;
    pCfg->cDbgInsnStep          = 0;
    pCfg->PspAddrDbgRunUpTo     = UINT32_MAX;
    pCfg->fLoadPspDir           = false;
    pCfg->fIncptSvc6            = false;
    pCfg->fTraceSvcs            = false;
    pCfg->fTimerRealtime        = false;
    pCfg->fBootRomSvcPageModify = true;
    pCfg->fIomLogAllAccesses    = false;
    pCfg->fProxyWrBuffer        = false;
    pCfg->fCcpProxy             = false;
    pCfg->fProxyBlockX86CoreRelease = false;
    pCfg->pvFlashRom            = NULL;
    pCfg->cbFlashRom            = 0;
    pCfg->pvOnChipBl            = NULL;
    pCfg->cbOnChipBl            = 0;
    pCfg->pvBinLoad             = NULL;
    pCfg->cbBinLoad             = 0;
    pCfg->pvBootRomSvcPage      = NULL;
    pCfg->cbBootRomSvcPage      = 0;
    pCfg->pszPspProxyAddr       = NULL;
    pCfg->PspAddrProxyTrustedOsHandover = 0;
    pCfg->pszTraceLog           = NULL;
    pCfg->pCpuProfile           = NULL;
    pCfg->pPspProfile           = NULL;
    pCfg->enmAcpiState          = PSPEMUACPISTATE_S5;
    pCfg->pszUartRemoteAddr     = NULL;
    pCfg->pszSpiFlashTrace      = NULL;
    pCfg->pszIoLog              = NULL;
    pCfg->pszIoLogReplay        = NULL;
    pCfg->pszCovTrace           = NULL;
    pCfg->cSockets              = 1;
    pCfg->cCcdsPerSocket        = 1;
    pCfg->paMemCreate           = NULL;
    pCfg->cMemCreate            = 0;
    pCfg->paMemPreload          = NULL;
    pCfg->cMemPreload           = 0;
    pCfg->paProxyMemWt          = NULL;
    pCfg->cProxyMemWt           = 0;
    pCfg->papszDevs             = NULL;
    pCfg->pCcpProxyIf           = NULL;
    pCfg->hDbgHlp               = NULL;
    pCfg->fSingleStepDumpCoreState = false;

    while ((ch = getopt_long (argc, argv, "hpbr8N:m:f:o:d:s:x:a:c:u:S:C:O:D:E:V:U:P:T:M:R:L:Y:W:IA", &g_aOptions[0], &idxOption)) != -1)
    {
        switch (ch)
        {
            case 'h':
            case 'H':
                PSPCfgHelp(argv[0], true /*fVerbose*/);
                exit(0);
                break;

            case 'm':
                if (!strcmp(optarg, "app"))
                    pCfg->enmMode = PSPEMUMODE_APP;
                else if (!strcmp(optarg, "sys"))
                    pCfg->enmMode = PSPEMUMODE_SYSTEM;
                else if (!strcmp(optarg, "on-chip-bl"))
                    pCfg->enmMode = PSPEMUMODE_SYSTEM_ON_CHIP_BL;
                else if (!strcmp(optarg, "trusted-os"))
                    pCfg->enmMode = PSPEMUMODE_TRUSTED_OS;
                else
                {
                    fprintf(stderr, "--emulation-mode takes only one of [app|sys|on-chip-bl|trusted-os] as the emulation mode\n");
                    return -1;
                }
                break;
            case 'f':
                pCfg->pszPathFlashRom = optarg;
                break;
            case 's':
                pCfg->pszPathBootRomSvcPage = optarg;
                break;
            case 'n':
                pCfg->fBootRomSvcPageModify = false;
                break;
            case 'o':
                pCfg->pszPathOnChipBl = optarg;
                break;
            case 'p':
                pCfg->fBinContainsHdr = true;
                break;
            case 'b':
                pCfg->pszPathBinLoad = optarg;
                break;
            case 'd':
                pCfg->uDbgPort = strtoul(optarg, NULL, 10);
                break;
            case 'l':
                pCfg->fLoadPspDir = true;
                break;
            case 'g':
                pCfg->fPspDbgMode = true;
                break;
            case 'x':
                pCfg->pszPspProxyAddr = optarg;
                break;
            case 't':
                pCfg->pszTraceLog = optarg;
                break;
            case 'a':
            {
                pCfg->pPspProfile = PSPProfilePspGetById(optarg);
                if (!pCfg->pPspProfile)
                {
                    fprintf(stderr, "The PSP profile \"%s\" could not be found\n", optarg);
                    return -1;
                }
                break;
            }
            case 'c':
            {
                pCfg->pCpuProfile = PSPProfileAmdCpuGetById(optarg);
                if (!pCfg->pCpuProfile)
                {
                    fprintf(stderr, "The CPU profile \"%s\" could not be found\n", optarg);
                    return -1;
                }

                if (!pCfg->pPspProfile) /* Can be overwritten with a dedicated PSP profile argument. */
                    pCfg->pPspProfile = pCfg->pCpuProfile->pPspProfile;
                break;
            }
            case 'i':
            {
                if (!strcasecmp(optarg, "s0"))
                    pCfg->enmAcpiState = PSPEMUACPISTATE_S0;
                else if (!strcasecmp(optarg, "s1"))
                    pCfg->enmAcpiState = PSPEMUACPISTATE_S1;
                else if (!strcasecmp(optarg, "s2"))
                    pCfg->enmAcpiState = PSPEMUACPISTATE_S2;
                else if (!strcasecmp(optarg, "s3"))
                    pCfg->enmAcpiState = PSPEMUACPISTATE_S3;
                else if (!strcasecmp(optarg, "s4"))
                    pCfg->enmAcpiState = PSPEMUACPISTATE_S4;
                else if (!strcasecmp(optarg, "s5"))
                    pCfg->enmAcpiState = PSPEMUACPISTATE_S5;
                else
                {
                    fprintf(stderr, "Unrecognised ACPI state: %s\n", optarg);
                    return -1;
                }
                break;
            }
            case '6':
                pCfg->fIncptSvc6 = true;
                break;
            case 'v':
                pCfg->fTraceSvcs = true;
                break;
            case 'u':
                pCfg->pszUartRemoteAddr = optarg;
                break;
            case 'r':
                pCfg->fTimerRealtime = true;
                break;
            case 'S':
                pCfg->cSockets = strtoul(optarg, NULL, 10);
                break;
            case 'C':
                pCfg->cCcdsPerSocket = strtoul(optarg, NULL, 10);
                break;
            case 'O':
                g_idSocketSingle = strtoul(optarg, NULL, 10);
                break;
            case 'D':
                g_idCcdSingle = strtoul(optarg, NULL, 10);
                break;
            case 'E':
                pCfg->papszDevs = pspEmuCfgParseDevices(optarg);
                break;
            case 'F':
                pCfg->pszSpiFlashTrace = optarg;
                break;
            case 'V':
                pCfg->pszCovTrace = optarg;
                break;
            case 'I':
                pCfg->fIomLogAllAccesses = true;
                break;
            case 'L':
                pCfg->pszIoLog = optarg;
                break;
            case 'Y':
                pCfg->pszIoLogReplay = optarg;
                break;
            case 'P':
                pCfg->fProxyWrBuffer = true;
                break;
            case 'G':
                pCfg->cDbgInsnStep = strtoul(optarg, NULL, 10);
                break;
            case 'U':
                pCfg->PspAddrDbgRunUpTo = strtoul(optarg, NULL, 0);
                break;
            case 'T':
                pCfg->PspAddrProxyTrustedOsHandover = strtoul(optarg, NULL, 0);
                break;
            case 'X':
                pCfg->fCcpProxy = true;
                break;
            case 'M':
            {
                int rc = pspEmuCfgMemPreloadParse(pCfg, optarg);
                if (STS_FAILURE(rc))
                    return rc;
                break;
            }
            case 'R':
            {
                int rc = pspEmuCfgMemRegionParse(pCfg, optarg);
                if (STS_FAILURE(rc))
                    return rc;
                break;
            }
            case 'W':
            {
                int rc = pspEmuCfgProxyMemRegionWtParse(pCfg, optarg);
                if (STS_FAILURE(rc))
                    return rc;
                break;
            }
            case 'A':
                pCfg->fSingleStepDumpCoreState = true;
                break;
            case '8':
                pCfg->fProxyBlockX86CoreRelease = true;
                break;
            default:
                fprintf(stderr, "Unrecognised option: -%c\n", optopt);
                return -1;
        }
    }

    if (pCfg->enmMode == PSPEMUMODE_INVALID)
    {
        fprintf(stderr, "--emulation-mode is mandatory\n");
        return -1;
    }

    if (   pCfg->cSockets < 1
        || pCfg->cSockets > 2)
    {
        fprintf(stderr, "--sockets argument must be in range [1..2]\n");
        return -1;
    }

    if (   pCfg->cCcdsPerSocket < 1
        || pCfg->cCcdsPerSocket > 4)
    {
        fprintf(stderr, "--ccds-per-socket argument must be in range [1..4]\n");
        return -1;
    }

    /* Do some sanity checks of the config here. */
    if (!pCfg->pszPathFlashRom)
    {
        fprintf(stderr, "Flash ROM path is required\n");
        return -1;
    }

    if (   !pCfg->pszPathOnChipBl
        && pCfg->enmMode == PSPEMUMODE_SYSTEM_ON_CHIP_BL)
    {
        fprintf(stderr, "The on chip bootloader binary is required for the selected emulation mode\n");
        return -1;
    }

    if (   pCfg->enmMode != PSPEMUMODE_SYSTEM_ON_CHIP_BL
        && !pCfg->pszPathBinLoad)
    {
        fprintf(stderr, "Loading the designated binary from the flash image is not implemented yet, please load the binary explicitely using --bin-load\n");
        return -1;
    }

    if (   pCfg->fIncptSvc6
        && pCfg->enmMode == PSPEMUMODE_APP)
    {
        fprintf(stderr, "Application mode and explicit SVC 6 interception are mutually exclusive (svc 6 is always intercepted in app mode)\n");
        return -1;
    }

    if (   pCfg->fTraceSvcs
        && pCfg->enmMode == PSPEMUMODE_APP)
    {
        fprintf(stderr, "Application mode and SVC tracing are mutually exclusive (svcs are always traced in app mode)\n");
        return -1;
    }

    if (   pCfg->pszIoLogReplay
        && pCfg->pszPspProxyAddr)
    {
        fprintf(stderr, "Proxy mode and I/O log replay are mutually exclusive\n");
        return STS_ERR_GENERAL_ERROR;
    }

    int rc = 0;
    if (pCfg->pszPathOnChipBl)
    {
        rc = PSPEmuFlashLoadFromFile(pCfg->pszPathOnChipBl, &pCfg->pvOnChipBl, &pCfg->cbOnChipBl);
        if (rc)
            fprintf(stderr, "Loading the on chip bootloader ROM failed with %d\n", rc);
    }

    if (!rc)
    {
        rc = PSPEmuFlashLoadFromFile(pCfg->pszPathFlashRom, &pCfg->pvFlashRom, &pCfg->cbFlashRom);
        if (rc)
            fprintf(stderr, "Loading the flash ROM failed with %d\n", rc);
    }

    if (   !rc
        && pCfg->pszPathBinLoad)
    {
        rc = PSPEmuFlashLoadFromFile(pCfg->pszPathBinLoad, &pCfg->pvBinLoad, &pCfg->cbBinLoad);
        if (rc)
            fprintf(stderr, "Loading the binary \"%s\" failed with %d\n", pCfg->pszPathBinLoad, rc);
    }

    if (   !rc
        && pCfg->pszPathBootRomSvcPage)
    {
        rc = PSPEmuFlashLoadFromFile(pCfg->pszPathBootRomSvcPage, &pCfg->pvBootRomSvcPage, &pCfg->cbBootRomSvcPage);
        if (rc)
            fprintf(stderr, "Loading the boot ROM service page from the given file failed with %d\n", rc);
    }

    if (rc)
        pspEmuCfgFree(pCfg);

    return rc;
}


/**
 * Executes the given CCD under debugger control.
 *
 * @returns Status code.
 * @param   hCcd                    The CCD instance to run in a debugger.
 * @param   pCfg                    The configuration.
 */
static int pspEmuDbgRun(PSPCCD hCcd, PCPSPEMUCFG pCfg)
{
    PSPCORE hPspCore = NULL;

    int rc = PSPEmuCcdQueryCore(hCcd, &hPspCore);
    if (!rc)
    {
        /*
         * Execute one instruction to initialize the CPU state properly
         * so the debugger has valid values to work with.
         */
        int rc = PSPEmuCoreExecRun(hPspCore, PSPEMU_CORE_EXEC_F_DEFAULT, 1, PSPEMU_CORE_EXEC_INDEFINITE);
        if (!rc)
        {
            PSPDBG hDbg = NULL;

            rc = PSPEmuDbgCreate(&hDbg, pCfg->uDbgPort, pCfg->cDbgInsnStep, pCfg->PspAddrDbgRunUpTo,
                                 &hCcd, 1, pCfg->hDbgHlp);
            if (!rc)
            {
                printf("Debugger is listening on port %u...\n", pCfg->uDbgPort);
                rc = PSPEmuDbgRunloop(hDbg);
            }
        }
    }

    return rc;
}


int main(int argc, char *argv[])
{
    PSPEMUCFG Cfg;

    /* Parse the config first. */
    int rc = pspEmuCfgParse(argc, argv, &Cfg);
    if (!rc)
    {
        /* Create a debug helper module if the debugger is going to be used. */
        if (Cfg.uDbgPort)
            rc = PSPEmuDbgHlpCreate(&Cfg.hDbgHlp);

        if (STS_SUCCESS(rc))
        {
            PSPCCD hCcd = NULL;
            if (   g_idSocketSingle != UINT32_MAX
                && g_idCcdSingle != UINT32_MAX)
                rc = PSPEmuCcdCreate(&hCcd, g_idSocketSingle, g_idCcdSingle, &Cfg);
            else
                rc = PSPEmuCcdCreate(&hCcd, 0, 0, &Cfg);

            if (!rc)
            {
                PSPPROXY hProxy = NULL;
                PSPIOLOGREPLAY hIoLogReplay = NULL;

                /* Setup the proxy if configured. */
                if (Cfg.pszPspProxyAddr)
                {
                    rc = PSPProxyCreate(&hProxy, &Cfg);
                    if (!rc)
                        rc = PSPProxyCcdRegister(hProxy, hCcd);
                }
                else if (Cfg.pszIoLogReplay)
                {
                    rc = PSPIoLogReplayCreate(&hIoLogReplay, Cfg.pszIoLogReplay);
                    if (STS_SUCCESS(rc))
                        rc = PSPIoLogReplayCcdRegister(hIoLogReplay, hCcd);
                }

                if (!rc)
                {
                    if (Cfg.uDbgPort)
                        rc = pspEmuDbgRun(hCcd, &Cfg);
                    else
                        rc = PSPEmuCcdRun(hCcd);
                }

                if (hProxy)
                {
                    PSPProxyCcdDeregister(hProxy, hCcd);
                    PSPProxyDestroy(hProxy);
                }

                if (hIoLogReplay)
                {
                    PSPIoLogReplayCcdDeregister(hIoLogReplay, hCcd);
                    PSPIoLogReplayDestroy(hIoLogReplay);
                }

                PSPEmuCcdDestroy(hCcd);
            }
        }

        pspEmuCfgFree(&Cfg);
    }
    else
        fprintf(stderr, "Parsing arguments failed with %d\n", rc);

    return 0;
}

