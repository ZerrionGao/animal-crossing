#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/runtime/ipc.h"

static const char *registered_name;
static IpcHandler registered_handler;
static IpcHandler child_handler;

void runtime_ipc_register(const char *name, IpcHandler handler, IpcOpen open, void (*close)(void *)) {
    (void)open;
    (void)close;
    registered_name = name;
    registered_handler = handler;
}

void ipc_out(IpcResponse *response, const void *data, uint32_t size) {
    assert(response->data_size + size <= sizeof response->data);
    memcpy(response->data + response->data_size, data, size);
    response->data_size += size;
}

void ipc_out_object(IpcResponse *response, const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    assert(response->nobjects < 8);
    response->objects[response->nobjects++] = (IpcOutObject){name, handler, state, close};
}

_Noreturn void ipc_unimplemented(const IpcSession *session, const IpcRequest *request) {
    (void)session;
    (void)request;
    abort();
}

#include "../engine/runtime/nifm_offline.c"

int main(void) {
    assert(registered_name && strcmp(registered_name, "nifm:u") == 0);
    assert(registered_handler);

    IpcSession session = {.name = "nifm:u"};
    IpcRequest request = {.cmd = 5};
    IpcResponse response = {0};
    registered_handler(&session, &request, &response);
    assert(response.nobjects == 1 && response.objects[0].handler);
    assert(strcmp(response.objects[0].name, "nifm:u") == 0);
    child_handler = response.objects[0].handler;

    IpcSession general = {.name = "nifm:u"};
    request.cmd = 18;
    memset(&response, 0, sizeof response);
    child_handler(&general, &request, &response);
    assert(response.result == 0 && response.data_size == 3);
    assert(response.data[0] == 0 && response.data[1] == 0 && response.data[2] == 0);

    request.cmd = 21;
    memset(&response, 0, sizeof response);
    child_handler(&general, &request, &response);
    assert(response.result == 0 && response.data_size == 1 && response.data[0] == 0);
    return 0;
}
