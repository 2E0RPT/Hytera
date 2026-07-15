#include <windows.h>
#include <winsnmp.h>
#include <stdio.h>
#include <conio.h>

#define ESC_KEY 27  

// Define explicit signatures using precise WinSNMP typings
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpStartup)(smiLPUINT32, smiLPUINT32, smiLPUINT32, smiLPUINT32, smiLPUINT32);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpCleanup)(VOID);
typedef HSNMP_SESSION  (WINAPI *pfnSnmpCreateSession)(HWND, UINT, SNMPAPI_CALLBACK, LPVOID);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpClose)(HSNMP_SESSION);
typedef HSNMP_ENTITY   (WINAPI *pfnSnmpStrToEntity)(HSNMP_SESSION, LPCSTR);
typedef HSNMP_CONTEXT  (WINAPI *pfnSnmpStrToContext)(HSNMP_SESSION, smiLPOCTETS);
typedef HSNMP_PDU      (WINAPI *pfnSnmpCreatePdu)(HSNMP_SESSION, smiINT, smiINT32, smiINT32, smiINT32, HSNMP_VBL);
typedef HSNMP_VBL      (WINAPI *pfnSnmpCreateVbl)(HSNMP_SESSION, smiLPOID, smiLPVALUE);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpSendMsg)(HSNMP_SESSION, HSNMP_ENTITY, HSNMP_ENTITY, HSNMP_CONTEXT, HSNMP_PDU);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpRecvMsg)(HSNMP_SESSION, HSNMP_ENTITY*, HSNMP_ENTITY*, HSNMP_CONTEXT*, HSNMP_PDU*);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpGetPduData)(HSNMP_PDU, smiLPINT, smiLPINT32, smiLPINT32, smiLPINT32, HSNMP_VBL*);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpGetVb)(HSNMP_VBL, smiUINT32, smiLPOID, smiLPVALUE);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpCountVbl)(HSNMP_VBL);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpFreeDescriptor)(smiUINT32, smiLPOCTETS);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpFreeEntity)(HSNMP_ENTITY);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpFreeContext)(HSNMP_CONTEXT);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpFreePdu)(HSNMP_PDU);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpFreeVbl)(HSNMP_VBL);
typedef SNMPAPI_STATUS (WINAPI *pfnSnmpStrToOid)(LPCSTR, smiLPOID);

typedef struct {
    const char *title;
    const char *oid_str;
} HyteraMetric;

int main() {
    char *repeater_ip = "192.168.1.167";  
    char *community = "public";           

    // Target telemetry nodes from the HYTERA-REPEATER-MIB
    HyteraMetric metrics[] = {
        { "PA Temp", "1.3.6.1.4.1.40297.1.2.1.2.2.0" },
        { "Supply V", "1.3.6.1.4.1.40297.1.2.1.2.1.0" },
        { "Radio St", "1.3.6.1.4.1.40297.1.2.2.6.0" }
    };
    int total_metrics = sizeof(metrics) / sizeof(metrics);

    printf("==================================================\n");
    printf(" Polling Hytera RD985 at %s via WinSNMP\n", repeater_ip);
    printf(" Press [ESCAPE] at any time to exit the loop.\n");
    printf("==================================================\n\n");

    // Dynamically load modern Windows SNMP engine
    HMODULE hWinSnmp = LoadLibrary("wsnmp32.dll");
    if (!hWinSnmp) {
        fprintf(stderr, "Fatal Error: Unable to load wsnmp32.dll.\n");
        return 1;
    }

    // Resolve API Entry points safely
    pfnSnmpStartup fnSnmpStartup = (pfnSnmpStartup)GetProcAddress(hWinSnmp, "SnmpStartup");
    pfnSnmpCleanup fnSnmpCleanup = (pfnSnmpCleanup)GetProcAddress(hWinSnmp, "SnmpCleanup");
    pfnSnmpCreateSession fnSnmpCreateSession = (pfnSnmpCreateSession)GetProcAddress(hWinSnmp, "SnmpCreateSession");
    pfnSnmpClose fnSnmpClose = (pfnSnmpClose)GetProcAddress(hWinSnmp, "SnmpClose");
    pfnSnmpStrToEntity fnSnmpStrToEntity = (pfnSnmpStrToEntity)GetProcAddress(hWinSnmp, "SnmpStrToEntity");
    pfnSnmpStrToContext fnSnmpStrToContext = (pfnSnmpStrToContext)GetProcAddress(hWinSnmp, "SnmpStrToContext");
    pfnSnmpCreatePdu fnSnmpCreatePdu = (pfnSnmpCreatePdu)GetProcAddress(hWinSnmp, "SnmpCreatePdu");
    pfnSnmpCreateVbl fnSnmpCreateVbl = (pfnSnmpCreateVbl)GetProcAddress(hWinSnmp, "SnmpCreateVbl");
    pfnSnmpSendMsg fnSnmpSendMsg = (pfnSnmpSendMsg)GetProcAddress(hWinSnmp, "SnmpSendMsg");
    pfnSnmpRecvMsg fnSnmpRecvMsg = (pfnSnmpRecvMsg)GetProcAddress(hWinSnmp, "SnmpRecvMsg");
    pfnSnmpGetPduData fnSnmpGetPduData = (pfnSnmpGetPduData)GetProcAddress(hWinSnmp, "SnmpGetPduData");
    pfnSnmpGetVb fnSnmpGetVb = (pfnSnmpGetVb)GetProcAddress(hWinSnmp, "SnmpGetVb");
    pfnSnmpCountVbl fnSnmpCountVbl = (pfnSnmpCountVbl)GetProcAddress(hWinSnmp, "SnmpCountVbl");
    pfnSnmpFreeDescriptor fnSnmpFreeDescriptor = (pfnSnmpFreeDescriptor)GetProcAddress(hWinSnmp, "SnmpFreeDescriptor");
    pfnSnmpFreeEntity fnSnmpFreeEntity = (pfnSnmpFreeEntity)GetProcAddress(hWinSnmp, "SnmpFreeEntity");
    pfnSnmpFreeContext fnSnmpFreeContext = (pfnSnmpFreeContext)GetProcAddress(hWinSnmp, "SnmpFreeContext");
    pfnSnmpFreePdu fnSnmpFreePdu = (pfnSnmpFreePdu)GetProcAddress(hWinSnmp, "SnmpFreePdu");
    pfnSnmpFreeVbl fnSnmpFreeVbl = (pfnSnmpFreeVbl)GetProcAddress(hWinSnmp, "SnmpFreeVbl");
    pfnSnmpStrToOid fnSnmpStrToOid = (pfnSnmpStrToOid)GetProcAddress(hWinSnmp, "SnmpStrToOid");

    smiUINT32 nMajor, nMinor, nLevel, nTranslateMode, nRetransmitMode;
    if (fnSnmpStartup(&nMajor, &nMinor, &nLevel, &nTranslateMode, &nRetransmitMode) != SNMPAPI_SUCCESS) {
        fprintf(stderr, "Fatal Error: WinSNMP subsystem initialization failed.\n");
        FreeLibrary(hWinSnmp);
        return 1;
    }

    HSNMP_SESSION session = fnSnmpCreateSession(NULL, 0, NULL, NULL);
    if (session == SNMPAPI_FAILURE) {
        fprintf(stderr, "Fatal Error: Session creation failed.\n");
        fnSnmpCleanup();
        FreeLibrary(hWinSnmp);
        return 1;
    }

    // Map Community string name
    smiOCTETS context_octets;
    context_octets.ptr = (smiLPBYTE)community;
    context_octets.len = (smiUINT32)strlen(community);

    HSNMP_ENTITY hTarget = fnSnmpStrToEntity(session, repeater_ip);
    HSNMP_CONTEXT hContext = fnSnmpStrToContext(session, &context_octets);

    // Continuous Processing Loop
    while (1) {
        if (_kbhit()) {
            char key = _getch();
            if (key == ESC_KEY) {
                printf("\nEscape detected. Exiting safely...\n");
                break;
            }
        }

        printf("\r[LIVE STATUS] ");

        for (int i = 0; i < total_metrics; i++) {
            smiOID target_oid;
            if (fnSnmpStrToOid(metrics[i].oid_str, &target_oid) == SNMPAPI_FAILURE) {
                printf("%s: OID Err | ", metrics[i].title);
                continue;
            }

            HSNMP_VBL hVbl = fnSnmpCreateVbl(session, &target_oid, NULL);
            HSNMP_PDU hPdu = fnSnmpCreatePdu(session, SNMP_PDU_GET, 1, 0, 0, hVbl);

            // Transmit SNMP request packet out to device interface
            if (fnSnmpSendMsg(session, NULL, hTarget, hContext, hPdu) == SNMPAPI_SUCCESS) {
                // Wait for network response (synchronous fallback loop)
                int retries = 10;
                BOOL packet_processed = FALSE;
                
                while (retries-- > 0 && !packet_processed) {
                    Sleep(50); 
                    HSNMP_ENTITY hSrc = NULL, hDst = NULL;
                    HSNMP_CONTEXT hCtx = NULL;
                    HSNMP_PDU hRecvPdu = NULL;

                    if (fnSnmpRecvMsg(session, &hSrc, &hDst, &hCtx, &hRecvPdu) == SNMPAPI_SUCCESS) {
                        smiINT pdu_type, req_id, err_status, err_index;
                        HSNMP_VBL hResponseVbl = NULL;

                        if (fnSnmpGetPduData(hRecvPdu, &pdu_type, &req_id, &err_status, &err_index, &hResponseVbl) == SNMPAPI_SUCCESS) {
                            if (err_status == SNMP_ERROR_NOERROR && fnSnmpCountVbl(hResponseVbl) > 0) {
                                smiOID out_oid;
                                smiVALUE out_val;
                                
                                if (fnSnmpGetVb(hResponseVbl, 1, &out_oid, &out_val) == SNMPAPI_SUCCESS) {
                                    if (out_val.syntax == SNMP_SYNTAX_INT || out_val.syntax == SNMP_SYNTAX_GAUGE32 || out_val.syntax == SNMP_SYNTAX_CNTR32) {
                                        printf("%s: %ld | ", metrics[i].title, out_val.value.sNumber);
                                    } else if (out_val.syntax == SNMP_SYNTAX_OCTETS) {
                                        char str_buf[128] = {0};
                                        size_t copy_len = (out_val.value.string.len < 127) ? out_val.value.string.len : 127;
                                        memcpy(str_buf, out_val.value.string.ptr, copy_len);
                                        printf("%s: %s | ", metrics[i].title, str_buf);
                                        fnSnmpFreeDescriptor(SNMP_SYNTAX_OCTETS, &out_val.value.string);
                                    } else {
                                        printf("%s: Raw type(%lu) | ", metrics[i].title, out_val.syntax);
                                    }
                                    fnSnmpFreeDescriptor(SNMP_SYNTAX_OID, (smiLPOCTETS)&out_oid);
                                }
                            } else {
                                printf("%s: SNMP Error | ", metrics[i].title);
                            }
                            fnSnmpFreeVbl(hResponseVbl);
                        }
                        
                        if (hSrc) fnSnmpFreeEntity(hSrc);
                        if (hDst) fnSnmpFreeEntity(hDst);
                        if (hCtx) fnSnmpFreeContext(hCtx);
                        if (hRecvPdu) fnSnmpFreePdu(hRecvPdu);
                        packet_processed = TRUE;
                    }
                }
                
                if (!packet_processed) {
                    printf("%s: Timeout | ", metrics[i].title);
                }
            } else {
                printf("%s: Offline | ", metrics[i].title);
            }

            // Cleanup local descriptor configurations safely
            fnSnmpFreeDescriptor(SNMP_SYNTAX_OID, (smiLPOCTETS)&target_oid);
            fnSnmpFreePdu(hPdu);
            fnSnmpFreeVbl(hVbl);
        }

        fflush(stdout);
        Sleep(2000); // 2 second metric poll interval spacing cadence
    }

    // Complete cleanup breakdown checklist safely
    fnSnmpFreeEntity(hTarget);
    fnSnmpFreeContext(hContext);
    fnSnmpClose(session);
    fnSnmpCleanup();
	FreeLibrary(hWinSnmp);
	return 0;
}
