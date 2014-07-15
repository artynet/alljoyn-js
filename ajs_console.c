/**
 * @file
 */
/******************************************************************************
 * Copyright (c) 2013, 2014, AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#define AJ_MODULE CONSOLE

#include "ajs.h"
#include "ajs_util.h"
#include "ajs_target.h"
#include "aj_crypto.h"
#include "ajs_services.h"

/**
 * Controls debug output for this module
 */
#ifndef NDEBUG
uint8_t dbgCONSOLE;
#endif

/*
 * Port number for the console service. This value must match the console port number defined in AllJoyn.js
 */
#define SCRIPT_CONSOLE_PORT 7714

#define ENDSWAP32(v) (((v) >> 24) | (((v) & 0xFF0000) >> 8) | (((v) & 0x00FF00) << 8) | ((v) << 24))

/*
 *  Reply codes for the eval and install methods
 */
#define SCRIPT_OK               0  /* script compiled and ran succesfully */
#define SCRIPT_SYNTAX_ERROR     1  /* script did not compile */
#define SCRIPT_EVAL_ERROR       2  /* script compiled but did not run */
#define SCRIPT_RESOURCE_ERROR   3  /* insufficient resources */
#define SCRIPT_NEED_RESET_ERROR 4  /* reset required before script can be installed */
#define SCRIPT_INTERNAL_ERROR   5  /* an undiagnosed internal error */

typedef enum {
    ENGINE_RUNNING, /* A script is installed and the engine is running */
    ENGINE_CLEAN,   /* The engine has been reset, there is no script running */
    ENGINE_DIRTY    /* The engine is in an unknown state */
} EngineState;

static EngineState engineState = ENGINE_RUNNING;

static const size_t MAX_EVAL_LEN = 1024;

static const char* const scriptConsoleIface[] = {
    "org.allseen.scriptConsole",
    "@engine>s",                                   /* Script engine supported e.g. JavaScript, Lua, Python, etc. */
    "@maxEvalLen>u",                               /* Maximum size script the eval method can handle */
    "@maxScriptLen>u",                             /* Maximum size script the install method can handle */
    "?eval script<ay status>y output>s",           /* Evaluate a short script and run it */
    "?install name<s script<ay status>y output>s", /* Install a new script, the script engine must be in a reset state */
    "?reset",                                      /* Reset the script engine */
    "?reboot",                                     /* Reboot the device */
    "!print txt>s",                                /* Send a print string to the controller */
    "!alert txt>s",                                /* Send an alert string to the controller */
    NULL
};

static const AJ_InterfaceDescription consoleInterfaces[] = {
    AJ_PropertiesIface,
    scriptConsoleIface,
    NULL
};

static const AJ_Object consoleObjects[] = {
    { "/ScriptConsole", consoleInterfaces, AJ_OBJ_FLAG_ANNOUNCED },
    { NULL, NULL }
};

#define GET_PROP_MSGID      AJ_APP_MESSAGE_ID(0, 0, AJ_PROP_GET)
#define SET_PROP_MSGID      AJ_APP_MESSAGE_ID(0, 0, AJ_PROP_SET)

#define SCRIPT_ENGINE_PROP  AJ_APP_PROPERTY_ID(0, 1, 0)
#define MAX_EVAL_LEN_PROP   AJ_APP_PROPERTY_ID(0, 1, 1)
#define MAX_SCRIPT_LEN_PROP AJ_APP_PROPERTY_ID(0, 1, 2)

#define EVAL_MSGID          AJ_APP_MESSAGE_ID(0,  1, 3)
#define INSTALL_MSGID       AJ_APP_MESSAGE_ID(0,  1, 4)
#define RESET_MSGID         AJ_APP_MESSAGE_ID(0,  1, 5)
#define REBOOT_MSGID        AJ_APP_MESSAGE_ID(0,  1, 6)
#define PRINT_SIGNAL_MSGID  AJ_APP_MESSAGE_ID(0,  1, 7)
#define ALERT_SIGNAL_MSGID  AJ_APP_MESSAGE_ID(0,  1, 8)

/**
 * Active session for this service
 */
static uint32_t consoleSession = 0;
static char consoleBusName[16];

static void SignalConsole(duk_context* ctx, uint32_t sigId)
{
    AJ_Status status;
    AJ_Message msg;
    size_t len = 0;
    size_t sz;
    duk_idx_t i;
    duk_idx_t nargs = duk_get_top(ctx);
    const char nul = 0;
    AJ_BusAttachment* bus = AJS_GetBusAttachment();

    status = AJ_MarshalSignal(bus, &msg, sigId, consoleBusName, consoleSession, 0, 0);
    if (status == AJ_OK) {
        for (i = 0; i < nargs; ++i) {
            duk_safe_to_lstring(ctx, i, &sz);
            len += sz;
        }
        status = AJ_DeliverMsgPartial(&msg, len + sizeof(uint32_t) + sizeof(nul));
    }
    if (status == AJ_OK) {
        status = AJ_MarshalRaw(&msg, &len, 4);
    }
    for (i = 0; i < nargs && status == AJ_OK; ++i) {
        const char* str = duk_safe_to_lstring(ctx, i, &sz);
        status = AJ_MarshalRaw(&msg, str, sz);
    }
    if (status == AJ_OK) {
        /*
         * Marshal the terminating NUL
         */
        status = AJ_MarshalRaw(&msg, &nul, 1);
    }
    if (status == AJ_OK) {
        status = AJ_DeliverMsg(&msg);
    }
    if (status != AJ_OK) {
        AJ_ErrPrintf(("Failed to deliver signal error:%s\n", AJ_StatusText(status)));
    }
}

static void AlertLocal(duk_context* ctx, uint8_t alert)
{
    const char* str;
    int nargs = duk_get_top(ctx);
    int i;

    for (i = 0; i < nargs; ++i) {
        duk_dup(ctx, i);
    }
    duk_concat(ctx, nargs);
    str = duk_get_string(ctx, -1);
    if (alert) {
        AJ_Printf("ALERT: %s\n", str);
    } else {
        if (dbgCONSOLE) {
            AJ_Printf("PRINT: %s\n", str);
        }
    }
    duk_pop(ctx);
}

void AJS_AlertHandler(duk_context* ctx, uint8_t alert)
{
    if (consoleSession) {
        SignalConsole(ctx, alert ? ALERT_SIGNAL_MSGID : PRINT_SIGNAL_MSGID);
    } else {
        AlertLocal(ctx, alert);
    }
}

static int SafeAlert(duk_context* ctx)
{
    AJS_AlertHandler(ctx, TRUE);
    return 0;
}

void AJS_ConsoleSignalError(duk_context* ctx)
{
    duk_safe_call(ctx, SafeAlert, 0, 0);
}

static AJ_Status EvalReply(duk_context* ctx, AJ_Message* msg, int dukStatus)
{
    AJ_Message reply;
    uint8_t replyStatus;
    const char* replyTxt;

    switch (dukStatus) {
    case DUK_EXEC_SUCCESS:
        replyStatus = SCRIPT_OK;
        break;

    case DUK_RET_EVAL_ERROR:
    case DUK_RET_TYPE_ERROR:
    case DUK_RET_RANGE_ERROR:
        break;

    case DUK_RET_SYNTAX_ERROR:
        replyStatus = SCRIPT_SYNTAX_ERROR;
        break;

    case DUK_RET_ALLOC_ERROR:
        replyStatus = SCRIPT_RESOURCE_ERROR;
        break;

    default:
        replyStatus = SCRIPT_INTERNAL_ERROR;
    }

    AJ_MarshalReplyMsg(msg, &reply);

    duk_to_string(ctx, -1);
    replyTxt = duk_get_string(ctx, -1);
    AJ_MarshalArgs(&reply, "ys", replyStatus, replyTxt);
    duk_pop(ctx);
    return AJ_DeliverMsg(&reply);
}

static AJ_Status Eval(duk_context* ctx, AJ_Message* msg)
{
    AJ_Message error;
    AJ_Status status;
    duk_int_t retval;
    size_t sz;
    const void* raw;
    uint32_t len;
    uint8_t endswap = (msg->hdr->endianess != AJ_NATIVE_ENDIAN);

    status = AJ_UnmarshalRaw(msg, &raw, sizeof(len), &sz);
    if (status != AJ_OK) {
        goto ErrorReply;
    }
    memcpy(&len, raw, sizeof(len));
    if (endswap) {
        len = ENDSWAP32(len);
    }
    if (len > MAX_EVAL_LEN) {
        retval = DUK_RET_ALLOC_ERROR;
        duk_push_string((ctx), "Eval expression too long");
    } else {
        uint8_t* js = AJ_Malloc(len);
        size_t l = len;
        uint8_t* p = js;
        if (!js) {
            status = AJ_ERR_RESOURCES;
            goto ErrorReply;
        }
        while (l) {
            status = AJ_UnmarshalRaw(msg, &raw, l, &sz);
            if (status != AJ_OK) {
                AJ_Free(js);
                goto ErrorReply;
            }
            memcpy(p, raw, sz);
            p += sz;
            l -= sz;
        }
        /*
         * Strip trailing NULs
         */
        while (len && !js[len - 1]) {
            --len;
        }
        duk_push_string(ctx, "ConsoleInput.js");
        retval = duk_pcompile_lstring_filename(ctx, 0, (const char*)js, len);
        AJ_Free(js);
        if (retval == DUK_EXEC_SUCCESS) {
            retval = duk_pcall(ctx, 0);
        }
        /*
         * A succesful eval leaves the engine in an unknown state
         */
        if (retval == DUK_EXEC_SUCCESS) {
            engineState = ENGINE_DIRTY;
        }
    }
    return EvalReply(ctx, msg, retval);

ErrorReply:

    AJ_MarshalStatusMsg(msg, &error, status);
    return AJ_DeliverMsg(&error);
}

static AJ_Status Install(duk_context* ctx, AJ_Message* msg)
{
    AJ_Message reply;
    AJ_Status status;
    const void* raw;
    const char* scriptName;
    size_t sz;
    uint8_t replyStatus;
    size_t len;
    uint8_t endswap = (msg->hdr->endianess != AJ_NATIVE_ENDIAN);

    /*
     * Scripts can only be installed on a clean engine
     */
    if (engineState != ENGINE_CLEAN) {
        AJ_Message reply;
        AJ_MarshalReplyMsg(msg, &reply);
        AJ_MarshalArgs(&reply, "ys", SCRIPT_NEED_RESET_ERROR, "Reset required");
        return AJ_DeliverMsg(&reply);
    }
    status = AJ_UnmarshalArgs(msg, "s", &scriptName);
    if (status != AJ_OK) {
        goto ErrorReply;
    }
    /*
     * Save the script name so it can be passed to the compiler
     */
    duk_push_global_stash(ctx);
    duk_push_string(ctx, scriptName);
    duk_put_prop_string(ctx, -2, "scriptName");
    AJ_InfoPrintf(("Installing script %s\n", scriptName));
    scriptName = NULL;
    /*
     * Load script and install it
     */
    status = AJ_UnmarshalRaw(msg, &raw, sizeof(len), &sz);
    if (status != AJ_OK) {
        goto ErrorReply;
    }
    memcpy(&len, raw, sizeof(len));
    if (endswap) {
        len = ENDSWAP32(len);
    }
    AJ_MarshalReplyMsg(msg, &reply);
    if (len > AJS_GetMaxScriptLen()) {
        replyStatus = SCRIPT_RESOURCE_ERROR;
        AJ_MarshalArgs(&reply, "ys", replyStatus, "Script too long");
        AJ_ErrPrintf(("Script installation failed - too long\n"));
        status = AJ_DeliverMsg(&reply);
    } else {
        void* scriptf = AJS_OpenScript(AJS_SCRIPT_WRITE);
        while (len) {
            status = AJ_UnmarshalRaw(msg, &raw, len, &sz);
            if (status != AJ_OK) {
                AJS_CloseScript(scriptf);
                goto ErrorReply;
            }
            AJS_WriteScript(scriptf, raw, sz);
            len -= sz;
        }
        AJS_CloseScript(scriptf);
        replyStatus = SCRIPT_OK;
        AJ_MarshalArgs(&reply, "ys", replyStatus, "Script installed");
        AJ_InfoPrintf(("Script succesfully installed\n"));
        status = AJ_DeliverMsg(&reply);
        if (status == AJ_OK) {
            /*
             * Return a RESTART_APP status code; this will cause the msg loop to exit and reload the
             * script engine and run the script we just installed.
             */
            status = AJ_ERR_RESTART_APP;
        }
    }
    return status;

ErrorReply:

    AJ_MarshalStatusMsg(msg, &reply, status);
    return AJ_DeliverMsg(&reply);
}

static AJ_Status Reset(AJ_Message* msg)
{
    AJ_Status status;
    AJ_Message reply;

    AJ_MarshalReplyMsg(msg, &reply);
    status = AJ_DeliverMsg(&reply);
    if (status == AJ_OK) {
        engineState = ENGINE_CLEAN;
        /*
         * The script engine must be restarted after a reset
         */
        status = AJ_ERR_RESTART_APP;
    }
    return status;
}

static AJ_Status PropGetHandler(AJ_Message* replyMsg, uint32_t propId, void* context)
{
    switch (propId) {
    case SCRIPT_ENGINE_PROP:
        return AJ_MarshalArgs(replyMsg, "s", "JavaScript");

    case MAX_EVAL_LEN_PROP:
        return AJ_MarshalArgs(replyMsg, "u", MAX_EVAL_LEN);

    case MAX_SCRIPT_LEN_PROP:
        return AJ_MarshalArgs(replyMsg, "u", (uint32_t)AJS_GetMaxScriptLen());

    default:
        return AJ_ERR_UNEXPECTED;
    }
}

static AJ_Status PropSetHandler(AJ_Message* replyMsg, uint32_t propId, void* context)
{
    return AJ_ERR_UNEXPECTED;
}

AJ_Status AJS_ConsoleMsgHandler(duk_context* ctx, AJ_Message* msg)
{
    AJ_Status status;

    if (msg->msgId == AJ_METHOD_ACCEPT_SESSION) {
        uint32_t sessionId;
        uint16_t port;
        char* joiner;
        status = AJ_UnmarshalArgs(msg, "qus", &port, &sessionId, &joiner);
        if (status != AJ_OK) {
            return status;
        }
        if (port == SCRIPT_CONSOLE_PORT) {
            /*
             * Only allow one controller at a time
             */
            if (consoleSession) {
                status = AJ_BusReplyAcceptSession(msg, FALSE);
            } else {
                status = AJ_BusReplyAcceptSession(msg, TRUE);
                if (status == AJ_OK) {
                    size_t sz = strlen(joiner) + 1;
                    AJ_InfoPrintf(("Accepted session session_id=%u joiner=%s\n", sessionId, joiner));
                    if (sz > sizeof(consoleBusName)) {
                        return AJ_ERR_RESOURCES;
                    }
                    consoleSession = sessionId;
                    memcpy(consoleBusName, joiner, sz);
                }
            }
        } else {
            /*
             * Not for us, reset the args so they can be unmarshaled again.
             */
            status = AJ_ResetArgs(msg);
            if (status == AJ_OK) {
                status = AJ_ERR_NO_MATCH;
            }
        }
        return status;
    }
    /*
     * If there is no console attached then this message is not for us
     */
    status = AJ_ERR_NO_MATCH;
    if (!consoleSession) {
        return status;
    }
    switch (msg->msgId) {
    case AJ_SIGNAL_SESSION_LOST_WITH_REASON:
        {
            uint32_t sessionId;

            status = AJ_UnmarshalArgs(msg, "u", &sessionId);
            if (status != AJ_OK) {
                return status;
            }
            if (sessionId == consoleSession) {
                consoleSession = 0;
                status = AJ_OK;
            } else {
                /*
                 * Not our session, reset the args so they can be unmarshaled again.
                 */
                status = AJ_ResetArgs(msg);
                if (status == AJ_OK) {
                    status = AJ_ERR_NO_MATCH;
                }
            }
        }
        break;

    case GET_PROP_MSGID:
        status = AJ_BusPropGet(msg, PropGetHandler, NULL);
        break;

    case SET_PROP_MSGID:
        status = AJ_BusPropSet(msg, PropSetHandler, NULL);
        break;

    case INSTALL_MSGID:
        status = Install(ctx, msg);
        break;

    case RESET_MSGID:
        status = Reset(msg);
        break;

    case REBOOT_MSGID:
        AJ_Reboot();
        break;

    case EVAL_MSGID:
        status = Eval(ctx, msg);
        break;

    default:
        break;
    }
    return status;
}

AJ_Status AJS_ConsoleInit(AJ_BusAttachment* aj)
{
    AJ_Status status;

    AJ_RegisterObjectList(consoleObjects, AJ_APP_ID_FLAG);
    status = AJ_BusBindSessionPort(aj, SCRIPT_CONSOLE_PORT, NULL, AJ_FLAG_NO_REPLY_EXPECTED);
    if (status != AJ_OK) {
        AJ_RegisterObjects(NULL, NULL);
    }
    return status;
}

void AJS_ConsoleTerminate()
{
    consoleSession = 0;
    engineState = ENGINE_DIRTY;
    AJ_RegisterObjects(NULL, NULL);
}
