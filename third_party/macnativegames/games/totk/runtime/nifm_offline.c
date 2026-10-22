#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

enum { NIFM_INTERFACE_INVALID = 0, NIFM_WIFI_STRENGTH_NONE = 0, NIFM_STATUS_CONNECTING_UNKNOWN_1 = 0 };

#ifdef GAME_ACNH
static void nifm_request_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response);
typedef struct { uint32_t writable[2]; atomic_uchar confirmation_option; atomic_bool submitted; } NifmRequestState;
static void nifm_request_close(void *opaque) {
    NifmRequestState *state = opaque;
    runtime_event_close_writable(state->writable[0]);
    runtime_event_close_writable(state->writable[1]);
    free(state);
}
#endif

static void nifm_general_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    switch (request->cmd) {
#ifdef GAME_ACNH
    case 4: {
        int32_t preset = 0;
        if (request->type != 6 || !request->domain ||
            request->domain_token != 0 || request->data_size != sizeof(preset) || !request->data ||
            request->has_pid || request->ncopy || request->nmove || request->nx ||
            request->na || request->nb || request->nw || request->nc) {
            fprintf(stderr,
                    "[acnh-net] nifm CreateRequest rejected shape: type=%u domain=%d object=%u token=%u data=%u pid=%d handles=%u/%u buffers=x%u/a%u/b%u/w%u/c%u\n",
                    request->type, request->domain, request->object_id, request->domain_token,
                    request->data_size, request->has_pid, request->ncopy, request->nmove,
                    request->nx, request->na, request->nb, request->nw, request->nc);
            ipc_unimplemented(session, request);
        }
        memcpy(&preset, request->data, sizeof(preset));
        if (preset != 2) {
            fprintf(stderr, "[acnh-net] nifm CreateRequest rejected RequirementPreset=%d\n", preset);
            ipc_unimplemented(session, request);
        }
        NifmRequestState *state = calloc(1, sizeof *state);
        if (!state) runtime_fail("NIFM IRequest state allocation");
        atomic_init(&state->confirmation_option, 0);
        atomic_init(&state->submitted, false);
        state->writable[0] = runtime_event_new();
        state->writable[1] = runtime_event_new();
        fprintf(stderr, "[acnh-net] nifm CreateRequest: RequirementPreset=2; IRequest created offline\n");
        ipc_out_object(response, "nifm:IRequest", nifm_request_handler, state, nifm_request_close);
        return;
    }
#endif
    case 18: {
        uint8_t interface_type = NIFM_INTERFACE_INVALID;
        uint8_t wifi_strength = NIFM_WIFI_STRENGTH_NONE;
        uint8_t connection_status = NIFM_STATUS_CONNECTING_UNKNOWN_1;
        IPC_OUT(response, interface_type);
        IPC_OUT(response, wifi_strength);
        IPC_OUT(response, connection_status);
        fprintf(stderr, "[totk-net] nifm GetInternetConnectionStatus: offline (no interface)\n");
        return;
    }
    case 21: {
        uint8_t accepted = 0;
        IPC_OUT(response, accepted);
        fprintf(stderr, "[totk-net] nifm IsAnyInternetRequestAccepted: false (offline)\n");
        return;
    }
    default:
        ipc_unimplemented(session, request);
    }
}

#ifdef GAME_ACNH
static void nifm_request_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    if (request->cmd == 0 || request->cmd == 1 || request->cmd == 4) {
        if (request->type != 6 || !request->domain || request->domain_token != 0 ||
            request->data_size != 0 || request->has_pid || request->ncopy || request->nmove ||
            request->nx || request->na || request->nb || request->nw || request->nc) {
            fprintf(stderr,
                    "[acnh-net] nifm IRequest %s rejected shape: type=%u domain=%d object=%u token=%u data=%u pid=%d handles=%u/%u buffers=x%u/a%u/b%u/w%u/c%u\n",
                    request->cmd == 0 ? "GetRequestState" : request->cmd == 1 ? "GetResult" : "Submit",
                    request->type, request->domain, request->object_id, request->domain_token,
                    request->data_size, request->has_pid, request->ncopy, request->nmove,
                    request->nx, request->na, request->nb, request->nw, request->nc);
            ipc_unimplemented(session, request);
        }
        NifmRequestState *state = session->state;
        if (!state) runtime_fail("NIFM IRequest missing state");
        if (request->cmd == 0) {
            uint32_t request_state = 1; /* 状态1用于尚未提交或请求失败，均不表示网络可用。 */
            IPC_OUT(response, request_state);
            fprintf(stderr, "[acnh-net] nifm IRequest GetRequestState=1 (offline)\n");
            return;
        }
        if (request->cmd == 1) {
            response->result = atomic_load_explicit(&state->submitted, memory_order_acquire)
                ? 0x8986eu : 0x26e6eu;
            fprintf(stderr, "[acnh-net] nifm IRequest GetResult=0x%x (offline policy)\n", response->result);
            return;
        }
        unsigned char option = atomic_load_explicit(&state->confirmation_option, memory_order_relaxed);
        if (option != 4) {
            fprintf(stderr, "[acnh-net] nifm IRequest Submit rejected confirmation option=%u\n", option);
            ipc_unimplemented(session, request);
        }
        atomic_store_explicit(&state->submitted, true, memory_order_release);
        runtime_event_signal(state->writable[0]);
        response->result = 0x8986eu;
        fprintf(stderr, "[acnh-net] nifm IRequest Submit: offline failure 0x8986e; first event signaled\n");
        return;
    }
    if (request->cmd == 11) {
        if (request->type != 6 || !request->domain || request->domain_token != 0 ||
            request->data_size != 1 || !request->data || request->has_pid || request->ncopy ||
            request->nmove || request->nx || request->na || request->nb || request->nw || request->nc) {
            fprintf(stderr,
                    "[acnh-net] nifm IRequest SetConnectionConfirmationOption rejected shape: type=%u domain=%d object=%u token=%u data=%u pid=%d handles=%u/%u buffers=x%u/a%u/b%u/w%u/c%u\n",
                    request->type, request->domain, request->object_id, request->domain_token,
                    request->data_size, request->has_pid, request->ncopy, request->nmove,
                    request->nx, request->na, request->nb, request->nw, request->nc);
            ipc_unimplemented(session, request);
        }
        uint8_t option = request->data[0];
        if (option < 1 || option > 5) {
            fprintf(stderr, "[acnh-net] nifm IRequest SetConnectionConfirmationOption rejected value=%u\n", option);
            ipc_unimplemented(session, request);
        }
        NifmRequestState *state = session->state;
        if (!state) runtime_fail("NIFM IRequest missing state");
        atomic_store_explicit(&state->confirmation_option, option, memory_order_relaxed);
        fprintf(stderr, "[acnh-net] nifm IRequest SetConnectionConfirmationOption=%u\n", option);
        return;
    }
    if (request->cmd == 2) {
        if (request->type != 6 || !request->domain || request->domain_token != 0 ||
            request->data_size != 0 || request->has_pid || request->ncopy || request->nmove ||
            request->nx || request->na || request->nb || request->nw || request->nc) {
            fprintf(stderr,
                    "[acnh-net] nifm IRequest GetSystemEventReadableHandles rejected shape: type=%u domain=%d object=%u token=%u data=%u pid=%d handles=%u/%u buffers=x%u/a%u/b%u/w%u/c%u\n",
                    request->type, request->domain, request->object_id, request->domain_token,
                    request->data_size, request->has_pid, request->ncopy, request->nmove,
                    request->nx, request->na, request->nb, request->nw, request->nc);
            ipc_unimplemented(session, request);
        }
        NifmRequestState *state = session->state;
        if (!state) runtime_fail("NIFM IRequest missing state");
        response->copy[0] = runtime_event_readable(state->writable[0]);
        response->copy[1] = runtime_event_readable(state->writable[1]);
        response->ncopy = 2;
        fprintf(stderr, "[acnh-net] nifm IRequest GetSystemEventReadableHandles: returned two readable handles\n");
        return;
    }
    fprintf(stderr,
            "[acnh-net] nifm IRequest unsupported cmd=%u type=%u domain=%d object=%u token=%u data=%u pid=%d handles=%u/%u buffers=x%u/a%u/b%u/w%u/c%u\n",
            request->cmd, request->type, request->domain, request->object_id,
            request->domain_token, request->data_size, request->has_pid,
            request->ncopy, request->nmove, request->nx, request->na,
            request->nb, request->nw, request->nc);
    ipc_unimplemented(session, request);
}
#endif

static void nifm_static_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    switch (request->cmd) {
    case 5:
        fprintf(stderr, "[totk-net] nifm CreateGeneralService: offline service ready\n");
        ipc_out_object(response, "nifm:u", nifm_general_handler, NULL, NULL);
        return;
    default:
        ipc_unimplemented(session, request);
    }
}

__attribute__((constructor)) static void register_nifm_offline(void) {
    runtime_ipc_register("nifm:u", nifm_static_handler, NULL, NULL);
}
