/* サービスへつなぐ口（nocturne_services.h）。 */
#include <nocturne_services.h>
#include <string.h>

n_handle n_service_connect(const char *name) {
    if (!name || !*name) return -N_ERR_INVALID;
    return (n_handle)__syscall(SYS_SERVICE_CONNECT, (long)name, (long)strlen(name), 0, 0, 0);
}
